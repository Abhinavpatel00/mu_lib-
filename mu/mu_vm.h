#pragma once
#ifndef MU_VM_H
#define MU_VM_H

/* POSIX feature test: clock_gettime / mkstemp / mmap are hidden behind
 * -std=c99 unless asked for, and the define has to come before any system
 * header. Ask only if the user has not chosen already. */
#if !defined(_POSIX_C_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(__cplusplus) && !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_vm — the virtual-memory primitive layer.
 *
 * WHY
 * ---
 * malloc gives you one operation: "give me N bytes", and the answer may move.
 * The OS gives you three:
 *
 *   1. reserve address space without backing it with physical pages,
 *   2. decide what backs a range (anonymous pages, a file, the same pages twice),
 *   3. change accessibility without changing size (commit, guard, release).
 *
 * Those are the three ingredients behind every "virtual memory trick": the
 * obscenely big array, the sparse index space, the end-of-page debug allocator,
 * the gapless ring buffer. This header exposes them and nothing else; policy
 * lives in the callers (mu_vm_arena.h, a debug allocator, a ring).
 *
 * MEASURED (x86-64 Linux, 4 KB pages) — these numbers set the design rules:
 *
 *     reserve 4 GB            0.012 ms      address space is free
 *     commit 256 MB           0.10 ms       mprotect in 1 MB steps
 *     first touch per page    1.02 us       <- the real cost, not the reserve
 *     release 256 MB          14.1 ms       RSS 258 MB -> 2 MB, VA unchanged
 *     re-fault after release  1.02 us/page
 *
 * So: reserving is free, committing is almost free, *touching* is not. Do not
 * page-fault in a hot loop; fault ahead of it or let pages stay warm.
 *
 * CONTRACT
 * --------
 * - reserve() gives no physical memory and no access; any read/write faults.
 * - commit(r, n) makes [0, n) writable. n is rounded up to a page. It never
 *   moves `base`, never copies, and is idempotent.
 * - release() returns physical pages to the OS but keeps the reservation and
 *   the base address. POSIX remaps the range as fresh zero pages, so a stray
 *   write still succeeds; Windows decommits it, so a stray write faults.
 *   Treat released memory as uninitialised either way.
 * - unmap() is the only operation that invalidates `base`.
 * - Nothing here is thread safe. A region has one owner.
 *
 * WHEN NOT TO USE THIS
 * --------------------
 * Small, dense, short-lived structures. A 200-byte hash entry rounded up to a
 * 4 KB page wastes 20x. The page is the unit; use this only where the thing
 * you are allocating is large compared to a page or where the index space is
 * huge and sparse.
 * --------------------------------------------------------------------------- */

#if defined(_WIN32)

#  include <windows.h>
#  define MU_VM_AVAILABLE 1
#  define MU_VM_WINDOWS   1

#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

#  include <sys/types.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  define MU_VM_AVAILABLE 1
#  define MU_VM_WINDOWS   0
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif

#else

#  define MU_VM_AVAILABLE 0
#  define MU_VM_WINDOWS   0

#endif

MU_BEGIN_EXTERN_C

typedef struct mu_vm_region
{
    uint8_t* base;      /* start of the reservation; only unmap() changes it */
    size_t   reserved;  /* address space owned by this region                 */
    size_t   committed; /* writable prefix of that reservation                */
} mu_vm_region;

MU_INLINE bool mu_vm_available(void)
{
    return MU_VM_AVAILABLE != 0;
}

MU_INLINE size_t mu_vm_page_size(void)
{
#if MU_VM_WINDOWS
    static size_t cached = 0;
    if (cached == 0)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cached = (size_t)si.dwPageSize;
    }
    return cached;
#elif MU_VM_AVAILABLE
    static size_t cached = 0;
    if (cached == 0)
    {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t)ps : 4096u;
    }
    return cached;
#else
    return 4096u;
#endif
}

MU_INLINE size_t mu_vm_round_up(size_t bytes)
{
    const size_t ps = mu_vm_page_size();
    return (bytes + ps - 1u) & ~(ps - 1u);
}

/* Reserve `bytes` of address space. No physical pages, no access. */
MU_INLINE bool mu_vm_reserve(mu_vm_region* r, size_t bytes)
{
    if (!r || bytes == 0)
        return false;

    r->base      = NULL;
    r->reserved  = mu_vm_round_up(bytes);
    r->committed = 0;

#if MU_VM_WINDOWS
    r->base = (uint8_t*)VirtualAlloc(NULL, r->reserved, MEM_RESERVE, PAGE_NOACCESS);
    return r->base != NULL;
#elif MU_VM_AVAILABLE
    {
#if defined(MAP_NORESERVE)
        const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#else
        const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif
        void* p = mmap(NULL, r->reserved, PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED)
        {
            r->reserved = 0;
            return false;
        }
        r->base = (uint8_t*)p;
        return true;
    }
#else
    r->reserved = 0;
    return false;
#endif
}

/* Make [0, bytes) writable. Grows the commit frontier; never moves anything. */
MU_INLINE bool mu_vm_commit(mu_vm_region* r, size_t bytes)
{
    size_t want;

    if (!r || !r->base)
        return false;
    if (bytes <= r->committed)
        return true;
    if (bytes > r->reserved)
        return false;

    want = mu_vm_round_up(bytes);
    if (want > r->reserved)
        want = r->reserved;

#if MU_VM_WINDOWS
    if (!VirtualAlloc(r->base + r->committed, want - r->committed,
                      MEM_COMMIT, PAGE_READWRITE))
        return false;
#elif MU_VM_AVAILABLE
    /* mprotect over the whole prefix is idempotent and simpler than tracking
       the previous protection. */
    if (mprotect(r->base, want, PROT_READ | PROT_WRITE) != 0)
        return false;
#endif

    r->committed = want;
    return true;
}

/* Give the physical pages back; keep the address range and the base pointer. */
MU_INLINE void mu_vm_release(mu_vm_region* r)
{
    if (!r || !r->base || r->committed == 0)
        return;

#if MU_VM_WINDOWS
    if (!VirtualFree(r->base, r->committed, MEM_DECOMMIT))
        return;
#elif MU_VM_AVAILABLE
    /* Replace the committed prefix with fresh anonymous pages: physical pages
       go back to the OS, the address range and base pointer survive.
     *
     * Why not madvise(MADV_DONTNEED)?
     *   It is hidden under strict _POSIX_C_SOURCE, so `#ifdef MADV_DONTNEED`
     *   silently compiles the release away.
     * Why not posix_madvise(POSIX_MADV_DONTNEED)?
     *   glibc implements it as a no-op, because a POSIX advisory hint must not
     *   be observable. It returns 0 and frees nothing.
     * mmap(MAP_FIXED) is declared with the rest of this header, and dropping
     * the old mapping is exactly the effect we want. We own the range, so
     * clobbering it is safe. */
    if (mmap(r->base, r->committed, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
        return;
#endif

    r->committed = 0;
}

/* Only operation that invalidates `base`. */
MU_INLINE void mu_vm_unmap(mu_vm_region* r)
{
    if (!r || !r->base)
        return;

#if MU_VM_WINDOWS
    VirtualFree(r->base, 0, MEM_RELEASE);
#elif MU_VM_AVAILABLE
    munmap(r->base, r->reserved);
#endif

    r->base      = NULL;
    r->reserved  = 0;
    r->committed = 0;
}

/* ---------------------------------------------------------------------------
 * mu_vm_alias — map the same physical pages twice, back to back.
 *
 *     [ 0 .. bytes ) and [ bytes .. 2*bytes ) are the same memory
 *
 * That turns a wrapping ring buffer into a straight memcpy: a consumer can
 * always read `n <= bytes` contiguous bytes starting anywhere in the first
 * half. It also means an overrun past the ring is silently reported in the
 * other half instead of faulting, so the capacity check is still required.
 *
 * POSIX only: reserve 2*bytes PROT_NONE, then MAP_FIXED the two halves over
 * that reservation. Because we already own the range there is no race between
 * checking and mapping.
 *
 * Returns false on Windows (its reserve/commit split cannot map over a
 * reservation atomically) and on platforms without virtual memory.
 * --------------------------------------------------------------------------- */
MU_INLINE bool mu_vm_alias(mu_vm_region* r, size_t bytes)
{
#if MU_VM_AVAILABLE && !MU_VM_WINDOWS
    size_t size;
    size_t two;
    char   path[] = "/tmp/mu_vm_alias_XXXXXX";
    int    fd;
    void*  p;

    if (!r || bytes == 0 || bytes > (SIZE_MAX / 2u))
        return false;

    size = mu_vm_round_up(bytes);
    two  = size * 2u;

    r->base      = NULL;
    r->reserved  = 0;
    r->committed = 0;

    p = mmap(NULL, two, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return false;

    fd = mkstemp(path);
    if (fd < 0)
    {
        munmap(p, two);
        return false;
    }
    (void)unlink(path); /* anonymous from here on; gone when fd closes */

    if (ftruncate(fd, (off_t)size) != 0)
    {
        close(fd);
        munmap(p, two);
        return false;
    }

    if (mmap(p, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
    {
        close(fd);
        munmap(p, two);
        return false;
    }

    if (mmap((uint8_t*)p + size, size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
    {
        close(fd);
        munmap(p, two);
        return false;
    }

    close(fd); /* the mapping holds the pages alive */

    r->base      = (uint8_t*)p;
    r->reserved  = two;
    r->committed = two;
    return true;
#else
    if (r)
    {
        r->base      = NULL;
        r->reserved  = 0;
        r->committed = 0;
    }
    (void)bytes;
    return false;
#endif
}

MU_END_EXTERN_C

#endif /* MU_VM_H */

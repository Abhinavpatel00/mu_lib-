/*
 * card_asm_dump.c
 * ============================================================
 * Pure assembly-inspection target.  Compile with:
 *   gcc -O2 -march=native -std=c11 -S -fverbose-asm card_asm_dump.c
 * Then diff the four functions in the .s output.
 * ============================================================
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint32_t next;
    uint8_t  info;
    char     title[3];
} Card;

/* ---------- 64-bit single compare ---------- */
bool eq_64(const Card *a, const Card *b) {
    uint64_t va, vb;
    memcpy(&va, a, 8);
    memcpy(&vb, b, 8);
    return va == vb;
}

/* ---------- two 32-bit compares ------------ */
bool eq_2x32(const Card *a, const Card *b) {
    uint32_t alo, ahi, blo, bhi;
    memcpy(&alo, (const char *)a, 4);
    memcpy(&ahi, (const char *)a + 4, 4);
    memcpy(&blo, (const char *)b, 4);
    memcpy(&bhi, (const char *)b + 4, 4);
    return (alo == blo) & (ahi == bhi);
}

/* ---------- field-by-field (naïve) ---------- */
bool eq_fields(const Card *a, const Card *b) {
    return a->next == b->next && a->info == b->info && a->title[0] == b->title[0] && a->title[1] == b->title[1] &&
           a->title[2] == b->title[2];
}

/* ---------- memcmp -------------------------- */
bool eq_memcmp(const Card *a, const Card *b) { return memcmp(a, b, sizeof(Card)) == 0; }

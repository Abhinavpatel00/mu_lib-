# `mu_debug_break.h` — portable software breakpoint

`mu_debug_break()` stops in an attached debugger at the call site. It emits a
real breakpoint instruction rather than a generic trap, so the debugger sees
the expected `SIGTRAP` (or platform equivalent) and can continue normally.

```c
#include "mu.h"

if (bad_state)
    mu_debug_break();
```

`mu.h` includes this header automatically. The macro form is also available
and is what `MU_PANIC()` uses:

```c
MU_DEBUG_BREAK();
```

## Why not `__builtin_trap()`?

On x86 `__builtin_trap()` emits `ud2`, which raises `SIGILL`, and the compiler
may assume the following code is unreachable. `mu_debug_break()` uses `int3`
instead and acts as a compiler barrier, so code after the breakpoint survives
optimization.

## Target behavior

| Target | Instruction / behavior |
|--------|------------------------|
| MSVC / clang-cl | `__debugbreak()` |
| x86, x86-64 | `int3` |
| ARM | `BKPT` (`.inst 0xe7f001f0`) |
| Thumb | `BKPT` (`.inst 0xde01`) |
| AArch64 (Apple clang) | `__builtin_debugtrap()` |
| AArch64 | `brk #0` (`.inst 0xd4200000`) |
| PowerPC | `twge r2, r2` (`.4byte 0x7d821008`) |
| RISC-V | `ebreak` (`.4byte 0x00100073`) |
| Other GCC/Clang | `__builtin_trap()` fallback |
| Other compilers | null write fallback |

The ARM and PowerPC breakpoint instructions have a known GDB stepping issue:
`step`/`continue` can get stuck on the breakpoint instruction. Jump over it
manually or use the `debugbreak-gdb.py` helper from the upstream project.

## Overriding

`MU_DEBUG_BREAK()` is a normal `#ifndef`-guarded macro. Define it before
including any `mu` header to replace the implementation:

```c
#define MU_DEBUG_BREAK() my_platform_trap()
#include "mu.h"
```

Override `MU_ASSERT`/`MU_PANIC` separately if you want different assertion
behavior; `MU_ASSERT` defaults to the C standard `assert()` and is compiled
out under `NDEBUG`.

## Notes

- Without a debugger the breakpoint usually terminates the process.
- `mu_debug_break()` is a `static inline` header function; no link dependency.
- Not a logging or abort facility. Use `MU_ASSERT` for invariants and
  `MU_PANIC()` when you want a breakpoint followed by `abort()`.

Instruction sequences adapted from Scott Tsai's
[debugbreak.h](https://github.com/scottt/debugbreak) (BSD-2-Clause). The full
notice is retained in `mu/mu_debug_break.h`.

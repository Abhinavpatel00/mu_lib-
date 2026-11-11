# Copilot instructions for `mylib`

## API and naming
- Keep public API names `mu_*` prefixed.
- Keep declarations and call sites mu-prefixed (`mu_*`), including bitset APIs.
- Prefer explicit, small functions over macro-heavy public APIs.
- Preserve existing naming and style unless a change is required.

## Compatibility
- Keep [mu.h](../mu.h) C99-compliant.
- Prefer C standard headers (`<stddef.h>`, `<stdint.h>`, `<stdbool.h>`, `<stdlib.h>`, `<string.h>`, `<stdio.h>`) and cglm in the public header 


## Code style
- Keep code clean and readable.
- Add short explanatory comments for non-obvious logic (especially bit math and memory layout).
- Avoid unnecessary branching and defensive checks that add noise without value.
- Do not break code into unnecessary helper functions. Longer functions are often better; do not extract a helper with one call site or split a function because it is long.
- Keep `if` blocks and null checks out of setup, demo and tool code: a failure there is fatal where it happens, and callers do not re-check. See `AGENTS.md` §12.5 and §12.6.

## Memory and checks
- Do **not** add unnecessary `malloc` null checks in normal paths unless the API contract explicitly requires them.
- Do not add redundant checks that duplicate invariants already guaranteed by callers.

## Testing
- For any new API behavior, add or update tests in [test.cpp](../test.cpp).
- Keep tests focused, deterministic, and simple to read.
- Validate observable behavior (set/reset/test, resize behavior, copies) rather than internal implementation details.

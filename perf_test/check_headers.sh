#!/bin/sh
# check_headers.sh -- the library must build with nothing but the C standard
# library, and every header must stand alone.
#
# Two checks, both cheap enough to run per commit:
#
#   1. no library header may include cglm/glm. mu_math.h is the math backend
#      now; perf_test/bench_math_vs_cglm.c may still include cglm, because it is
#      a benchmark, not part of the library.
#   2. each mu/*.h compiles as the first include of an empty translation unit,
#      in C99, C11 and C++14, with -Wall -Wextra -Werror and no cglm on the
#      include path. A header that only works after some other header has been
#      included is a header with a hidden dependency.
#
#   sh perf_test/check_headers.sh          # or: make -C perf_test headers-test
#
# Exit status is the number of failures, capped at 125.

set -u
cd "$(dirname "$0")/.." || exit 125

CC=${CC:-gcc}
CXX=${CXX:-g++}
TMP=$(mktemp -d) || exit 125
trap 'rm -rf "$TMP"' EXIT INT TERM

fails=0
files=0

echo "check_headers: no third-party math in the library"

if grep -n --include='*.h' -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](cglm|glm)/' mu/*.h >"$TMP/hits" 2>/dev/null; then
    echo "  FAIL cglm/glm include in a library header:"
    sed 's/^/        /' "$TMP/hits"
    fails=$((fails + 3))
else
    echo "  ok   no cglm/glm include in mu/*.h"
fi

echo "check_headers: every header stands alone (C99, C11, C++14)"

for h in mu/*.h; do
    base=$(basename "$h")
    printf '#include "%s"\nint main(void) { return 0; }\n' "$base" >"$TMP/tu.c"
    cp "$TMP/tu.c" "$TMP/tu.cpp"
    files=$((files + 1))

    for std in c99 c11; do
        if ! $CC -std=$std -O1 -Wall -Wextra -Werror -I mu -I . -fsyntax-only "$TMP/tu.c" 2>"$TMP/err"; then
            echo "  FAIL $h ($std)"
            head -4 "$TMP/err" | sed 's/^/        /'
            fails=$((fails + 1))
        fi
    done
    if ! $CXX -std=c++14 -O1 -Wall -Wextra -Werror -I mu -I . -fsyntax-only "$TMP/tu.cpp" 2>"$TMP/err"; then
        echo "  FAIL $h (c++14)"
        head -4 "$TMP/err" | sed 's/^/        /'
        fails=$((fails + 1))
    fi
done

echo "check_headers: the umbrella mu.h, same three modes"
printf '#include "mu.h"\nint main(void) { return 0; }\n' >"$TMP/all.c"
cp "$TMP/all.c" "$TMP/all.cpp"
for std in c99 c11; do
    if ! $CC -std=$std -O1 -Wall -Wextra -Werror -I mu -I . -fsyntax-only "$TMP/all.c" 2>"$TMP/err"; then
        echo "  FAIL mu.h ($std)"
        head -4 "$TMP/err" | sed 's/^/        /'
        fails=$((fails + 1))
    fi
done
if ! $CXX -std=c++14 -O1 -Wall -Wextra -Werror -I mu -I . -fsyntax-only "$TMP/all.cpp" 2>"$TMP/err"; then
    echo "  FAIL mu.h (c++14)"
    head -4 "$TMP/err" | sed 's/^/        /'
    fails=$((fails + 1))
fi

echo "check_headers: $files headers, $fails failure(s)"
[ "$fails" -eq 0 ] || exit $((fails > 125 ? 125 : fails))
echo "PASSED: library has no cglm dependency and every header stands alone"
exit 0

# Performance & DOD Proof Suite Makefile
# Builds both original microbenchmarks and the 5 comprehensive DOD/SIMD/DS/MT proof suites.

CC       = gcc
CFLAGS   = -O3 -march=native -std=c11 -Wall -Wextra -Werror
INCLUDES = -I.. -I../mu

ORIG_TARGETS = card_bench poker_bench sched_bench
ST_PROOFS    = bench_memory_cache bench_branch_ilp_alu bench_simd_avx bench_highperf_ds bench_advanced_c_isa bench_mylib_upgrade
MT_PROOFS    = bench_multithreading_separate

all: $(ORIG_TARGETS) $(ST_PROOFS) $(MT_PROOFS)

card_bench: card_bench.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $<

poker_bench: poker_bench.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $<

sched_bench: sched_bench.c
	$(CC) $(CFLAGS) $(INCLUDES) -pthread -o $@ $<

bench_memory_cache: bench_memory_cache.c bench_common.h
	$(CC) $(CFLAGS) -o $@ bench_memory_cache.c

bench_branch_ilp_alu: bench_branch_ilp_alu.c bench_common.h
	$(CC) $(CFLAGS) -o $@ bench_branch_ilp_alu.c -lm

bench_simd_avx: bench_simd_avx.c bench_common.h
	$(CC) $(CFLAGS) -o $@ bench_simd_avx.c

bench_highperf_ds: bench_highperf_ds.c bench_common.h
	$(CC) $(CFLAGS) -o $@ bench_highperf_ds.c

bench_advanced_c_isa: bench_advanced_c_isa.c bench_common.h
	$(CC) $(CFLAGS) -o $@ bench_advanced_c_isa.c

bench_mylib_upgrade: bench_mylib_upgrade.c bench_common.h ../mu.c ../mu.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench_mylib_upgrade.c ../mu.c

bench_multithreading_separate: bench_multithreading_separate.c bench_common.h
	$(CC) $(CFLAGS) -pthread -o $@ bench_multithreading_separate.c

card_asm_dump.s: card_asm_dump.c
	$(CC) -O3 -march=native -std=c99 $(INCLUDES) -S -fverbose-asm -masm=intel -o $@ $<

run-proofs: $(ST_PROOFS)
	./bench_memory_cache
	./bench_branch_ilp_alu
	./bench_simd_avx
	./bench_highperf_ds
	./bench_advanced_c_isa
	./bench_mylib_upgrade

# ---------------------------------------------------------------------------
# mu_math.h: correctness gate + cglm comparison bench.
#
# CGLM_INC points at a cglm checkout (header-only). Both targets need it.
#   make -C perf_test math-test                      # gate only
#   make -C perf_test math-bench                     # full paired table + CSV
# ---------------------------------------------------------------------------
CGLM_INC ?= ../../cglm/include

math-test: test_math
	./test_math

test_math: test_math.c ../mu/mu_math.h
	$(CC) $(CFLAGS) $(INCLUDES) -I$(CGLM_INC) -o $@ test_math.c -lm

# The math bench is measured, not proof-of-concept: it wants >=21 paired reps
# on one pinned core, so it is NOT part of run-proofs.
math-bench: bench_math
	./bench_math 0

bench_math: bench_math_vs_cglm.c ../mu/mu_math.h
	$(CC) $(CFLAGS) $(INCLUDES) -I$(CGLM_INC) -o $@ bench_math_vs_cglm.c -lm

run-mt-proofs: $(MT_PROOFS)
	./bench_multithreading_separate

# ---------------------------------------------------------------------------
# New-header gates and benchmarks (mu_bench, mu_log, mu_dlist, mu_skiplist,
# mu_scope_stack, and the cofactor normal transform).
# ---------------------------------------------------------------------------
DS_TESTS = test_skiplist test_scope_stack test_log test_bench test_wfc

test_wfc: test_wfc.c ../mu/mu_wfc.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_wfc.c -lm

wfc-test: test_wfc
	./test_wfc

wfc-bench: bench_wfc.c ../mu/mu_wfc.h
	$(CC) $(CFLAGS) $(INCLUDES) -o bench_wfc bench_wfc.c -lm
	./bench_wfc

ds-test: $(DS_TESTS)
	@for t in $(DS_TESTS); do ./$$t || exit 1; done

test_skiplist: test_skiplist.c ../mu/mu_skiplist.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_skiplist.c

test_scope_stack: test_scope_stack.c ../mu/mu_scope_stack.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_scope_stack.c

test_log: test_log.c ../mu/mu_log.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_log.c

test_bench: test_bench.c ../mu/mu_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_bench.c -lm

# The intrusive-vs-node benchmark wants a large working set and many paired
# reps, so it is not part of run-proofs: it is measured on a pinned core.
bench-intrusive: bench_intrusive.c ../mu/mu_dlist.h
	$(CC) $(CFLAGS) $(INCLUDES) -o bench_intrusive bench_intrusive.c -lm
	taskset -c 0 ./bench_intrusive 1048576

# Cofactor normal transform vs transpose(inverse(M)), via mu_bench.h.
bench-normals: bench_normals.c ../mu/mu_math.h ../mu/mu_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -o bench_normals bench_normals.c -lm
	taskset -c 0 ./bench_normals --samples=21 --confidence=6

# ---------------------------------------------------------------------------
# mu_shapes.h: correctness gate, generator/mesh-surgery benchmark, and the
# par_shapes.h comparison. PAR_SHAPES points at a par_shapes.h checkout (the
# header this one replaces); only the comparison target needs it.
# ---------------------------------------------------------------------------
PAR_SHAPES ?= ../../par_shapes.h

shapes-test: test_shapes
	./test_shapes

test_shapes: test_shapes.c ../mu/mu_shapes.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_shapes.c -lm

# Wants >=21 samples on one pinned core: not part of run-proofs.
shapes-bench: bench_shapes
	taskset -c 0 ./bench_shapes --samples=21 --confidence=6

bench_shapes: bench_shapes.c ../mu/mu_shapes.h ../mu/mu_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench_shapes.c -lm

# -Wno-unused-parameter: par_shapes.h has unused userdata params in its own
# parametric callbacks. It is upstream code; not ours to fix.
shapes-par: bench_shapes_vs_par
	@test -f $(PAR_SHAPES) || { echo "missing $(PAR_SHAPES); set PAR_SHAPES=<path to par_shapes.h>"; exit 1; }
	sh shapes_vs_par.sh

bench_shapes_vs_par: bench_shapes_vs_par.c ../mu/mu_shapes.h ../mu/mu_bench.h $(PAR_SHAPES)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INCLUDES) -I../.. -o $@ bench_shapes_vs_par.c -lm

# ---------------------------------------------------------------------------
# mu_easing.h: correctness gate, per-call/batch benchmark, and the
# par_easings.h comparison. PAR_EASINGS points at a par_easings.h checkout; the
# gate uses it for a parity section when it is there and skips it when it is not.
# ---------------------------------------------------------------------------
PAR_EASINGS ?= ../../par_easings.h
PAR_EASING_FLAGS = $(if $(wildcard $(PAR_EASINGS)),-DMU_EASING_WITH_PAR -I../..,)

easing-test: test_easing
	./test_easing

test_easing: test_easing.c ../mu/mu_easing.h $(wildcard $(PAR_EASINGS))
	$(CC) $(CFLAGS) $(INCLUDES) $(PAR_EASING_FLAGS) -o $@ test_easing.c -lm

# Wants >=21 samples on one pinned core: not part of run-proofs.
easing-bench: bench_easing
	taskset -c 0 ./bench_easing --samples=21 --confidence=6

bench_easing: bench_easing.c ../mu/mu_easing.h ../mu/mu_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench_easing.c -lm

# -Wno-unused-parameter: par_easings.h's own helpers take unused (b, c, d)
# parameters. It is upstream code; not ours to fix.
easing-par: bench_easing_vs_par
	@test -f $(PAR_EASINGS) || { echo "missing $(PAR_EASINGS); set PAR_EASINGS=<path to par_easings.h>"; exit 1; }
	sh easing_vs_par.sh

bench_easing_vs_par: bench_easing_vs_par.c ../mu/mu_easing.h ../mu/mu_bench.h $(PAR_EASINGS)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INCLUDES) -I../.. -o $@ bench_easing_vs_par.c -lm

# ---------------------------------------------------------------------------
# mu_roguelike.h: correctness gate plus the algorithmic A/B benches. The
# baselines (the previous FOV, the previous FIFO distance field, the previous
# duplicate-push A*, the rejection-sampling room generator) live in
# bench_roguelike.c so both sides of every comparison are in one binary.
# ---------------------------------------------------------------------------
rogue-test: test_roguelike
	./test_roguelike

test_roguelike: test_roguelike.c ../mu/mu_roguelike.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_roguelike.c -lm

# Wants >=21 samples on one pinned core: not part of run-proofs.
rogue-bench: bench_roguelike
	taskset -c 0 ./bench_roguelike --samples=21 --confidence=6

bench_roguelike: bench_roguelike.c ../mu/mu_roguelike.h ../mu/mu_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench_roguelike.c -lm

clean:
	rm -f $(ORIG_TARGETS) $(ST_PROOFS) $(MT_PROOFS) card_asm_dump.s \
	      test_math bench_math results_math.csv $(DS_TESTS) bench_intrusive bench_normals bench_wfc \
	      test_shapes bench_shapes bench_shapes_vs_par test_easing bench_easing bench_easing_vs_par \
	      test_roguelike bench_roguelike

.PHONY: all run-proofs run-mt-proofs math-test math-bench ds-test wfc-test wfc-bench bench-intrusive bench-normals \
        shapes-test shapes-bench shapes-par easing-test easing-bench easing-par clean

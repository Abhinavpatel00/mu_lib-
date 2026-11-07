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

run-mt-proofs: $(MT_PROOFS)
	./bench_multithreading_separate

clean:
	rm -f $(ORIG_TARGETS) $(ST_PROOFS) $(MT_PROOFS) card_asm_dump.s

.PHONY: all run-proofs run-mt-proofs clean

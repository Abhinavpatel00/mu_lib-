CC      = gcc
CFLAGS  = -O2 -march=native -std=c11 -pthread -Wall -Wextra
MATH    = -lm

BINS = 01_task_pool 02_atomic_alloc 03_double_buffer \
       04_thread_local_buf 05_false_sharing \
       06_ownership_transfer 07_particle_parallel 08_memory_model

.PHONY: all bench clean tsan

all: $(BINS)

01_task_pool:         01_task_pool.c         ; $(CC) $(CFLAGS) $< -o $@
02_atomic_alloc:      02_atomic_alloc.c      ; $(CC) $(CFLAGS) $< -o $@
03_double_buffer:     03_double_buffer.c     ; $(CC) $(CFLAGS) $< -o $@
04_thread_local_buf:  04_thread_local_buf.c  ; $(CC) $(CFLAGS) $< -o $@
05_false_sharing:     05_false_sharing.c     ; $(CC) $(CFLAGS) $< -o $@
06_ownership_transfer:06_ownership_transfer.c; $(CC) $(CFLAGS) $< -o $@
07_particle_parallel: 07_particle_parallel.c ; $(CC) $(CFLAGS) $< $(MATH) -o $@
08_memory_model:      08_memory_model.c      ; $(CC) $(CFLAGS) $< -o $@

# Run all benchmarks in order
bench: all
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 1+2: Tasks not threads — thread pool with real-core count"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./01_task_pool
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 4: Atomic bump allocator vs mutex bump"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./02_atomic_alloc
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 5: Double-buffered event bus"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./03_double_buffer
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 6: Thread-local write buffers (zero contention)"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./04_thread_local_buf
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 7: False sharing vs cache-line aligned counters"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./05_false_sharing
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 3: Single-owner data — acquire/release pipeline"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./06_ownership_transfer
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  TIP 8+9: Particle split + atomic latch dependency"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./07_particle_parallel
	@echo ""
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  Herb Sutter: volatile vs _Atomic vs memory_order"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	./08_memory_model

# Build with ThreadSanitizer to catch real data races
tsan:
	$(CC) -g -fsanitize=thread -std=c11 -pthread 08_memory_model.c -o 08_tsan
	./08_tsan

clean:
	rm -f $(BINS) 08_tsan

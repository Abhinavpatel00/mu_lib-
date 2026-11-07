	.file	"test.c"
# GNU C23 (GCC) version 15.2.1 20260103 (x86_64-pc-linux-gnu)
#	compiled by GNU C version 15.2.1 20260103, GMP version 6.3.0, MPFR version 4.2.2, MPC version 1.3.1, isl version isl-0.27-GMP

# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed: -march=tigerlake -mmmx -mpopcnt -msse -msse2 -msse3 -mssse3 -msse4.1 -msse4.2 -mavx -mavx2 -mno-sse4a -mno-fma4 -mno-xop -mfma -mavx512f -mbmi -mbmi2 -maes -mpclmul -mavx512vl -mavx512bw -mavx512dq -mavx512cd -mavx512vbmi -mavx512ifma -mavx512vpopcntdq -mavx512vbmi2 -mgfni -mvpclmulqdq -mavx512vnni -mavx512bitalg -mno-avx512bf16 -mavx512vp2intersect -mno-3dnow -madx -mabm -mno-cldemote -mclflushopt -mclwb -mno-clzero -mcx16 -mno-enqcmd -mf16c -mfsgsbase -mfxsr -mno-hle -msahf -mno-lwp -mlzcnt -mmovbe -mmovdir64b -mmovdiri -mno-mwaitx -mno-pconfig -mpku -mprfchw -mno-ptwrite -mrdpid -mrdrnd -mrdseed -mno-rtm -mno-serialize -mno-sgx -msha -mshstk -mno-tbm -mno-tsxldtrk -mvaes -mno-waitpkg -mno-wbnoinvd -mxsave -mxsavec -mxsaveopt -mxsaves -mno-amx-tile -mno-amx-int8 -mno-amx-bf16 -mno-uintr -mno-hreset -mno-kl -mno-widekl -mno-avxvnni -mno-avx512fp16 -mno-avxifma -mno-avxvnniint8 -mno-avxneconvert -mno-cmpccxadd -mno-amx-fp16 -mno-prefetchi -mno-raoint -mno-amx-complex -mno-avxvnniint16 -mno-sm3 -mno-sha512 -mno-sm4 -mno-apxf -mno-usermsr -mno-avx10.2 -mno-amx-avx512 -mno-amx-tf32 -mno-amx-transpose -mno-amx-fp8 -mno-movrs -mno-amx-movrs --param=l1-cache-size=48 --param=l1-cache-line-size=64 --param=l2-cache-size=8192 -mtune=tigerlake -O3
	.text
	.p2align 4
	.type	slow_identity.constprop.0, @function
slow_identity.constprop.0:
.LFB7487:
	.cfi_startproc
# test.c:47: }
	movl	$42, %eax	#,
	ret	
	.cfi_endproc
.LFE7487:
	.size	slow_identity.constprop.0, .-slow_identity.constprop.0
	.section	.rodata.str1.1,"aMS",@progbits,1
.LC0:
	.string	"=== MU macro demo ===\n"
.LC1:
	.string	"Array count: %zu\n"
.LC2:
	.string	"Sum (abs style): %d\n"
.LC3:
	.string	"Noinline test: %d\n"
.LC4:
	.string	"Min(10,20): %d\n"
.LC5:
	.string	"Max(10,20): %d\n"
.LC6:
	.string	"Swap result: a=%d b=%d\n"
.LC7:
	.string	"Is 64 pow2? %d\n"
.LC8:
	.string	"Is 70 pow2? %d\n"
.LC9:
	.string	"1 KB = %llu\n"
.LC10:
	.string	"1 MB = %llu\n"
.LC11:
	.string	"1 GB = %llu\n"
.LC12:
	.string	"Hello Mu"
.LC13:
	.string	"Stringify test: %s\n"
.LC14:
	.string	"Concat variable value: %d\n"
.LC15:
	.string	"Offset of example: %zu\n"
	.section	.rodata.str1.8,"aMS",@progbits,1
	.align 8
.LC16:
	.string	"Recovered header via container_of: %d\n"
	.align 8
.LC17:
	.string	"Aligned buffer address mod 64: %llu\n"
	.section	.rodata.str1.1
.LC18:
	.string	"Trailing zeroes: %d\n"
.LC19:
	.string	"Leading zeroes: %d\n"
.LC20:
	.string	"Popcount: %d\n"
.LC21:
	.string	"\nAll tests completed."
	.section	.text.unlikely,"ax",@progbits
.LCOLDB22:
	.section	.text.startup,"ax",@progbits
.LHOTB22:
	.p2align 4
	.globl	main
	.type	main, @function
main:
.LFB7486:
	.cfi_startproc
	pushq	%r12	#
	.cfi_def_cfa_offset 16
	.cfi_offset 12, -16
	pushq	%rbp	#
	.cfi_def_cfa_offset 24
	.cfi_offset 6, -24
	pushq	%rbx	#
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
	subq	$32, %rsp	#,
	.cfi_def_cfa_offset 64
# test.c:63:     printf("=== MU macro demo ===\n\n");
	movq	%fs:40, %rdi	# MEM[(<address-space-1> long unsigned int *)40B],
	movq	%rdi, 24(%rsp)	#, D.44760
	leaq	.LC0(%rip), %rdi	#,
# test.c:80:     MU_PREFETCH(&arr[0]);
	prefetcht0	(%rsp)	#
# test.c:63:     printf("=== MU macro demo ===\n\n");
	call	puts@PLT	#
# test.c:70:     printf("Array count: %zu\n", MU_ARRAY_COUNT(arr));
	movl	$5, %esi	#,
	leaq	.LC1(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:74:     printf("Sum (abs style): %d\n", total);
	movl	$15, %esi	#,
	leaq	.LC2(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:77:     printf("Noinline test: %d\n", slow_identity(42));
	movl	$42, %esi	#,
	leaq	.LC3(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:83:     printf("Min(10,20): %d\n", MU_MIN(10, 20));
	movl	$10, %esi	#,
	leaq	.LC4(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:84:     printf("Max(10,20): %d\n", MU_MAX(10, 20));
	movl	$20, %esi	#,
	leaq	.LC5(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:89:     printf("Swap result: a=%d b=%d\n", a, b);
	movl	$5, %edx	#,
	movl	$9, %esi	#,
	leaq	.LC6(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:92:     printf("Is 64 pow2? %d\n", MU_IS_POW2(64));
	movl	$1, %esi	#,
	leaq	.LC7(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:93:     printf("Is 70 pow2? %d\n", MU_IS_POW2(70));
	xorl	%esi, %esi	#
	leaq	.LC8(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:96:     printf("1 KB = %llu\n", MU_KB(1));
	movl	$1024, %esi	#,
	leaq	.LC9(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:97:     printf("1 MB = %llu\n", MU_MB(1));
	movl	$1048576, %esi	#,
	leaq	.LC10(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:98:     printf("1 GB = %llu\n", MU_GB(1));
	movl	$1073741824, %esi	#,
	leaq	.LC11(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:101:     printf("Stringify test: %s\n", MU_TOSTRING(Hello Mu));
	leaq	.LC12(%rip), %rsi	#,
	leaq	.LC13(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:105:     printf("Concat variable value: %d\n", test_123);
	movl	$777, %esi	#,
	leaq	.LC14(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:113:     printf("Offset of example: %zu\n", MU_OFFSET_OF(Wrapper, example));
	movl	$8, %esi	#,
	leaq	.LC15(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:118:     printf("Recovered header via container_of: %d\n", recovered->header);
	movl	$11, %esi	#,
	leaq	.LC16(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:121:     printf("Aligned buffer address mod 64: %llu\n", (unsigned long long)((uintptr_t)aligned_buffer % 64));
	xorl	%esi, %esi	#
	leaq	.LC17(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# /usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1/include/ia32intrin.h:114:   return __builtin_ia32_rdtsc ();
	rdtsc	
# test.c:125:     printf("Trailing zeroes: %d\n", mu_trailing_zeroes_u64(mask));
	leaq	.LC18(%rip), %rdi	#,
# /usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1/include/ia32intrin.h:114:   return __builtin_ia32_rdtsc ();
	salq	$32, %rdx	#, tmp135
	movq	%rax, %rbx	# tmp134, tmp134
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	orq	%rdx, %rbx	# tmp135, tmp134
	sete	%bpl	#, _37
	sete	%r12b	#, _38
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	movzbl	%bpl, %ebp	# _37, _1
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	movzbl	%r12b, %r12d	# _38, _38
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	orq	%rbx, %r12	# tmp134, _39
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	movl	%ebp, %esi	# _1, _50
# mu.h:264:     return (uint32_t)__builtin_popcountll(x);
	popcntq	%rbx, %rbx	# tmp134, tmp145
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	tzcntq	%r12, %rax	# _39, tmp138
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	sall	$6, %esi	#, _50
# mu.h:245:     return __builtin_clzll(x | (x == 0)) + (uint32_t)(x == 0);
	lzcntq	%r12, %r12	# _39, tmp142
# mu.h:224:     return __builtin_ctzll(x | (x == 0)) + ((uint32_t)(x == 0) * 64u);
	addl	%eax, %esi	# tmp138, _43
# test.c:125:     printf("Trailing zeroes: %d\n", mu_trailing_zeroes_u64(mask));
	xorl	%eax, %eax	#
	call	printf@PLT	#
# mu.h:245:     return __builtin_clzll(x | (x == 0)) + (uint32_t)(x == 0);
	leal	0(%rbp,%r12), %esi	#, _36
# test.c:127:     printf("Leading zeroes: %d\n", mu_leading_zeroes_u64(mask));
	leaq	.LC19(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# test.c:129:     printf("Popcount: %d\n", mu_popcount_u64(mask));
	xorl	%eax, %eax	#
# mu.h:264:     return (uint32_t)__builtin_popcountll(x);
	movl	%ebx, %esi	# tmp145, _27
# test.c:129:     printf("Popcount: %d\n", mu_popcount_u64(mask));
	leaq	.LC20(%rip), %rdi	#,
	call	printf@PLT	#
# test.c:132:     MU_ASSERT(mu_popcount_u64(mask) == 3);
	cmpl	$3, %ebx	#, tmp145
	jne	.L6	#,
# test.c:134:     printf("\nAll tests completed.\n");
	leaq	.LC21(%rip), %rdi	#,
	call	puts@PLT	#
# test.c:137: }
	movq	24(%rsp), %rax	# D.44760, tmp150
	subq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp150
	jne	.L8	#,
	addq	$32, %rsp	#,
	.cfi_remember_state
	.cfi_def_cfa_offset 32
	xorl	%eax, %eax	#
	popq	%rbx	#
	.cfi_def_cfa_offset 24
	popq	%rbp	#
	.cfi_def_cfa_offset 16
	popq	%r12	#
	.cfi_def_cfa_offset 8
	ret	
.L8:
	.cfi_restore_state
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.type	main.cold, @function
main.cold:
.LFSB7486:
.L6:
	.cfi_def_cfa_offset 64
	.cfi_offset 3, -32
	.cfi_offset 6, -24
	.cfi_offset 12, -16
# test.c:132:     MU_ASSERT(mu_popcount_u64(mask) == 3);
	ud2	
	.cfi_endproc
.LFE7486:
	.section	.text.startup
	.size	main, .-main
	.section	.text.unlikely
	.size	main.cold, .-main.cold
.LCOLDE22:
	.section	.text.startup
.LHOTE22:
	.ident	"GCC: (GNU) 15.2.1 20260103"
	.section	.note.GNU-stack,"",@progbits

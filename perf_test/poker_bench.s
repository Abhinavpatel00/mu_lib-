	.file	"poker_bench.c"
# GNU C11 (GCC) version 16.2.1 20260810 (x86_64-pc-linux-gnu)
#	compiled by GNU C version 16.2.1 20260810, GMP version 6.3.0, MPFR version 4.2.2, MPC version 1.4.1, isl version isl-0.28-GMP

# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed: -march=tigerlake -mmmx -mpopcnt -msse -msse2 -msse3 -mssse3 -msse4.1 -msse4.2 -mavx -mavx2 -mno-sse4a -mno-fma4 -mno-xop -mfma -mavx512f -mbmi -mbmi2 -maes -mpclmul -mavx512vl -mavx512bw -mavx512dq -mavx512cd -mavx512vbmi -mavx512ifma -mavx512vpopcntdq -mavx512vbmi2 -mgfni -mvpclmulqdq -mavx512vnni -mavx512bitalg -mno-avx512bf16 -mavx512vp2intersect -mno-3dnow -madx -mabm -mno-cldemote -mclflushopt -mclwb -mno-clzero -mcx16 -mno-enqcmd -mf16c -mfsgsbase -mfxsr -mno-hle -msahf -mno-lwp -mlzcnt -mmovbe -mmovdir64b -mmovdiri -mno-mwaitx -mno-pconfig -mpku -mprfchw -mno-ptwrite -mrdpid -mrdrnd -mrdseed -mno-rtm -mno-serialize -mno-sgx -msha -mshstk -mno-tbm -mno-tsxldtrk -mvaes -mno-waitpkg -mno-wbnoinvd -mxsave -mxsavec -mxsaveopt -mxsaves -mno-amx-tile -mno-amx-int8 -mno-amx-bf16 -mno-uintr -mno-hreset -mno-kl -mno-widekl -mno-avxvnni -mno-avx512fp16 -mno-avxifma -mno-avxvnniint8 -mno-avxneconvert -mno-cmpccxadd -mno-amx-fp16 -mno-prefetchi -mno-raoint -mno-amx-complex -mno-avxvnniint16 -mno-sm3 -mno-sha512 -mno-sm4 -mno-apxf -mno-usermsr -mno-avx10.1 -mno-avx10.2 -mno-amx-avx512 -mno-amx-tf32 -mno-amx-fp8 -mno-movrs -mno-amx-movrs -mno-avx512bmm --param=l1-cache-size=48 --param=l1-cache-line-size=64 --param=l2-cache-size=8192 -mtune=tigerlake -O2 -std=c11
	.text
	.p2align 4
	.type	build_52, @function
build_52:
.LFB34:
	.cfi_startproc
	pushq	%r12	#
	.cfi_def_cfa_offset 16
	.cfi_offset 12, -16
	leaq	RANK_STR(%rip), %r8	#, ivtmp.128
# poker_bench.c:323:             deck[idx] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)(idx+1 < 52 ? idx+1 : 0)),
	xorl	%r11d, %r11d	# ivtmp.118
	leaq	SUIT_CHAR(%rip), %r12	#, tmp118
	xorl	%r10d, %r10d	# tmp120
# poker_bench.c:319: static void build_52(Card deck[DECK_SZ]) {
	pushq	%rbp	#
	.cfi_def_cfa_offset 24
	.cfi_offset 6, -24
# poker_bench.c:323:             deck[idx] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)(idx+1 < 52 ? idx+1 : 0)),
	xorl	%ebp, %ebp	# ivtmp.127
# poker_bench.c:319: static void build_52(Card deck[DECK_SZ]) {
	pushq	%rbx	#
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
.L2:
# poker_bench.c:314:     c.title[1]= SUIT_CHAR[suit];
	movzbl	(%r12,%r11), %ebx	# MEM[(char *)&SUIT_CHAR + ivtmp.118_49 * 1], _27
	movl	%r11d, %r9d	# ivtmp.118, _56
	movq	%rbp, %rax	# ivtmp.127, ivtmp.94
	xorl	%edx, %edx	# ivtmp.110
	movl	$50, %esi	#, pretmp_46
	jmp	.L6	#
	.p2align 6
	.p2align 4,,10
	.p2align 3
.L4:
# poker_bench.c:322:         for (int r = 0; r < 13; ++r)
	incq	%rax	# ivtmp.94
# poker_bench.c:313:     c.title[0]= RANK_STR[rank][0];
	movq	(%r8,%rax,8), %rcx	# MEM[(const char * *)_28 + ivtmp.94_32 * 8], MEM[(const char * *)_28 + ivtmp.94_32 * 8]
	movzbl	(%rcx), %esi	# *pretmp_23, pretmp_46
.L6:
	cmpl	$51, %eax	#, ivtmp.94
	movzbl	%sil, %ecx	# pretmp_46, pretmp_46
	leal	1(%rax), %esi	#, tmp119
# poker_bench.c:323:             deck[idx] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)(idx+1 < 52 ? idx+1 : 0)),
	movb	$0, 7(%rdi,%rax,8)	#, MEM[(char *)deck_17(D) + 7B + ivtmp.94_31 * 8]
	cmovge	%r10d, %esi	# tmp119,, tmp120, _1
	movb	%bl, %ch	# _27, pretmp_46
	movw	%cx, 5(%rdi,%rax,8)	# pretmp_46, MEM <vector(2) char> [(char *)deck_17(D) + 5B + ivtmp.94_31 * 8]
	movl	%esi, (%rdi,%rax,8)	# _1, MEM[(unsigned int *)deck_17(D) + ivtmp.94_31 * 8]
# poker_bench.c:61:     return (uint8_t)((rank << RANK_SHIFT) | suit);
	movl	%edx, %esi	# ivtmp.110, tmp116
# poker_bench.c:322:         for (int r = 0; r < 13; ++r)
	addl	$4, %edx	#, ivtmp.110
# poker_bench.c:61:     return (uint8_t)((rank << RANK_SHIFT) | suit);
	orl	%r9d, %esi	# _56, tmp116
	movb	%sil, 4(%rdi,%rax,8)	# tmp116, MEM[(unsigned char *)deck_17(D) + 4B + ivtmp.94_31 * 8]
# poker_bench.c:322:         for (int r = 0; r < 13; ++r)
	cmpb	$52, %dl	#, ivtmp.110
	jne	.L4	#,
# poker_bench.c:321:     for (int s = 0; s < 4; ++s)
	addq	$13, %rbp	#, ivtmp.127
	incq	%r11	# ivtmp.118
	subq	$104, %r8	#, ivtmp.128
	cmpq	$52, %rbp	#, ivtmp.127
	jne	.L2	#,
# poker_bench.c:325: }
	popq	%rbx	#
	.cfi_def_cfa_offset 24
	popq	%rbp	#
	.cfi_def_cfa_offset 16
	popq	%r12	#
	.cfi_def_cfa_offset 8
	ret	
	.cfi_endproc
.LFE34:
	.size	build_52, .-build_52
	.p2align 4
	.globl	bench_rank_bitshift
	.type	bench_rank_bitshift, @function
bench_rank_bitshift:
.LFB18:
	.cfi_startproc
# poker_bench.c:90:     for (int i = 0; i < n; ++i) sum += info_rank(deck[i].info);
	testl	%esi, %esi	# n
	jle	.L14	#,
	movl	%esi, %esi	# n, _21
# poker_bench.c:89:     uint32_t sum = 0;
	xorl	%edx, %edx	# <retval>
	leaq	(%rdi,%rsi,8), %rcx	#, _15
	.p2align 5
	.p2align 4,,10
	.p2align 3
.L13:
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	4(%rdi), %eax	# MEM[(unsigned char *)_22 + 4B], _11
# poker_bench.c:90:     for (int i = 0; i < n; ++i) sum += info_rank(deck[i].info);
	addq	$8, %rdi	#, ivtmp.137
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _11
# poker_bench.c:90:     for (int i = 0; i < n; ++i) sum += info_rank(deck[i].info);
	andl	$15, %eax	#, _5
# poker_bench.c:90:     for (int i = 0; i < n; ++i) sum += info_rank(deck[i].info);
	addl	%eax, %edx	# _5, <retval>
# poker_bench.c:90:     for (int i = 0; i < n; ++i) sum += info_rank(deck[i].info);
	cmpq	%rdi, %rcx	# ivtmp.137, _15
	jne	.L13	#,
# poker_bench.c:92: }
	movl	%edx, %eax	# <retval>,
	ret	
	.p2align 4,,10
	.p2align 3
.L14:
# poker_bench.c:89:     uint32_t sum = 0;
	xorl	%edx, %edx	# <retval>
# poker_bench.c:92: }
	movl	%edx, %eax	# <retval>,
	ret	
	.cfi_endproc
.LFE18:
	.size	bench_rank_bitshift, .-bench_rank_bitshift
	.p2align 4
	.globl	bench_rank_lut
	.type	bench_rank_lut, @function
bench_rank_lut:
.LFB19:
	.cfi_startproc
# poker_bench.c:97:     for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
	testl	%esi, %esi	# n
	jle	.L19	#,
	movl	%esi, %esi	# n, _21
# poker_bench.c:96:     uint32_t sum = 0;
	xorl	%eax, %eax	# <retval>
	leaq	rank_lut(%rip), %rcx	#, tmp119
	leaq	(%rdi,%rsi,8), %rsi	#, _15
	.p2align 5
	.p2align 4,,10
	.p2align 3
.L18:
# poker_bench.c:97:     for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
	movzbl	4(%rdi), %edx	# MEM[(unsigned char *)_22 + 4B], _5
# poker_bench.c:97:     for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
	addq	$8, %rdi	#, ivtmp.147
# poker_bench.c:97:     for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
	movzbl	(%rcx,%rdx), %edx	# rank_lut[_5], _7
# poker_bench.c:97:     for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
	addl	%edx, %eax	# _7, <retval>
# poker_bench.c:97:     for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
	cmpq	%rdi, %rsi	# ivtmp.147, _15
	jne	.L18	#,
	ret	
	.p2align 4,,10
	.p2align 3
.L19:
# poker_bench.c:96:     uint32_t sum = 0;
	xorl	%eax, %eax	# <retval>
# poker_bench.c:99: }
	ret	
	.cfi_endproc
.LFE19:
	.size	bench_rank_lut, .-bench_rank_lut
	.p2align 4
	.globl	bench_suit_mask
	.type	bench_suit_mask, @function
bench_suit_mask:
.LFB20:
	.cfi_startproc
# poker_bench.c:107:     for (int i = 0; i < n; ++i) sum += info_suit(deck[i].info);
	testl	%esi, %esi	# n
	jle	.L24	#,
	movl	%esi, %esi	# n, _15
# poker_bench.c:106:     uint32_t sum = 0;
	xorl	%edx, %edx	# <retval>
	leaq	(%rdi,%rsi,8), %rcx	#, _6
	.p2align 5
	.p2align 4,,10
	.p2align 3
.L23:
# poker_bench.c:63: static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
	movzbl	4(%rdi), %eax	# MEM[(unsigned char *)_19 + 4B], MEM[(unsigned char *)_19 + 4B]
# poker_bench.c:107:     for (int i = 0; i < n; ++i) sum += info_suit(deck[i].info);
	addq	$8, %rdi	#, ivtmp.157
# poker_bench.c:107:     for (int i = 0; i < n; ++i) sum += info_suit(deck[i].info);
	andl	$3, %eax	#, _5
# poker_bench.c:107:     for (int i = 0; i < n; ++i) sum += info_suit(deck[i].info);
	addl	%eax, %edx	# _5, <retval>
# poker_bench.c:107:     for (int i = 0; i < n; ++i) sum += info_suit(deck[i].info);
	cmpq	%rdi, %rcx	# ivtmp.157, _6
	jne	.L23	#,
# poker_bench.c:109: }
	movl	%edx, %eax	# <retval>,
	ret	
	.p2align 4,,10
	.p2align 3
.L24:
# poker_bench.c:106:     uint32_t sum = 0;
	xorl	%edx, %edx	# <retval>
# poker_bench.c:109: }
	movl	%edx, %eax	# <retval>,
	ret	
	.cfi_endproc
.LFE20:
	.size	bench_suit_mask, .-bench_suit_mask
	.p2align 4
	.globl	flush_field
	.type	flush_field, @function
flush_field:
.LFB21:
	.cfi_startproc
# poker_bench.c:63: static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
	movzbl	4(%rdi), %eax	# h_8(D)->info, _12
	movzbl	12(%rdi), %edx	# MEM[(const struct Card *)h_8(D) + 8B].info, _13
	andl	$3, %eax	#, _12
	andl	$3, %edx	#, _13
# poker_bench.c:123:         && info_suit(h[4].info) == s;
	cmpb	%dl, %al	# _13, _12
	je	.L27	#,
.L29:
# poker_bench.c:118: bool flush_field(const Hand h) {
	xorl	%eax, %eax	# <retval>
	ret	
	.p2align 4,,10
	.p2align 3
.L27:
# poker_bench.c:63: static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
	movzbl	20(%rdi), %edx	# MEM[(const struct Card *)h_8(D) + 16B].info, _11
	andl	$3, %edx	#, _11
# poker_bench.c:121:         && info_suit(h[2].info) == s
	cmpb	%al, %dl	# _12, _11
	jne	.L29	#,
# poker_bench.c:63: static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
	movzbl	28(%rdi), %eax	# MEM[(const struct Card *)h_8(D) + 24B].info, _10
	andl	$3, %eax	#, _10
# poker_bench.c:122:         && info_suit(h[3].info) == s
	cmpb	%dl, %al	# _11, _10
	jne	.L29	#,
# poker_bench.c:63: static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
	movzbl	36(%rdi), %edx	# MEM[(const struct Card *)h_8(D) + 32B].info, MEM[(const struct Card *)h_8(D) + 32B].info
	andl	$3, %edx	#, _9
# poker_bench.c:123:         && info_suit(h[4].info) == s;
	cmpb	%al, %dl	# _10, _9
	sete	%al	#, <retval>
# poker_bench.c:124: }
	ret	
	.cfi_endproc
.LFE21:
	.size	flush_field, .-flush_field
	.p2align 4
	.globl	flush_word
	.type	flush_word, @function
flush_word:
.LFB22:
	.cfi_startproc
# poker_bench.c:140:     memcpy(&w0,&h[0],8); memcpy(&w1,&h[1],8);
	movq	(%rdi), %rax	# MEM <unsigned long> [(char * {ref-all})h_15(D)], _9
# poker_bench.c:140:     memcpy(&w0,&h[0],8); memcpy(&w1,&h[1],8);
	movq	8(%rdi), %r9	# MEM <unsigned long> [(char * {ref-all})h_15(D) + 8B], _10
# poker_bench.c:141:     memcpy(&w2,&h[2],8); memcpy(&w3,&h[3],8);
	movq	16(%rdi), %r8	# MEM <unsigned long> [(char * {ref-all})h_15(D) + 16B], _11
# poker_bench.c:141:     memcpy(&w2,&h[2],8); memcpy(&w3,&h[3],8);
	movq	24(%rdi), %rsi	# MEM <unsigned long> [(char * {ref-all})h_15(D) + 24B], _12
# poker_bench.c:143:     uint64_t suits = (w0 | w1 | w2 | w3 | w4) & SUIT_IN_WORD;
	movq	%rax, %rdx	# _9, _1
# poker_bench.c:144:     uint64_t must  = (w0 & w1 & w2 & w3 & w4) & SUIT_IN_WORD;
	andq	%r9, %rax	# _10, _5
# poker_bench.c:142:     memcpy(&w4,&h[4],8);
	movq	32(%rdi), %rcx	# MEM <unsigned long> [(char * {ref-all})h_15(D) + 32B], _13
# poker_bench.c:143:     uint64_t suits = (w0 | w1 | w2 | w3 | w4) & SUIT_IN_WORD;
	orq	%r9, %rdx	# _10, _1
# poker_bench.c:144:     uint64_t must  = (w0 & w1 & w2 & w3 & w4) & SUIT_IN_WORD;
	andq	%r8, %rax	# _11, _6
# poker_bench.c:143:     uint64_t suits = (w0 | w1 | w2 | w3 | w4) & SUIT_IN_WORD;
	orq	%r8, %rdx	# _11, _2
# poker_bench.c:144:     uint64_t must  = (w0 & w1 & w2 & w3 & w4) & SUIT_IN_WORD;
	andq	%rsi, %rax	# _12, _7
# poker_bench.c:143:     uint64_t suits = (w0 | w1 | w2 | w3 | w4) & SUIT_IN_WORD;
	orq	%rsi, %rdx	# _12, _3
# poker_bench.c:144:     uint64_t must  = (w0 & w1 & w2 & w3 & w4) & SUIT_IN_WORD;
	andq	%rcx, %rax	# _13, _8
# poker_bench.c:143:     uint64_t suits = (w0 | w1 | w2 | w3 | w4) & SUIT_IN_WORD;
	orq	%rcx, %rdx	# _13, _4
# poker_bench.c:145:     return suits == must;   /* all bits identical ↔ all same suit */
	xorq	%rdx, %rax	# _4, _17
	movabsq	$12884901888, %rdx	#, tmp126
	testq	%rdx, %rax	# tmp126, _17
	sete	%al	#, _18
# poker_bench.c:146: }
	ret	
	.cfi_endproc
.LFE22:
	.size	flush_word, .-flush_word
	.p2align 4
	.globl	straight_sort
	.type	straight_sort, @function
straight_sort:
.LFB24:
	.cfi_startproc
	subq	$24, %rsp	#,
	.cfi_def_cfa_offset 32
	movq	%fs:40, %rdx	# MEM[(<address-space-1> long unsigned int *)40B], ivtmp.197
	movq	%rdx, 8(%rsp)	# ivtmp.197, D.6363
	leaq	4(%rdi), %rdx	#, ivtmp.197
# poker_bench.c:163: bool straight_sort(const Hand h) {
	leaq	3(%rsp), %rcx	#, ivtmp.199
	addq	$44, %rdi	#, _125
.L32:
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	(%rdx), %eax	# MEM[(unsigned char *)_128], _37
# poker_bench.c:165:     for (int i = 0; i < 5; ++i) r[i] = info_rank(h[i].info);
	addq	$8, %rdx	#, ivtmp.197
	incq	%rcx	# ivtmp.199
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _37
	andl	$15, %eax	#, tmp145
	movb	%al, -1(%rcx)	# tmp145, MEM[(unsigned char *)_127]
# poker_bench.c:165:     for (int i = 0; i < 5; ++i) r[i] = info_rank(h[i].info);
	cmpq	%rdx, %rdi	# ivtmp.197, _125
	jne	.L32	#,
	leaq	4(%rsp), %r8	#, ivtmp.189
	xorl	%edi, %edi	# ivtmp.183
.L36:
# poker_bench.c:156:         uint8_t key = r[i]; int j = i-1;
	movzbl	(%r8), %esi	# MEM[(uint8_t *)_137], key
# poker_bench.c:156:         uint8_t key = r[i]; int j = i-1;
	movl	%edi, %edx	# ivtmp.183, j
	movq	%r8, %rax	# ivtmp.189, ivtmp.178
	.p2align 5
	.p2align 4,,10
	.p2align 3
.L33:
# poker_bench.c:157:         while (j >= 0 && r[j] > key) { r[j+1] = r[j]; --j; }
	movzbl	-1(%rax), %ecx	# MEM[(uint8_t *)_145 + -1B], _49
# poker_bench.c:157:         while (j >= 0 && r[j] > key) { r[j+1] = r[j]; --j; }
	cmpb	%cl, %sil	# _49, key
	jnb	.L46	#,
# poker_bench.c:157:         while (j >= 0 && r[j] > key) { r[j+1] = r[j]; --j; }
	decl	%edx	# j
# poker_bench.c:157:         while (j >= 0 && r[j] > key) { r[j+1] = r[j]; --j; }
	movb	%cl, (%rax)	# _49, MEM[(uint8_t *)_145]
# poker_bench.c:157:         while (j >= 0 && r[j] > key) { r[j+1] = r[j]; --j; }
	decq	%rax	# ivtmp.178
	cmpl	$-1, %edx	#, j
	jne	.L33	#,
	leaq	3(%rsp), %rax	#, _85
.L34:
# poker_bench.c:155:     for (int i = 1; i < 5; ++i) {
	incq	%rdi	# ivtmp.183
# poker_bench.c:158:         r[j+1] = key;
	movb	%sil, (%rax)	# key, *prephitmp_86
# poker_bench.c:155:     for (int i = 1; i < 5; ++i) {
	incq	%r8	# ivtmp.189
	cmpq	$4, %rdi	#, ivtmp.183
	jne	.L36	#,
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	movzbl	7(%rsp), %edx	# r[4],
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	movzbl	3(%rsp), %r8d	# r[0],
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	movzbl	4(%rsp), %ecx	# r[1],
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	movzbl	5(%rsp), %edi	# r[2],
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	movzbl	6(%rsp), %esi	# r[3],
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	cmpb	$12, %dl	#, _5
	je	.L47	#,
.L37:
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	movl	%ecx, %eax	# _11, _14
	subl	%r8d, %eax	# _13, _14
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	movl	%edi, %r8d	# _17, _18
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	cmpl	$1, %eax	#, _14
	sete	%al	#, _15
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	subl	%ecx, %r8d	# _11, _18
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	cmpl	$1, %r8d	#, _18
	sete	%cl	#, _19
	andl	%ecx, %eax	# _19, _20
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	movl	%esi, %ecx	# _22, _23
	subl	%edi, %ecx	# _17, _23
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	cmpl	$1, %ecx	#, _23
	sete	%cl	#, _24
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	subl	%esi, %edx	# _22, _26
	andl	%ecx, %eax	# _24, _34
# poker_bench.c:170:     return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
	cmpl	$1, %edx	#, _26
	sete	%dl	#, _27
	andl	%edx, %eax	# _27, <retval>
.L31:
# poker_bench.c:171: }
	movq	8(%rsp), %rdx	# D.6363, tmp171
	subq	%fs:40, %rdx	# MEM[(<address-space-1> long unsigned int *)40B], tmp171
	jne	.L48	#,
	addq	$24, %rsp	#,
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret	
	.p2align 4,,10
	.p2align 3
.L46:
	.cfi_restore_state
# poker_bench.c:158:         r[j+1] = key;
	leal	1(%rdx), %eax	#, _83
# poker_bench.c:158:         r[j+1] = key;
	leaq	3(%rsp,%rax), %rax	#, _85
	jmp	.L34	#
.L47:
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	testb	%r8b, %r8b	# pretmp_165
	jne	.L37	#,
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	cmpb	$1, %cl	#, pretmp_166
	jne	.L37	#,
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	cmpb	$2, %dil	#, pretmp_167
	jne	.L37	#,
# poker_bench.c:169:         return true;
	movl	$1, %eax	#, <retval>
# poker_bench.c:168:     if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
	cmpb	$3, %sil	#, pretmp_168
	jne	.L37	#,
	jmp	.L31	#
.L48:
# poker_bench.c:171: }
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
.LFE24:
	.size	straight_sort, .-straight_sort
	.p2align 4
	.globl	straight_bitmask
	.type	straight_bitmask, @function
straight_bitmask:
.LFB25:
	.cfi_startproc
	leaq	4(%rdi), %rcx	#, ivtmp.228
# poker_bench.c:176:     uint16_t bits = 0;
	xorl	%edx, %edx	# bits
	addq	$44, %rdi	#, _17
# poker_bench.c:177:     for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << info_rank(h[i].info));
	movl	$1, %esi	#, tmp131
.L50:
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	(%rcx), %eax	# MEM[(unsigned char *)_10], _20
# poker_bench.c:177:     for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << info_rank(h[i].info));
	addq	$8, %rcx	#, ivtmp.228
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _20
	andl	$15, %eax	#, _24
# poker_bench.c:177:     for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << info_rank(h[i].info));
	shlx	%eax, %esi, %eax	# _24, tmp131, _6
# poker_bench.c:177:     for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << info_rank(h[i].info));
	orl	%eax, %edx	# _6, bits
# poker_bench.c:177:     for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << info_rank(h[i].info));
	cmpq	%rcx, %rdi	# ivtmp.228, _17
	jne	.L50	#,
# poker_bench.c:179:     if (bits & (1u << RANK_ACE)) bits |= 1u; /* duplicate ace as rank -1 */
	movl	%edx, %eax	# bits, tmp155
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	vpcmpeqd	%ymm1, %ymm1, %ymm1	# tmp139
# poker_bench.c:179:     if (bits & (1u << RANK_ACE)) bits |= 1u; /* duplicate ace as rank -1 */
	orl	$1, %eax	#, tmp155
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	vpsrld	$27, %ymm1, %ymm1	#, tmp139, tmp138
# poker_bench.c:179:     if (bits & (1u << RANK_ACE)) bits |= 1u; /* duplicate ace as rank -1 */
	testb	$16, %dh	#, bits
	cmovne	%eax, %edx	# tmp155,, bits
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	movzwl	%dx, %edx	# bits, _7
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	vpbroadcastd	%edx, %ymm0	# _7, _48
	vpsravd	.LC0(%rip), %ymm0, %ymm0	#, _48, vect__8.210_51
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	vpandd	%ymm1, %ymm0, %ymm0	# tmp138, vect__8.210_51, vect__9.211_52
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	vpcmpd	$0, %ymm1, %ymm0, %k0	#, tmp138, vect__9.211_52, mask_patt_38.212_53
	kmovb	%k0, %eax	# mask_patt_38.212_53, mask_patt_38.212_53
	cmpb	$1, %al	#, mask_patt_38.212_53
	sbbl	%ecx, %ecx	# ivtmp_13
	andl	$-8, %ecx	#, ivtmp_13
	cmpb	$1, %al	#, mask_patt_38.212_53
	sbbl	%eax, %eax	# shift
# poker_bench.c:181:     for (int shift = 0; shift <= 9; ++shift) {
	andl	$8, %eax	#, shift
# poker_bench.c:181:     for (int shift = 0; shift <= 9; ++shift) {
	leal	10(%rcx,%rax), %esi	#, _78
	.p2align 4
	.p2align 4,,10
	.p2align 3
.L54:
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	sarx	%eax, %edx, %ecx	# shift, _7, _34
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	notl	%ecx	# tmp159
	andl	$31, %ecx	#, tmp159
	je	.L56	#,
# poker_bench.c:181:     for (int shift = 0; shift <= 9; ++shift) {
	incl	%eax	# shift
# poker_bench.c:181:     for (int shift = 0; shift <= 9; ++shift) {
	cmpl	%eax, %esi	# shift, _78
	jne	.L54	#,
# poker_bench.c:184:     return false;
	xorl	%eax, %eax	# <retval>
	vzeroupper
# poker_bench.c:185: }
	ret	
	.p2align 4,,10
	.p2align 3
.L56:
# poker_bench.c:182:         if ((bits >> shift & 0x1F) == 0x1F) return true;
	movl	$1, %eax	#, <retval>
	vzeroupper
# poker_bench.c:185: }
	ret	
	.cfi_endproc
.LFE25:
	.size	straight_bitmask, .-straight_bitmask
	.p2align 4
	.globl	sort_hand_insertion
	.type	sort_hand_insertion, @function
sort_hand_insertion:
.LFB26:
	.cfi_startproc
	subq	$24, %rsp	#,
	.cfi_def_cfa_offset 32
# poker_bench.c:193: void sort_hand_insertion(Hand h) {
	xorl	%r8d, %r8d	# ivtmp.245
	movq	%fs:40, %r9	# MEM[(<address-space-1> long unsigned int *)40B], h
	movq	%r9, 8(%rsp)	# h, D.6420
	movq	%rdi, %r9	# h, h
	leaq	8(%r9), %rdi	#, ivtmp.251
.L65:
# poker_bench.c:195:         Card key = h[i]; int j = i - 1;
	movq	(%rdi), %rax	# MEM[(struct Card *)_44], MEM[(struct Card *)_44]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	4(%rdi), %esi	# MEM[(unsigned char *)_44 + 4B], _26
# poker_bench.c:195:         Card key = h[i]; int j = i - 1;
	movl	%r8d, %ecx	# ivtmp.245, j
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movq	%rdi, %rdx	# ivtmp.251, ivtmp.242
# poker_bench.c:195:         Card key = h[i]; int j = i - 1;
	movq	%rax, (%rsp)	# MEM[(struct Card *)_44], key
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %sil	#, _26
	andl	$15, %esi	#, _24
	.p2align 6
	.p2align 4,,10
	.p2align 3
.L62:
	movzbl	-4(%rdx), %eax	# MEM[(unsigned char *)_51 + -4B], _32
	shrb	$2, %al	#, _32
	andl	$15, %eax	#, _33
# poker_bench.c:197:         while (j >= 0 && info_rank(h[j].info) > kr) {
	cmpb	%al, %sil	# _33, _24
	jnb	.L71	#,
# poker_bench.c:198:             h[j+1] = h[j]; --j;
	movq	-8(%rdx), %rax	# MEM[(struct Card *)_51 + -8B], MEM[(struct Card *)_51 + -8B]
# poker_bench.c:198:             h[j+1] = h[j]; --j;
	decl	%ecx	# j
# poker_bench.c:197:         while (j >= 0 && info_rank(h[j].info) > kr) {
	subq	$8, %rdx	#, ivtmp.242
# poker_bench.c:198:             h[j+1] = h[j]; --j;
	movq	%rax, 8(%rdx)	# MEM[(struct Card *)_51 + -8B], MEM[(struct Card *)_51]
# poker_bench.c:197:         while (j >= 0 && info_rank(h[j].info) > kr) {
	cmpl	$-1, %ecx	#, j
	jne	.L62	#,
	movq	%r9, %rax	# h, _71
.L63:
# poker_bench.c:200:         h[j+1] = key;
	movq	(%rsp), %rdx	# key, key
# poker_bench.c:194:     for (int i = 1; i < 5; ++i) {
	incq	%r8	# ivtmp.245
	addq	$8, %rdi	#, ivtmp.251
# poker_bench.c:200:         h[j+1] = key;
	movq	%rdx, (%rax)	# key, *prephitmp_72
# poker_bench.c:194:     for (int i = 1; i < 5; ++i) {
	cmpq	$4, %r8	#, ivtmp.245
	jne	.L65	#,
# poker_bench.c:202: }
	movq	8(%rsp), %rax	# D.6420, tmp133
	subq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp133
	jne	.L72	#,
	addq	$24, %rsp	#,
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret	
	.p2align 4,,10
	.p2align 3
.L71:
	.cfi_restore_state
# poker_bench.c:200:         h[j+1] = key;
	leal	1(%rcx), %eax	#, _67
# poker_bench.c:200:         h[j+1] = key;
	leaq	(%r9,%rax,8), %rax	#, _71
	jmp	.L63	#
.L72:
# poker_bench.c:202: }
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
.LFE26:
	.size	sort_hand_insertion, .-sort_hand_insertion
	.p2align 4
	.globl	sort_hand_network
	.type	sort_hand_network, @function
sort_hand_network:
.LFB27:
	.cfi_startproc
	subq	$24, %rsp	#,
	.cfi_def_cfa_offset 32
# poker_bench.c:212: void sort_hand_network(Hand h) {
	movq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp132
	movq	%rax, 8(%rsp)	# tmp132, D.6425
	xorl	%eax, %eax	# tmp132
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	4(%rdi), %r9d	# h_29(D)->info, _73
	movzbl	12(%rdi), %esi	# MEM[(struct Card *)h_29(D) + 8B].info, _79
	shrb	$2, %r9b	#, _73
	shrb	$2, %sil	#, _79
	andl	$15, %r9d	#, prephitmp_155
	andl	$15, %esi	#, prephitmp_219
# poker_bench.c:214:     SWAP_IF(h[0],h[1]); SWAP_IF(h[3],h[4]);
	cmpb	%r9b, %sil	# prephitmp_155, prephitmp_219
	jnb	.L74	#,
# poker_bench.c:214:     SWAP_IF(h[0],h[1]); SWAP_IF(h[3],h[4]);
	movq	8(%rdi), %rdx	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 8B]
	movq	(%rdi), %rax	# *h_29(D), *h_29(D)
	movq	%rdx, (%rdi)	# MEM[(struct Card *)h_29(D) + 8B], *h_29(D)
	movq	%rax, (%rsp)	# *h_29(D), MEM[(struct  *)_148]
	movq	%rax, 8(%rdi)	# *h_29(D), MEM[(struct Card *)h_29(D) + 8B]
	movl	%r9d, %eax	# prephitmp_155, prephitmp_155
	movl	%esi, %r9d	# prephitmp_219, prephitmp_155
	movl	%eax, %esi	# prephitmp_155, prephitmp_219
.L74:
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	28(%rdi), %eax	# MEM[(struct Card *)h_29(D) + 24B].info, _61
	movzbl	36(%rdi), %r8d	# MEM[(struct Card *)h_29(D) + 32B].info, _67
	shrb	$2, %al	#, _61
	shrb	$2, %r8b	#, _67
	andl	$15, %eax	#, _60
	andl	$15, %r8d	#, _66
# poker_bench.c:214:     SWAP_IF(h[0],h[1]); SWAP_IF(h[3],h[4]);
	cmpb	%al, %r8b	# _60, _66
	jnb	.L75	#,
# poker_bench.c:214:     SWAP_IF(h[0],h[1]); SWAP_IF(h[3],h[4]);
	movq	32(%rdi), %rcx	# MEM[(struct Card *)h_29(D) + 32B], MEM[(struct Card *)h_29(D) + 32B]
	movq	24(%rdi), %rdx	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 24B]
	movq	%rcx, 24(%rdi)	# MEM[(struct Card *)h_29(D) + 32B], MEM[(struct Card *)h_29(D) + 24B]
	movq	%rdx, (%rsp)	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct  *)_148]
	movq	%rdx, 32(%rdi)	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 32B]
	movl	%eax, %edx	# _60, _60
	movl	%r8d, %eax	# _66, _60
	movl	%edx, %r8d	# _60, _66
.L75:
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	20(%rdi), %edx	# MEM[(struct Card *)h_29(D) + 16B].info, _49
	shrb	$2, %dl	#, _49
	andl	$15, %edx	#, _48
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	cmpb	%dl, %r8b	# _48, _66
	jnb	.L76	#,
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	movq	32(%rdi), %r10	# MEM[(struct Card *)h_29(D) + 32B], MEM[(struct Card *)h_29(D) + 32B]
	movzbl	36(%rdi), %ecx	# MEM[(struct Card *)h_29(D) + 32B], tmp190
	movq	16(%rdi), %r8	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 16B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %cl	#, _176
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	movq	%r10, 16(%rdi)	# MEM[(struct Card *)h_29(D) + 32B], MEM[(struct Card *)h_29(D) + 16B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	andl	$15, %ecx	#, _176
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	movq	%r8, (%rsp)	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct  *)_148]
	movq	%r8, 32(%rdi)	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 32B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movl	%edx, %r8d	# _48, _66
	movl	%ecx, %edx	# _176, _48
.L76:
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	cmpb	%dl, %al	# _48, _60
	jnb	.L77	#,
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	movq	16(%rdi), %rcx	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 16B]
	movzbl	28(%rdi), %edx	# MEM[(struct Card *)h_29(D) + 24B], tmp194
	movq	24(%rdi), %rax	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 24B]
	movq	%rcx, (%rsp)	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct  *)_148]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %dl	#, tmp194
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	movq	%rcx, 24(%rdi)	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 24B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrq	$34, %rcx	#, _188
	andl	$15, %ecx	#, _188
# poker_bench.c:215:     SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
	movq	%rax, 16(%rdi)	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 16B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movq	%rcx, %rax	# _188, _60
	movl	%edx, %ecx	# tmp194, _192
	andl	$15, %ecx	#, _192
	movl	%ecx, %edx	# _192, _48
.L77:
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	cmpb	%r9b, %al	# prephitmp_155, _60
	jnb	.L78	#,
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	movq	(%rdi), %rcx	# *h_29(D), *h_29(D)
	movzbl	28(%rdi), %eax	# MEM[(struct Card *)h_29(D) + 24B], tmp196
	movq	24(%rdi), %r9	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 24B]
	movq	%rcx, (%rsp)	# *h_29(D), MEM[(struct  *)_148]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _200
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	movq	%rcx, 24(%rdi)	# *h_29(D), MEM[(struct Card *)h_29(D) + 24B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrq	$34, %rcx	#, _204
	andl	$15, %eax	#, _200
	andl	$15, %ecx	#, _204
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	movq	%r9, (%rdi)	# MEM[(struct Card *)h_29(D) + 24B], *h_29(D)
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movl	%eax, %r9d	# _200, prephitmp_155
	movq	%rcx, %rax	# _204, _60
.L78:
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	cmpb	%r9b, %dl	# prephitmp_155, _48
	jnb	.L79	#,
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	movq	(%rdi), %rcx	# *h_29(D), *h_29(D)
	movq	16(%rdi), %rdx	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 16B]
	movq	%rcx, (%rsp)	# *h_29(D), MEM[(struct  *)_148]
	movq	%rcx, 16(%rdi)	# *h_29(D), MEM[(struct Card *)h_29(D) + 16B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrq	$34, %rcx	#, _210
	andl	$15, %ecx	#, _210
# poker_bench.c:216:     SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
	movq	%rdx, (%rdi)	# MEM[(struct Card *)h_29(D) + 16B], *h_29(D)
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movq	%rcx, %rdx	# _210, _48
.L79:
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	cmpb	%sil, %r8b	# prephitmp_219, _66
	jnb	.L80	#,
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	movq	8(%rdi), %rcx	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 8B]
	movq	32(%rdi), %r8	# MEM[(struct Card *)h_29(D) + 32B], MEM[(struct Card *)h_29(D) + 32B]
	movzbl	36(%rdi), %esi	# MEM[(struct Card *)h_29(D) + 32B], tmp202
	movq	%rcx, (%rsp)	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct  *)_148]
	movq	%r8, 8(%rdi)	# MEM[(struct Card *)h_29(D) + 32B], MEM[(struct Card *)h_29(D) + 8B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %sil	#, _216
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	movq	%rcx, 32(%rdi)	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 32B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	andl	$15, %esi	#, prephitmp_219
.L80:
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	cmpb	%sil, %al	# prephitmp_219, _60
	jnb	.L81	#,
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	movq	8(%rdi), %rcx	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 8B]
	movzbl	28(%rdi), %eax	# MEM[(struct Card *)h_29(D) + 24B], tmp204
	movq	24(%rdi), %rsi	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 24B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _222
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	movq	%rcx, (%rsp)	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct  *)_148]
	movq	%rcx, 24(%rdi)	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 24B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	andl	$15, %eax	#, _222
# poker_bench.c:217:     SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
	movq	%rsi, 8(%rdi)	# MEM[(struct Card *)h_29(D) + 24B], MEM[(struct Card *)h_29(D) + 8B]
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movl	%eax, %esi	# _222, prephitmp_219
.L81:
# poker_bench.c:218:     SWAP_IF(h[1],h[2]);
	cmpb	%sil, %dl	# prephitmp_219, _48
	jnb	.L73	#,
# poker_bench.c:218:     SWAP_IF(h[1],h[2]);
	movq	8(%rdi), %rax	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 8B]
	movq	16(%rdi), %rdx	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 16B]
	movq	%rax, (%rsp)	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct  *)_148]
	movq	%rdx, 8(%rdi)	# MEM[(struct Card *)h_29(D) + 16B], MEM[(struct Card *)h_29(D) + 8B]
	movq	%rax, 16(%rdi)	# MEM[(struct Card *)h_29(D) + 8B], MEM[(struct Card *)h_29(D) + 16B]
.L73:
# poker_bench.c:219: }
	movq	8(%rsp), %rax	# D.6425, tmp206
	subq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp206
	jne	.L86	#,
	addq	$24, %rsp	#,
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret	
.L86:
	.cfi_restore_state
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
.LFE27:
	.size	sort_hand_network, .-sort_hand_network
	.p2align 4
	.globl	eval_hand
	.type	eval_hand, @function
eval_hand:
.LFB28:
	.cfi_startproc
	subq	$40, %rsp	#,
	.cfi_def_cfa_offset 48
# poker_bench.c:231:     uint16_t suit_bits = 0;
	xorl	%esi, %esi	# suit_bits
# poker_bench.c:237:         suit_bits |= (uint16_t)(1u << suits[i]);
	movl	$1, %r8d	#, tmp156
# poker_bench.c:228: int eval_hand(const Hand h) {
	movq	%fs:40, %r11	# MEM[(<address-space-1> long unsigned int *)40B], h
	movq	%r11, 24(%rsp)	# h, D.6456
	movq	%rdi, %r11	# h, h
# poker_bench.c:230:     uint8_t rank_cnt[13] = {0};
	movq	$0, 11(%rsp)	#, rank_cnt
	leaq	6(%rsp), %r9	#, ivtmp.271
	movq	$0, 16(%rsp)	#, rank_cnt
	movq	%r9, %rdi	# ivtmp.271, ivtmp.287
	leaq	4(%r11), %rcx	#, ivtmp.285
	leaq	44(%r11), %r10	#, _73
.L88:
# poker_bench.c:234:         ranks[i] = info_rank(h[i].info);
	movzbl	(%rcx), %edx	# MEM[(unsigned char *)_81], _4
# poker_bench.c:233:     for (int i = 0; i < 5; ++i) {
	addq	$8, %rcx	#, ivtmp.285
	incq	%rdi	# ivtmp.287
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movl	%edx, %eax	# _4, _52
# poker_bench.c:63: static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
	andl	$3, %edx	#, _40
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _52
# poker_bench.c:237:         suit_bits |= (uint16_t)(1u << suits[i]);
	shlx	%edx, %r8d, %edx	# _40, tmp156, _9
# poker_bench.c:237:         suit_bits |= (uint16_t)(1u << suits[i]);
	orl	%edx, %esi	# _9, suit_bits
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	andl	$15, %eax	#, _50
# poker_bench.c:234:         ranks[i] = info_rank(h[i].info);
	movb	%al, -1(%rdi)	# _50, MEM[(unsigned char *)_83]
# poker_bench.c:236:         rank_cnt[ranks[i]]++;
	andl	$15, %eax	#, _5
	incb	11(%rsp,%rax)	# rank_cnt[_5]
# poker_bench.c:233:     for (int i = 0; i < 5; ++i) {
	cmpq	%rcx, %r10	# ivtmp.285, _73
	jne	.L88	#,
# poker_bench.c:240:     bool is_flush    = (suit_bits & (suit_bits - 1)) == 0; /* only 1 bit set */
	movzwl	%si, %r8d	# suit_bits, _11
# poker_bench.c:241:     bool is_straight = straight_bitmask(h);
	movq	%r11, %rdi	# h,
	call	straight_bitmask	#
# poker_bench.c:243:     if (is_straight && is_flush) {
	blsr	%r8d, %edx	# _11, tmp176
	jne	.L89	#,
	testb	%al, %al	# is_straight
	je	.L89	#,
	leaq	5(%r9), %rsi	#, _125
# poker_bench.c:245:         uint16_t bits = 0;
	xorl	%edx, %edx	# bits
# poker_bench.c:246:         for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
	movl	$1, %ecx	#, tmp162
.L90:
# poker_bench.c:246:         for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
	movzbl	(%r9), %eax	# MEM[(unsigned char *)_123], _16
# poker_bench.c:246:         for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
	incq	%r9	# ivtmp.271
# poker_bench.c:246:         for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
	shlx	%eax, %ecx, %eax	# _16, tmp162, _17
# poker_bench.c:246:         for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
	orl	%eax, %edx	# _17, bits
# poker_bench.c:246:         for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
	cmpq	%rsi, %r9	# _125, ivtmp.271
	jne	.L90	#,
# poker_bench.c:247:         return (bits == 0x1F00u) ? 9 : 8; /* royal : straight flush */
	xorl	%ecx, %ecx	# <retval>
	cmpw	$7936, %dx	#, bits
	sete	%cl	#, <retval>
	addl	$8, %ecx	#, <retval>
.L87:
# poker_bench.c:264: }
	movq	24(%rsp), %rax	# D.6456, tmp178
	subq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp178
	jne	.L123	#,
	movl	%ecx, %eax	# <retval>,
	addq	$40, %rsp	#,
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret	
	.p2align 4,,10
	.p2align 3
.L89:
	.cfi_restore_state
# poker_bench.c:249:     if (is_flush)    return 5;
	movl	$5, %ecx	#, <retval>
# poker_bench.c:249:     if (is_flush)    return 5;
	blsr	%r8d, %r8d	# _11, tmp177
	je	.L87	#,
# poker_bench.c:250:     if (is_straight) return 4;
	movl	$4, %ecx	#, <retval>
# poker_bench.c:250:     if (is_straight) return 4;
	testb	%al, %al	# is_straight
	jne	.L87	#,
	leaq	11(%rsp), %rax	#, ivtmp.278
	leaq	24(%rsp), %rsi	#, _101
# poker_bench.c:252:     int pairs = 0, trips = 0, quads = 0;
	xorl	%edi, %edi	# quads
# poker_bench.c:252:     int pairs = 0, trips = 0, quads = 0;
	xorl	%r8d, %r8d	# trips
# poker_bench.c:252:     int pairs = 0, trips = 0, quads = 0;
	xorl	%ecx, %ecx	# <retval>
	jmp	.L95	#
	.p2align 4,,10
	.p2align 3
.L92:
# poker_bench.c:255:         else if (rank_cnt[r] == 3) trips++;
	cmpb	$3, %dl	#, _30
	je	.L124	#,
# poker_bench.c:256:         else if (rank_cnt[r] == 2) pairs++;
	cmpb	$2, %dl	#, _30
	sete	%dl	#, tmp172
	movzbl	%dl, %edx	# tmp172, tmp172
	addl	%edx, %ecx	# tmp172, <retval>
.L93:
# poker_bench.c:253:     for (int r = 0; r < 13; ++r) {
	incq	%rax	# ivtmp.278
	cmpq	%rax, %rsi	# ivtmp.278, _101
	je	.L125	#,
.L95:
# poker_bench.c:254:         if (rank_cnt[r] == 4) quads++;
	movzbl	(%rax), %edx	# MEM[(unsigned char *)_86], _30
# poker_bench.c:254:         if (rank_cnt[r] == 4) quads++;
	cmpb	$4, %dl	#, _30
	jne	.L92	#,
# poker_bench.c:253:     for (int r = 0; r < 13; ++r) {
	incq	%rax	# ivtmp.278
# poker_bench.c:254:         if (rank_cnt[r] == 4) quads++;
	incl	%edi	# quads
# poker_bench.c:253:     for (int r = 0; r < 13; ++r) {
	cmpq	%rax, %rsi	# ivtmp.278, _101
	jne	.L95	#,
.L125:
# poker_bench.c:258:     if (quads)               return 7;
	testl	%edi, %edi	# quads
	jne	.L101	#,
# poker_bench.c:259:     if (trips && pairs)      return 6;
	testl	%r8d, %r8d	# trips
	je	.L121	#,
	testl	%ecx, %ecx	# <retval>
	jne	.L102	#,
# poker_bench.c:260:     if (trips)               return 3;
	testl	%r8d, %r8d	# trips
	jne	.L103	#,
.L121:
# poker_bench.c:261:     if (pairs == 2)          return 2;
	cmpl	$2, %ecx	#, <retval>
	je	.L87	#,
# poker_bench.c:262:     if (pairs == 1)          return 1;
	cmpl	$1, %ecx	#, <retval>
	sete	%cl	#, <retval>
	movzbl	%cl, %ecx	# <retval>, <retval>
	jmp	.L87	#
	.p2align 4,,10
	.p2align 3
.L124:
# poker_bench.c:255:         else if (rank_cnt[r] == 3) trips++;
	incl	%r8d	# trips
	jmp	.L93	#
	.p2align 4,,10
	.p2align 3
.L101:
# poker_bench.c:258:     if (quads)               return 7;
	movl	$7, %ecx	#, <retval>
	jmp	.L87	#
	.p2align 4,,10
	.p2align 3
.L102:
# poker_bench.c:259:     if (trips && pairs)      return 6;
	movl	$6, %ecx	#, <retval>
	jmp	.L87	#
.L103:
# poker_bench.c:260:     if (trips)               return 3;
	movl	$3, %ecx	#, <retval>
	jmp	.L87	#
.L123:
# poker_bench.c:264: }
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
.LFE28:
	.size	eval_hand, .-eval_hand
	.p2align 4
	.globl	shuffle_deck
	.type	shuffle_deck, @function
shuffle_deck:
.LFB30:
	.cfi_startproc
	subq	$24, %rsp	#,
	.cfi_def_cfa_offset 32
# poker_bench.c:278: void shuffle_deck(Card *deck, int n) {
	movq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp118
	movq	%rax, 8(%rsp)	# tmp118, D.6471
	xorl	%eax, %eax	# tmp118
# poker_bench.c:279:     for (int i = n - 1; i > 0; --i) {
	cmpl	$1, %esi	#, n
	jle	.L126	#,
	movl	%esi, %eax	# ivtmp.298, _58
	movq	rng_state(%rip), %rcx	# rng_state, rng_state_lsm.291
	movq	%rdi, %r9	# deck, deck
	leaq	(%rdi,%rax,8), %rdi	#, ivtmp.300
	.p2align 4,,10
	.p2align 3
.L128:
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	movq	%rcx, %rdx	# rng_state_lsm.291, _22
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	movq	-8(%rdi), %r8	# MEM[(struct Card *)_55 + -8B], MEM[(struct Card *)_55 + -8B]
# poker_bench.c:279:     for (int i = n - 1; i > 0; --i) {
	subq	$8, %rdi	#, ivtmp.300
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	salq	$13, %rdx	#, _22
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	xorq	%rcx, %rdx	# rng_state_lsm.291, _23
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	movq	%r8, (%rsp)	# MEM[(struct Card *)_55 + -8B], tmp
# poker_bench.c:272:     rng_state ^= rng_state >> 7;
	movq	%rdx, %rax	# _23, _24
	shrq	$7, %rax	#, _24
# poker_bench.c:272:     rng_state ^= rng_state >> 7;
	xorq	%rdx, %rax	# _23, _25
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	xorl	%edx, %edx	# _7
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	movq	%rax, %rcx	# _25, _26
	salq	$17, %rcx	#, _26
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	xorq	%rax, %rcx	# _25, rng_state_lsm.291
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	movl	%ecx, %eax	# rng_state_lsm.291, tmp129
	divl	%esi	# ivtmp.298
# poker_bench.c:279:     for (int i = n - 1; i > 0; --i) {
	decl	%esi	# ivtmp.298
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	leaq	(%r9,%rdx,8), %rax	#, _9
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	movq	(%rax), %rdx	# *_9, *_9
	movq	%rdx, (%rdi)	# *_9, MEM[(struct Card *)_55 + -8B]
# poker_bench.c:281:         Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
	movq	%r8, (%rax)	# MEM[(struct Card *)_55 + -8B], *_9
# poker_bench.c:279:     for (int i = n - 1; i > 0; --i) {
	cmpl	$1, %esi	#, ivtmp.298
	jne	.L128	#,
	movq	%rcx, rng_state(%rip)	# rng_state_lsm.291, rng_state
.L126:
# poker_bench.c:283: }
	movq	8(%rsp), %rax	# D.6471, tmp138
	subq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp138
	jne	.L133	#,
	addq	$24, %rsp	#,
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret	
.L133:
	.cfi_restore_state
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
.LFE30:
	.size	shuffle_deck, .-shuffle_deck
	.p2align 4
	.globl	eq_64
	.type	eq_64, @function
eq_64:
.LFB31:
	.cfi_startproc
# poker_bench.c:288:     return card_word(a) == card_word(b);
	movq	(%rsi), %rax	# MEM <unsigned long> [(char * {ref-all})b_4(D)], tmp109
	cmpq	%rax, (%rdi)	# tmp109, MEM <unsigned long> [(char * {ref-all})a_2(D)]
	sete	%al	#, _6
# poker_bench.c:289: }
	ret	
	.cfi_endproc
.LFE31:
	.size	eq_64, .-eq_64
	.p2align 4
	.globl	eq_fields
	.type	eq_fields, @function
eq_fields:
.LFB32:
	.cfi_startproc
# poker_bench.c:295:         && a->title[2] == b->title[2];
	movl	(%rsi), %eax	# b_14(D)->next, tmp121
	cmpl	%eax, (%rdi)	# tmp121, a_13(D)->next
	je	.L136	#,
.L138:
# poker_bench.c:291: bool eq_fields(const Card *a, const Card *b) {
	xorl	%eax, %eax	# <retval>
	ret	
	.p2align 4,,10
	.p2align 3
.L136:
# poker_bench.c:292:     return a->next == b->next && a->info == b->info
	movzbl	4(%rsi), %eax	# b_14(D)->info, tmp122
	cmpb	%al, 4(%rdi)	# tmp122, a_13(D)->info
	jne	.L138	#,
# poker_bench.c:293:         && a->title[0] == b->title[0]
	movzbl	5(%rsi), %eax	# b_14(D)->title[0], tmp123
	cmpb	%al, 5(%rdi)	# tmp123, a_13(D)->title[0]
	jne	.L138	#,
# poker_bench.c:294:         && a->title[1] == b->title[1]
	movzbl	6(%rsi), %eax	# b_14(D)->title[1], tmp124
	cmpb	%al, 6(%rdi)	# tmp124, a_13(D)->title[1]
	jne	.L138	#,
# poker_bench.c:295:         && a->title[2] == b->title[2];
	movzbl	7(%rsi), %eax	# b_14(D)->title[2], tmp125
	cmpb	%al, 7(%rdi)	# tmp125, a_13(D)->title[2]
	sete	%al	#, <retval>
# poker_bench.c:296: }
	ret	
	.cfi_endproc
.LFE32:
	.size	eq_fields, .-eq_fields
	.section	.rodata.str1.1,"aMS",@progbits,1
.LC3:
	.string	"Str. flush"
.LC4:
	.string	"Royal flush"
.LC5:
	.string	"High card"
.LC6:
	.string	"Pair"
.LC7:
	.string	"Two pair"
.LC8:
	.string	"Trips"
.LC9:
	.string	"Straight"
.LC10:
	.string	"Flush"
.LC11:
	.string	"Full house"
.LC12:
	.string	"Quads"
	.section	.rodata.str1.8,"aMS",@progbits,1
	.align 8
.LC13:
	.string	"sizeof(Card)=%zu  N_CARDS=%dM  N_HANDS=%dM  REPS=%d\n\n"
	.section	.rodata.str1.1
.LC22:
	.string	"alloc"
	.section	.rodata.str1.8
	.align 8
.LC23:
	.base64	"4pSB4pSB4pSBIDEuIENhcmQgRXF1YWxpdHkgKCVkTSBjYXJkIHBhaXJzKSDilIHilIHilIEKAA=="
	.section	.rodata.str1.1
.LC27:
	.string	"eq_64  (1\303\22764-bit cmpq)"
	.section	.rodata.str1.8
	.align 8
.LC28:
	.string	"  %-38s  %6.1f Mop/s  (%.4f s)  chk=%llu\n"
	.section	.rodata.str1.1
.LC29:
	.string	"eq_fields (field-by-field)"
	.section	.rodata.str1.8
	.align 8
.LC30:
	.base64	"CuKUgeKUgeKUgSAyLiBSYW5rIEV4dHJhY3Rpb24gKCVkTSBjYXJkcykg4pSB4pSB4pSBCgA="
	.align 8
.LC32:
	.string	"rank_bitshift  (info>>2 & 0xF)"
	.section	.rodata.str1.1
.LC33:
	.string	"rank_lut       (lut[info])"
	.section	.rodata.str1.8
	.align 8
.LC34:
	.base64	"CuKUgeKUgeKUgSAzLiBTdWl0IEV4dHJhY3Rpb24gKCVkTSBjYXJkcykg4pSB4pSB4pSBCgA="
	.section	.rodata.str1.1
.LC35:
	.string	"suit_mask  (info & 0x03)"
	.section	.rodata.str1.8
	.align 8
.LC36:
	.base64	"CuKUgeKUgeKUgSA0LiBGbHVzaCBEZXRlY3Rpb24gKCVkTSBoYW5kcykg4pSB4pSB4pSBCgA="
	.align 8
.LC38:
	.string	"flush_field  (5\303\227 suit compare)"
	.align 8
.LC39:
	.string	"flush_word   (word-mask OR/AND)"
	.align 8
.LC40:
	.base64	"CuKUgeKUgeKUgSA1LiBTdHJhaWdodCBEZXRlY3Rpb24gKCVkTSBoYW5kcykg4pSB4pSB4pSBCgA="
	.align 8
.LC41:
	.string	"straight_sort    (sort+gap check)"
	.align 8
.LC42:
	.string	"straight_bitmask (rank bits fold)"
	.align 8
.LC43:
	.base64	"CuKUgeKUgeKUgSA2LiBIYW5kIFNvcnRpbmcgNSBjYXJkcyAoJWRNIGhhbmRzKSDilIHilIHilIEKAA=="
	.align 8
.LC44:
	.string	"insertion_sort  (branch-heavy)"
	.align 8
.LC45:
	.string	"network_sort    (9 comparators)"
	.align 8
.LC46:
	.base64	"CuKUgeKUgeKUgSA3LiBGdWxsIEhhbmQgRXZhbHVhdGlvbiAoJWRNIGhhbmRzKSDilIHilIHilIEKAA=="
	.align 8
.LC47:
	.string	"eval_hand  (flush+straight+counts)"
	.align 8
.LC48:
	.base64	"CuKUgeKUgeKUgSA4LiBEZWNrIFNodWZmbGUgKCVkayBzaHVmZmxlcyBvZiA1MiBjYXJkcykg4pSB4pSB4pSBCgA="
	.section	.rodata.str1.1
.LC50:
	.string	"fisher_yates  (52-card deck)"
	.section	.rodata.str1.8
	.align 8
.LC51:
	.base64	"CuKUgeKUgeKUgSBIYW5kIGRpc3RyaWJ1dGlvbiAoZnJvbSAlZE0gZGVhbHQgaGFuZHMpIOKUgeKUgeKUgQoA"
	.section	.rodata.str1.1
.LC54:
	.string	"  %-14s  %8u  (%5.2f%%)\n"
	.section	.text.unlikely,"ax",@progbits
.LCOLDB62:
	.section	.text.startup,"ax",@progbits
.LHOTB62:
	.p2align 4
	.globl	main
	.type	main, @function
main:
.LFB36:
	.cfi_startproc
	leaq	8(%rsp), %r10	#,
	.cfi_def_cfa 10, 0
	andq	$-32, %rsp	#,
# poker_bench.c:377:     printf("sizeof(Card)=%zu  N_CARDS=%dM  N_HANDS=%dM  REPS=%d\n\n",
	movl	$4, %ecx	#,
	movl	$16, %edx	#,
# poker_bench.c:376: {
	pushq	-8(%r10)	#
# poker_bench.c:377:     printf("sizeof(Card)=%zu  N_CARDS=%dM  N_HANDS=%dM  REPS=%d\n\n",
	movl	$5, %r8d	#,
	movl	$8, %esi	#,
	leaq	.LC13(%rip), %rdi	#,
# poker_bench.c:376: {
	pushq	%rbp	#
	movq	%rsp, %rbp	#,
	.cfi_escape 0x10,0x6,0x2,0x76,0
	pushq	%r15	#
	pushq	%r14	#
	pushq	%r13	#
	pushq	%r12	#
	pushq	%r10	#
	.cfi_escape 0xf,0x3,0x76,0x58,0x6
	.cfi_escape 0x10,0xf,0x2,0x76,0x78
	.cfi_escape 0x10,0xe,0x2,0x76,0x70
	.cfi_escape 0x10,0xd,0x2,0x76,0x68
	.cfi_escape 0x10,0xc,0x2,0x76,0x60
	pushq	%rbx	#
	subq	$736, %rsp	#,
	.cfi_escape 0x10,0x3,0x2,0x76,0x50
	movq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp522
	movq	%rax, -56(%rbp)	# tmp522, D.6797
	leaq	.LC8(%rip), %rax	#, tmp522
	vmovq	.LC57(%rip), %xmm5	#, tmp1116
	vmovq	.LC58(%rip), %xmm6	#, tmp1117
	vmovq	.LC59(%rip), %xmm7	#, tmp1119
	vpinsrq	$1, %rax, %xmm5, %xmm0	# tmp522, tmp1116, tmp521
	leaq	.LC6(%rip), %rax	#, tmp525
	vpinsrq	$1, %rax, %xmm6, %xmm1	# tmp525, tmp1117, tmp524
	leaq	.LC12(%rip), %rax	#, tmp529
	vinserti64x2	$0x1, %xmm0, %ymm1, %ymm5	# tmp521, tmp524, _170
	vpinsrq	$1, %rax, %xmm7, %xmm0	# tmp529, tmp1119, tmp528
	leaq	.LC10(%rip), %rax	#, tmp532
	vmovdqa	%ymm5, -752(%rbp)	# _170, %sfp
	vmovq	.LC60(%rip), %xmm5	#, tmp1120
	vpinsrq	$1, %rax, %xmm5, %xmm1	# tmp532, tmp1120, tmp531
# poker_bench.c:377:     printf("sizeof(Card)=%zu  N_CARDS=%dM  N_HANDS=%dM  REPS=%d\n\n",
	xorl	%eax, %eax	#
	vinserti64x2	$0x1, %xmm0, %ymm1, %ymm6	# tmp528, tmp531, _182
	vmovdqa	%ymm6, -784(%rbp)	# _182, %sfp
	vzeroupper
	call	printf@PLT	#
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	movl	$8, %edx	#, tmp538
	leaq	rank_lut(%rip), %rax	#, ivtmp.559
# poker_bench.c:377:     printf("sizeof(Card)=%zu  N_CARDS=%dM  N_HANDS=%dM  REPS=%d\n\n",
	vmovdqa	.LC0(%rip), %ymm1	#, vect_vec_iv_.350
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	vpbroadcastd	%edx, %ymm9	# tmp538, tmp537
	movl	$16, %edx	#, tmp544
	vmovdqa	.LC15(%rip), %ymm3	#, tmp957
	vmovdqa	.LC18(%rip), %ymm11	#, tmp958
	vpbroadcastd	%edx, %ymm8	# tmp544, tmp543
	movl	$24, %edx	#, tmp548
	vmovdqa	.LC19(%rip), %ymm10	#, tmp959
	leaq	256(%rax), %rcx	#, _77
	vpbroadcastd	%edx, %ymm7	# tmp548, tmp547
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	movl	$252645135, %edx	#, tmp560
	vpbroadcastd	%edx, %ymm6	# tmp560, tmp559
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	movl	$32, %edx	#, tmp563
	vpbroadcastd	%edx, %ymm5	# tmp563, tmp562
	.p2align 4,,10
	.p2align 3
.L140:
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	vpaddd	%ymm9, %ymm1, %ymm2	# tmp537, vect_vec_iv_.350, _663
	vmovdqa	%ymm1, %ymm0	# vect_vec_iv_.350, vect_patt_291.352_200
	vpaddd	%ymm7, %ymm1, %ymm4	# tmp547, vect_vec_iv_.350, _870
	addq	$32, %rax	#, ivtmp.559
	vpermt2w	%ymm2, %ymm3, %ymm0	# _663, tmp957, vect_patt_291.352_200
	vpaddd	%ymm8, %ymm1, %ymm2	# tmp543, vect_vec_iv_.350, _871
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	vpaddd	%ymm5, %ymm1, %ymm1	# tmp562, vect_vec_iv_.350, vect_vec_iv_.350
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	vpermt2w	%ymm4, %ymm3, %ymm2	# _870, tmp957, vect_patt_291.352_192
	vpermt2b	%ymm2, %ymm11, %ymm0	# vect_patt_291.352_192, tmp958, vect_patt_291.351_191
	vgf2p8affineqb	$0, %ymm10, %ymm0, %ymm0	#, tmp959, vect_patt_291.351_191, vect_patt_406.353_190
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	vpandq	%ymm6, %ymm0, %ymm0	# tmp559, vect_patt_406.353_190, vect__310.355_188
# poker_bench.c:84:     for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
	vmovdqa	%ymm0, -32(%rax)	# vect__310.355_188, MEM <vector(32) unsigned char> [(unsigned char *)_93]
	cmpq	%rax, %rcx	# ivtmp.559, _77
	jne	.L140	#,
# poker_bench.c:332:     hands_pool = aligned_alloc(64, N_HANDS * sizeof(Hand));
	movl	$167772160, %esi	#,
	movl	$64, %edi	#,
	vzeroupper
	call	aligned_alloc@PLT	#
# poker_bench.c:333:     cards_pool = aligned_alloc(64, N_CARDS * sizeof(Card));
	movl	$134217728, %esi	#,
	movl	$64, %edi	#,
# poker_bench.c:332:     hands_pool = aligned_alloc(64, N_HANDS * sizeof(Hand));
	movq	%rax, hands_pool(%rip)	# tmp1046, hands_pool
# poker_bench.c:332:     hands_pool = aligned_alloc(64, N_HANDS * sizeof(Hand));
	movq	%rax, %rbx	#, tmp1046
# poker_bench.c:333:     cards_pool = aligned_alloc(64, N_CARDS * sizeof(Card));
	call	aligned_alloc@PLT	#
# poker_bench.c:333:     cards_pool = aligned_alloc(64, N_CARDS * sizeof(Card));
	movq	%rax, cards_pool(%rip)	# _400, cards_pool
# poker_bench.c:333:     cards_pool = aligned_alloc(64, N_CARDS * sizeof(Card));
	movq	%rax, %r12	#, _400
# poker_bench.c:334:     if (!hands_pool || !cards_pool) { perror("alloc"); exit(1); }
	testq	%rbx, %rbx	# tmp1046
	je	.L204	#,
	testq	%rax, %rax	# _400
	je	.L204	#,
# poker_bench.c:337:     build_52(deck);
	leaq	-480(%rbp), %rdi	#, tmp1123
	leaq	167772160(%rbx), %r13	#, _115
	call	build_52	#
	jmp	.L143	#
	.p2align 4,,10
	.p2align 3
.L248:
	vzeroupper
.L143:
# poker_bench.c:341:         shuffle_deck(deck, DECK_SZ);
	movl	$52, %esi	#,
	leaq	-480(%rbp), %rdi	#, tmp1124
# poker_bench.c:340:     for (int h = 0; h < N_HANDS; ++h) {
	addq	$40, %rbx	#, ivtmp.553
# poker_bench.c:341:         shuffle_deck(deck, DECK_SZ);
	call	shuffle_deck	#
# poker_bench.c:342:         for (int c = 0; c < 5; ++c) hands_pool[h][c] = deck[c];
	vmovdqu	-480(%rbp), %ymm0	# MEM <char[1:40]> [(void *)_131], MEM <char[1:40]> [(void *)_131]
	movq	-448(%rbp), %rax	# MEM <char[1:40]> [(void *)_131], MEM <char[1:40]> [(void *)_131]
	vmovdqu	%ymm0, -40(%rbx)	# MEM <char[1:40]> [(void *)_131], MEM <char[1:40]> [(void *)_404]
	movq	%rax, -8(%rbx)	# MEM <char[1:40]> [(void *)_131], MEM <char[1:40]> [(void *)_404]
# poker_bench.c:340:     for (int h = 0; h < N_HANDS; ++h) {
	cmpq	%rbx, %r13	# ivtmp.553, _115
	jne	.L248	#,
	movq	rng_state(%rip), %r14	# rng_state, rng_state_lsm.348
	movq	%r12, %rcx	# _400, ivtmp.546
# poker_bench.c:346:     for (int i = 0; i < N_CARDS; ++i) {
	xorl	%edi, %edi	# i
	leaq	RANK_STR(%rip), %r9	#, tmp953
	leaq	SUIT_CHAR(%rip), %r8	#, tmp950
	.p2align 4,,10
	.p2align 3
.L148:
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	movq	%r14, %rsi	# rng_state_lsm.348, _409
	xorl	%ebx, %ebx	# *_440
	salq	$13, %rsi	#, _409
	movq	%rsi, %rdx	# _409, _409
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	xorq	%r14, %rdx	# rng_state_lsm.348, _410
# poker_bench.c:272:     rng_state ^= rng_state >> 7;
	movq	%rdx, %rax	# _410, _411
	shrq	$7, %rax	#, _411
# poker_bench.c:272:     rng_state ^= rng_state >> 7;
	xorq	%rdx, %rax	# _410, _412
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	movq	%rax, %rdx	# _412, _413
	salq	$17, %rdx	#, _413
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	xorq	%rax, %rdx	# _412, _414
# poker_bench.c:347:         int r = fast_rand() % 13;
	movl	%edx, %eax	# _414, _414
	imulq	$1321528399, %rax, %rax	#, _414, tmp581
	shrq	$34, %rax	#, _416
	leal	(%rax,%rax,2), %esi	#, tmp585
	leal	(%rax,%rsi,4), %esi	#, tmp587
	movl	%edx, %eax	# _414, _416
	subl	%esi, %eax	# tmp587, _416
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	movq	%rdx, %rsi	# _414, _417
	salq	$13, %rsi	#, _417
# poker_bench.c:313:     c.title[0]= RANK_STR[rank][0];
	movl	%eax, %r11d	# _416, _416
# poker_bench.c:61:     return (uint8_t)((rank << RANK_SHIFT) | suit);
	sall	$2, %eax	#, _437
# poker_bench.c:271:     rng_state ^= rng_state << 13;
	xorq	%rdx, %rsi	# _414, _418
# poker_bench.c:313:     c.title[0]= RANK_STR[rank][0];
	movq	(%r9,%r11,8), %r11	# RANK_STR[_439], RANK_STR[_439]
# poker_bench.c:272:     rng_state ^= rng_state >> 7;
	movq	%rsi, %rdx	# _418, _419
	shrq	$7, %rdx	#, _419
	movb	(%r11), %bl	# *_440, *_440
# poker_bench.c:349:         cards_pool[i] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)i);
	movl	%edi, (%rcx)	# i, MEM[(unsigned int *)_125]
# poker_bench.c:272:     rng_state ^= rng_state >> 7;
	xorq	%rsi, %rdx	# _418, _420
# poker_bench.c:349:         cards_pool[i] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)i);
	movb	$0, 7(%rcx)	#, MEM[(char *)_125 + 7B]
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	movq	%rdx, %rsi	# _420, _421
# poker_bench.c:348:         int s = fast_rand() % 4;
	movl	%edx, %r10d	# _420, s
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	salq	$17, %rsi	#, _421
# poker_bench.c:348:         int s = fast_rand() % 4;
	andl	$3, %r10d	#, s
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	xorq	%rdx, %rsi	# _420, _421
# poker_bench.c:314:     c.title[1]= SUIT_CHAR[suit];
	andl	$3, %edx	#, s
# poker_bench.c:61:     return (uint8_t)((rank << RANK_SHIFT) | suit);
	orl	%r10d, %eax	# s, tmp601
# poker_bench.c:273:     rng_state ^= rng_state << 17;
	movq	%rsi, %r14	# _421, rng_state_lsm.348
	movq	%r8, %rsi	# tmp950, tmp950
# poker_bench.c:61:     return (uint8_t)((rank << RANK_SHIFT) | suit);
	movb	%al, 4(%rcx)	# tmp601, MEM[(unsigned char *)_125 + 4B]
	movb	(%rsi,%rdx), %bh	# SUIT_CHAR[s_424], *_440
# poker_bench.c:349:         cards_pool[i] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)i);
	movw	%bx, 5(%rcx)	# *_440, MEM <vector(2) char> [(char *)_125 + 5B]
# poker_bench.c:350:         if (i > 0 && (i & 1)) cards_pool[i] = cards_pool[i-1]; /* 50% dup */
	testl	%edi, %edi	# i
	je	.L250	#,
# poker_bench.c:350:         if (i > 0 && (i & 1)) cards_pool[i] = cards_pool[i-1]; /* 50% dup */
	testb	$1, %dil	#, i
	jne	.L146	#,
# poker_bench.c:346:     for (int i = 0; i < N_CARDS; ++i) {
	incl	%edi	# i
# poker_bench.c:346:     for (int i = 0; i < N_CARDS; ++i) {
	addq	$8, %rcx	#, ivtmp.546
	jmp	.L148	#
	.p2align 4,,10
	.p2align 3
.L250:
	addq	$8, %rcx	#, ivtmp.546
# poker_bench.c:346:     for (int i = 0; i < N_CARDS; ++i) {
	movl	$1, %edi	#, i
	jmp	.L148	#
	.p2align 4,,10
	.p2align 3
.L146:
# poker_bench.c:350:         if (i > 0 && (i & 1)) cards_pool[i] = cards_pool[i-1]; /* 50% dup */
	movq	-8(%rcx), %rax	# MEM[(struct Card *)_125 + -8B], MEM[(struct Card *)_125 + -8B]
# poker_bench.c:346:     for (int i = 0; i < N_CARDS; ++i) {
	incl	%edi	# i
# poker_bench.c:350:         if (i > 0 && (i & 1)) cards_pool[i] = cards_pool[i-1]; /* 50% dup */
	movq	%rax, (%rcx)	# MEM[(struct Card *)_125 + -8B], MEM[(struct Card *)_125]
# poker_bench.c:346:     for (int i = 0; i < N_CARDS; ++i) {
	cmpl	$16777216, %edi	#, i
	je	.L147	#,
	addq	$8, %rcx	#, ivtmp.546
	jmp	.L148	#
.L147:
	movq	%r14, rng_state(%rip)	# rng_state_lsm.348, rng_state
	vxorps	%xmm5, %xmm5, %xmm5	# tmp1059
# poker_bench.c:384:     printf("━━━ 1. Card Equality (%dM card pairs) ━━━\n", N_CARDS>>20);
	xorl	%eax, %eax	#
	movl	$16, %esi	#,
	vmovaps	%xmm5, -688(%rbp)	# tmp1059, %sfp
	leaq	.LC23(%rip), %rdi	#,
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	movl	$5, %r14d	#, ivtmp_304
# poker_bench.c:384:     printf("━━━ 1. Card Equality (%dM card pairs) ━━━\n", N_CARDS>>20);
	vzeroupper
	call	printf@PLT	#
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	movq	.LC2(%rip), %rax	#, tmp1135
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	movq	cards_pool(%rip), %r15	# cards_pool, pretmp_885
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	xorl	%ebx, %ebx	# _chk
	movq	%rax, -696(%rbp)	# tmp1135, %sfp
	leaq	134217720(%r15), %r12	#, _130
.L152:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1136
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm5	# %sfp, tmp1059
	movq	%r15, %rdx	# pretmp_885, ivtmp.532
	vcvtsi2sdq	-664(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1060
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _447, _448
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm5, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1138, tmp1061
	vaddsd	%xmm1, %xmm0, %xmm6	# _445, _448, _449
	vmovq	%xmm6, %r13	# _449, _449
	.p2align 4,,10
	.p2align 3
.L149:
	movq	%rdx, %rdi	# ivtmp.532, ivtmp.532
	addq	$8, %rdx	#, ivtmp.532
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	movq	%rdx, %rsi	# ivtmp.532,
	call	eq_64	#
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	movzbl	%al, %eax	# _396, _8
	addq	%rax, %rbx	# _8, _chk
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	cmpq	%rdx, %r12	# ivtmp.532, _130
	jne	.L149	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1140
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm6	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _453
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	vmovq	%r13, %xmm4	# _449, _449
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1062
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1062, _453, _453
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1063
	vaddsd	%xmm0, %xmm1, %xmm1	# _451, _454, _455
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	vsubsd	%xmm4, %xmm1, %xmm1	# _449, _455, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _239
	vmovsd	%xmm1, -696(%rbp)	# _239, %sfp
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	decl	%r14d	# ivtmp_304
	jne	.L152	#,
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	vmovsd	.LC25(%rip), %xmm6	#, tmp1145
	movq	%rbx, %rdx	# _chk,
	leaq	.LC27(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	movl	$5, %r14d	#, ivtmp_316
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:385:     TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
	vdivsd	%xmm1, %xmm6, %xmm0	# _239, tmp1145, _10
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _10, _mops_202
	call	printf@PLT	#
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	movq	.LC2(%rip), %rax	#, tmp1146
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	movq	cards_pool(%rip), %r15	# cards_pool, pretmp_887
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	movq	%rax, -696(%rbp)	# tmp1146, %sfp
	leaq	134217720(%r15), %r12	#, _136
.L156:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1147
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm4	# %sfp, tmp1059
	movq	%r15, %rdx	# pretmp_887, ivtmp.518
	vcvtsi2sdq	-664(%rbp), %xmm4, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1064
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _459, _460
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm4, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1065
	vaddsd	%xmm1, %xmm0, %xmm2	# _457, _460, _461
	vmovq	%xmm2, %r13	# _461, _461
	.p2align 4,,10
	.p2align 3
.L153:
	movq	%rdx, %rdi	# ivtmp.518, ivtmp.518
	addq	$8, %rdx	#, ivtmp.518
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	movq	%rdx, %rsi	# ivtmp.518,
	call	eq_fields	#
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	movzbl	%al, %eax	# _387, _19
	addq	%rax, %rbx	# _19, _chk
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	cmpq	%rdx, %r12	# ivtmp.518, _136
	jne	.L153	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1151
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm3	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _465
	vcvtsi2sdq	-664(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1066
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1066, _465, _465
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1067
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	vmovq	%r13, %xmm3	# _461, _461
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vaddsd	%xmm0, %xmm1, %xmm1	# _463, _466, _467
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	vsubsd	%xmm3, %xmm1, %xmm1	# _461, _467, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _238
	vmovsd	%xmm1, -696(%rbp)	# _238, %sfp
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	decl	%r14d	# ivtmp_316
	jne	.L156	#,
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	vmovsd	.LC25(%rip), %xmm2	#, tmp1156
	movq	%rbx, %rdx	# _chk,
	leaq	.LC29(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	movl	$5, %r14d	#, ivtmp_333
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:387:     TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
	vdivsd	%xmm1, %xmm2, %xmm0	# _238, tmp1156, _21
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _21, _mops_204
	call	printf@PLT	#
# poker_bench.c:391:     printf("\n━━━ 2. Rank Extraction (%dM cards) ━━━\n", N_CARDS>>20);
	movl	$16, %esi	#,
	leaq	.LC30(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	movq	.LC2(%rip), %rax	#, tmp1157
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	movq	cards_pool(%rip), %r15	# cards_pool, pretmp_889
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	movq	%rax, -696(%rbp)	# tmp1157, %sfp
	leaq	134217728(%r15), %r12	#, _142
.L160:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1158
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm5	# %sfp, tmp1059
	movq	%r15, %r8	# pretmp_889, ivtmp.506
	vcvtsi2sdq	-664(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1068
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _471, _472
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm5, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1069
	vaddsd	%xmm1, %xmm0, %xmm7	# _469, _472, _473
	vmovq	%xmm7, %r13	# _473, _473
	.p2align 4,,10
	.p2align 3
.L157:
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	movq	%r8, %rdi	# ivtmp.506,
	movl	$1, %esi	#,
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	addq	$8, %r8	#, ivtmp.506
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	call	bench_rank_bitshift	#
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	movl	%eax, %eax	# _378, _26
	addq	%rax, %rbx	# _26, _chk
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	cmpq	%r8, %r12	# ivtmp.506, _142
	jne	.L157	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1162
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm6	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _477
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	vmovq	%r13, %xmm5	# _473, _473
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1070
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1070, _477, _477
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1071
	vaddsd	%xmm0, %xmm1, %xmm1	# _475, _478, _479
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	vsubsd	%xmm5, %xmm1, %xmm1	# _473, _479, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _235
	vmovsd	%xmm1, -696(%rbp)	# _235, %sfp
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	decl	%r14d	# ivtmp_333
	jne	.L160	#,
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	vmovsd	.LC31(%rip), %xmm5	#, tmp1167
	movq	%rbx, %rdx	# _chk,
	leaq	.LC32(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	movl	$5, %r14d	#, ivtmp_343
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:392:     TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
	vdivsd	%xmm1, %xmm5, %xmm0	# _235, tmp1167, _28
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _28, _mops_207
	call	printf@PLT	#
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	movq	.LC2(%rip), %rax	#, tmp1168
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	movq	cards_pool(%rip), %r15	# cards_pool, pretmp_891
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	movq	%rax, -696(%rbp)	# tmp1168, %sfp
	leaq	134217728(%r15), %r12	#, _147
.L164:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1169
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm7	# %sfp, tmp1059
	movq	%r15, %r8	# pretmp_891, ivtmp.494
	vcvtsi2sdq	-664(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1072
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _483, _484
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm7, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1171, tmp1073
	vaddsd	%xmm1, %xmm0, %xmm4	# _481, _484, _485
	vmovq	%xmm4, %r13	# _485, _485
	.p2align 4,,10
	.p2align 3
.L161:
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	movq	%r8, %rdi	# ivtmp.494,
	movl	$1, %esi	#,
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	addq	$8, %r8	#, ivtmp.494
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	call	bench_rank_lut	#
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	movl	%eax, %eax	# _368, _33
	addq	%rax, %rbx	# _33, _chk
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	cmpq	%r8, %r12	# ivtmp.494, _147
	jne	.L161	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1173
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm4	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _489
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	vmovq	%r13, %xmm6	# _485, _485
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm4, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1074
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1074, _489, _489
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm4, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1075
	vaddsd	%xmm0, %xmm1, %xmm1	# _487, _490, _491
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	vsubsd	%xmm6, %xmm1, %xmm1	# _485, _491, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _231
	vmovsd	%xmm1, -696(%rbp)	# _231, %sfp
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	decl	%r14d	# ivtmp_343
	jne	.L164	#,
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	vmovsd	.LC31(%rip), %xmm6	#, tmp1178
	movq	%rbx, %rdx	# _chk,
	leaq	.LC33(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	movl	$5, %r14d	#, ivtmp_354
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:394:     TIME_IT("rank_lut       (lut[info])",      N_CARDS,
	vdivsd	%xmm1, %xmm6, %xmm0	# _231, tmp1178, _35
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _35, _mops_211
	call	printf@PLT	#
# poker_bench.c:398:     printf("\n━━━ 3. Suit Extraction (%dM cards) ━━━\n", N_CARDS>>20);
	movl	$16, %esi	#,
	leaq	.LC34(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	movq	.LC2(%rip), %rax	#, tmp1179
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	movq	cards_pool(%rip), %r15	# cards_pool, pretmp_893
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	movq	%rax, -696(%rbp)	# tmp1179, %sfp
	leaq	134217728(%r15), %r12	#, _152
.L168:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1180
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm3	# %sfp, tmp1059
	movq	%r15, %r8	# pretmp_893, ivtmp.482
	vcvtsi2sdq	-664(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1076
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _495, _496
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm3, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1077
	vaddsd	%xmm1, %xmm0, %xmm3	# _493, _496, _497
	vmovq	%xmm3, %r13	# _497, _497
	.p2align 4,,10
	.p2align 3
.L165:
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	movq	%r8, %rdi	# ivtmp.482,
	movl	$1, %esi	#,
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	addq	$8, %r8	#, ivtmp.482
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	call	bench_suit_mask	#
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	movl	%eax, %eax	# _359, _40
	addq	%rax, %rbx	# _40, _chk
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	cmpq	%r8, %r12	# ivtmp.482, _152
	jne	.L165	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1184
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm5	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _501
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	vmovq	%r13, %xmm2	# _497, _497
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1078
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1078, _501, _501
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1186, tmp1079
	vaddsd	%xmm0, %xmm1, %xmm1	# _499, _502, _503
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	vsubsd	%xmm2, %xmm1, %xmm1	# _497, _503, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _230
	vmovsd	%xmm1, -696(%rbp)	# _230, %sfp
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	decl	%r14d	# ivtmp_354
	jne	.L168	#,
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	vmovsd	.LC31(%rip), %xmm2	#, tmp1189
	movq	%rbx, %rdx	# _chk,
	leaq	.LC35(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_367
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:399:     TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
	vdivsd	%xmm1, %xmm2, %xmm0	# _230, tmp1189, _42
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _42, _mops_216
	call	printf@PLT	#
# poker_bench.c:403:     printf("\n━━━ 4. Flush Detection (%dM hands) ━━━\n", N_HANDS>>20);
	movl	$4, %esi	#,
	leaq	.LC36(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1190
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_895
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1190, %sfp
	leaq	167772160(%r15), %r12	#, _157
.L172:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1191
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm6	# %sfp, tmp1059
	movq	%r15, %rcx	# pretmp_895, ivtmp.470
	vcvtsi2sdq	-664(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1080
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _507, _508
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm6, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1081
	vaddsd	%xmm1, %xmm0, %xmm5	# _505, _508, _509
	vmovq	%xmm5, %r13	# _509, _509
	.p2align 4,,10
	.p2align 3
.L169:
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	movq	%rcx, %rdi	# ivtmp.470,
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	addq	$40, %rcx	#, ivtmp.470
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	call	flush_field	#
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	movzbl	%al, %eax	# _349, _47
	addq	%rax, %rbx	# _47, _chk
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	cmpq	%rcx, %r12	# ivtmp.470, _157
	jne	.L169	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1195
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm7	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _513
	vcvtsi2sdq	-664(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1082
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1082, _513, _513
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1083
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	vmovq	%r13, %xmm7	# _509, _509
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vaddsd	%xmm0, %xmm1, %xmm1	# _511, _514, _515
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	vsubsd	%xmm7, %xmm1, %xmm1	# _509, _515, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _227
	vmovsd	%xmm1, -696(%rbp)	# _227, %sfp
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	decl	%r14d	# ivtmp_367
	jne	.L172	#,
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm5	#, tmp1200
	movq	%rbx, %rdx	# _chk,
	leaq	.LC38(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_381
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:404:     TIME_IT("flush_field  (5× suit compare)", N_HANDS,
	vdivsd	%xmm1, %xmm5, %xmm0	# _227, tmp1200, _49
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _49, _mops_220
	call	printf@PLT	#
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1201
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_897
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1201, %sfp
	leaq	167772160(%r15), %r12	#, _162
.L176:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	movl	$1, %edi	#,
	leaq	-672(%rbp), %rsi	#, tmp1202
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm4	# %sfp, tmp1059
	movq	%r15, %rdi	# pretmp_897, ivtmp.458
	vcvtsi2sdq	-664(%rbp), %xmm4, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1084
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _519, _520
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm4, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1085
	vaddsd	%xmm1, %xmm0, %xmm6	# _517, _520, _521
	vmovq	%xmm6, %r13	# _521, _521
	.p2align 4,,10
	.p2align 3
.L173:
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	call	flush_word	#
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	addq	$40, %rdi	#, ivtmp.458
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	movzbl	%al, %eax	# _339, _54
	addq	%rax, %rbx	# _54, _chk
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	cmpq	%rdi, %r12	# ivtmp.458, _162
	jne	.L173	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1206
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm3	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _525
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	vmovq	%r13, %xmm4	# _521, _521
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1086
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1086, _525, _525
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1087
	vaddsd	%xmm0, %xmm1, %xmm1	# _523, _526, _527
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	vsubsd	%xmm4, %xmm1, %xmm1	# _521, _527, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _223
	vmovsd	%xmm1, -696(%rbp)	# _223, %sfp
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	decl	%r14d	# ivtmp_381
	jne	.L176	#,
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm6	#, tmp1211
	movq	%rbx, %rdx	# _chk,
	leaq	.LC39(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_391
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:406:     TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
	vdivsd	%xmm1, %xmm6, %xmm0	# _223, tmp1211, _56
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _56, _mops_224
	call	printf@PLT	#
# poker_bench.c:410:     printf("\n━━━ 5. Straight Detection (%dM hands) ━━━\n", N_HANDS>>20);
	movl	$4, %esi	#,
	leaq	.LC40(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1212
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_899
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1212, %sfp
	leaq	167772160(%r15), %r12	#, _252
.L180:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1213
	movl	$1, %edi	#,
	movq	%r15, %r13	# pretmp_899, ivtmp.446
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm2	# %sfp, tmp1059
	vcvtsi2sdq	-664(%rbp), %xmm2, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1088
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _531, _532
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm2, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1089
	vaddsd	%xmm1, %xmm0, %xmm2	# _529, _532, _533
	vmovsd	%xmm2, -704(%rbp)	# _533, %sfp
	.p2align 4,,10
	.p2align 3
.L177:
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	movq	%r13, %rdi	# ivtmp.446,
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	addq	$40, %r13	#, ivtmp.446
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	call	straight_sort	#
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	movzbl	%al, %eax	# _330, _61
	addq	%rax, %rbx	# _61, _chk
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	cmpq	%r13, %r12	# ivtmp.446, _252
	jne	.L177	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1217
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm5	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _537
	vcvtsi2sdq	-664(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1090
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1090, _537, _537
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1091
	vaddsd	%xmm0, %xmm1, %xmm1	# _535, _538, _539
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	vsubsd	-704(%rbp), %xmm1, %xmm1	# %sfp, _539, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _222
	vmovsd	%xmm1, -696(%rbp)	# _222, %sfp
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	decl	%r14d	# ivtmp_391
	jne	.L180	#,
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm7	#, tmp1222
	movq	%rbx, %rdx	# _chk,
	leaq	.LC41(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_425
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:411:     TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
	vdivsd	%xmm1, %xmm7, %xmm0	# _222, tmp1222, _63
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _63, _mops_228
	call	printf@PLT	#
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1223
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_901
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1223, %sfp
	leaq	167772160(%r15), %r12	#, _257
.L184:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1224
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm6	# %sfp, tmp1059
	movq	%r15, %r8	# pretmp_901, ivtmp.434
	vcvtsi2sdq	-664(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1092
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _543, _544
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm6, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1226, tmp1093
	vaddsd	%xmm1, %xmm0, %xmm7	# _541, _544, _545
	vmovq	%xmm7, %r13	# _545, _545
	.p2align 4,,10
	.p2align 3
.L181:
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	movq	%r8, %rdi	# ivtmp.434,
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	addq	$40, %r8	#, ivtmp.434
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	call	straight_bitmask	#
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	movzbl	%al, %eax	# _320, _68
	addq	%rax, %rbx	# _68, _chk
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	cmpq	%r8, %r12	# ivtmp.434, _257
	jne	.L181	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1228
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm7	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _549
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	vmovq	%r13, %xmm5	# _545, _545
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1094
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1094, _549, _549
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1095
	vaddsd	%xmm0, %xmm1, %xmm1	# _547, _550, _551
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	vsubsd	%xmm5, %xmm1, %xmm1	# _545, _551, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _219
	vmovsd	%xmm1, -696(%rbp)	# _219, %sfp
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	decl	%r14d	# ivtmp_425
	jne	.L184	#,
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm2	#, tmp1233
	movq	%rbx, %rdx	# _chk,
	leaq	.LC42(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_603
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:413:     TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
	vdivsd	%xmm1, %xmm2, %xmm0	# _219, tmp1233, _70
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _70, _mops_232
	call	printf@PLT	#
# poker_bench.c:417:     printf("\n━━━ 6. Hand Sorting 5 cards (%dM hands) ━━━\n", N_HANDS>>20);
	movl	$4, %esi	#,
	leaq	.LC43(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1234
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_903
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1234, %sfp
	leaq	167772160(%r15), %r12	#, _858
.L188:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1235
	movl	$1, %edi	#,
	movq	%r15, %r13	# pretmp_903, ivtmp.422
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm4	# %sfp, tmp1059
	vcvtsi2sdq	-664(%rbp), %xmm4, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1096
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _555, _556
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm4, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1097
	vaddsd	%xmm1, %xmm0, %xmm4	# _553, _556, _557
	vmovsd	%xmm4, -704(%rbp)	# _557, %sfp
	.p2align 4,,10
	.p2align 3
.L185:
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	movq	32(%r13), %rax	# MEM <unsigned char[40]> [(char * {ref-all})_299], MEM <unsigned char[40]> [(char * {ref-all})_299]
	vmovdqu	0(%r13), %ymm0	# MEM <unsigned char[40]> [(char * {ref-all})_299], MEM <unsigned char[40]> [(char * {ref-all})_299]
	leaq	-656(%rbp), %rdi	#, tmp1239
	movq	%rax, -624(%rbp)	# MEM <unsigned char[40]> [(char * {ref-all})_299], MEM <unsigned char[40]> [(char * {ref-all})&scratch]
	vmovdqa	%ymm0, -656(%rbp)	# MEM <unsigned char[40]> [(char * {ref-all})_299], MEM <unsigned char[40]> [(char * {ref-all})&scratch]
	vzeroupper
	call	sort_hand_insertion	#
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	-652(%rbp), %eax	# MEM <uint8_t> [(void *)&scratch + 4B], _329
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	addq	$40, %r13	#, ivtmp.422
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %al	#, _329
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	andl	$15, %eax	#, _76
	addq	%rax, %rbx	# _76, _chk
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	cmpq	%r12, %r13	# _858, ivtmp.422
	jne	.L185	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1241
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm3	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _561
	vcvtsi2sdq	-664(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1098
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1098, _561, _561
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1243, tmp1099
	vaddsd	%xmm0, %xmm1, %xmm1	# _559, _562, _563
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	vsubsd	-704(%rbp), %xmm1, %xmm1	# %sfp, _563, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _215
	vmovsd	%xmm1, -696(%rbp)	# _215, %sfp
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	decl	%r14d	# ivtmp_603
	jne	.L188	#,
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm4	#, tmp1246
	movq	%rbx, %rdx	# _chk,
	leaq	.LC44(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_614
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:420:     TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
	vdivsd	%xmm1, %xmm4, %xmm0	# _215, tmp1246, _78
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _78, _mops_236
	call	printf@PLT	#
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1247
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_905
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1247, %sfp
	leaq	167772160(%r15), %r12	#, _394
.L192:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1248
	movl	$1, %edi	#,
	movq	%r15, %r13	# pretmp_905, ivtmp.408
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm2	# %sfp, tmp1059
	vcvtsi2sdq	-664(%rbp), %xmm2, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1100
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _567, _568
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm2, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1101
	vaddsd	%xmm1, %xmm0, %xmm3	# _565, _568, _569
	vmovsd	%xmm3, -704(%rbp)	# _569, %sfp
	.p2align 4,,10
	.p2align 3
.L189:
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	movq	32(%r13), %rdx	# MEM <unsigned char[40]> [(char * {ref-all})_386], MEM <unsigned char[40]> [(char * {ref-all})_386]
	vmovdqu	0(%r13), %ymm0	# MEM <unsigned char[40]> [(char * {ref-all})_386], MEM <unsigned char[40]> [(char * {ref-all})_386]
	leaq	-656(%rbp), %rdi	#, tmp1252
	movq	%rdx, -624(%rbp)	# MEM <unsigned char[40]> [(char * {ref-all})_386], MEM <unsigned char[40]> [(char * {ref-all})&scratch]
	movzbl	4(%r13), %edx	# MEM[(char * {ref-all})_386 + 4B], MEM[(char * {ref-all})_386 + 4B]
	vmovdqa	%ymm0, -656(%rbp)	# MEM <unsigned char[40]> [(char * {ref-all})_386], MEM <unsigned char[40]> [(char * {ref-all})&scratch]
	movb	%dl, -652(%rbp)	# MEM[(char * {ref-all})_386 + 4B], MEM <uint8_t> [(void *)&scratch + 4B]
	vzeroupper
	call	sort_hand_network	#
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	movzbl	-652(%rbp), %edx	# MEM <uint8_t> [(void *)&scratch + 4B], _348
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	addq	$40, %r13	#, ivtmp.408
# poker_bench.c:64: static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }
	shrb	$2, %dl	#, _348
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	andl	$15, %edx	#, _84
	addq	%rdx, %rbx	# _84, _chk
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	cmpq	%r12, %r13	# _394, ivtmp.408
	jne	.L189	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1254
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm5	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _573
	vcvtsi2sdq	-664(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1102
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1102, _573, _573
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm5, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1256, tmp1103
	vaddsd	%xmm0, %xmm1, %xmm1	# _571, _574, _575
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	vsubsd	-704(%rbp), %xmm1, %xmm1	# %sfp, _575, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _214
	vmovsd	%xmm1, -696(%rbp)	# _214, %sfp
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	decl	%r14d	# ivtmp_614
	jne	.L192	#,
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm5	#, tmp1259
	movq	%rbx, %rdx	# _chk,
	leaq	.LC45(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	movl	$5, %r14d	#, ivtmp_618
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	xorl	%ebx, %ebx	# _chk
# poker_bench.c:424:     TIME_IT("network_sort    (9 comparators)", N_HANDS,
	vdivsd	%xmm1, %xmm5, %xmm0	# _214, tmp1259, _86
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _86, _mops_240
	call	printf@PLT	#
# poker_bench.c:430:     printf("\n━━━ 7. Full Hand Evaluation (%dM hands) ━━━\n", N_HANDS>>20);
	movl	$4, %esi	#,
	leaq	.LC46(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	movq	.LC2(%rip), %rax	#, tmp1260
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	movq	hands_pool(%rip), %r15	# hands_pool, pretmp_907
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	movq	%rax, -696(%rbp)	# tmp1260, %sfp
	leaq	167772160(%r15), %r12	#, _366
.L196:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1261
	movl	$1, %edi	#,
	movq	%r15, %r13	# pretmp_907, ivtmp.396
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm6	# %sfp, tmp1059
	vcvtsi2sdq	-664(%rbp), %xmm6, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1104
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _579, _580
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm6, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1105
	vaddsd	%xmm1, %xmm0, %xmm5	# _577, _580, _581
	vmovsd	%xmm5, -704(%rbp)	# _581, %sfp
	.p2align 4,,10
	.p2align 3
.L193:
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	movq	%r13, %rdi	# ivtmp.396,
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	addq	$40, %r13	#, ivtmp.396
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	call	eval_hand	#
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	movl	%eax, %eax	# _91, _92
	addq	%rax, %rbx	# _92, _chk
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	cmpq	%r13, %r12	# ivtmp.396, _366
	jne	.L193	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1265
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm7	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _585
	vcvtsi2sdq	-664(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1106
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1106, _585, _585
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm7, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1107
	vaddsd	%xmm0, %xmm1, %xmm1	# _583, _586, _587
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	vsubsd	-704(%rbp), %xmm1, %xmm1	# %sfp, _587, _dt
	vminsd	-696(%rbp), %xmm1, %xmm1	# %sfp, _dt, _210
	vmovsd	%xmm1, -696(%rbp)	# _210, %sfp
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	decl	%r14d	# ivtmp_618
	jne	.L196	#,
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	vmovsd	.LC37(%rip), %xmm6	#, tmp1270
	movq	%rbx, %rdx	# _chk,
	leaq	.LC47(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	xorl	%r12d, %r12d	# _chk
# poker_bench.c:438:     build_52(sdeck);
	movl	$5, %r15d	#, ivtmp_630
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	movabsq	$0x41cdcd6500000000, %r14	#, _best
# poker_bench.c:431:     TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
	vdivsd	%xmm1, %xmm6, %xmm0	# _210, tmp1270, _94
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _94, _mops_243
	call	printf@PLT	#
# poker_bench.c:436:     printf("\n━━━ 8. Deck Shuffle (%dk shuffles of 52 cards) ━━━\n", N_SHUFFLES/1000);
	movl	$500, %esi	#,
	leaq	.LC48(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:438:     build_52(sdeck);
	leaq	-480(%rbp), %rdi	#, tmp1271
	call	build_52	#
	vzeroupper
.L200:
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1272
	movl	$1, %edi	#,
	movl	$500000, %ebx	#, ivtmp_636
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm4	# %sfp, tmp1059
	vcvtsi2sdq	-664(%rbp), %xmm4, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1108
	vmulsd	.LC24(%rip), %xmm0, %xmm0	#, _591, _592
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm4, %xmm1	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1109
	vaddsd	%xmm1, %xmm0, %xmm6	# _589, _592, _593
	vmovq	%xmm6, %r13	# _593, _593
	.p2align 4,,10
	.p2align 3
.L197:
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	movl	$52, %esi	#,
	leaq	-480(%rbp), %rdi	#, tmp1276
	call	shuffle_deck	#
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	movzbl	-476(%rbp), %eax	# MEM[(struct Card[52] *)_131][0].info, _96
	addq	%rax, %r12	# _96, _chk
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	decl	%ebx	# ivtmp_636
	jne	.L197	#,
# poker_bench.c:69:     clock_gettime(CLOCK_MONOTONIC, &ts);
	leaq	-672(%rbp), %rsi	#, tmp1277
	movl	$1, %edi	#,
	call	clock_gettime@PLT	#
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vmovaps	-688(%rbp), %xmm3	# %sfp, tmp1059
	vmovsd	.LC24(%rip), %xmm1	#, _597
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	vmovq	%r13, %xmm4	# _593, _593
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-664(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_nsec, tmp1059, tmp1110
	vmulsd	%xmm0, %xmm1, %xmm1	# tmp1110, _597, _597
# poker_bench.c:70:     return ts.tv_sec + ts.tv_nsec * 1e-9;
	vcvtsi2sdq	-672(%rbp), %xmm3, %xmm0	# MEM[(struct timespec *)_137].tv_sec, tmp1059, tmp1111
	vmovq	%r14, %xmm3	# _best, _best
	vaddsd	%xmm0, %xmm1, %xmm1	# _595, _598, _599
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	vsubsd	%xmm4, %xmm1, %xmm1	# _593, _599, _dt
	vminsd	%xmm3, %xmm1, %xmm1	# _best, _dt, _16
	vmovq	%xmm1, %r14	# _16, _best
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	decl	%r15d	# ivtmp_630
	jne	.L200	#,
# poker_bench.c:439:     TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
	vmovsd	.LC49(%rip), %xmm0	#, tmp911
	movq	%r12, %rdx	# _chk,
	leaq	.LC50(%rip), %rsi	#,
	leaq	.LC28(%rip), %rdi	#,
	movl	$2, %eax	#,
	vdivsd	%xmm1, %xmm0, %xmm0	# _16, tmp911, _98
	vdivsd	.LC26(%rip), %xmm0, %xmm0	#, _98, _mops_248
	call	printf@PLT	#
# poker_bench.c:444:     printf("\n━━━ Hand distribution (from %dM dealt hands) ━━━\n", N_HANDS>>20);
	movl	$4, %esi	#,
	leaq	.LC51(%rip), %rdi	#,
	xorl	%eax, %eax	#
	call	printf@PLT	#
# poker_bench.c:445:     const char *hand_names[] = {
	vmovq	.LC61(%rip), %xmm6	#, tmp1284
	leaq	.LC4(%rip), %rax	#, tmp518
	vmovdqa	-752(%rbp), %ymm7	# %sfp, _170
	vmovdqa	-784(%rbp), %ymm3	# %sfp, _182
	movq	hands_pool(%rip), %rbx	# hands_pool, ivtmp.379
# poker_bench.c:450:     uint32_t counts[10] = {0};
	movq	$0, -576(%rbp)	#, counts
# poker_bench.c:445:     const char *hand_names[] = {
	vpinsrq	$1, %rax, %xmm6, %xmm0	# tmp518, tmp1284, _165
	vmovdqa	%ymm7, -560(%rbp)	# _170, MEM <vector(4) long unsigned int> [(const char * *)&hand_names]
	vmovdqa	%xmm0, -496(%rbp)	# _165, MEM <vector(2) long unsigned int> [(const char * *)&hand_names + 64B]
# poker_bench.c:450:     uint32_t counts[10] = {0};
	vpxor	%xmm0, %xmm0, %xmm0	# tmp917
	leaq	167772160(%rbx), %r12	#, _770
# poker_bench.c:445:     const char *hand_names[] = {
	vmovdqa	%ymm3, -528(%rbp)	# _182, MEM <vector(4) long unsigned int> [(const char * *)&hand_names + 32B]
# poker_bench.c:450:     uint32_t counts[10] = {0};
	vmovdqu	%ymm0, -608(%rbp)	# tmp917, counts
	vzeroupper
	.p2align 4,,10
	.p2align 3
.L201:
# poker_bench.c:451:     for (int i = 0; i < N_HANDS; ++i) counts[eval_hand(hands_pool[i])]++;
	movq	%rbx, %rdi	# ivtmp.379,
# poker_bench.c:451:     for (int i = 0; i < N_HANDS; ++i) counts[eval_hand(hands_pool[i])]++;
	addq	$40, %rbx	#, ivtmp.379
# poker_bench.c:451:     for (int i = 0; i < N_HANDS; ++i) counts[eval_hand(hands_pool[i])]++;
	call	eval_hand	#
# poker_bench.c:451:     for (int i = 0; i < N_HANDS; ++i) counts[eval_hand(hands_pool[i])]++;
	movl	%eax, %eax	# _103, _103
	incl	-608(%rbp,%rax,4)	# counts[_103]
# poker_bench.c:451:     for (int i = 0; i < N_HANDS; ++i) counts[eval_hand(hands_pool[i])]++;
	cmpq	%r12, %rbx	# _770, ivtmp.379
	jne	.L201	#,
	leaq	-608(%rbp), %rbx	#, ivtmp.371
	leaq	-560(%rbp), %r13	#, ivtmp.372
	leaq	-568(%rbp), %r12	#, _318
	.p2align 4,,10
	.p2align 3
.L202:
# poker_bench.c:454:                hand_names[i], counts[i], counts[i]*100.0/N_HANDS);
	movl	(%rbx), %edx	# MEM[(unsigned int *)_802], _106
# poker_bench.c:454:                hand_names[i], counts[i], counts[i]*100.0/N_HANDS);
	vmovapd	-688(%rbp), %xmm7	# %sfp, tmp1285
# poker_bench.c:453:         printf("  %-14s  %8u  (%5.2f%%)\n",
	leaq	.LC54(%rip), %rdi	#,
	movl	$1, %eax	#,
	movq	0(%r13), %rsi	# MEM[(const char * *)_307], MEM[(const char * *)_307]
# poker_bench.c:452:     for (int i = 0; i < 10; ++i)
	addq	$4, %rbx	#, ivtmp.371
	addq	$8, %r13	#, ivtmp.372
# poker_bench.c:454:                hand_names[i], counts[i], counts[i]*100.0/N_HANDS);
	vcvtusi2sdl	%edx, %xmm7, %xmm0	# _106, tmp1285, tmp1112
	vmulsd	.LC52(%rip), %xmm0, %xmm0	#, _107, _108
# poker_bench.c:453:         printf("  %-14s  %8u  (%5.2f%%)\n",
	vmulsd	.LC53(%rip), %xmm0, %xmm0	#, _108, _109
	call	printf@PLT	#
# poker_bench.c:452:     for (int i = 0; i < 10; ++i)
	cmpq	%rbx, %r12	# ivtmp.371, _318
	jne	.L202	#,
# poker_bench.c:456:     free(hands_pool);
	movq	hands_pool(%rip), %rdi	# hands_pool,
	call	free@PLT	#
# poker_bench.c:457:     free(cards_pool);
	movq	cards_pool(%rip), %rdi	# cards_pool,
	call	free@PLT	#
# poker_bench.c:459: }
	movq	-56(%rbp), %rax	# D.6797, tmp1114
	subq	%fs:40, %rax	# MEM[(<address-space-1> long unsigned int *)40B], tmp1114
	jne	.L251	#,
	addq	$736, %rsp	#,
	xorl	%eax, %eax	#
	popq	%rbx	#
	popq	%r10	#
	.cfi_remember_state
	.cfi_def_cfa 10, 0
	popq	%r12	#
	popq	%r13	#
	popq	%r14	#
	popq	%r15	#
	popq	%rbp	#
	leaq	-8(%r10), %rsp	#,
	.cfi_def_cfa 7, 8
	ret	
.L251:
	.cfi_restore_state
	call	__stack_chk_fail@PLT	#
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.type	main.cold, @function
main.cold:
.LFSB36:
.L204:
	.cfi_escape 0xf,0x3,0x76,0x58,0x6
	.cfi_escape 0x10,0x3,0x2,0x76,0x50
	.cfi_escape 0x10,0x6,0x2,0x76,0
	.cfi_escape 0x10,0xc,0x2,0x76,0x60
	.cfi_escape 0x10,0xd,0x2,0x76,0x68
	.cfi_escape 0x10,0xe,0x2,0x76,0x70
	.cfi_escape 0x10,0xf,0x2,0x76,0x78
# poker_bench.c:334:     if (!hands_pool || !cards_pool) { perror("alloc"); exit(1); }
	leaq	.LC22(%rip), %rdi	#,
	call	perror@PLT	#
# poker_bench.c:334:     if (!hands_pool || !cards_pool) { perror("alloc"); exit(1); }
	movl	$1, %edi	#,
	call	exit@PLT	#
	.cfi_endproc
.LFE36:
	.section	.text.startup
	.size	main, .-main
	.section	.text.unlikely
	.size	main.cold, .-main.cold
.LCOLDE62:
	.section	.text.startup
.LHOTE62:
	.local	cards_pool
	.comm	cards_pool,8,8
	.local	hands_pool
	.comm	hands_pool,8,8
	.section	.rodata
	.type	SUIT_CHAR, @object
	.size	SUIT_CHAR, 5
SUIT_CHAR:
	.string	"CDHS"
	.section	.rodata.str1.1
.LC63:
	.string	"2"
.LC64:
	.string	"3"
.LC65:
	.string	"4"
.LC66:
	.string	"5"
.LC67:
	.string	"6"
.LC68:
	.string	"7"
.LC69:
	.string	"8"
.LC70:
	.string	"9"
.LC71:
	.string	"T"
.LC72:
	.string	"J"
.LC73:
	.string	"Q"
.LC74:
	.string	"K"
.LC75:
	.string	"A"
	.section	.data.rel.ro.local,"aw"
	.align 32
	.type	RANK_STR, @object
	.size	RANK_STR, 104
RANK_STR:
	.quad	.LC63
	.quad	.LC64
	.quad	.LC65
	.quad	.LC66
	.quad	.LC67
	.quad	.LC68
	.quad	.LC69
	.quad	.LC70
	.quad	.LC71
	.quad	.LC72
	.quad	.LC73
	.quad	.LC74
	.quad	.LC75
	.data
	.align 8
	.type	rng_state, @object
	.size	rng_state, 8
rng_state:
	.quad	-2401053089206453570
	.local	rank_lut
	.comm	rank_lut,256,32
	.section	.rodata.cst32,"aM",@progbits,32
	.align 32
.LC0:
	.long	0
	.long	1
	.long	2
	.long	3
	.long	4
	.long	5
	.long	6
	.long	7
	.section	.rodata.cst8,"aM",@progbits,8
	.align 8
.LC2:
	.long	0
	.long	1104006501
	.section	.rodata.cst32
	.align 32
.LC15:
	.value	0
	.value	2
	.value	4
	.value	6
	.value	8
	.value	10
	.value	12
	.value	14
	.value	16
	.value	18
	.value	20
	.value	22
	.value	24
	.value	26
	.value	28
	.value	30
	.align 32
.LC18:
	.byte	0
	.byte	2
	.byte	4
	.byte	6
	.byte	8
	.byte	10
	.byte	12
	.byte	14
	.byte	16
	.byte	18
	.byte	20
	.byte	22
	.byte	24
	.byte	26
	.byte	28
	.byte	30
	.byte	32
	.byte	34
	.byte	36
	.byte	38
	.byte	40
	.byte	42
	.byte	44
	.byte	46
	.byte	48
	.byte	50
	.byte	52
	.byte	54
	.byte	56
	.byte	58
	.byte	60
	.byte	62
	.align 32
.LC19:
	.byte	0
	.byte	0
	.byte	-128
	.byte	64
	.byte	32
	.byte	16
	.byte	8
	.byte	4
	.byte	0
	.byte	0
	.byte	-128
	.byte	64
	.byte	32
	.byte	16
	.byte	8
	.byte	4
	.byte	0
	.byte	0
	.byte	-128
	.byte	64
	.byte	32
	.byte	16
	.byte	8
	.byte	4
	.byte	0
	.byte	0
	.byte	-128
	.byte	64
	.byte	32
	.byte	16
	.byte	8
	.byte	4
	.section	.rodata.cst8
	.align 8
.LC24:
	.long	-400107883
	.long	1041313291
	.align 8
.LC25:
	.long	-536870912
	.long	1097859071
	.align 8
.LC26:
	.long	0
	.long	1093567616
	.align 8
.LC31:
	.long	0
	.long	1097859072
	.align 8
.LC37:
	.long	0
	.long	1095761920
	.align 8
.LC49:
	.long	0
	.long	1092519040
	.align 8
.LC52:
	.long	0
	.long	1079574528
	.align 8
.LC53:
	.long	0
	.long	1049624576
	.section	.data.rel.ro.local
	.align 8
.LC57:
	.quad	.LC7
	.align 8
.LC58:
	.quad	.LC5
	.align 8
.LC59:
	.quad	.LC11
	.align 8
.LC60:
	.quad	.LC9
	.align 8
.LC61:
	.quad	.LC3
	.ident	"GCC: (GNU) 16.2.1 20260810"
	.section	.note.GNU-stack,"",@progbits

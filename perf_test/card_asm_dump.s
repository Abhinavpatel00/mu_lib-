	.file	"card_asm_dump.c"
	.intel_syntax noprefix
# GNU C99 (Debian 14.2.0-19) version 14.2.0 (x86_64-linux-gnu)
#	compiled by GNU C version 14.2.0, GMP version 6.3.0, MPFR version 4.2.1, MPC version 1.3.1, isl version isl-0.27-GMP

# warning: MPFR header version 4.2.1 differs from library version 4.2.2.
# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed: -march=icelake-server -mmmx -mpopcnt -msse -msse2 -msse3 -mssse3 -msse4.1 -msse4.2 -mavx -mavx2 -mno-sse4a -mno-fma4 -mno-xop -mfma -mavx512f -mbmi -mbmi2 -maes -mpclmul -mavx512vl -mavx512bw -mavx512dq -mavx512cd -mavx512vbmi -mavx512ifma -mavx512vpopcntdq -mavx512vbmi2 -mgfni -mvpclmulqdq -mavx512vnni -mavx512bitalg -mno-avx512bf16 -mno-avx512vp2intersect -mno-3dnow -madx -mabm -mno-cldemote -mclflushopt -mclwb -mno-clzero -mcx16 -mno-enqcmd -mf16c -mfsgsbase -mfxsr -mhle -msahf -mno-lwp -mlzcnt -mmovbe -mno-movdir64b -mno-movdiri -mno-mwaitx -mno-pconfig -mno-pku -mprfchw -mno-ptwrite -mrdpid -mrdrnd -mrdseed -mrtm -mno-serialize -mno-sgx -msha -mno-shstk -mno-tbm -mno-tsxldtrk -mvaes -mno-waitpkg -mno-wbnoinvd -mxsave -mxsavec -mxsaveopt -mxsaves -mno-amx-tile -mno-amx-int8 -mno-amx-bf16 -mno-uintr -mno-hreset -mno-kl -mno-widekl -mno-avxvnni -mno-avx512fp16 -mno-avxifma -mno-avxvnniint8 -mno-avxneconvert -mno-cmpccxadd -mno-amx-fp16 -mno-prefetchi -mno-raoint -mno-amx-complex -mno-avxvnniint16 -mno-sm3 -mno-sha512 -mno-sm4 -mno-apxf -mno-usermsr --param=l1-cache-size=48 --param=l1-cache-line-size=64 --param=l2-cache-size=55296 -mtune=icelake-server -masm=intel -O3 -std=c99 -fasynchronous-unwind-tables
	.text
	.p2align 4
	.globl	eq_64
	.type	eq_64, @function
eq_64:
.LFB0:
	.cfi_startproc
# card_asm_dump.c:24:     return va == vb;
	mov	rax, QWORD PTR [rsi]	# tmp109, MEM <unsigned long> [(char * {ref-all})b_4(D)]
	cmp	QWORD PTR [rdi], rax	# MEM <unsigned long> [(char * {ref-all})a_2(D)], tmp109
	sete	al	#, _6
# card_asm_dump.c:25: }
	ret	
	.cfi_endproc
.LFE0:
	.size	eq_64, .-eq_64
	.p2align 4
	.globl	eq_2x32
	.type	eq_2x32, @function
eq_2x32:
.LFB1:
	.cfi_startproc
# card_asm_dump.c:34:     return (alo == blo) & (ahi == bhi);
	mov	eax, DWORD PTR [rsi]	# tmp117, MEM <unsigned int> [(char * {ref-all})b_8(D)]
	mov	ecx, DWORD PTR 4[rsi]	# tmp118, MEM <unsigned int> [(char * {ref-all})b_8(D) + 4B]
	cmp	DWORD PTR [rdi], eax	# MEM <unsigned int> [(char * {ref-all})a_5(D)], tmp117
	sete	al	#, _1
	cmp	DWORD PTR 4[rdi], ecx	# MEM <unsigned int> [(char * {ref-all})a_5(D) + 4B], tmp118
	sete	dl	#, _2
	and	eax, edx	# _3, _2
# card_asm_dump.c:35: }
	ret	
	.cfi_endproc
.LFE1:
	.size	eq_2x32, .-eq_2x32
	.p2align 4
	.globl	eq_fields
	.type	eq_fields, @function
eq_fields:
.LFB2:
	.cfi_startproc
# card_asm_dump.c:39:     return a->next == b->next && a->info == b->info && a->title[0] == b->title[0] && a->title[1] == b->title[1] &&
	mov	edx, DWORD PTR [rsi]	# tmp121, b_14(D)->next
	xor	eax, eax	# <retval>
	cmp	DWORD PTR [rdi], edx	# a_13(D)->next, tmp121
	je	.L10	#,
.L4:
# card_asm_dump.c:41: }
	ret	
	.p2align 4,,10
	.p2align 3
.L10:
# card_asm_dump.c:39:     return a->next == b->next && a->info == b->info && a->title[0] == b->title[0] && a->title[1] == b->title[1] &&
	movzx	ecx, BYTE PTR 4[rsi]	# tmp122, b_14(D)->info
	cmp	BYTE PTR 4[rdi], cl	# a_13(D)->info, tmp122
	jne	.L4	#,
# card_asm_dump.c:39:     return a->next == b->next && a->info == b->info && a->title[0] == b->title[0] && a->title[1] == b->title[1] &&
	movzx	ecx, BYTE PTR 5[rsi]	# tmp123, b_14(D)->title[0]
	cmp	BYTE PTR 5[rdi], cl	# a_13(D)->title[0], tmp123
	jne	.L4	#,
# card_asm_dump.c:39:     return a->next == b->next && a->info == b->info && a->title[0] == b->title[0] && a->title[1] == b->title[1] &&
	movzx	ecx, BYTE PTR 6[rsi]	# tmp124, b_14(D)->title[1]
	cmp	BYTE PTR 6[rdi], cl	# a_13(D)->title[1], tmp124
	jne	.L4	#,
# card_asm_dump.c:39:     return a->next == b->next && a->info == b->info && a->title[0] == b->title[0] && a->title[1] == b->title[1] &&
	movzx	eax, BYTE PTR 7[rsi]	# tmp125, b_14(D)->title[2]
	cmp	BYTE PTR 7[rdi], al	# a_13(D)->title[2], tmp125
	sete	al	#, <retval>
# card_asm_dump.c:41: }
	ret	
	.cfi_endproc
.LFE2:
	.size	eq_fields, .-eq_fields
	.p2align 4
	.globl	eq_memcmp
	.type	eq_memcmp, @function
eq_memcmp:
.LFB3:
	.cfi_startproc
# card_asm_dump.c:44: bool eq_memcmp(const Card *a, const Card *b) { return memcmp(a, b, sizeof(Card)) == 0; }
	mov	rax, QWORD PTR [rsi]	# tmp109, MEM <unsigned long> [(char * {ref-all})b_4(D)]
	cmp	QWORD PTR [rdi], rax	# MEM <unsigned long> [(char * {ref-all})a_3(D)], tmp109
	sete	al	#, _10
# card_asm_dump.c:44: bool eq_memcmp(const Card *a, const Card *b) { return memcmp(a, b, sizeof(Card)) == 0; }
	ret	
	.cfi_endproc
.LFE3:
	.size	eq_memcmp, .-eq_memcmp
	.ident	"GCC: (Debian 14.2.0-19) 14.2.0"
	.section	.note.GNU-stack,"",@progbits

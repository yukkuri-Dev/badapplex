#ifndef _SETJMP_H
#define _SETJMP_H

// SH4(-m4-nofpu)向けの最小 setjmp/longjmp。
// 実体をヘッダー内の__asm__で直接定義しているため、複数の翻訳単位から
// includeすると多重定義でリンクエラーになる。1つの.cファイルからのみ
// includeすること(現状はLuaのldo.cのみが使用)。
// FPUを使わないビルド前提のため、退避対象は整数レジスタのみ:
// r8,r9,r10,r11,r12,r13,r14,r15(sp),pr の9ワード。
typedef struct {
	unsigned long regs[9];
} jmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

__asm__ (
	".text\n"
	".align 2\n"
	".globl _setjmp\n"
	".type _setjmp, @function\n"
	"_setjmp:\n"
	"	mov.l r8, @(0,r4)\n"
	"	mov.l r9, @(4,r4)\n"
	"	mov.l r10, @(8,r4)\n"
	"	mov.l r11, @(12,r4)\n"
	"	mov.l r12, @(16,r4)\n"
	"	mov.l r13, @(20,r4)\n"
	"	mov.l r14, @(24,r4)\n"
	"	mov.l r15, @(28,r4)\n"
	"	sts pr, r0\n"
	"	mov.l r0, @(32,r4)\n"
	"	mov #0, r0\n"
	"	rts\n"
	"	nop\n"
	".size _setjmp, .-_setjmp\n"
);

__asm__ (
	".text\n"
	".align 2\n"
	".globl _longjmp\n"
	".type _longjmp, @function\n"
	"_longjmp:\n"
	"	mov.l @(0,r4), r8\n"
	"	mov.l @(4,r4), r9\n"
	"	mov.l @(8,r4), r10\n"
	"	mov.l @(12,r4), r11\n"
	"	mov.l @(16,r4), r12\n"
	"	mov.l @(20,r4), r13\n"
	"	mov.l @(24,r4), r14\n"
	"	mov.l @(28,r4), r15\n"
	"	mov.l @(32,r4), r0\n"
	"	lds r0, pr\n"
	"	mov r5, r0\n"
	"	tst r0, r0\n"
	"	bf 1f\n"
	"	mov #1, r0\n"
	"1:\n"
	"	rts\n"
	"	nop\n"
	".size _longjmp, .-_longjmp\n"
);

#endif /* _SETJMP_H */

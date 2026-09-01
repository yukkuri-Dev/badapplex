#include "ram_exec.h"

extern char _sramtext[];
extern char _eramtext[];
extern char _sramtext_load[];

__attribute__((noinline, section(".ram_text")))
static int add_one(int x)
{
	return x + 1;
}

__attribute__((noinline, section(".ram_text")))
static int loop_sum(int n)
{
	int i, s = 0;
	for (i = 0; i < n; i++)
		s = add_one(s);
	return s;
}

void ram_exec_init(void)
{
	unsigned long size = (unsigned long)(_eramtext - _sramtext);
	unsigned char *src = (unsigned char *)_sramtext_load;
	unsigned char *dst = (unsigned char *)_sramtext;

	for (unsigned long i = 0; i < size; i++)
		dst[i] = src[i];

	unsigned char *p = dst;
	unsigned char *end = dst + size;
	for (; p < end; p += 32)
		__asm__ __volatile__ ("ocbwb @%0" : : "r"(p) : "memory");
	for (p = dst; p < end; p += 32)
		__asm__ __volatile__ ("icbi @%0" : : "r"(p) : "memory");
}

int ram_exec_test(int n)
{
	// loop_sumはリンカにより実行アドレス(0x8c800000台)で解決されている。
	// ram_exec_init()でROM->RAMコピー済みであれば、通常のC呼び出しで
	// RAM上のコード(loop_sum内からadd_oneへの内部呼び出し含む)が動くはず。
	return loop_sum(n);
}

#include "overclock.h"

#include <stdint.h>
#include <sh4a/cpg.h>

// gnuboy-ex (src/sys/exword.c) の cpg_init/cpg_fini を移植したもの。
// FRQCR (クロック生成部) を直接叩くのでレジスタアドレスだけそのまま使い、
// gnuboy-ex 本体(rc/sys層)には依存しない。

#define FRQCR (*(volatile unsigned long *)(0xa4150000))

static unsigned long saved_frqcr = 0;
static int enabled = 0;

void overclock_enable(void)
{
	if (enabled)
		return;

	saved_frqcr = FRQCR;
	set_pll_mult(0b011011);
	set_bclk_div(CLK_DIV_2);
	set_bclk_div(CLK_DIV_4);
	set_shclk_div(CLK_DIV_4);
	frqcr_kick();
	enabled = 1;
}

void overclock_disable(void)
{
	if (!enabled)
		return;

	FRQCR = saved_frqcr;
	frqcr_kick();
	enabled = 0;
}

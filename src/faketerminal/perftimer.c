#include "perftimer.h"

#include <sh4a/tmu.h>

// src/libdataplus/src/cpu/sh4a/tmu.s の tmu0_start 実装から逆算した
// レジスタ配置: TSTR=0xa4490004, (TCOR/TCNT/TCR base)=0xa4490008,
// チャネルごとに 0xC(12) バイト刻み。TCNT はそのチャネルの先頭+4。
#define TMU_TSTR      (*(volatile uint8_t  *)0xa4490004)
#define TMU_TCNT(ch)  (*(volatile uint32_t *)(0xa4490008 + (ch) * 12 + 4))

#define PERF_CHANNEL TIMER1

static int g_inited;

void perftimer_init(void)
{
	if (g_inited)
		return;

	// TCOR(周期)=TCNTの初期値としてtmu0_startに渡す。0xFFFFFFFFにして
	// できるだけ長い自由走行のダウンカウンタとして使う。
	tmu0_stop(PERF_CHANNEL);
	tmu0_start(PERF_CHANNEL, TIMER_CLK_4, 0xFFFFFFFFu);
	g_inited = 1;
}

uint32_t perftimer_ticks(void)
{
	// TCNTはダウンカウンタなので、単調増加な値にするため反転する。
	// perftimer_init() 未呼び出しなら常に0を返す(呼び忘れをゼロ差分で
	// 気づけるようにする)。
	if (!g_inited)
		return 0;
	return ~TMU_TCNT(PERF_CHANNEL);
}

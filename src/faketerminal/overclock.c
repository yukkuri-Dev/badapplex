#include "overclock.h"

#include <stdint.h>
#include <sh4a/cpg.h>

// gnuboy-ex (src/sys/exword.c) の cpg_init/cpg_fini を移植したもの。
// FRQCR (クロック生成部) を直接叩くのでレジスタアドレスだけそのまま使い、
// gnuboy-ex 本体(rc/sys層)には依存しない。

#define FRQCR (*(volatile unsigned long *)(0xa4150000))

// SH7724データシート FRQCRA §17.5.1 によれば STC[5:0] (PLL倍率設定) で
// 許可される値は7パターンのみで、他は "Other settings are prohibited"。
// 逓倍率の計算式 Multiplier=(STC+1)*2 に当てはめると 0b011011 (STC=27)
// は x56 になり、データシート上の最大保証値 x48 (PLL_48x, STC=23) を
// 超えた未定義設定 -- gnuboy-ex由来の元コードがそのまま使っていた値。
//
// 当初はDMA/backbuf方式で見えていた画面の水平ズレの原因をこのPLL倍率だと
// 疑い、PLL_48xまで下げて検証した。しかし実際の原因はDMA転送元アドレス
// (SAR3)にmemmgr poolの任意アドレスを指定していたことで、VRAM直後の
// 固定アドレスに変更したところPLL倍率に関わらずズレは解消した。
// つまりこのx56設定自体は無罪と判明している。データシート上は未定義値
// のままなので長時間動作時の安定性・発熱等は未検証だが、実測でx48比
// avg_ticksが大幅に改善し、実用上ここまでのところ問題なく動いている
// ため、この値のまま使う判断をしている。
#define PLL_56x_UNOFFICIAL 0b011011

static unsigned long saved_frqcr = 0;
static int enabled = 0;

void overclock_enable(void)
{
	if (enabled)
		return;

	saved_frqcr = FRQCR;
	set_pll_mult(PLL_56x_UNOFFICIAL);
	set_bclk_div(CLK_DIV_4);
	set_shclk_div(CLK_DIV_4);
	frqcr_kick();

	// PLLが新しい倍率で物理的にロックするまでの猶予。専用のロック検出
	// ビットやdelay APIが無いため、ビジーウェイトで最低限待つ。
	// enable直後すぐにDMAを発火すると初回だけ画面が乱れる症状があり、
	// PLLロック待ち不足が疑われている。
	for (volatile uint32_t i = 0; i < 100000u; ++i)
		;

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

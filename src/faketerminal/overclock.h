#ifndef FAKETERMINAL_OVERCLOCK_H
#define FAKETERMINAL_OVERCLOCK_H

// SH4A の PLL/クロック分周比を変更してCPUを高クロック動作させる。
// gnuboy-ex (src/sys/exword.c) の cpg_init/cpg_fini 相当。
// 動画デコードのような重い処理の前後で overclock_enable/disable を
// 対にして呼ぶ想定 (disable で元のFRQCR値に戻す)。
void overclock_enable(void);
void overclock_disable(void);

#endif /* FAKETERMINAL_OVERCLOCK_H */

#ifndef FAKETERMINAL_BAPX_DMA_H
#define FAKETERMINAL_BAPX_DMA_H

// bapx.c (VRAM直接デコード方式、安定動作中) とは別に、backbuf(通常RAM)へ
// デコードしてDMA一括転送する方式を再検証するための実装。
//
// 経緯: 元々このDMA方式で実装していたが実機でフレームがズレる不具合が
// あり、VRAM直接方式に戻して解決した。その後、overclock_enable() が
// データシート上未定義のPLL倍率(STC=27, x56相当)を設定していたことが
// 判明し、データシート最大保証値(PLL_48x, STC=23, x48)に修正した上で
// PLLロック待ちも追加した。この状態でDMA方式のズレが実際に解消するかを
// 切り分けるために、bapx.c とは独立にもう一度DMA方式を用意する。
//
// path: 例 "\\\\crd0\\badapple.bin" (SDカード) など
// 戻り値: 0 で正常終了(再生完了 or ユーザ中断)、負数でエラー。
int bapx_dma_play_file(const char *path);

// bapx_dma_play_file のエラーコードを人間可読な文字列にする。
const char *bapx_dma_strerror(int err);

#endif /* FAKETERMINAL_BAPX_DMA_H */

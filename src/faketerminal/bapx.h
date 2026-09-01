#ifndef FAKETERMINAL_BAPX_H
#define FAKETERMINAL_BAPX_H

// BAPX (Bad Apple eXword) 動画コンテナの実機プレイヤ。
//
// resources/player.c (Linux/SDL2 版) のデコーダを移植したもの。
// 動画本体は数MBあり ROM (127KB) には収まらないため、ファイルから
// 1フレームずつストリーミングして VRAM に直接デコードする。
//
// path: 例 "\\\\crd0\\badapple.bin" (SDカード) など
// 戻り値: 0 で正常終了(再生完了 or ユーザ中断)、負数でエラー。
int bapx_play_file(const char *path);

// bapx_play_file のエラーコードを人間可読な文字列にする。
const char *bapx_strerror(int err);

// 切り分け用テスト: デコーダを一切使わず、backbuf(通常RAM)に固定の
// 市松模様を直接書いてキャッシュライトバック+DMA転送するだけ。
// これで崩れるなら原因は DMA/backbuf/キャッシュ側、崩れないなら
// デコードロジック側にあると判断できる。frames 回だけ市松の白黒を
// 反転させながら繰り返す(0なら1回だけ)。
void bapx_test_checker(int frames);

// 切り分け用テスト2: bapx_test_checker と同じ市松模様を描くが、
// 毎フレームの前に path から実際に sys_seek/sys_read でデータを
// 読み込む(内容は使い捨てる)。bapx_play_file と同じファイルI/O経路を
// 通しつつ描画内容は固定にすることで、「ファイルI/O自体が悪さをして
// いるのか」「ランレングス展開ロジックが悪さをしているのか」を
// 切り分けられる。
void bapx_test_checker_with_io(const char *path, int frames);

#endif /* FAKETERMINAL_BAPX_H */

#ifndef LIBCT_TERMINAL_H
#define LIBCT_TERMINAL_H

#include <stdint.h>

// 端末に表示できる最大列数・行数(画面サイズとフォント幅から決まる固定上限)
#define CT_TERM_COLS 64
// スクロールバックとして保持する行数(画面に見えている行数より多く持つ)
#define CT_TERM_ROWS 64
// Enterで確定した行を遡れる履歴の件数
#define CT_TERM_HISTORY 16

// Enterで行が確定した時に呼ばれるコールバックの型(確定した行の文字列を受け取る)
typedef void (*ct_terminal_commit_cb)(const char *line);

// 端末バッファを初期化する。画面サイズとフォント寸法から表示行/列数を計算する。
void ct_terminal_init(uint16_t screen_width, uint16_t screen_height);
// Enterで行が確定するたびに呼ばれるコールバックを登録する(NULLで解除)
void ct_terminal_set_commit_callback(ct_terminal_commit_cb cb);
// 画面をクリアし、カーソルを先頭に戻す
void ct_terminal_clear(void);
// 1文字入力する('\n' は改行、'\b' はバックスペースとして扱う)
void ct_terminal_putc(char c);
// ヌル終端文字列を1文字ずつ ct_terminal_putc に流し込む
void ct_terminal_puts(const char *s);
// 表示開始行を相対移動してスクロールする(delta<0で上、delta>0で下)
void ct_terminal_scroll(int16_t delta);
// キー入力を1回分読み取り、文字入力・改行・削除・スクロールに反映する
void ct_terminal_handle_key(int keycode);
// 現在のバッファ内容とカーソルを再描画する
void ct_terminal_draw(void);

#endif /* LIBCT_TERMINAL_H */

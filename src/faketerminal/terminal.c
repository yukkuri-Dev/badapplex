#include "terminal.h"
#include "resource.h"
#include "../libct/input.h"
#include "../libct/print.h"
#include <string.h>
#include <graphics/text.h>
#include <graphics/color.h>
#include <sh4a/input/exword_keys.h>
#include <sh4a/input/keypad.h>

static char lines[CT_TERM_ROWS][CT_TERM_COLS + 1];
static uint16_t line_len[CT_TERM_ROWS];
static int16_t cursor_row;
static int16_t cursor_col;
static int16_t view_top;
static int16_t visible_rows;

// Enterで確定した入力行の履歴(古い順、新しい行は末尾に積む)
static char history[CT_TERM_HISTORY][CT_TERM_COLS + 1];
static int16_t history_count;
// KEY_HISTORY で遡っている位置。history_count なら「履歴を辿っていない」状態
static int16_t history_cursor;
// 履歴を辿り始めた時点での編集中(未確定)行の退避先
static char pending_line[CT_TERM_COLS + 1];

static uint16_t term_x;
static uint16_t term_y;
static uint16_t term_fg;
static uint16_t term_bg;

static uint8_t glyph_w;
static uint8_t glyph_h;

static ct_terminal_commit_cb commit_cb;

void ct_terminal_set_commit_callback(ct_terminal_commit_cb cb)
{
	commit_cb = cb;
}

static void new_line(void)
{
	if (cursor_row + 1 < CT_TERM_ROWS) {
		++cursor_row;
	} else {
		// リングバッファとして先頭行を捨てて全行を1つ上にずらす
		for (int16_t i = 1; i < CT_TERM_ROWS; ++i) {
			memcpy(lines[i - 1], lines[i], CT_TERM_COLS + 1);
			line_len[i - 1] = line_len[i];
		}
	}
	cursor_col = 0;
	lines[cursor_row][0] = '\0';
	line_len[cursor_row] = 0;

	// カーソルが画面外に出たら追従スクロールする
	if (cursor_row - view_top >= visible_rows)
		view_top = cursor_row - visible_rows + 1;
}

void ct_terminal_init(uint16_t screen_width, uint16_t screen_height)
{
	struct font *fnt = get_font();
	glyph_w = fnt->width;
	glyph_h = fnt->height;

	term_x = 0;
	term_y = 0;
	term_fg = create_rgb16(255, 255, 255);
	term_bg = create_rgb16(0, 0, 0);

	visible_rows = (int16_t)(screen_height / glyph_h);
	if (visible_rows > CT_TERM_ROWS)
		visible_rows = CT_TERM_ROWS;

	ct_terminal_clear();
}

void ct_terminal_clear(void)
{
	for (int16_t i = 0; i < CT_TERM_ROWS; ++i) {
		lines[i][0] = '\0';
		line_len[i] = 0;
	}
	cursor_row = 0;
	cursor_col = 0;
	view_top = 0;
	history_count = 0;
	history_cursor = 0;
}

// 現在編集中の行(確定前)の内容を dst にコピーする
static void copy_current_line(char *dst)
{
	memcpy(dst, lines[cursor_row], line_len[cursor_row] + 1);
}

// 現在編集中の行を text で丸ごと置き換える(履歴呼び戻し用)
static void replace_current_line(const char *text)
{
	uint16_t len = (uint16_t)strlen(text);
	if (len > CT_TERM_COLS)
		len = CT_TERM_COLS;
	memcpy(lines[cursor_row], text, len);
	lines[cursor_row][len] = '\0';
	line_len[cursor_row] = len;
	cursor_col = len;
}

// Enter確定: 現在行を履歴に積み、コールバックに通知してから改行する
static void commit_line(void)
{
	char committed[CT_TERM_COLS + 1];
	copy_current_line(committed);

	if (line_len[cursor_row] > 0) {
		if (history_count < CT_TERM_HISTORY) {
			memcpy(history[history_count], lines[cursor_row], line_len[cursor_row] + 1);
			++history_count;
		} else {
			// 満杯なら最古を捨てて詰める
			for (int16_t i = 1; i < CT_TERM_HISTORY; ++i)
				memcpy(history[i - 1], history[i], CT_TERM_COLS + 1);
			memcpy(history[CT_TERM_HISTORY - 1], lines[cursor_row], line_len[cursor_row] + 1);
		}
	}
	history_cursor = history_count;
	new_line();

	if (commit_cb != NULL)
		commit_cb(committed);
}

// KEY_HISTORY: 押すたびに1件ずつ過去へ遡り、最古まで行くと止まる
static void recall_history(void)
{
	if (history_count == 0)
		return;

	if (history_cursor == history_count) {
		// 履歴を辿り始める時点の未確定行を退避しておく
		copy_current_line(pending_line);
	}
	if (history_cursor > 0) {
		--history_cursor;
		replace_current_line(history[history_cursor]);
	}
}

// KEY_JUMP: 履歴を辿っている間、押すたびに1件ずつ新しい方向へ進む。
// 最新の履歴より先に進んだら、遡り始める前の未確定行に復元する。
static void advance_history(void)
{
	if (history_cursor >= history_count)
		return;

	++history_cursor;
	if (history_cursor == history_count)
		replace_current_line(pending_line);
	else
		replace_current_line(history[history_cursor]);
}

void ct_terminal_putc(char c)
{
	if (c == '\n') {
		new_line();
		return;
	}
	if (c == '\b') {
		if (cursor_col > 0) {
			--cursor_col;
			lines[cursor_row][cursor_col] = '\0';
			line_len[cursor_row] = cursor_col;
		}
		return;
	}
	if (cursor_col >= CT_TERM_COLS) {
		new_line();
	}
	lines[cursor_row][cursor_col] = c;
	++cursor_col;
	lines[cursor_row][cursor_col] = '\0';
	line_len[cursor_row] = cursor_col;
}

void ct_terminal_puts(const char *s)
{
	while (*s != '\0') {
		ct_terminal_putc(*s);
		++s;
	}
}

void ct_terminal_scroll(int16_t delta)
{
	int16_t max_top = cursor_row - visible_rows + 1;
	if (max_top < 0)
		max_top = 0;

	view_top += delta;
	if (view_top < 0)
		view_top = 0;
	if (view_top > max_top)
		view_top = max_top;
}

void ct_terminal_handle_key(int keycode)
{
	if (keycode < 0)
		return;

	switch (keycode) {
	case KEY_ENTER:
		commit_line();
		return;
	case KEY_BACKSPACE:
		history_cursor = history_count;
		ct_terminal_putc('\b');
		return;
	case KEY_UP:
		ct_terminal_scroll(-1);
		return;
	case KEY_DOWN:
		ct_terminal_scroll(1);
		return;
	case KEY_HISTORY:
		recall_history();
		return;
	case KEY_JUMP:
		advance_history();
		return;
	default:
		break;
	}

	// SHIFT+SYMBOL併用時は数字段(本体印字: QWERTYUIOP = 1234567890)
	if (get_key_state(KEY_SHIFT) && get_key_state(KEY_SYMBOL)) {
		char digit = '\0';
		switch (keycode) {
		case KEY_CHAR_Q: digit = '1'; break;
		case KEY_CHAR_W: digit = '2'; break;
		case KEY_CHAR_E: digit = '3'; break;
		case KEY_CHAR_R: digit = '4'; break;
		case KEY_CHAR_T: digit = '5'; break;
		case KEY_CHAR_Y: digit = '6'; break;
		case KEY_CHAR_U: digit = '7'; break;
		case KEY_CHAR_I: digit = '8'; break;
		case KEY_CHAR_O: digit = '9'; break;
		case KEY_CHAR_P: digit = '0'; break;
		default: break;
		}
		if (digit != '\0') {
			history_cursor = history_count;
			ct_terminal_putc(digit);
			return;
		}
	}

	// SYMBOLキー併用時のみ割り当てる最低限の記号(パス区切り操作に必要な分だけ)
	if (get_key_state(KEY_SYMBOL)) {
		switch (keycode) {
		case KEY_CHAR_M: history_cursor = history_count; ct_terminal_putc('.'); return;
		case KEY_CHAR_N: history_cursor = history_count; ct_terminal_putc(' '); return;
		case KEY_CHAR_B: history_cursor = history_count; ct_terminal_putc('\\'); return;
		default: break;
		}
	}

	char c = keycode_to_text(keycode);
	if (c != '\0') {
		// SHIFT併用時は小文字として扱う
		if (get_key_state(KEY_SHIFT))
			c += ('a' - 'A');
		history_cursor = history_count;
		ct_terminal_putc(c);
	}
}

void ct_terminal_draw(void)
{
	ct_screen_clear(term_bg);

	for (int16_t row = 0; row < visible_rows; ++row) {
		int16_t src = view_top + row;
		if (src < 0 || src > cursor_row)
			break;
		ct_print(term_x, term_y + row * glyph_h, lines[src], term_fg);
	}

	// カーソルが表示範囲内にある時だけ点滅位置を描画する
	int16_t cursor_screen_row = cursor_row - view_top;
	if (cursor_screen_row >= 0 && cursor_screen_row < visible_rows) {
		set_pen(term_fg);
		draw_cursor(term_x + cursor_col * glyph_w, term_y + cursor_screen_row * glyph_h);
	}
}

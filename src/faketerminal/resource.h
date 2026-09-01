#ifndef _FAKETERMINAL_RESOURCE_H
#define _FAKETERMINAL_RESOURCE_H

#include <stdint.h>
#include <graphics/text.h>

// 文字コード表を持たない単体グリフ(アイコン)リソース。
// struct font の文字コード→インデックス変換ロジック(render_text)には乗せない。
extern struct font ui_resource_info;

void draw_cursor(uint16_t x, uint16_t y);

#endif //_FAKETERMINAL_RESOURCE_H

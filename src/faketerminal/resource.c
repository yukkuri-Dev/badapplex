#include "resource.h"

const unsigned char ui_resource_Data[] = {
    //cursor
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
		0xFF, 	/*  [********]  */
};
struct font ui_resource_info = {
	.width = 8,
	.height = 12,
	.widths = 0,
	.data = ui_resource_Data,
};

void draw_cursor(uint16_t x, uint16_t y)
{
	uint16_t scanline = (ui_resource_info.width + 7) / 8;
	draw_glyph(x, y, ui_resource_info.width, ui_resource_info.height, scanline, ui_resource_info.data);
}

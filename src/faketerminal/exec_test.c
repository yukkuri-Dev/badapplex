#include "exec_test.h"
#include <graphics/lcdc.h>
#include <stdint.h>

// clear_vram(unsigned char *vram, unsigned long size) をSH4向けに
// コンパイルして取り出した生の機械語(r4=vram, r5=size, ISO Cの呼び出し規約通り)。
// ソース:
//   void clear_vram(unsigned char *vram, unsigned long size) {
//       unsigned long i;
//       for (i = 0; i < size; i++) vram[i] = 0;
//   }
static const unsigned char clear_vram_code[] __attribute__((aligned(4))) = {
	0x25, 0x58, 0x89, 0x05, 0xe1, 0x00, 0x00, 0x09, 0x24, 0x10, 0x45, 0x10,
	0x8f, 0xfc, 0x74, 0x01, 0x00, 0x0b, 0x00, 0x09
};

// BSS上のバッファにコピーしてから実行する(rodataでなくRAM上での実行を検証するため)
static unsigned char exec_buf[sizeof(clear_vram_code)] __attribute__((aligned(4)));

typedef void (*clear_vram_fn)(unsigned char *vram, unsigned long size);

void exec_test_clear_vram(void)
{
	for (unsigned long i = 0; i < sizeof(clear_vram_code); i++)
		exec_buf[i] = clear_vram_code[i];

	// SH4はストア後、オペランドキャッシュの内容が即座に命令キャッシュへ
	// 反映される保証がない。ライトバック+無効化を明示的に発行する。
	unsigned char *p = exec_buf;
	unsigned char *end = exec_buf + sizeof(exec_buf);
	for (; p < end; p += 32) {
		__asm__ __volatile__ ("ocbwb @%0" : : "r"(p) : "memory");
	}
	for (p = exec_buf; p < end; p += 32) {
		__asm__ __volatile__ ("icbi @%0" : : "r"(p) : "memory");
	}

	uint16_t width, height;
	lcdc_get_dimensions(&width, &height);
	void *vram = lcdc_get_vram_address();

	clear_vram_fn fn = (clear_vram_fn)(void *)exec_buf;
	fn((unsigned char *)vram, (unsigned long)width * height * 2);

	lcdc_copy_vram();
}

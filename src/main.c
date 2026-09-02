#include <graphics/init.h>
#include <graphics/drawing.h>
#include <graphics/color.h>
#include <graphics/text.h>
#include <graphics/lcdc.h>
#include <sh4a/input/keypad.h>
#include <sh4a/input/exword_keys.h>
#include "libct/print.h"
#include "libct/input.h"
#include "faketerminal/resource.h"
#include "faketerminal/terminal.h"
#include "faketerminal/shell.h"
#define SCREEN_WIDTH 528
#define SCREEN_HEIGHT 320
#define VRAM_ADDRESS ((void *)0xac200000)

static void on_line_committed(const char *line)
{
  ct_shell_exec(line);
}

int main(void) {
  memmgr_init();

  // gnuboy-ex (src/sys/video.c vid_preinit) と同じく、LCDCへ実際の
  // ウィンドウ幅/VRAMアドレスを明示する。これを呼ばないとLCDCコントローラは
  // 電源投入時のファームウェア既定値のウィンドウ幅のままになり、
  // DMA側が送る528ピッチのデータと食い違って、行を追うごとに横ズレが
  // 蓄積していく(市松模様の固定パターンでも再現する不具合の原因だった)。
  graphics_init(SCREEN_WIDTH, SCREEN_HEIGHT, VRAM_ADDRESS);

  ct_shell_init();
  ct_terminal_init(SCREEN_WIDTH, SCREEN_HEIGHT);
  ct_terminal_set_commit_callback(on_line_committed);
  ct_terminal_puts("FakeTerminal built with BadApplex \n");
  ct_terminal_puts("Type HELP for available commands\n");
  ct_terminal_puts("Press POWER key to exit.\n\n");
  ct_terminal_draw();
  lcdc_copy_vram();

  while (1) {
    keypad_read();
    if (get_key_state(KEY_POWER)) return -2;

    // SYMBOL/SHIFTとの同時押しを取りこぼさないよう、押されている全キーを走査する
    // (ct_get_current_keycodeは最初の1件しか返さないため、修飾キー併用操作には使えない)
    int redraw = 0;
    for (int col = 0; col <= 8; ++col) {
      for (int row = 1; row <= 8; ++row) {
        uint8_t key_code = (uint8_t)((col + 1) * 10 + row);
        if (get_key_pressed(key_code)) {
          ct_terminal_handle_key(key_code);
          redraw = 1;
        }
      }
    }
    if (redraw) {
      ct_terminal_draw();
      lcdc_copy_vram();
    }
  }
}

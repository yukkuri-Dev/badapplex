/*
 * bapx_sdl_harness - src/faketerminal/bapx.c のロジックを一切改変せず
 * (writeback_range の実機専用キャッシュ命令だけ no-op にして) SDL2 で
 * 動かすための検証用ハーネス。
 *
 * 目的: 実機だけで発生するフレームのズレが、bapx_decode/blit_run/
 * fill_run のロジック起因なのか、実機固有の何か(ハードウェア/最適化/
 * メモリ)起因なのかを、実際のソースをそのまま実行して切り分けること。
 * resources/player.c (別実装の参照デコーダ)とは別物 -- こちらは
 * bapx.c 本体のコピーに実機APIのスタブを足しただけ。
 *
 * 存在しない/意味を持たない操作は no-op にしてある:
 *   - writeback_range: キャッシュ操作命令(ocbwb/synco)は host に無いので
 *     何もしない(host はキャッシュコヒーレンシの問題が無い)
 *   - overclock_enable/disable: そもそも実機版でも呼び出しを外している
 *     ("overclock無効のまま"というユーザー指示に合わせ、呼び出し自体を
 *     含めていない)
 *   - keypad_read/get_key_state/get_key_pressed: SDLのキー入力に置き換え
 *     (POWER/BACK相当をESC/Qにマップして中断できるようにしただけ)
 *
 * ビルド: make -f Makefile.harness (resources/Makefile に追記予定)
 *   もしくは:
 *   cc -O3 -Wall -o bapx_sdl_harness bapx_sdl_harness.c \
 *      $(pkg-config --cflags --libs sdl2)
 * 実行: ./bapx_sdl_harness <file.bin> [scale]
 */
#define _POSIX_C_SOURCE 200809L

#include <SDL2/SDL.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 実機APIのスタブ (sys_*, memmgr_*, lcdc_*, keypad/get_key_*)         */
/* ------------------------------------------------------------------ */

#define FILE_RD 1

static int sys_open(const char *path, int mode)
{
	(void)mode;
	return open(path, O_RDONLY);
}
static int sys_close(int fd) { return close(fd); }
static int sys_seek(int fd, int off, int whence) { (void)whence; return (int)lseek(fd, off, SEEK_SET); }
static int sys_read(int fd, void *buf, int n) { return (int)read(fd, buf, (size_t)n); }

static void *memmgr_alloc(size_t n) { return malloc(n); }
static void  memmgr_free(void *p) { free(p); }
static void  memmgr_init(void) {}

/* 実機は 528x320 固定。lcdc_get_dimensions もそれに合わせて固定値を返す */
#define HOST_SCREEN_W 528
#define HOST_SCREEN_H 320

static uint16_t *g_framebuffer; /* SDLに映すための実VRAM相当 */
static void     *g_vram_addr;   /* lcdc_set/get_vram_address が指す先 */

static void lcdc_get_dimensions(uint16_t *w, uint16_t *h)
{
	*w = HOST_SCREEN_W;
	*h = HOST_SCREEN_H;
}
static void  lcdc_set_vram_address(void *addr) { g_vram_addr = addr; }
static void *lcdc_get_vram_address(void) { return g_vram_addr; }

/* 実機の lcdc_copy_vram は vram(=g_vram_addr) から LCDC へ DMA するだけ。
 * host では「表示用フレームバッファへコピー」で代替する。 */
static void lcdc_copy_vram(void)
{
	memcpy(g_framebuffer, g_vram_addr, (size_t)HOST_SCREEN_W * HOST_SCREEN_H * 2);
}

static int g_key_power, g_key_back;
static void keypad_read(void) { /* SDLイベントは main ループ側でポンプする */ }
static int  get_key_state(int code)   { return code == 10 ? g_key_power : 0; }   /* KEY_POWER=10 */
static int  get_key_pressed(int code) { return code == 75 ? g_key_back  : 0; }   /* KEY_BACK=75 */

/* ------------------------------------------------------------------ */
/* ここから先は src/faketerminal/bapx.c の中身をそのまま持ち込んだもの。 *
 * 実機専用のキャッシュ命令(writeback_range内)だけ no-op にしている。   */
/* ------------------------------------------------------------------ */

/*
 * コンテナ構造 (リトルエンディアン):
 *   0   'B' 'A' 'P' 'X'
 *   4   u16 width          表示領域の幅(画面幅ではない)
 *   6   u16 height
 *   8   u16 fps
 *   10  u16 flags          bit0 BINARY, bit1 VARINT
 *   12  u32 frame_count
 *   16  u32 index[frame_count + 1]
 *   ..  data
 *
 * ランレングス符号:
 *   VARINT off : u16 length
 *   VARINT on  : 0xxxxxxx            長さ 0..127   (1 byte)
 *                1xxxxxxx yyyyyyyy   長さ 0..32767 (2 bytes)
 *
 * 色:
 *   BINARY on  : 暗黙。ランごとに白黒が交互に入れ替わり、最初は黒。
 *   BINARY off : 長さの後ろに u16 rgb565 が続く。
 */

#define BAPX_FLAG_BINARY 1u
#define BAPX_FLAG_VARINT 2u
#define BAPX_FLAG_KNOWN  (BAPX_FLAG_BINARY | BAPX_FLAG_VARINT)

#define BAPX_HEADER_BYTES 16
#define BAPX_FRAME_BUF 8192
#define BAPX_INDEX_WINDOW 512

#define BAPX_ERR_OPEN    -1
#define BAPX_ERR_READ    -2
#define BAPX_ERR_MAGIC   -3
#define BAPX_ERR_HEADER  -4
#define BAPX_ERR_FLAGS   -5
#define BAPX_ERR_SCREEN  -6
#define BAPX_ERR_MEMORY  -7

static const char *bapx_strerror(int err)
{
	switch (err) {
	case 0:               return "ok";
	case BAPX_ERR_OPEN:   return "cannot open file";
	case BAPX_ERR_READ:   return "read error";
	case BAPX_ERR_MAGIC:  return "not a BAPX file";
	case BAPX_ERR_HEADER: return "bad header";
	case BAPX_ERR_FLAGS:  return "unsupported flags";
	case BAPX_ERR_SCREEN: return "picture larger than screen";
	case BAPX_ERR_MEMORY: return "out of memory";
	default:              return "unknown error";
	}
}

/* 実機では ocbwb+synco でキャッシュをメモリへ書き戻していたが、host には
 * その概念が無い(malloc されたメモリはCPUから見て常に一貫している)ので
 * no-op。 */
static void writeback_range(const void *addr, size_t size)
{
	(void)addr;
	(void)size;
}

/* ------------------------------------------------------------------ */
/* 描画                                                                */
/* ------------------------------------------------------------------ */

static uint16_t *fill_run(uint16_t *out, uint16_t col, uint32_t len)
{
	uint32_t pair = (uint32_t)col | ((uint32_t)col << 16);

	if (len && ((uintptr_t)out & 3u)) {
		*out++ = col;
		len--;
	}
	while (len >= 2) {
		memcpy(out, &pair, 4);
		out += 2;
		len -= 2;
	}
	if (len)
		*out++ = col;
	return out;
}

struct blit {
	uint16_t *out;
	uint32_t  row_left;
	uint32_t  gap;
	uint32_t  w;
	uint32_t  left;
};

static void blit_init(struct blit *b, uint16_t *origin, uint32_t w, uint32_t h,
                      uint32_t pitch)
{
	b->out      = origin;
	b->row_left = w;
	b->gap      = pitch - w;
	b->w        = w;
	b->left     = w * h;
}

static void blit_run(struct blit *b, uint16_t col, uint32_t len)
{
	if (len > b->left)
		len = b->left;
	b->left -= len;

	while (len) {
		uint32_t n = (b->row_left < len) ? b->row_left : len;
		b->out = fill_run(b->out, col, n);
		len         -= n;
		b->row_left -= n;
		if (b->row_left == 0 && (len || b->left)) {
			b->out     += b->gap;
			b->row_left = b->w;
		}
	}
}

/* ------------------------------------------------------------------ */
/* フレーム復号                                                        */
/* ------------------------------------------------------------------ */

static int read_len_varint(const uint8_t *p, const uint8_t *end, uint32_t *len)
{
	if (p >= end)
		return 0;
	uint8_t b0 = *p;
	if (!(b0 & 0x80u)) {
		*len = b0;
		return 1;
	}
	if (p + 1 >= end)
		return 0;
	*len = ((uint32_t)(b0 & 0x7Fu) << 8) | p[1];
	return 2;
}

static int read_len_u16(const uint8_t *p, const uint8_t *end, uint32_t *len)
{
	if (p + 2 > end)
		return 0;
	*len = (uint32_t)(p[0] | (p[1] << 8));
	return 2;
}

struct bapx {
	int      fd;
	uint16_t w, h, fps, flags;
	uint32_t frame_count;
	uint32_t data_off;

	uint32_t idx[BAPX_INDEX_WINDOW];
	uint32_t idx_base;
	uint32_t idx_len;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int bapx_load_index(struct bapx *v, uint32_t frame)
{
	if (v->idx_len >= 2 && frame >= v->idx_base &&
	    frame + 1 < v->idx_base + v->idx_len)
		return 0;

	uint32_t base = frame;
	uint32_t want = v->frame_count + 1 - base;
	if (want > BAPX_INDEX_WINDOW)
		want = BAPX_INDEX_WINDOW;

	uint32_t off = BAPX_HEADER_BYTES + base * 4;
	if (sys_seek(v->fd, off, 0) < 0)
		return BAPX_ERR_READ;

	uint8_t *raw = (uint8_t *)v->idx;
	int want_bytes = (int)(want * 4);
	int got = 0;
	while (got < want_bytes) {
		int r = sys_read(v->fd, raw + got, want_bytes - got);
		if (r <= 0)
			break;
		got += r;
	}
	if (got < 8)
		return BAPX_ERR_READ;

	uint32_t n = (uint32_t)got / 4;
	for (uint32_t i = 0; i < n; ++i)
		v->idx[i] = rd32(raw + i * 4);

	v->idx_base = base;
	v->idx_len  = n;
	return 0;
}

static int bapx_open(struct bapx *v, const char *path)
{
	uint8_t hdr[BAPX_HEADER_BYTES];

	v->fd = sys_open(path, FILE_RD);
	if (v->fd < 0)
		return BAPX_ERR_OPEN;

	int got = 0;
	while (got < BAPX_HEADER_BYTES) {
		int r = sys_read(v->fd, hdr + got, BAPX_HEADER_BYTES - got);
		if (r <= 0)
			break;
		got += r;
	}
	if (got < BAPX_HEADER_BYTES) {
		sys_close(v->fd);
		return BAPX_ERR_READ;
	}
	if (hdr[0] != 'B' || hdr[1] != 'A' || hdr[2] != 'P' || hdr[3] != 'X') {
		sys_close(v->fd);
		return BAPX_ERR_MAGIC;
	}

	v->w           = rd16(hdr + 4);
	v->h           = rd16(hdr + 6);
	v->fps         = rd16(hdr + 8);
	v->flags       = rd16(hdr + 10);
	v->frame_count = rd32(hdr + 12);

	if (v->w == 0 || v->h == 0 || v->frame_count == 0) {
		sys_close(v->fd);
		return BAPX_ERR_HEADER;
	}
	if (v->flags & ~BAPX_FLAG_KNOWN) {
		sys_close(v->fd);
		return BAPX_ERR_FLAGS;
	}
	if (v->fps == 0)
		v->fps = 30;

	v->data_off = BAPX_HEADER_BYTES + (v->frame_count + 1) * 4;
	v->idx_base = 0;
	v->idx_len  = 0;
	return 0;
}

static void bapx_decode(const struct bapx *v, const uint8_t *p, const uint8_t *end,
                        uint16_t *origin, uint32_t pitch)
{
	const int varint = (v->flags & BAPX_FLAG_VARINT) != 0;
	const int binary = (v->flags & BAPX_FLAG_BINARY) != 0;

	struct blit b;
	blit_init(&b, origin, v->w, v->h, pitch);

	uint16_t col = 0x0000;

	while (b.left) {
		uint32_t len;
		int used = varint ? read_len_varint(p, end, &len)
		                  : read_len_u16(p, end, &len);
		if (!used)
			break;
		p += used;

		if (!binary) {
			if (p + 2 > end)
				break;
			col = rd16(p);
			p += 2;
		}

		blit_run(&b, col, len);

		if (binary)
			col = (uint16_t)~col;
	}

	if (b.left)
		blit_run(&b, 0x0000, b.left);
}

/* ------------------------------------------------------------------ */
/* 再生 (bapx_play_file をほぼそのまま。overclock呼び出しは実機版が既に  *
 * 無効化されているのでこちらも含めていない)                            */
/* ------------------------------------------------------------------ */

static int bapx_play_file(const char *path, SDL_Renderer *ren, SDL_Texture *tex, int scale)
{
	struct bapx v;
	int err = bapx_open(&v, path);
	if (err != 0)
		return err;

	uint16_t screen_w, screen_h;
	lcdc_get_dimensions(&screen_w, &screen_h);

	if (v.w > screen_w || v.h > screen_h) {
		sys_close(v.fd);
		return BAPX_ERR_SCREEN;
	}

	uint8_t *buf = (uint8_t *)memmgr_alloc(BAPX_FRAME_BUF);
	if (buf == NULL) {
		sys_close(v.fd);
		return BAPX_ERR_MEMORY;
	}

	size_t    fb_bytes = (size_t)screen_w * screen_h * 2;
	uint16_t *backbuf = (uint16_t *)memmgr_alloc(fb_bytes);
	if (backbuf == NULL) {
		memmgr_free(buf);
		sys_close(v.fd);
		return BAPX_ERR_MEMORY;
	}

	void    *saved_vram = lcdc_get_vram_address();
	uint32_t x0 = (uint32_t)(screen_w - v.w) / 2;
	uint32_t y0 = (uint32_t)(screen_h - v.h) / 2;

	memset(backbuf, 0, fb_bytes);
	writeback_range(backbuf, fb_bytes);
	uint16_t *origin = backbuf + (size_t)y0 * screen_w + x0;

	lcdc_set_vram_address(backbuf);

	double t_start = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
	double frame_dt = 1.0 / (v.fps ? v.fps : 30);

	int rc = 0;
	for (uint32_t n = 0; n < v.frame_count; ++n) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT)
				g_key_power = 1;
			else if (e.type == SDL_KEYDOWN &&
			         (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))
				g_key_power = 1;
			else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_BACKSPACE)
				g_key_back = 1;
		}
		keypad_read();
		if (get_key_state(10) || get_key_pressed(75)) { /* KEY_POWER / KEY_BACK */
			g_key_back = 0;
			break;
		}

		err = bapx_load_index(&v, n);
		if (err != 0) {
			rc = err;
			break;
		}

		uint32_t i    = n - v.idx_base;
		uint32_t off  = v.idx[i];
		uint32_t next = v.idx[i + 1];
		uint32_t size = (next > off) ? next - off : 0;
		if (size > BAPX_FRAME_BUF)
			size = BAPX_FRAME_BUF;

		if (sys_seek(v.fd, (int)(v.data_off + off), 0) < 0) {
			rc = BAPX_ERR_READ;
			break;
		}

		uint32_t got = 0;
		while (got < size) {
			int r = sys_read(v.fd, buf + got, (int)(size - got));
			if (r <= 0)
				break;
			got += (uint32_t)r;
		}

		bapx_decode(&v, buf, buf + got, origin, screen_w);
		writeback_range(origin, (size_t)(v.h - 1) * screen_w * 2 + (size_t)v.w * 2);
		lcdc_copy_vram();

		/* SDLへの表示 (実機のLCD相当) */
		SDL_UpdateTexture(tex, NULL, g_framebuffer, screen_w * 2);
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
		(void)scale;

		/* fps相当にペース合わせ (実機は間に合う速度で回すだけなので
		 * 目視比較用に軽く待つ) */
		double now = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
		double target = t_start + frame_dt * (n + 1);
		if (target > now)
			SDL_Delay((Uint32)((target - now) * 1000.0));
	}

	lcdc_set_vram_address(saved_vram);
	memmgr_free(backbuf);
	memmgr_free(buf);
	sys_close(v.fd);
	return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.bin> [scale]\n", argv[0]);
		return 1;
	}
	int scale = (argc > 2) ? atoi(argv[2]) : 2;
	if (scale < 1)
		scale = 1;

	memmgr_init();

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window *win = SDL_CreateWindow("bapx_sdl_harness (bapx.c そのまま実行)",
	                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                    HOST_SCREEN_W * scale, HOST_SCREEN_H * scale,
	                                    SDL_WINDOW_RESIZABLE);
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
	if (win && !ren)
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!win || !ren) {
		fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_RenderSetLogicalSize(ren, HOST_SCREEN_W, HOST_SCREEN_H);

	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
	                                     SDL_TEXTUREACCESS_STREAMING,
	                                     HOST_SCREEN_W, HOST_SCREEN_H);
	g_framebuffer = malloc((size_t)HOST_SCREEN_W * HOST_SCREEN_H * 2);
	if (!tex || !g_framebuffer) {
		fprintf(stderr, "allocation failed\n");
		return 1;
	}
	memset(g_framebuffer, 0, (size_t)HOST_SCREEN_W * HOST_SCREEN_H * 2);

	int rc = bapx_play_file(argv[1], ren, tex, scale);
	if (rc != 0)
		fprintf(stderr, "play: %s\n", bapx_strerror(rc));

	free(g_framebuffer);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return rc ? 1 : 0;
}

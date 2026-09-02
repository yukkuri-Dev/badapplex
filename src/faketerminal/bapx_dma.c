#include "bapx_dma.h"
#include "terminal.h"
#include "overclock.h"
#include "perftimer.h"
#include "../libc/memmgr.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <syscalls/syscalls.h>
#include <graphics/lcdc.h>
#include <sh4a/input/keypad.h>
#include <sh4a/input/exword_keys.h>

/*
 * BAPXコンテナのパース/デコードは bapx.c と同一のロジック
 * (host上のSDL2ハーネスで全フレーム一致検証済み)。再生ループだけを
 * backbuf+DMA方式にしたバージョンをここに用意する。
 *
 * コンテナ構造 (リトルエンディアン):
 *   0   'B' 'A' 'P' 'X'
 *   4   u16 width
 *   6   u16 height
 *   8   u16 fps
 *   10  u16 flags          bit0 BINARY, bit1 VARINT
 *   12  u32 frame_count
 *   16  u32 index[frame_count + 1]
 *   ..  data
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

const char *bapx_dma_strerror(int err)
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

// DMAC はキャッシュを経由せず物理メモリを直接読むため、通常RAM(P1、
// キャッシュ有効)上のバックバッファをCPUで書いた後は、DMA転送を投げる
// 前にオペランドキャッシュを明示的にライトバックする必要がある。
static void writeback_range(const void *addr, size_t size)
{
	uintptr_t p   = (uintptr_t)addr & ~31u;
	uintptr_t end = (uintptr_t)addr + size;
	for (; p < end; p += 32)
		__asm__ __volatile__ ("ocbwb @%0" : : "r"(p) : "memory");
	__asm__ __volatile__ ("synco" ::: "memory");
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

struct bapx_dma {
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

static int bapx_dma_load_index(struct bapx_dma *v, uint32_t frame)
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

static int bapx_dma_open(struct bapx_dma *v, const char *path)
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

static void bapx_dma_decode(const struct bapx_dma *v, const uint8_t *p, const uint8_t *end,
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
/* 再生 (backbuf + DMA一括転送方式)                                     */
/* ------------------------------------------------------------------ */

int bapx_dma_play_file(const char *path)
{
	struct bapx_dma v;
	int err = bapx_dma_open(&v, path);
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

	size_t fb_bytes = (size_t)screen_w * screen_h * 2;

#ifdef BAPX_DMA_FIXED_BACKBUF
	// 切り分け用: SAR3にVRAM(0xac200000)以外の任意アドレスを指定する
	// こと自体がこのハードウェアで想定されていない(LCDC_struct.mdの
	// リファレンス実装はSAR3を常に0xac200000固定で使っている)のでは
	// ないかという仮説を検証するため、memmgr poolではなく VRAM 直後の
	// 外部メモリアドレスに backbuf を固定配置する。同じチップセレクト/
	// バンク内のアドレスならDMAC側の扱いがVRAM本体と同じになるはず、
	// という考え方。memmgr_allocを使わないのでmemmgr_freeもしない。
	uint16_t *backbuf = (uint16_t *)BAPX_DMA_FIXED_BACKBUF;
#else
	// VRAM(0xac200000, P2非キャッシュ領域)へランレングスをそのまま
	// 展開すると1画素ごとの読み書きが遅いため、通常RAM(キャッシュ有効)
	// 上にバックバッファを確保してそこへデコードし、フレーム単位で
	// DMA一括転送する。
	uint16_t *backbuf = (uint16_t *)memmgr_alloc(fb_bytes);
	if (backbuf == NULL) {
		memmgr_free(buf);
		sys_close(v.fd);
		return BAPX_ERR_MEMORY;
	}
#endif

#ifdef BAPX_DMA_USE_P2
	// 切り分け用: memmgr_alloc が返すのはP1(0x8x......、キャッシュ可能)の
	// アドレス。writeback_range(ocbwb+synco)で対処しているつもりでも
	// 実機ではズレが残ったため、そもそもキャッシュコヒーレンシーが原因
	// なのかを確定させる目的で、上位アドレスビットをP2(0xA0000000、
	// キャッシュ不可)に変換したエイリアスを使っていた。ズレの原因は
	// SAR3への任意アドレス指定自体(BAPX_DMA_FIXED_BACKBUFで確認)と
	// 判明したので、P2化は本来もう不要 -- だが速度計測用にどちらでも
	// 切り替えられるよう残している。P2経由だとキャッシュの恩恵が
	// 一切無く、backbuf方式の速度メリットを自ら消してしまうため、
	// 既定では使わない。
	uint16_t *decode_target = (uint16_t *)(((uintptr_t)backbuf & 0x1fffffffu) | 0xa0000000u);
#else
	// P1(キャッシュ有効)のまま使う。backbuf方式が本来持つはずの速度
	// メリットを活かすにはこちらが必要。DMA前に writeback_range で
	// キャッシュをメモリへ書き戻す。
	uint16_t *decode_target = backbuf;
#endif

	void    *saved_vram = lcdc_get_vram_address();
	uint32_t x0 = (uint32_t)(screen_w - v.w) / 2;
	uint32_t y0 = (uint32_t)(screen_h - v.h) / 2;

	// レターボックスはここで一度だけ塗る。以降は表示領域しか触らない。
	memset(decode_target, 0, fb_bytes);
#ifndef BAPX_DMA_USE_P2
	writeback_range(decode_target, fb_bytes);
#endif
	uint16_t *origin = decode_target + (size_t)y0 * screen_w + x0;

	lcdc_set_vram_address(backbuf);

	// 定格クロックでもDMA/backbuf方式単体で同じズレが再現することを
	// 確認済み(overclockは無罪、DMA/backbuf方式そのものにバグがある)。
	// 速度面ではoverclockが必要なので復元する。
	overclock_enable();

	perftimer_init();
	uint32_t decoded_frames = 0;
	uint64_t ticks_total    = 0;
	uint32_t ticks_worst    = 0;

	int rc = 0;
	for (uint32_t n = 0; n < v.frame_count; ++n) {
		keypad_read();
		if (get_key_state(KEY_POWER) || get_key_pressed(KEY_BACK))
			break;

		err = bapx_dma_load_index(&v, n);
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

		uint32_t t0 = perftimer_ticks();
		bapx_dma_decode(&v, buf, buf + got, origin, screen_w);
#ifndef BAPX_DMA_USE_P2
		// DMAはキャッシュを経由しないので、転送前にCPUキャッシュの
		// 内容をメモリへ確定させる(表示領域だけで足りる)
		writeback_range(origin, (size_t)(v.h - 1) * screen_w * 2 + (size_t)v.w * 2);
#endif
		lcdc_copy_vram();
		// 切り分け用(既に無罪と判定済み、既定では無効): lcdc_copy_vram()
		// のCHCR3完了待ちループの後に追加ウェイトを挟んでも実機のズレは
		// 変わらなかった。
#ifdef BAPX_DMA_POST_COPY_DELAY_TICKS
		{
			volatile uint32_t settle = 0;
			while (settle < BAPX_DMA_POST_COPY_DELAY_TICKS)
				++settle;
		}
#endif
		uint32_t dt = perftimer_ticks() - t0;

		++decoded_frames;
		ticks_total += dt;
		if (dt > ticks_worst)
			ticks_worst = dt;
	}

	overclock_disable();
	lcdc_set_vram_address(saved_vram);

	if (decoded_frames > 0) {
		uint32_t avg = (uint32_t)(ticks_total / decoded_frames);
		char line[96];
		sprintf(line, "[dma] frames=%u avg_ticks=%u worst_ticks=%u\n",
		        (unsigned int)decoded_frames, (unsigned int)avg,
		        (unsigned int)ticks_worst);
		ct_terminal_puts(line);
	}

#ifndef BAPX_DMA_FIXED_BACKBUF
	memmgr_free(backbuf);
#endif
	memmgr_free(buf);
	sys_close(v.fd);
	return rc;
}

#include "bapx.h"
#include "terminal.h"
#include "overclock.h"
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

// 1フレームあたりの最大バイト数。badapple.bin の実測最大は約4KBだが、
// 余裕を見て確保する。これを超えるフレームは打ち切られる(黒で埋める)。
#define BAPX_FRAME_BUF 8192

// インデックス表(frame_count+1 エントリ)は数万バイトになるため全体を
// メモリに持たず、必要な範囲だけを読み直す窓を使う。
#define BAPX_INDEX_WINDOW 512

#define BAPX_ERR_OPEN    -1
#define BAPX_ERR_READ    -2
#define BAPX_ERR_MAGIC   -3
#define BAPX_ERR_HEADER  -4
#define BAPX_ERR_FLAGS   -5
#define BAPX_ERR_SCREEN  -6
#define BAPX_ERR_MEMORY  -7

const char *bapx_strerror(int err)
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
//
// ocbp+読み出しまで強化しても実機のズレは解消せず、むしろ悪化・低速化
// したため、キャッシュ同期はこの症状の本質的な原因ではないと判断した。
// 切り分け用の CHECKER コマンド (市松模様を直接書いてDMA転送するだけの
// 最小テスト) の結果が出るまでは、素朴な ocbwb+synco に留めておく。
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

/*
 * 32bit 単位で書く。カーソルが4バイト境界にないときだけ先頭1画素を
 * 単独で書くため、表示幅と左マージンを偶数に保つと毎行の補正が消える。
 * 528幅の画面に428幅の絵ならマージン50で常に整列する。
 */
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

// 画面より狭い表示領域の矩形内を走るカーソル
struct blit {
	uint16_t *out;      // 次に書く画素
	uint32_t  row_left; // 現在の行の残り画素数
	uint32_t  gap;      // pitch - 表示幅 (画素数)
	uint32_t  w;        // 表示幅
	uint32_t  left;     // フレーム全体の残り画素数
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
		/*
		 * 行が埋まり、かつ書くものが残っているときだけ折り返す。
		 * b->left だけで判定すると、この run の分を既に引いた後なので
		 * フレーム最後の run で row_left が 0 のまま無限ループする。
		 */
		if (b->row_left == 0 && (len || b->left)) {
			b->out     += b->gap;
			b->row_left = b->w;
		}
	}
}

/* ------------------------------------------------------------------ */
/* フレーム復号                                                        */
/* ------------------------------------------------------------------ */

// どちらも消費バイト数を返す。トークンが途中で切れていたら 0。
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
	uint32_t data_off;       // データ部のファイル先頭からのオフセット

	// インデックス表の読み込み窓
	uint32_t idx[BAPX_INDEX_WINDOW];
	uint32_t idx_base;       // idx[0] が表す絶対フレーム番号
	uint32_t idx_len;        // idx に入っている有効エントリ数
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// frame 番号を含むインデックス窓を読み込む。frame と frame+1 の両方を
// 得る必要があるため、窓には最低2エントリを確保する。
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

	// idx をバイト列として読み、その場でリトルエンディアン解釈する
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

/*
 * フレーム n を、pitch 幅のフレームバッファ内 origin から始まる矩形へ
 * 復号する。表示領域だけを触るので、周囲のレターボックスは前フレームの
 * まま保たれる。
 */
static void bapx_decode(const struct bapx *v, const uint8_t *p, const uint8_t *end,
                        uint16_t *origin, uint32_t pitch)
{
	const int varint = (v->flags & BAPX_FLAG_VARINT) != 0;
	const int binary = (v->flags & BAPX_FLAG_BINARY) != 0;

	struct blit b;
	blit_init(&b, origin, v->w, v->h, pitch);

	uint16_t col = 0x0000; // binary モードでは最初の run が黒

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

	if (b.left) // 途中で切れたフレームは黒で埋める
		blit_run(&b, 0x0000, b.left);
}

/* ------------------------------------------------------------------ */
/* 再生                                                                */
/* ------------------------------------------------------------------ */

int bapx_play_file(const char *path)
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

	// VRAM(0xac200000)へランレングスをそのまま展開する。backbuf+DMA
	// 一括転送方式を試したが実機でフレームがズレる問題が解消せず、
	// 最初に実装していたVRAM直接デコード方式(こちらは実機で問題なく
	// 動いていた)に戻した。バックバッファ/writeback_range/DMA関連の
	// コードは撤去している。
	uint16_t *vram = (uint16_t *)lcdc_get_vram_address();
	uint32_t  x0 = (uint32_t)(screen_w - v.w) / 2;
	uint32_t  y0 = (uint32_t)(screen_h - v.h) / 2;

	// レターボックスはここで一度だけ塗る。以降は表示領域しか触らない。
	for (uint32_t i = 0; i < (uint32_t)screen_w * screen_h; ++i)
		vram[i] = 0x0000;
	uint16_t *origin = vram + (size_t)y0 * screen_w + x0;

	// デコード+VRAM書き込みを毎フレーム間に合わせるため、再生中だけ
	// CPUを高クロック動作させる(gnuboy-ex cpg_init/cpg_fini相当)。
	overclock_enable();

	int rc = 0;
	for (uint32_t n = 0; n < v.frame_count; ++n) {
		// 電源/戻るキーで中断
		keypad_read();
		if (get_key_state(KEY_POWER) || get_key_pressed(KEY_BACK))
			break;

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
		lcdc_copy_vram();
	}

	overclock_disable();
	memmgr_free(buf);
	sys_close(v.fd);
	return rc;
}

// 切り分け用: bapx_play_file と同じ経路(memmgr_alloc したbackbufに書き、
// writeback_rangeしてlcdc_copy_vram)だけを、デコーダを使わず素通しで
// 検証する。8x8マスの市松模様を画面全体に敷き詰める。
void bapx_test_checker(int frames)
{
	uint16_t screen_w, screen_h;
	lcdc_get_dimensions(&screen_w, &screen_h);

	size_t    fb_bytes = (size_t)screen_w * screen_h * 2;
	uint16_t *backbuf = (uint16_t *)memmgr_alloc(fb_bytes);
	if (backbuf == NULL)
		return;

	void *saved_vram = lcdc_get_vram_address();
	lcdc_set_vram_address(backbuf);

	if (frames <= 0)
		frames = 1;

	for (int f = 0; f < frames; ++f) {
		int phase = f & 1;
		for (uint32_t y = 0; y < screen_h; ++y) {
			uint16_t *row = backbuf + (size_t)y * screen_w;
			for (uint32_t x = 0; x < screen_w; ++x) {
				int cell = ((x >> 3) + (y >> 3) + phase) & 1;
				row[x] = cell ? 0xFFFF : 0x0000;
			}
		}

		writeback_range(backbuf, fb_bytes);
		lcdc_copy_vram();

		keypad_read();
		if (get_key_state(KEY_POWER) || get_key_pressed(KEY_BACK))
			break;

		// 目視で確認できる速度まで落とすためのビジーウェイト
		// (専用のタイマ/delay APIが無いため)。だいたい数百ms程度。
		for (volatile uint32_t busy = 0; busy < 3000000u; ++busy)
			;
	}

	lcdc_set_vram_address(saved_vram);
	memmgr_free(backbuf);
}

// 切り分け用2: bapx_play_file と全く同じファイルI/O経路(bapx_open →
// ループ内で bapx_load_index + sys_seek + sys_read)を通すが、読んだ
// バイト列はbapx_decodeに渡さず捨てて、代わりに固定の市松模様を描く。
void bapx_test_checker_with_io(const char *path, int frames)
{
	struct bapx v;
	if (bapx_open(&v, path) != 0)
		return;

	uint16_t screen_w, screen_h;
	lcdc_get_dimensions(&screen_w, &screen_h);

	uint8_t *buf = (uint8_t *)memmgr_alloc(BAPX_FRAME_BUF);
	if (buf == NULL) {
		sys_close(v.fd);
		return;
	}

	size_t    fb_bytes = (size_t)screen_w * screen_h * 2;
	uint16_t *backbuf = (uint16_t *)memmgr_alloc(fb_bytes);
	if (backbuf == NULL) {
		memmgr_free(buf);
		sys_close(v.fd);
		return;
	}

	void *saved_vram = lcdc_get_vram_address();
	lcdc_set_vram_address(backbuf);

	if (frames <= 0)
		frames = 1;
	if ((uint32_t)frames > v.frame_count)
		frames = (int)v.frame_count;

	for (int f = 0; f < frames; ++f) {
		uint32_t n = (uint32_t)f;

		// play と同じインデックス取得+シーク+読み込み(内容は捨てる)
		if (bapx_load_index(&v, n) != 0)
			break;
		uint32_t i    = n - v.idx_base;
		uint32_t off  = v.idx[i];
		uint32_t next = v.idx[i + 1];
		uint32_t size = (next > off) ? next - off : 0;
		if (size > BAPX_FRAME_BUF)
			size = BAPX_FRAME_BUF;

		if (sys_seek(v.fd, (int)(v.data_off + off), 0) < 0)
			break;
		uint32_t got = 0;
		while (got < size) {
			int r = sys_read(v.fd, buf + got, (int)(size - got));
			if (r <= 0)
				break;
			got += (uint32_t)r;
		}

		// 描画は市松模様固定(読んだ内容は使わない)
		int phase = f & 1;
		for (uint32_t y = 0; y < screen_h; ++y) {
			uint16_t *row = backbuf + (size_t)y * screen_w;
			for (uint32_t x = 0; x < screen_w; ++x) {
				int cell = ((x >> 3) + (y >> 3) + phase) & 1;
				row[x] = cell ? 0xFFFF : 0x0000;
			}
		}

		writeback_range(backbuf, fb_bytes);
		lcdc_copy_vram();

		keypad_read();
		if (get_key_state(KEY_POWER) || get_key_pressed(KEY_BACK))
			break;

		for (volatile uint32_t busy = 0; busy < 3000000u; ++busy)
			;
	}

	lcdc_set_vram_address(saved_vram);
	memmgr_free(backbuf);
	memmgr_free(buf);
	sys_close(v.fd);
}

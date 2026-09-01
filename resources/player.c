/*
 * bapxplay - BAPX video player for Linux / SDL2
 *
 * The container holds only the active picture area; the letterbox is painted
 * once by the player and never touched again.
 *
 * Container layout (little-endian):
 *   0   'B' 'A' 'P' 'X'
 *   4   u16 width            picture width, not screen width
 *   6   u16 height
 *   8   u16 fps
 *   10  u16 flags            bit0 BINARY, bit1 VARINT
 *   12  u32 frame_count
 *   16  u32 index[frame_count + 1]
 *   ..  data
 *
 * Run length encoding:
 *   VARINT off : u16 length
 *   VARINT on  : 0xxxxxxx                 length 0..127        (1 byte)
 *                1xxxxxxx yyyyyyyy        length 0..32767      (2 bytes)
 *
 * Colour:
 *   BINARY on  : implicit, alternates per run, first run is black.  A run
 *                longer than the length field is split into an EVEN number
 *                of tokens so the implied colour survives the split.
 *   BINARY off : u16 rgb565 follows each length.
 *
 * Build: make
 * Usage: ./bapxplay <file.bin> [scale] [-s WxH]
 */
#define _POSIX_C_SOURCE 200809L

#include <SDL2/SDL.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BAPX_FLAG_BINARY 1u
#define BAPX_FLAG_VARINT 2u
#define BAPX_FLAG_KNOWN  (BAPX_FLAG_BINARY | BAPX_FLAG_VARINT)

#define DEFAULT_SCREEN_W 528
#define DEFAULT_SCREEN_H 320

/* ------------------------------------------------------------------ */
/* container                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t        *map;
    size_t          map_size;
    uint16_t        w, h, fps, flags;
    uint32_t        frame_count;
    const uint32_t *index;      /* frame_count + 1 entries */
    const uint8_t  *data;
    size_t          data_size;
} bapx_t;

static int bapx_open(bapx_t *v, const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        perror(path);
        return -1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }
    if ((size_t)st.st_size < 20) {
        fprintf(stderr, "%s: file too small to be a BAPX container\n", path);
        close(fd);
        return -1;
    }

    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    const uint8_t *p = m;
    if (memcmp(p, "BAPX", 4) != 0) {
        fprintf(stderr, "%s: bad magic (expected BAPX)\n", path);
        munmap(m, (size_t)st.st_size);
        return -1;
    }

    uint16_t w, h, fps, flags;
    uint32_t n;
    memcpy(&w,     p +  4, 2);
    memcpy(&h,     p +  6, 2);
    memcpy(&fps,   p +  8, 2);
    memcpy(&flags, p + 10, 2);
    memcpy(&n,     p + 12, 4);

    if (w == 0 || h == 0 || n == 0) {
        fprintf(stderr, "%s: degenerate header (%ux%u, %u frames)\n",
                path, w, h, n);
        munmap(m, (size_t)st.st_size);
        return -1;
    }
    if (flags & ~BAPX_FLAG_KNOWN) {
        fprintf(stderr, "%s: unknown flag bits 0x%04x\n",
                path, flags & (unsigned)~BAPX_FLAG_KNOWN);
        munmap(m, (size_t)st.st_size);
        return -1;
    }
    if (fps == 0)
        fps = 30;

    size_t index_bytes = ((size_t)n + 1) * 4;
    if (16 + index_bytes > (size_t)st.st_size) {
        fprintf(stderr, "%s: index table runs past end of file\n", path);
        munmap(m, (size_t)st.st_size);
        return -1;
    }

    v->map         = m;
    v->map_size    = (size_t)st.st_size;
    v->w           = w;
    v->h           = h;
    v->fps         = fps;
    v->flags       = flags;
    v->frame_count = n;
    v->index       = (const uint32_t *)(p + 16);
    v->data        = p + 16 + index_bytes;
    v->data_size   = v->map_size - 16 - index_bytes;

    for (uint32_t i = 0; i <= n; i++) {
        if (v->index[i] > v->data_size ||
            (i > 0 && v->index[i] < v->index[i - 1])) {
            fprintf(stderr, "%s: corrupt index at frame %u\n", path, i);
            munmap(m, v->map_size);
            return -1;
        }
    }
    return 0;
}

static void bapx_close(bapx_t *v)
{
    if (v->map)
        munmap(v->map, v->map_size);
    v->map = NULL;
}

/* ------------------------------------------------------------------ */
/* blitting                                                           */
/* ------------------------------------------------------------------ */

/*
 * Writes 32 bits at a time. The first pixel is emitted singly when the
 * cursor is not 4-byte aligned, so keep the picture width even and the
 * left margin even too: on a 528-wide screen a 428-wide picture gives
 * margin 50 and every row starts aligned, while 426 gives margin 51 and
 * pays the fixup on every row.
 */
static inline uint16_t *fill_run(uint16_t *out, uint16_t col, uint32_t len)
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

/* Cursor that walks the picture rectangle inside a wider framebuffer. */
typedef struct {
    uint16_t *out;       /* next pixel to write                 */
    uint32_t  row_left;  /* pixels remaining on the current row  */
    uint32_t  gap;       /* pitch - picture width, in pixels     */
    uint32_t  w;         /* picture width                       */
    uint32_t  left;      /* pixels remaining in the whole frame  */
} blit_t;

static void blit_init(blit_t *b, uint16_t *origin, uint32_t w, uint32_t h,
                      uint32_t pitch)
{
    b->out      = origin;
    b->row_left = w;
    b->gap      = pitch - w;
    b->w        = w;
    b->left     = w * h;
}

static void blit_run(blit_t *b, uint16_t col, uint32_t len)
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
         * Wrap when the row is full and anything is still to be written --
         * either the rest of this run or a later one. Testing b->left alone
         * is wrong: it has already had this run subtracted, so the final run
         * of a frame would leave row_left stuck at 0 and spin forever.
         */
        if (b->row_left == 0 && (len || b->left)) {
            b->out     += b->gap;
            b->row_left = b->w;
        }
    }
}

/* ------------------------------------------------------------------ */
/* frame decoding                                                     */
/* ------------------------------------------------------------------ */

/* Both return bytes consumed, or 0 if the token is truncated. */
static inline int read_len_varint(const uint8_t *p, const uint8_t *end,
                                  uint32_t *len)
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

static inline int read_len_u16(const uint8_t *p, const uint8_t *end,
                               uint32_t *len)
{
    if (p + 2 > end)
        return 0;
    *len = (uint32_t)(p[0] | (p[1] << 8));
    return 2;
}

/*
 * Decode frame n into the picture rectangle at `origin` inside a framebuffer
 * of the given pitch. Only the picture area is touched, so the letterbox
 * around it survives untouched from one frame to the next.
 */
static void bapx_decode(const bapx_t *v, uint32_t n, uint16_t *origin,
                        uint32_t pitch)
{
    const uint8_t *p      = v->data + v->index[n];
    const uint8_t *end    = v->data + v->index[n + 1];
    const int      varint = (v->flags & BAPX_FLAG_VARINT) != 0;
    const int      binary = (v->flags & BAPX_FLAG_BINARY) != 0;

    blit_t b;
    blit_init(&b, origin, v->w, v->h, pitch);

    uint16_t col = 0x0000;      /* binary mode: the first run is black */

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
            col = (uint16_t)(p[0] | (p[1] << 8));
            p += 2;
        }

        blit_run(&b, col, len);

        if (binary)
            col = (uint16_t)~col;
    }

    if (b.left)                 /* short or truncated frame: pad black */
        blit_run(&b, 0x0000, b.left);
}

/* ------------------------------------------------------------------ */
/* player                                                             */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    static Uint64 freq;
    if (!freq)
        freq = SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() / (double)freq;
}

int main(int argc, char **argv)
{
    const char *path     = NULL;
    int         scale    = 2;
    uint32_t    screen_w = DEFAULT_SCREEN_W;
    uint32_t    screen_h = DEFAULT_SCREEN_H;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            unsigned a, b;
            if (sscanf(argv[++i], "%ux%u", &a, &b) != 2 || !a || !b) {
                fprintf(stderr, "bad -s argument\n");
                return 1;
            }
            screen_w = a;
            screen_h = b;
        } else if (!path) {
            path = argv[i];
        } else {
            scale = atoi(argv[i]);
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s <file.bin> [scale] [-s WxH]\n", argv[0]);
        return 1;
    }
    if (scale < 1)
        scale = 1;

    bapx_t v = {0};
    if (bapx_open(&v, path) < 0)
        return 1;

    if (v.w > screen_w || v.h > screen_h) {
        fprintf(stderr, "picture %ux%u does not fit the %ux%u screen\n",
                v.w, v.h, screen_w, screen_h);
        bapx_close(&v);
        return 1;
    }

    uint32_t x0       = (screen_w - v.w) / 2;
    uint32_t y0       = (screen_h - v.h) / 2;
    double   duration = (double)v.frame_count / v.fps;

    printf("picture %ux%u at +%u+%u on %ux%u  %u fps  %u frames  %.1f s\n"
           "flags 0x%04x (%s, %s)  data %.2f MB  %zu B/frame avg\n",
           v.w, v.h, x0, y0, screen_w, screen_h, v.fps, v.frame_count, duration,
           v.flags,
           (v.flags & BAPX_FLAG_BINARY) ? "binary" : "rgb565",
           (v.flags & BAPX_FLAG_VARINT) ? "varint" : "u16",
           (double)v.data_size / (1024.0 * 1024.0),
           v.data_size / v.frame_count);
    if (x0 & 1u)
        printf("note: left margin %u is odd, so every row pays the alignment "
               "fixup in fill_run\n", x0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        bapx_close(&v);
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("bapxplay",
                                       SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED,
                                       (int)screen_w * scale,
                                       (int)screen_h * scale,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1,
                                                 SDL_RENDERER_ACCELERATED)
                            : NULL;
    if (win && !ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!win || !ren) {
        fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_Quit();
        bapx_close(&v);
        return 1;
    }

    SDL_RenderSetLogicalSize(ren, (int)screen_w, (int)screen_h);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         (int)screen_w, (int)screen_h);
    uint16_t *fb = malloc((size_t)screen_w * screen_h * 2);
    if (!tex || !fb) {
        fprintf(stderr, "allocation failed: %s\n", SDL_GetError());
        return 1;
    }

    /* the letterbox is painted once here and never rewritten */
    memset(fb, 0, (size_t)screen_w * screen_h * 2);
    uint16_t *origin = fb + (size_t)y0 * screen_w + x0;

    double   t_start   = now_sec();
    double   base      = 0.0;
    int      paused    = 0;
    int      running   = 1;
    int64_t  shown     = -1;
    uint32_t cur       = 0;

    double   dec_accum = 0.0;
    double   dec_worst = 0.0;
    uint32_t dec_count = 0;
    uint32_t dropped   = 0;
    double   ui_next   = 0.0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN) {
                double seek = 0.0;
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    running = 0;
                    break;
                case SDLK_SPACE:
                    if (paused) {
                        t_start = now_sec();
                        paused  = 0;
                    } else {
                        base   = cur;
                        paused = 1;
                    }
                    break;
                case SDLK_RIGHT: seek =   5.0 * v.fps; break;
                case SDLK_LEFT:  seek =  -5.0 * v.fps; break;
                case SDLK_UP:    seek =  30.0 * v.fps; break;
                case SDLK_DOWN:  seek = -30.0 * v.fps; break;
                case SDLK_PERIOD:
                    base   = (double)cur + 1;
                    paused = 1;
                    break;
                case SDLK_COMMA:
                    base   = (cur > 0) ? (double)cur - 1 : 0;
                    paused = 1;
                    break;
                case SDLK_HOME:
                    base    = 0;
                    t_start = now_sec();
                    break;
                default:
                    break;
                }
                if (seek != 0.0) {
                    double f = (double)cur + seek;
                    if (f < 0)
                        f = 0;
                    if (f >= v.frame_count)
                        f = v.frame_count - 1;
                    base    = f;
                    t_start = now_sec();
                }
            }
        }

        /* playhead is always derived from the anchor, so error never accrues */
        if (paused) {
            double f = base;
            if (f < 0)
                f = 0;
            if (f > v.frame_count - 1)
                f = v.frame_count - 1;
            cur = (uint32_t)f;
        } else {
            double f = base + (now_sec() - t_start) * v.fps;
            if (f >= v.frame_count) {
                base    = 0;
                t_start = now_sec();
                f       = 0;
            }
            cur = (uint32_t)f;
        }

        if ((int64_t)cur != shown) {
            if (shown >= 0 && (int64_t)cur > shown + 1)
                dropped += (uint32_t)(cur - shown - 1);

            double t0 = now_sec();
            bapx_decode(&v, cur, origin, screen_w);
            double dt = now_sec() - t0;

            dec_accum += dt;
            if (dt > dec_worst)
                dec_worst = dt;
            dec_count++;

            SDL_UpdateTexture(tex, NULL, fb, (int)screen_w * 2);
            shown = cur;
        }

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        double t = now_sec();
        if (t >= ui_next) {
            char title[256];
            snprintf(title, sizeof title,
                     "bapxplay  %u/%u  %.1f/%.1fs  decode avg %.2fms "
                     "worst %.2fms  dropped %u%s",
                     cur, v.frame_count, (double)cur / v.fps, duration,
                     dec_count ? dec_accum / dec_count * 1000.0 : 0.0,
                     dec_worst * 1000.0, dropped, paused ? "  [PAUSED]" : "");
            SDL_SetWindowTitle(win, title);
            ui_next = t + 0.25;
        }

        SDL_Delay(1);
    }

    printf("decode: avg %.3f ms, worst %.3f ms over %u frames, %u dropped\n",
           dec_count ? dec_accum / dec_count * 1000.0 : 0.0,
           dec_worst * 1000.0, dec_count, dropped);

    free(fb);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    bapx_close(&v);
    return 0;
}

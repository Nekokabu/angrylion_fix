/*
 * rdp_load_hazard_demo (bare-metal build) - shows the RDP load hazard.
 *
 * On real hardware the last TMEM writes of a load are still pending when the
 * next command arrives (the last 15 "slots" of the load; every command that
 * only sets state retires one slot; syncs, drawing commands and loads
 * complete the rest). A command that changes the load tile or the texture
 * image soon after Load_Tile, with no sync in between, therefore changes the
 * tail of the load.
 *
 * Each panel clears TMEM to red, loads a 32x16 RGBA16 texture with Load_Tile,
 * sends GAP no-ops, then the change, with no sync before it:
 *
 *  1) Set_Tile on the load tile with a new TMEM address (+2 KiB).
 *     Top image: TMEM at the original address. Bottom image: TMEM at the new
 *     address. Real hardware writes the end of the load to the new address:
 *     the top image misses its end (red), the bottom one shows it.
 *  2) Set_Texture_Image with a second texture (orange). Real hardware reads
 *     the last row from it.
 *
 * Real hardware (NTSC and PAL consoles): 1) moved at gaps 0, 4 and 8, not at
 * 16; 2) orange last row at gaps 0 and 2, not at 4 or 8. An emulator that
 * completes every load at once shows no effect in any panel.
 * Under each panel: HAZARD or NONE, and whether that matches real hardware.
 *
 * No libdragon runtime, so it also runs in emulators that cannot run
 * current libdragon ROMs. It boots with libdragon's public-domain
 * compatibility IPL3.
 */

#include <stdint.h>
#include "font8x8.h"

#define W 320
#define H 240

#define TEX_W   32
#define TEX_H   16
#define ZOOM    2               /* horizontal magnification */
#define ZOOM_Y  4               /* vertical magnification */
#define ROW0    8               /* the images show texture rows 8..15 */
#define IMG_W   (TEX_W * ZOOM)
#define IMG_H   ((TEX_H - ROW0) * ZOOM_Y)

#define PANEL_X(k)  (12 + (k) * 76)
#define P1_LABEL_Y  28
#define P1_OLD_Y    38
#define P1_NEW_Y    74
#define P1_STAT_Y   110
#define P2_LABEL_Y  140
#define P2_IMG_Y    150
#define P2_STAT_Y   186

/* --- hardware ----------------------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_INTR      REG(0xA4300008)
#define HW_MI_MASK      REG(0xA430000C)
#define HW_VI(n)        REG(0xA4400000 + 4 * (n))
#define DPC_CLR_XBUS    0x001
#define DPC_CLR_FREEZE  0x004
#define DPC_CLR_FLUSH   0x010
#define DPC_PENDING     0x600
#define MI_INTR_DP      0x20
#define MI_CLEAR_DP_INT 0x800

#define TV_TYPE         (*(volatile uint32_t *)0x80000300)  /* set by the compat IPL3 */

#define PHYS(p)         ((uint32_t)(p) & 0x1FFFFFFF)
#define UNCACHED(p)     ((void *)(PHYS(p) | 0xA0000000))

/* framebuffers and textures in uncached RDRAM, clear of code, data and stack */
static uint16_t *const fbs[2] = { (uint16_t *)0xA0100000, (uint16_t *)0xA0140000 };
static uint16_t *const tex_a  = (uint16_t *)0xA0180000;     /* 32x16 RGBA16 */
static uint16_t *const tex_b  = (uint16_t *)0xA0181000;     /* 32x16 RGBA16 */
static uint16_t *const red4k  = (uint16_t *)0xA0182000;     /* 4 KiB of red */

#define HW_SI_STATUS    REG(0xA4800018)
#define PIF_RAM_LAST    (*(volatile uint32_t *)0xBFC007FC)

static void si_wait(void) { while (HW_SI_STATUS & 3) {} }

/* tell the PIF the boot process is finished (the compat IPL3 leaves this to
 * the program) */
static void pif_terminate_boot(void)
{
    si_wait();
    uint32_t v = PIF_RAM_LAST;
    si_wait();
    PIF_RAM_LAST = v | 8;
    si_wait();
}

/* 320x240, 16-bit, no anti-aliasing or resampling, libdragon's register
 * values. X_SCALE must be above 0x200 (VI glitch on NTSC otherwise). */
static void vi_setup(int at_vblank)
{
    static const uint32_t pal[7]  = { 0x0404233a, 0x00000271, 0x00150c69, 0x0c6f0c6e, 0x00800300, 0x005f0239, 0x0009026b };
    static const uint32_t ntsc[7] = { 0x03e52239, 0x0000020d, 0x00000c15, 0x0c150c15, 0x006c02ec, 0x00230203, 0x000e0204 };
    static const uint32_t mpal[7] = { 0x04651e39, 0x0000020d, 0x00040c11, 0x0c190c1a, 0x006c02ec, 0x00230203, 0x000e0204 };
    const uint32_t *t = TV_TYPE == 0 ? pal : TV_TYPE == 2 ? mpal : ntsc;

    if (at_vblank)
        while ((HW_VI(4) & 0x3FE) != 2) {}
    HW_VI(3) = 2;
    HW_VI(7) = t[2];
    HW_VI(8) = t[3];
    HW_VI(5) = t[0];
    HW_VI(11) = t[6];
    HW_VI(6) = t[1];
    HW_VI(9) = t[4];
    HW_VI(10) = t[5];
    HW_VI(1) = PHYS(fbs[0]);
    HW_VI(2) = W;
    HW_VI(12) = 0x201;
    HW_VI(13) = 0x400;
    HW_VI(0) = 0x3302;
}

static void vi_init(void)
{
    vi_setup(0);
    vi_setup(1);
}

static void vi_wait_vblank(void)
{
    while ((HW_VI(4) & 0x3FE) >= 10) {}
    while ((HW_VI(4) & 0x3FE) < 10) {}
}

/* --- RDP --------------------------------------------------------------------------- */

#define CMD(op)         ((uint64_t)(op) << 56)
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)   /* 10.2 fixed point */

enum { FMT_RGBA = 0 };
enum { SZ_16 = 2 };

#define SOM_FILL    (3ull << 52)
#define SOM_TEX     ((0ull << 52) | (3ull << 42) | (0xFull << 36))   /* 1-cycle, point sampling */

static uint64_t combine_tex(void)
{
    const uint64_t a = 15, b = 15, c = 31, d = 1, aa = 7, ab = 7, ac = 7, ad = 1;
    return CMD(0x3C)
         | a << 52 | c << 47 | aa << 44 | ac << 41
         | a << 37 | c << 32 | b << 28 | b << 24
         | aa << 21 | ac << 18 | d << 15 | ab << 12
         | ad << 9 | d << 6 | ab << 3 | ad;
}

static uint32_t rgba5551(int r, int g, int b)
{
    return (uint32_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1);
}

#define COL_RED     rgba5551(0xE0, 0x10, 0x10)
#define COL_ORANGE  rgba5551(0xFF, 0xA0, 0x00)
#define COL_BG      rgba5551(0x18, 0x18, 0x30)

typedef struct { uint64_t *p; } cbuf_t;
static inline void emit(cbuf_t *c, uint64_t w) { *c->p++ = w; }

static void set_texture_image(cbuf_t *c, uint16_t *tex, int width)
{
    emit(c, CMD(0x3D) | (uint64_t)FMT_RGBA << 53 | (uint64_t)SZ_16 << 51 | (uint64_t)(width - 1) << 32 | PHYS(tex));
}

static void set_tile(cbuf_t *c, int tile, int line, int tmem)
{
    emit(c, CMD(0x35) | (uint64_t)FMT_RGBA << 53 | (uint64_t)SZ_16 << 51 | (uint64_t)line << 41
         | (uint64_t)tmem << 32 | (uint64_t)tile << 24);
}

static void tile_cmd(cbuf_t *c, int op, int tile, int sl, int tl, int sh, int th)
{
    emit(c, CMD(op) | FX2(sl) << 44 | FX2(tl) << 32 | (uint64_t)tile << 24 | FX2(sh) << 12 | FX2(th));
}

static void syncs(cbuf_t *c) { emit(c, CMD(0x27)); emit(c, CMD(0x26)); emit(c, CMD(0x28)); }

#define LINE_WORDS  (TEX_W * 2 / 8)     /* 8: 64-byte rows */
#define TMEM_OLD    0x000
#define TMEM_NEW    0x100               /* +2 KiB */

/* clear TMEM to red, then Load_Tile of texture A into TMEM_OLD */
static void emit_load(cbuf_t *c)
{
    set_texture_image(c, red4k, 2048);
    set_tile(c, 7, 0, 0);
    emit(c, CMD(0x26));
    emit(c, CMD(0x33) | 7ull << 24 | (uint64_t)(2048 - 1) << 12);     /* Load_Block, 4 KiB */
    syncs(c);
    set_texture_image(c, tex_a, TEX_W);
    set_tile(c, 7, LINE_WORDS, TMEM_OLD);
    emit(c, CMD(0x26));
    tile_cmd(c, 0x34, 7, 0, 0, TEX_W - 1, TEX_H - 1);                 /* Load_Tile */
}

/* draw rows ROW0..15 of the 32x16 texture at TMEM address `tmem`, magnified */
static void emit_draw(cbuf_t *c, int tmem, int x, int y)
{
    syncs(c);
    emit(c, CMD(0x2F) | SOM_TEX);
    set_tile(c, 0, LINE_WORDS, tmem);
    tile_cmd(c, 0x32, 0, 0, 0, TEX_W - 1, TEX_H - 1);                 /* Set_Tile_Size */
    emit(c, CMD(0x24) | FX2(x + IMG_W) << 44 | FX2(y + IMG_H) << 32 | FX2(x) << 12 | FX2(y));
    emit(c, (uint64_t)(ROW0 << 5) << 32 | (uint64_t)(0x400 / ZOOM) << 16 | (uint64_t)(0x400 / ZOOM_Y));
}

static const int gaps1[4] = { 0, 4, 8, 16 };
static const int gaps2[4] = { 0, 2, 4, 8 };
static const int hw1[4] = { 1, 1, 1, 0 };   /* real hardware: hazard visible */
static const int hw2[4] = { 1, 1, 0, 0 };

static uint64_t cmdbuf[2048] __attribute__((aligned(16)));

static void draw_frame(uint16_t *fb)
{
    cbuf_t cb = { UNCACHED(cmdbuf) }, *c = &cb;
    uint64_t *c0 = cb.p;

    emit(c, CMD(0x3F) | 2ull << 51 | (uint64_t)(W - 1) << 32 | PHYS(fb));   /* Set_Color_Image */
    emit(c, CMD(0x2D) | FX2(W) << 12 | FX2(H));                             /* Set_Scissor */
    emit(c, combine_tex());
    emit(c, CMD(0x2F) | SOM_FILL);
    emit(c, CMD(0x37) | (uint64_t)COL_BG << 16 | COL_BG);
    emit(c, CMD(0x36) | FX2(W - 1) << 44 | FX2(H - 1) << 32);

    for (int k = 0; k < 4; k++)
    {
        /* 1) Set_Tile with a new TMEM address */
        emit_load(c);
        for (int i = 0; i < gaps1[k]; i++) emit(c, CMD(0x00));
        set_tile(c, 7, LINE_WORDS, TMEM_NEW);
        emit_draw(c, TMEM_OLD, PANEL_X(k), P1_OLD_Y);
        emit_draw(c, TMEM_NEW, PANEL_X(k), P1_NEW_Y);

        /* 2) Set_Texture_Image with texture B */
        emit_load(c);
        for (int i = 0; i < gaps2[k]; i++) emit(c, CMD(0x00));
        set_texture_image(c, tex_b, TEX_W);
        emit_draw(c, TMEM_OLD, PANEL_X(k), P2_IMG_Y);
    }
    emit(c, CMD(0x29));                                                     /* Sync_Full */

    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PHYS(c0);
    HW_DPC_END = PHYS(cb.p);
    for (uint32_t t = 0x4000000; t && !(HW_MI_INTR & MI_INTR_DP); t--) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* --- CPU drawing --------------------------------------------------------------------- */

static void text(uint16_t *px, int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    for (; *s; s++, x += 8)
        for (int j = 0; j < 8; j++)
        {
            unsigned char bits = __font_data[(unsigned char)*s * 8 + j];
            for (int i = 0; i < 8; i++)
                px[(y + j) * W + x + i] = (bits & (0x80 >> i)) ? fg : bg;
        }
}

static char *put_u(char *p, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_s(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* texel (tx, ty) of the image drawn at (x, y) */
static uint16_t texel(const uint16_t *px, int x, int y, int tx, int ty)
{
    return px[(y + (ty - ROW0) * ZOOM_Y) * W + x + tx * ZOOM];
}

static void label(uint16_t *px, int k, int y, int gap)
{
    char s[16], *p = s;
    p = put_s(p, "GAP ");
    p = put_u(p, (uint32_t)gap);
    *p = 0;
    text(px, PANEL_X(k), y, s, (uint16_t)rgba5551(0xFF, 0xFF, 0xFF), (uint16_t)COL_BG);
}

/* HAZARD / NONE, green if it matches real hardware */
static int verdict(uint16_t *px, int k, int y, int seen, int expect)
{
    uint16_t ok = (uint16_t)rgba5551(0x40, 0xFF, 0x40), bad = (uint16_t)rgba5551(0xFF, 0x40, 0x40);
    text(px, PANEL_X(k), y, seen ? "HAZARD" : "NONE  ", seen == expect ? ok : bad, (uint16_t)COL_BG);
    text(px, PANEL_X(k), y + 9, seen == expect ? "=HW   " : "DIFF  ", seen == expect ? ok : bad, (uint16_t)COL_BG);
    return seen == expect;
}

static void fill_textures(void)
{
    for (int y = 0; y < TEX_H; y++)
        for (int x = 0; x < TEX_W; x++)
        {
            /* texture A: blue-green, rows alternate in brightness, columns in 4-texel steps */
            int lum = (y & 1) ? 0xF0 : 0xA0;
            int step = (x >> 2) * 14;
            tex_a[y * TEX_W + x] = (uint16_t)rgba5551(0x20 + step, lum, 0xFF - step);
            tex_b[y * TEX_W + x] = (uint16_t)(((x ^ y) & 4) ? COL_ORANGE : rgba5551(0xFF, 0xE0, 0x40));
        }
    for (int i = 0; i < 2048; i++) red4k[i] = (uint16_t)COL_RED;
}

int main(void)
{
    int cur = 0;

    HW_MI_MASK = 0x555;             /* mask all RCP interrupts */
    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;
    pif_terminate_boot();
    for (int i = 0; i < W * H; i++) fbs[0][i] = fbs[1][i] = 1;
    fill_textures();
    vi_init();

    for (;;)
    {
        uint16_t *px = fbs[cur];
        draw_frame(px);

        uint16_t white = (uint16_t)rgba5551(0xFF, 0xFF, 0xFF), grey = (uint16_t)rgba5551(0xB0, 0xB0, 0xC0);
        uint16_t bg = (uint16_t)COL_BG;
        int match = 0;

        text(px, 8, 4, TV_TYPE == 0 ? "RDP LOAD HAZARD DEMO (BARE) PAL"
                     : TV_TYPE == 2 ? "RDP LOAD HAZARD DEMO (BARE) MPAL"
                     : "RDP LOAD HAZARD DEMO (BARE) NTSC", white, bg);
        text(px, 8, 16, "1) SET_TILE NEW TMEM ADDR AFTER LOAD", grey, bg);
        text(px, 8, 128, "2) SET_TEX_IMAGE B AFTER LOAD", grey, bg);

        for (int k = 0; k < 4; k++)
        {
            int x = PANEL_X(k);
            /* 1) hazard: the last texel of the original image is still red */
            int seen1 = texel(px, x, P1_OLD_Y, TEX_W - 1, TEX_H - 1) == (uint16_t)COL_RED;
            /* 2) hazard: the last row comes from texture B */
            uint16_t t2 = texel(px, x, P2_IMG_Y, 0, TEX_H - 1);
            int seen2 = t2 == tex_b[(TEX_H - 1) * TEX_W];
            label(px, k, P1_LABEL_Y, gaps1[k]);
            label(px, k, P2_LABEL_Y, gaps2[k]);
            match += verdict(px, k, P1_STAT_Y, seen1, hw1[k]);
            match += verdict(px, k, P2_STAT_Y, seen2, hw2[k]);
        }

        char s[48], *p = s;
        p = put_s(p, "MATCHES REAL N64: ");
        p = put_u(p, (uint32_t)match);
        p = put_s(p, "/8");
        *p = 0;
        text(px, 8, 208, s, match == 8 ? (uint16_t)rgba5551(0x40, 0xFF, 0x40) : (uint16_t)rgba5551(0xFF, 0x40, 0x40), bg);
        text(px, 8, 220, "HW: 1) MOVED AT 0,4,8  2) B AT 0,2", white, bg);
        text(px, 8, 229, "ROWS 8-15 SHOWN. RED = NOT LOADED", grey, bg);

        vi_wait_vblank();
        HW_VI(1) = PHYS(px);
        cur ^= 1;
    }
}

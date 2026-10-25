/*
 * rdp_firstpixel_demo (bare-metal build) - shows the 2-cycle "first pixel of
 * a span" behaviour.
 *
 * Same test as the libdragon build, without the libdragon runtime, so that
 * it also runs in emulators that cannot run current libdragon ROMs (for
 * example mupen64plus 2.6.0 and Rosalie's Mupen GUI, which lack the LLD/SCD
 * instructions and 64-bit addressing that libdragon uses). It boots with
 * libdragon's public-domain compatibility IPL3.
 *
 * In 2-cycle mode the RDP's first blender cycle blends with the memory colour
 * of the *previous* pixel the pipeline read, so the first pixel of each span
 * uses the value read last on the previous span. For a span ending at the
 * framebuffer's right edge that is the pixel just past it, i.e. pixel 0 of
 * the next row.
 *
 * The screen is a background of rows in alternating dark and light shades
 * (no two rows within 15 of each other have the same colour). Two bands
 * are blended over it with memory used in BOTH blender cycles:
 *
 *   band A: x = 0..319   (the value carried over is pixel (0, y): the row's
 *                         own first pixel)
 *   band B: x = 16..319  (the value carried over is still pixel (0, y), which
 *                         the band does not cover)
 *
 * The first row of a band inherits the value read last before the band. For
 * band B that is band A's last read, (0, 104). Band A follows the background,
 * which is drawn in fill mode; real hardware occasionally carries an
 * unrelated value across fill mode drawing, so an invisible 2-cycle
 * primitive on row 7 runs after the background: it writes every pixel back
 * unchanged and its last read is (0, 8). Rows 8 and 24 have the same colour,
 * as do rows 104 and 120.
 *
 * On real hardware the first pixel of every band row comes out the same as
 * the rest of the row (confirmed on NTSC and PAL consoles). An emulator that
 * carries the value from another row shows the first column in the wrong
 * (other row's) shade; one that depends on thread timing makes it flicker.
 * The magnified views on the right show the left edge of each band, and the
 * status lines count rows whose first pixel differs from the second.
 */

#include <stdint.h>
#include "font8x8.h"

#define W 320
#define H 240

#define BAND_A_Y0   24
#define BAND_A_Y1   104     /* exclusive */
#define BAND_A_X0   0
#define BAND_B_Y0   120
#define BAND_B_Y1   200
#define BAND_B_X0   16

#define ZOOM        4
#define ZOOM_W      16
#define ZOOM_H      20
#define ZOOM_X      244

/* --- hardware ----------------------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_INTR      REG(0xA4300008)
#define HW_VI(n)        REG(0xA4400000 + 4 * (n))
#define DPC_CLR_XBUS    0x001
#define DPC_CLR_FREEZE  0x004
#define DPC_CLR_FLUSH   0x010
#define DPC_PENDING     0x600
#define MI_INTR_DP      0x20
#define MI_CLEAR_DP_INT 0x800
#define HW_MI_MASK      REG(0xA430000C)

#define TV_TYPE         (*(volatile uint32_t *)0x80000300)  /* set by the compat IPL3 */

#define PHYS(p)         ((uint32_t)(p) & 0x1FFFFFFF)
#define UNCACHED(p)     ((void *)(PHYS(p) | 0xA0000000))

/* two framebuffers in uncached RDRAM, clear of code, data and stack */
static uint16_t *const fbs[2] = { (uint16_t *)0xA0100000, (uint16_t *)0xA0140000 };

#define HW_SI_STATUS    REG(0xA4800018)
#define PIF_RAM_LAST    (*(volatile uint32_t *)0xBFC007FC)

static void si_wait(void) { while (HW_SI_STATUS & 3) {} }

/* tell the PIF the boot process is finished (the compat IPL3 leaves this to
 * the program; commercial games do it in osInitialize) */
static void pif_terminate_boot(void)
{
    si_wait();
    uint32_t v = PIF_RAM_LAST;
    si_wait();
    PIF_RAM_LAST = v | 8;
    si_wait();
}

/* 320x240, 16-bit, no anti-aliasing or resampling, with libdragon's register
 * values and write order. X_SCALE must be above 0x200: with interpolation
 * off (AA mode 3) on a 16-bit framebuffer, the VI glitches on real hardware
 * when h_start is below 128 and X_SCALE <= 0x200. NTSC's h_start is 108, so
 * an earlier version with X_SCALE 0x200 showed a broken picture on NTSC
 * consoles but not on PAL (h_start 128). angrylion-rdp-plus emulates this
 * since after its 1.6 release. */
static void vi_setup(int at_vblank)
{
    /* BURST, V_TOTAL, H_TOTAL, H_TOTAL_LEAP, H_VIDEO, V_VIDEO, V_BURST */
    static const uint32_t pal[7]  = { 0x0404233a, 0x00000271, 0x00150c69, 0x0c6f0c6e, 0x00800300, 0x005f0239, 0x0009026b };
    static const uint32_t ntsc[7] = { 0x03e52239, 0x0000020d, 0x00000c15, 0x0c150c15, 0x006c02ec, 0x00230203, 0x000e0204 };
    static const uint32_t mpal[7] = { 0x04651e39, 0x0000020d, 0x00040c11, 0x0c190c1a, 0x006c02ec, 0x00230203, 0x000e0204 };
    const uint32_t *t = TV_TYPE == 0 ? pal : TV_TYPE == 2 ? mpal : ntsc;

    if (at_vblank)
        while ((HW_VI(4) & 0x3FE) != 2) {}
    HW_VI(3) = 2;                       /* V_INTR */
    HW_VI(7) = t[2];                    /* H_TOTAL */
    HW_VI(8) = t[3];                    /* H_TOTAL_LEAP */
    HW_VI(5) = t[0];                    /* BURST */
    HW_VI(11) = t[6];                   /* V_BURST */
    HW_VI(6) = t[1];                    /* V_TOTAL (progressive) */
    HW_VI(9) = t[4];                    /* H_VIDEO */
    HW_VI(10) = t[5];                   /* V_VIDEO */
    HW_VI(1) = PHYS(fbs[0]);
    HW_VI(2) = W;
    HW_VI(12) = 0x201;                  /* X_SCALE: 320 -> 640 (as libdragon) */
    HW_VI(13) = 0x400;                  /* Y_SCALE: 240 lines */
    HW_VI(0) = 0x3302;                  /* 16-bit, pixel advance 3, AA/resample off */
}

/* The VI only counts lines once it is configured, so the first pass cannot
 * wait for vblank; the second pass writes everything again at vblank. */
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

/* Set_Other_Modes: 2-cycle, no dither, image read, force blend, and in both
 * blender cycles: P = input (cycle 0) / blended (cycle 1), A = combiner
 * alpha, M = memory colour, B = 1 - A */
#define SOM_DEMO    ((1ull << 52) | (3ull << 42) | (0xFull << 36) \
                   | (1ull << 22) | (1ull << 20) | (1ull << 14) | (1ull << 6))
#define SOM_FILL    (3ull << 52)
/* 2-cycle with image read; cycle 1: P = 0, A = 0, M = memory, B = 1, so every
 * pixel is written back unchanged */
#define SOM_INVISIBLE ((1ull << 52) | (0xFull << 36) | (3ull << 24) | (1ull << 20) | (2ull << 16) \
                   | (1ull << 14) | (1ull << 6))

/* Set_Combine_Mode: PRIM colour and PRIM alpha in both cycles */
static uint64_t combine_prim(void)
{
    const uint64_t a = 15, b = 15, c = 31, d = 3, aa = 7, ab = 7, ac = 7, ad = 3;
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

/* dark and light rows alternate; the shade steps every two rows with a
 * period of 16 rows */
static uint32_t bg_color(int y)
{
    int level = (y >> 1) & 7;
    if (y & 1)
        return rgba5551(0xF0, 0xF0 - level * 20, 0x60 + level * 20);
    return rgba5551(0x10 + level * 20, 0x10, 0x90 - level * 16);
}

static uint64_t cmdbuf[1024] __attribute__((aligned(16)));

static void draw_frame(uint16_t *fb)
{
    uint64_t *c0 = UNCACHED(cmdbuf), *c = c0;

    *c++ = CMD(0x3F) | 2ull << 51 | (uint64_t)(W - 1) << 32 | PHYS(fb);  /* Set_Color_Image */
    *c++ = CMD(0x2D) | FX2(W) << 12 | FX2(H);                            /* Set_Scissor */
    *c++ = combine_prim();

    /* background, one row at a time (fill mode, inclusive coordinates) */
    *c++ = CMD(0x2F) | SOM_FILL;
    for (int y = 0; y < H; y++)
    {
        *c++ = CMD(0x27);                                                /* Sync_Pipe */
        *c++ = CMD(0x37) | (uint64_t)bg_color(y) << 16 | bg_color(y);    /* Set_Fill_Color */
        *c++ = CMD(0x36) | FX2(W - 1) << 44 | FX2(y) << 32 | FX2(0) << 12 | FX2(y);
    }

    /* invisible 2-cycle row: defines the value band A's first row inherits */
    *c++ = CMD(0x27);
    *c++ = CMD(0x2F) | SOM_INVISIBLE;
    *c++ = CMD(0x36) | FX2(W) << 44 | FX2(8) << 32 | FX2(0) << 12 | FX2(7);

    /* the two bands; in 1/2-cycle mode right and bottom edges are exclusive */
    *c++ = CMD(0x27);
    *c++ = CMD(0x2F) | SOM_DEMO;
    *c++ = CMD(0x3A) | 0xFF202080u;                                      /* Set_Prim_Color */
    *c++ = CMD(0x36) | FX2(W) << 44 | FX2(BAND_A_Y1) << 32 | FX2(BAND_A_X0) << 12 | FX2(BAND_A_Y0);
    *c++ = CMD(0x27);
    *c++ = CMD(0x3A) | 0x20C02080u;
    *c++ = CMD(0x36) | FX2(W) << 44 | FX2(BAND_B_Y1) << 32 | FX2(BAND_B_X0) << 12 | FX2(BAND_B_Y0);
    *c++ = CMD(0x29);                                                    /* Sync_Full */

    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PHYS(c0);
    HW_DPC_END = PHYS(c);
    for (uint32_t t = 0x4000000; t && !(HW_MI_INTR & MI_INTR_DP); t--) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* --- CPU drawing --------------------------------------------------------------------- */

static int count_wrong(const uint16_t *px, int x0, int y0, int y1)
{
    int wrong = 0;
    for (int y = y0; y < y1; y++)
        if (px[y * W + x0] != px[y * W + x0 + 1])
            wrong++;
    return wrong;
}

static void box(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px[(y + j) * W + x + i] = col;
}

static void zoom(uint16_t *px, int sx, int sy, int dx, int dy)
{
    uint16_t buf[ZOOM_H][ZOOM_W];
    for (int y = 0; y < ZOOM_H; y++)
        for (int x = 0; x < ZOOM_W; x++)
            buf[y][x] = px[(sy + y) * W + sx + x];

    box(px, dx - 2, dy - 2, ZOOM_W * ZOOM + 4, ZOOM_H * ZOOM + 4, (uint16_t)rgba5551(0xFF, 0xFF, 0x00));
    for (int y = 0; y < ZOOM_H * ZOOM; y++)
        for (int x = 0; x < ZOOM_W * ZOOM; x++)
            px[(dy + y) * W + dx + x] = buf[y / ZOOM][x / ZOOM];
}

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

/* append a decimal number */
static char *put_u(char *p, uint32_t v, int width)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) tmp[n++] = ' ';
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_s(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static void status(uint16_t *px, int y, char band, int wrong, uint32_t bad, uint32_t frames)
{
    char line[48], *p = line;
    *p++ = band;
    p = put_s(p, ": WRONG ROWS ");
    p = put_u(p, (uint32_t)wrong, 2);
    p = put_s(p, "  BAD FRAMES ");
    p = put_u(p, bad, 0);
    *p++ = '/';
    p = put_u(p, frames, 0);
    *p = 0;
    text(px, 8, y, line, wrong ? (uint16_t)rgba5551(0xFF, 0x40, 0x40) : (uint16_t)rgba5551(0x40, 0xFF, 0x40), 1);
}

int main(void)
{
    uint32_t frames = 0, bad_a = 0, bad_b = 0;
    int first = 1, cur = 0;

    HW_MI_MASK = 0x555;             /* mask all RCP interrupts */
    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;
    pif_terminate_boot();
    for (int i = 0; i < W * H; i++) fbs[0][i] = fbs[1][i] = 1;
    vi_init();

    for (;;)
    {
        uint16_t *px = fbs[cur];
        draw_frame(px);

        int wa = count_wrong(px, BAND_A_X0, BAND_A_Y0, BAND_A_Y1);
        int wb = count_wrong(px, BAND_B_X0, BAND_B_Y0, BAND_B_Y1);
        /* the very first frame inherits the RDP's power-on state, skip it */
        if (first) first = 0;
        else
        {
            frames++;
            if (wa) bad_a++;
            if (wb) bad_b++;
        }

        zoom(px, BAND_A_X0, BAND_A_Y0, ZOOM_X, BAND_A_Y0 + 2);
        zoom(px, BAND_B_X0 - 4, BAND_B_Y0, ZOOM_X, BAND_B_Y0 + 2);

        uint16_t white = (uint16_t)rgba5551(0xFF, 0xFF, 0xFF), black = 1;
        text(px, 8, 4, TV_TYPE == 0 ? "RDP 2-CYCLE FIRST PIXEL (BARE) PAL"
                     : TV_TYPE == 2 ? "RDP 2-CYCLE FIRST PIXEL (BARE) MPAL"
                     : "RDP 2-CYCLE FIRST PIXEL (BARE) NTSC", white, black);
        status(px, 12, 'A', wa, bad_a, frames);
        status(px, 208, 'B', wb, bad_b, frames);
        text(px, 8, 220, "HARDWARE: 0 WRONG ROWS, FIRST COLUMN", white, black);
        text(px, 8, 228, "MATCHES ITS ROW IN BOTH ZOOMS", white, black);

        vi_wait_vblank();
        HW_VI(1) = PHYS(px);
        cur ^= 1;
    }
}

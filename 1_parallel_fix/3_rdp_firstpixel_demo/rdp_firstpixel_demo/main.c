/*
 * rdp_firstpixel_demo - shows the 2-cycle "first pixel of a span" behaviour
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
 * The first row of a band has no previous span in the same primitive and
 * inherits the value read last before the band. For band B that is band A's
 * last read, (0, 104). Band A follows the background, which is drawn in fill
 * mode; real hardware occasionally carries an unrelated value across fill
 * mode drawing (rdp_carry_probe v2: a few percent of the time, independent
 * of fill colour and count), so an invisible 2-cycle primitive on row 7 runs
 * after the background: it writes every pixel back unchanged and its last
 * read is (0, 8). Rows 8 and 24 have the same colour, as do rows 104 and 120.
 *
 * Either way the carried value equals the background of the same row, so on
 * real hardware the first pixel of every band row comes out the same as the
 * rest of the row (confirmed on NTSC and PAL consoles for everything except
 * the fill mode case above). An emulator that carries the value from another
 * row shows the first column in the wrong (other row's) shade; one that depends on thread
 * timing makes it flicker. The magnified views on the right show the left
 * edge of each band, and the status lines count rows whose first pixel
 * differs from the second.
 */

#include <libdragon.h>
#include <stdio.h>

#define W 320
#define H 240

#define BAND_A_Y0   24
#define BAND_A_Y1   104     /* exclusive */
#define BAND_A_X0   0
#define BAND_B_Y0   120
#define BAND_B_Y1   200
#define BAND_B_X0   16

#define ZOOM        4
#define ZOOM_W      16      /* source columns */
#define ZOOM_H      20      /* source rows */
#define ZOOM_X      244

/* Set_Other_Modes: 2-cycle, no dither, image read, force blend, and in both
 * blender cycles: P = input (cycle 0) / blended (cycle 1), A = combiner
 * alpha, M = memory colour, B = 1 - A */
/* The RDP is driven directly through the DP registers (no RSP microcode),
 * so the ROM also runs in emulators with a high-level RSP plugin and every
 * command is exactly as written here. */
#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_INTR      REG(0xA4300008)
#define DPC_CLR_XBUS    0x001
#define DPC_PENDING     0x600
#define MI_INTR_DP      0x20
#define MI_CLEAR_DP_INT 0x800

#define CMD(op)         ((uint64_t)(op) << 56)
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)   /* 10.2 fixed point */

/* Set_Other_Modes: 2-cycle, no dither, image read, force blend, and in both
 * blender cycles: P = input (cycle 0) / blended (cycle 1), A = combiner
 * alpha, M = memory colour, B = 1 - A */
#define SOM_DEMO    ((1ull << 52)                   /* cycle type 2 */ \
                   | (3ull << 42)                   /* bi_lerp 0/1 */ \
                   | (0xFull << 36)                 /* rgb/alpha dither off */ \
                   | (1ull << 22) | (1ull << 20)    /* M = memory, both cycles */ \
                   | (1ull << 14)                   /* force blend */ \
                   | (1ull << 6))                   /* image read */
#define SOM_FILL    (3ull << 52)
/* 2-cycle with image read; cycle 1: P = 0, A = 0, M = memory, B = 1, so every
 * pixel is written back unchanged (cycle 0 is not used by the output) */
#define SOM_INVISIBLE ((1ull << 52) | (0xFull << 36) | (3ull << 24) | (1ull << 20) | (2ull << 16) \
                   | (1ull << 14) | (1ull << 6))

/* Set_Combine_Mode: output PRIM colour and PRIM alpha in both cycles */
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
    return (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1;
}

/* Background row colours: dark and light rows alternate, and the shade
 * steps every two rows with a period of 16 rows, so a value taken from any
 * row up to 15 rows away has a different colour. */
static uint32_t bg_color(int y)
{
    int level = (y >> 1) & 7;
    if (y & 1)
        return rgba5551(0xF0, 0xF0 - level * 20, 0x60 + level * 20);
    return rgba5551(0x10 + level * 20, 0x10, 0x90 - level * 16);
}

static uint64_t cmdbuf[1024] __attribute__((aligned(16)));

static void draw_frame(surface_t *disp)
{
    uint64_t *c0 = UncachedAddr(cmdbuf), *c = c0;

    *c++ = CMD(0x3F) | 0ull << 53 | 2ull << 51 | (uint64_t)(W - 1) << 32 | PhysicalAddr(disp->buffer);
    *c++ = CMD(0x2D) | FX2(W) << 12 | FX2(H);                           /* Set_Scissor */
    *c++ = combine_prim();

    /* background, one row at a time (fill mode, inclusive coordinates) */
    *c++ = CMD(0x2F) | SOM_FILL;
    for (int y = 0; y < H; y++) {
        *c++ = CMD(0x27);                                               /* Sync_Pipe */
        *c++ = CMD(0x37) | (uint64_t)bg_color(y) << 16 | bg_color(y);   /* Set_Fill_Color */
        *c++ = CMD(0x36) | FX2(W - 1) << 44 | FX2(y) << 32 | FX2(0) << 12 | FX2(y);
    }

    /* the two bands: 2-cycle, memory blended in both cycles; in 1/2-cycle
     * mode the rectangle's right and bottom edges are exclusive */
    /* invisible 2-cycle row: defines the value band A's first row inherits */
    *c++ = CMD(0x27);
    *c++ = CMD(0x2F) | SOM_INVISIBLE;
    *c++ = CMD(0x36) | FX2(W) << 44 | FX2(8) << 32 | FX2(0) << 12 | FX2(7);

    *c++ = CMD(0x27);
    *c++ = CMD(0x2F) | SOM_DEMO;
    *c++ = combine_prim();
    *c++ = CMD(0x3A) | 0xFF202080u;                                     /* Set_Prim_Color */
    *c++ = CMD(0x36) | FX2(W) << 44 | FX2(BAND_A_Y1) << 32 | FX2(BAND_A_X0) << 12 | FX2(BAND_A_Y0);
    *c++ = CMD(0x27);
    *c++ = CMD(0x3A) | 0x20C02080u;
    *c++ = CMD(0x36) | FX2(W) << 44 | FX2(BAND_B_Y1) << 32 | FX2(BAND_B_X0) << 12 | FX2(BAND_B_Y0);
    *c++ = CMD(0x29);                                                   /* Sync_Full */

    /* nothing of the framebuffer may be in the CPU cache while the RDP draws */
    data_cache_hit_writeback_invalidate(disp->buffer, W * H * 2);

    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PhysicalAddr(c0);
    HW_DPC_END = PhysicalAddr(c);
    while (!(HW_MI_INTR & MI_INTR_DP)) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* rows of a band whose first pixel differs from the second */
static int count_wrong(surface_t *disp, int x0, int y0, int y1)
{
    uint16_t *px = UncachedUShortAddr(disp->buffer);
    int stride = disp->stride / 2, wrong = 0;
    for (int y = y0; y < y1; y++)
        if (px[y * stride + x0] != px[y * stride + x0 + 1])
            wrong++;
    return wrong;
}

/* CPU copy of the band's left edge, magnified, with a frame around it */
static void zoom(surface_t *disp, int sx, int sy, int dx, int dy)
{
    uint16_t *px = UncachedUShortAddr(disp->buffer);
    int stride = disp->stride / 2;
    uint16_t buf[ZOOM_H][ZOOM_W];

    for (int y = 0; y < ZOOM_H; y++)
        for (int x = 0; x < ZOOM_W; x++)
            buf[y][x] = px[(sy + y) * stride + sx + x];

    uint32_t frame = graphics_make_color(0xFF, 0xFF, 0x00, 0xFF);
    graphics_draw_box(disp, dx - 2, dy - 2, ZOOM_W * ZOOM + 4, ZOOM_H * ZOOM + 4, frame);
    for (int y = 0; y < ZOOM_H * ZOOM; y++)
        for (int x = 0; x < ZOOM_W * ZOOM; x++)
            px[(dy + y) * stride + dx + x] = buf[y / ZOOM][x / ZOOM];
}

int main(void)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_DISABLED);

    uint32_t frames = 0, bad_a = 0, bad_b = 0;
    int first = 1;
    char line[64];

    while (1)
    {
        surface_t *disp = display_get();
        draw_frame(disp);

        int wa = count_wrong(disp, BAND_A_X0, BAND_A_Y0, BAND_A_Y1);
        int wb = count_wrong(disp, BAND_B_X0, BAND_B_Y0, BAND_B_Y1);
        /* the very first frame inherits the RDP's power-on state, skip it */
        if (first) first = 0;
        else {
            frames++;
            if (wa) bad_a++;
            if (wb) bad_b++;
        }

        zoom(disp, BAND_A_X0, BAND_A_Y0, ZOOM_X, BAND_A_Y0 + 2);
        zoom(disp, BAND_B_X0 - 4, BAND_B_Y0, ZOOM_X, BAND_B_Y0 + 2);

        uint32_t white = graphics_make_color(0xFF, 0xFF, 0xFF, 0xFF);
        uint32_t black = graphics_make_color(0x00, 0x00, 0x00, 0xFF);
        uint32_t red = graphics_make_color(0xFF, 0x40, 0x40, 0xFF);
        uint32_t green = graphics_make_color(0x40, 0xFF, 0x40, 0xFF);

        graphics_set_color(white, black);
        graphics_draw_text(disp, 8, 4, "RDP 2-CYCLE FIRST PIXEL TEST");
        snprintf(line, sizeof line, "A: WRONG ROWS %2d  BAD FRAMES %lu/%lu",
                 wa, (unsigned long)bad_a, (unsigned long)frames);
        graphics_set_color(wa ? red : green, black);
        graphics_draw_text(disp, 8, 12, line);
        snprintf(line, sizeof line, "B: WRONG ROWS %2d  BAD FRAMES %lu/%lu",
                 wb, (unsigned long)bad_b, (unsigned long)frames);
        graphics_set_color(wb ? red : green, black);
        graphics_draw_text(disp, 8, 208, line);
        graphics_set_color(white, black);
        graphics_draw_text(disp, 8, 220, "HARDWARE: 0 WRONG ROWS, FIRST COLUMN");
        graphics_draw_text(disp, 8, 228, "MATCHES ITS ROW IN BOTH ZOOMS");

        data_cache_hit_writeback_invalidate(disp->buffer, W * H * 2);
        display_show(disp);
    }
}

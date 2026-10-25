/*
 * rdp_carry_probe - which memory colour does the first pixel of a 2-cycle
 * primitive blend with?
 *
 * In 2-cycle mode the first blender cycle uses the memory colour of the
 * previous pixel the pipeline read. Inside a primitive that is the last pixel
 * read on the previous scanline. For the first scanline of a primitive it is
 * whatever the RDP read last before it - and rdp_firstpixel_demo showed that
 * real hardware does not agree with angrylion-rdp-plus there.
 *
 * Method: a 32-bit framebuffer is filled by the CPU with "address codes":
 * every pixel holds its own coordinates (R = x low bits, G = y, B = 0x80 | x
 * bit 8). The probe primitive is a 2-cycle rectangle whose blender copies the
 * first cycle's memory input straight to the output:
 *
 *   cycle 0: P = memory, A = 0, M = memory, B = 1   ->  out = memory colour
 *   cycle 1: P = cycle 0, A = 0, M = cycle 0, B = 1  ->  out = cycle 0
 *
 * so every probe pixel shows the memory colour the first blender cycle saw,
 * i.e. the code of the pixel read before it. The first pixel of the first
 * probe row shows what was carried in from before the probe.
 *
 * Each test runs a known "anchor" primitive first (last pixel read: (240,50))
 * and then something in between, then the probe. Results (decoded
 * coordinates or raw colours) are shown on screen and written to
 * sd:/rdp_carry/rdp_carry_NNN.txt.
 *
 * The RDP is driven directly through the DP registers (no RSP microcode).
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define VERSION     "1"
#define RUNS        16
#define FB_W        320
#define FB_H        240

/* --- hardware --------------------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_VERSION   REG(0xA4300004)
#define HW_MI_INTR      REG(0xA4300008)
#define DPC_CLR_XBUS    0x001
#define DPC_CLR_FREEZE  0x004
#define DPC_CLR_FLUSH   0x010
#define DPC_PENDING     0x600
#define MI_INTR_DP      0x20
#define MI_CLEAR_DP_INT 0x800

/* --- RDP commands -------------------------------------------------------------- */

#define CMD(op)         ((uint64_t)(op) << 56)
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)   /* 10.2 fixed point */

#define SOM_1CYC        (0ull << 52)
#define SOM_2CYC        (1ull << 52)
#define SOM_FILL        (3ull << 52)
#define SOM_NO_DITHER   (0xFull << 36)
#define SOM_FORCE_BLEND (1ull << 14)
#define SOM_IMAGE_READ  (1ull << 6)
/* blender fields: P (1a), A (1b), M (2a), B (2b), cycle 0 and cycle 1 */
#define BL(p0, p1, a0, a1, m0, m1, b0, b1) \
    ((uint64_t)(p0) << 30 | (uint64_t)(p1) << 28 | (uint64_t)(a0) << 26 | (uint64_t)(a1) << 24 | \
     (uint64_t)(m0) << 22 | (uint64_t)(m1) << 20 | (uint64_t)(b0) << 18 | (uint64_t)(b1) << 16)

/* probe: both cycles output the first cycle's memory input unchanged */
#define SOM_PROBE       (SOM_2CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | SOM_IMAGE_READ | BL(1, 0, 3, 3, 1, 0, 2, 2))
/* the same blender without image read */
#define SOM_PROBE_NORD  (SOM_2CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | BL(1, 0, 3, 3, 1, 0, 2, 2))
/* 1-cycle, reads memory and writes it back unchanged */
#define SOM_1C_READ     (SOM_1CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | SOM_IMAGE_READ | BL(1, 1, 3, 3, 1, 1, 2, 2))
/* 1-cycle, no image read, writes the combiner output (PRIM colour) */
#define SOM_1C_PRIM     (SOM_1CYC | SOM_NO_DITHER)

#define PRIM_COLOR      0x102030FFu     /* written by the 1-cycle PRIM rectangle */
#define FILL_COLOR      0xF1F213E0u     /* fill mode colour */

/* Set_Combine_Mode: PRIM colour / alpha in both cycles */
static uint64_t combine_prim(void)
{
    const uint64_t a = 15, b = 15, c = 31, d = 3, aa = 7, ab = 7, ac = 7, ad = 3;
    return CMD(0x3C)
         | a << 52 | c << 47 | aa << 44 | ac << 41
         | a << 37 | c << 32 | b << 28 | b << 24
         | aa << 21 | ac << 18 | d << 15 | ab << 12
         | ad << 9 | d << 6 | ab << 3 | ad;
}

typedef struct { uint64_t *start, *p; } cbuf_t;
static inline void emit(cbuf_t *c, uint64_t w) { *c->p++ = w; }

static uint32_t *fb;
static cbuf_t cb;

static void begin(cbuf_t *c)
{
    c->p = c->start;
    emit(c, CMD(0x3F) | 0ull << 53 | 3ull << 51 | (uint64_t)(FB_W - 1) << 32 | PhysicalAddr(fb));
    emit(c, CMD(0x2D) | FX2(FB_W) << 12 | FX2(FB_H));             /* Set_Scissor */
    emit(c, combine_prim());
    emit(c, CMD(0x3A) | PRIM_COLOR);                               /* Set_Prim_Color */
}

static void som(cbuf_t *c, uint64_t mode) { emit(c, CMD(0x27)); emit(c, CMD(0x2F) | mode); }

/* 1/2-cycle rectangle: x1, y1 exclusive */
static void rect(cbuf_t *c, int x0, int y0, int x1, int y1)
{
    emit(c, CMD(0x36) | FX2(x1) << 44 | FX2(y1) << 32 | FX2(x0) << 12 | FX2(y0));
}

/* fill mode rectangle: inclusive */
static void fill_rect(cbuf_t *c, int x0, int y0, int x1, int y1)
{
    emit(c, CMD(0x36) | FX2(x1) << 44 | FX2(y1) << 32 | FX2(x0) << 12 | FX2(y0));
}

/* rectangle drawn as a Fill_Triangle; lft = 1: major edge on the left
 * (spans drawn left to right), lft = 0: major edge on the right */
static void tri_rect(cbuf_t *c, int x0, int y0, int x1, int y1, int lft)
{
    uint64_t yh = (uint64_t)(y0 * 4) & 0x3FFF, yl = (uint64_t)(y1 * 4) & 0x3FFF;
    uint64_t xmaj = (uint64_t)(lft ? x0 : x1) << 16, xmin = (uint64_t)(lft ? x1 : x0) << 16;
    emit(c, CMD(0x08) | (uint64_t)lft << 55 | yl << 32 | yl << 16 | yh);
    emit(c, (xmin & 0xFFFFFFFF) << 32);        /* XL, DxLDy = 0 */
    emit(c, (xmaj & 0xFFFFFFFF) << 32);        /* XH, DxHDy = 0 */
    emit(c, (xmin & 0xFFFFFFFF) << 32);        /* XM, DxMDy = 0 */
}

static void run(cbuf_t *c)
{
    emit(c, CMD(0x29));                                            /* Sync_Full */
    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PhysicalAddr(c->start);
    HW_DPC_END = PhysicalAddr(c->p);
    uint32_t timeout = 0x2000000;
    while (!(HW_MI_INTR & MI_INTR_DP) && --timeout) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* --- address codes -------------------------------------------------------------- */

static uint32_t code(int x, int y)
{
    return (uint32_t)(x & 0xFF) << 24 | (uint32_t)(y & 0xFF) << 16 | (uint32_t)(0x80 | ((x >> 8) & 1)) << 8 | 0xE0;
}

static void fill_codes(void)
{
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            fb[y * FB_W + x] = code(x, y);
}

/* "(x,y)" for an address code, "#RRGGBB" for anything else */
static void decode(uint32_t v, char *out, size_t n)
{
    if (((v >> 8) & 0xFE) == 0x80)
        snprintf(out, n, "(%lu,%lu)", (unsigned long)((v >> 24) | ((v >> 8) & 1) << 8), (unsigned long)((v >> 16) & 0xFF));
    else
        snprintf(out, n, "#%06lX", (unsigned long)(v >> 8));
}

/* --- tests -------------------------------------------------------------------------- */

/* probe: rows PY0..PY1-1, x PX0..PX1-1 */
#define PX0 64
#define PX1 128
#define PY0 100
#define PY1 104

enum { PR_RECT, PR_RECT_WRAP, PR_TRI_LFT1, PR_TRI_LFT0 };

typedef struct {
    const char *name;
    const char *desc;
    int probe;
} test_t;

static void anchor(cbuf_t *c)
{
    som(c, SOM_PROBE);
    rect(c, 200, 50, 240, 51);          /* one row; last pixel read: (240,50) */
}

static void probe(cbuf_t *c, int kind)
{
    som(c, SOM_PROBE);
    switch (kind)
    {
    case PR_RECT:       rect(c, PX0, PY0, PX1, PY1); break;
    case PR_RECT_WRAP:  rect(c, 0, PY0, FB_W, PY1); break;
    case PR_TRI_LFT1:   tri_rect(c, PX0, PY0, PX1, PY1, 1); break;
    case PR_TRI_LFT0:   tri_rect(c, PX0, PY0, PX1, PY1, 0); break;
    }
}

static const test_t tests[] = {
    { "direct",     "anchor, probe",                                 PR_RECT },
    { "syncfull",   "anchor, Sync_Full (new list), probe",           PR_RECT },
    { "fill1",      "anchor, fill mode rect 320x1 at y 150",         PR_RECT },
    { "fillsmall",  "anchor, fill mode rect 8x1 at (8,150)",         PR_RECT },
    { "fill240",    "anchor, 240 fill mode rows (like the demo)",    PR_RECT },
    { "1cyc_read",  "anchor, 1-cycle rect with image read 20..59 y 60", PR_RECT },
    { "1cyc_prim",  "anchor, 1-cycle PRIM rect, no image read, y 62", PR_RECT },
    { "2cyc_nord",  "anchor, 2-cycle rect without image read, y 64", PR_RECT },
    { "syncpipe",   "anchor, Sync_Pipe",                             PR_RECT },
    { "som_fill",   "anchor, Set_Other_Modes fill, then probe mode", PR_RECT },
    { "colorimage", "anchor, Set_Color_Image (same buffer)",         PR_RECT },
    { "anchorwrap", "anchor ending at right edge (200..319, y 50)",  PR_RECT },
    { "probewrap",  "anchor, probe spans x 0..319",                  PR_RECT_WRAP },
    { "tri_lft1",   "anchor, probe as triangle, left major",         PR_TRI_LFT1 },
    { "tri_lft0",   "anchor, probe as triangle, right major",        PR_TRI_LFT0 },
};
#define N_TESTS ((int)(sizeof tests / sizeof tests[0]))

static void build_and_run(int t)
{
    begin(&cb);
    if (t == 11)
    {
        som(&cb, SOM_PROBE);
        rect(&cb, 200, 50, FB_W, 51);   /* last pixel read: (320,50) = (0,51) */
    }
    else
        anchor(&cb);

    switch (t)
    {
    case 1:
        run(&cb);
        begin(&cb);
        break;
    case 2:  som(&cb, SOM_FILL); emit(&cb, CMD(0x37) | FILL_COLOR); fill_rect(&cb, 0, 150, FB_W - 1, 150); break;
    case 3:  som(&cb, SOM_FILL); emit(&cb, CMD(0x37) | FILL_COLOR); fill_rect(&cb, 8, 150, 15, 150); break;
    case 4:
        som(&cb, SOM_FILL);
        emit(&cb, CMD(0x37) | FILL_COLOR);
        for (int y = 0; y < FB_H; y++)
            if (y < PY0 - 2 || y >= PY1 + 2)        /* keep the probe area intact */
                fill_rect(&cb, 0, y, FB_W - 1, y);
        break;
    case 5:  som(&cb, SOM_1C_READ); rect(&cb, 20, 60, 60, 61); break;
    case 6:  som(&cb, SOM_1C_PRIM); rect(&cb, 20, 62, 60, 63); break;
    case 7:  som(&cb, SOM_PROBE_NORD); rect(&cb, 20, 64, 60, 65); break;
    case 8:  emit(&cb, CMD(0x27)); break;
    case 9:  som(&cb, SOM_FILL); break;
    case 10: emit(&cb, CMD(0x27)); emit(&cb, CMD(0x3F) | 3ull << 51 | (uint64_t)(FB_W - 1) << 32 | PhysicalAddr(fb)); break;
    default: break;
    }
    probe(&cb, tests[t].probe);
    run(&cb);
}

/* --- output ---------------------------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } text_t;
static text_t out;

static void tprintf(text_t *t, const char *fmt, ...)
{
    va_list ap;
    if (t->cap - t->len < 512)
    {
        t->cap = t->cap ? t->cap * 2 : 65536;
        t->buf = realloc(t->buf, t->cap);
    }
    va_start(ap, fmt);
    t->len += vsnprintf(t->buf + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
}

static void show(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    console_render();
}

/* first pixel of a probe row in drawing order */
static int first_x(int kind) { return kind == PR_RECT_WRAP ? 0 : kind == PR_TRI_LFT0 ? PX1 - 1 : PX0; }
static int second_x(int kind) { return kind == PR_RECT_WRAP ? 1 : kind == PR_TRI_LFT0 ? PX1 - 2 : PX0 + 1; }

typedef struct { uint32_t v; int n; } tally_t;

static void tally_add(tally_t *t, int *n, uint32_t v)
{
    for (int i = 0; i < *n; i++)
        if (t[i].v == v) { t[i].n++; return; }
    if (*n < 4) { t[*n].v = v; t[*n].n = 1; (*n)++; }
}

static int save_to_sd(void)
{
    char path[64];
    struct stat st;
    int n;

    if (!debug_init_sdfs("sd:/", -1)) { show("NO SD CARD - RESULTS NOT SAVED\n"); return 0; }
    mkdir("sd:/rdp_carry", 0777);
    for (n = 1; n < 1000; n++)
    {
        snprintf(path, sizeof path, "sd:/rdp_carry/rdp_carry_%03d.txt", n);
        if (stat(path, &st) != 0) break;
    }
    FILE *f = fopen(path, "w");
    int ok = f && fwrite(out.buf, 1, out.len, f) == out.len;
    if (f && fclose(f) != 0) ok = 0;
    show(ok ? "SAVED %s\n" : "SD WRITE FAILED %s\n", path);
    debug_close_sdfs();
    return ok;
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    fb = malloc_uncached_aligned(64, FB_W * FB_H * 4);
    cb.start = malloc_uncached_aligned(64, 16384);

    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    show("RDP CARRY PROBE v" VERSION "  %s\n", get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL");
    show("ROW0 = CARRIED IN, ROW1 = FROM ROW0\n");

    tprintf(&out, "# rdp_carry_probe v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n",
            (unsigned long)HW_MI_VERSION, get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL");
    tprintf(&out, "# probe pixels show the memory colour the first blender cycle used; (x,y) = address code\n");
    tprintf(&out, "# probe rect x %d..%d, rows %d..%d; anchor's last read (240,50)\n", PX0, PX1 - 1, PY0, PY1 - 1);
    tprintf(&out, "test\tname\trun\trow\tfirst_x\tfirst\tfirst_raw\tsecond\tsecond_raw\n");

    for (int t = 0; t < N_TESTS; t++)
    {
        tally_t r0[4], r1[4];
        int n0 = 0, n1 = 0;
        int kind = tests[t].probe, fx = first_x(kind), sx = second_x(kind);

        for (int r = 0; r < RUNS; r++)
        {
            fill_codes();
            /* vary the start time a little: the CPU and VI keep running */
            wait_ticks(r * 997);
            build_and_run(t);

            for (int row = 0; row < PY1 - PY0; row++)
            {
                uint32_t v1 = fb[(PY0 + row) * FB_W + fx], v2 = fb[(PY0 + row) * FB_W + sx];
                char d1[24], d2[24];
                decode(v1, d1, sizeof d1);
                decode(v2, d2, sizeof d2);
                tprintf(&out, "%d\t%s\t%d\t%d\t%d\t%s\t%08lX\t%s\t%08lX\n", t, tests[t].name, r, PY0 + row, fx,
                        d1, (unsigned long)v1, d2, (unsigned long)v2);
                if (row == 0) tally_add(r0, &n0, v1);
                if (row == 1) tally_add(r1, &n1, v1);
            }
        }

        char line[96] = "", d[24];
        for (int i = 0; i < n0; i++)
        {
            decode(r0[i].v, d, sizeof d);
            snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i ? " " : "", d);
            if (n0 > 1) snprintf(line + strlen(line), sizeof line - strlen(line), "x%d", r0[i].n);
        }
        decode(r1[0].v, d, sizeof d);
        show("%-10s %-15s %s%s\n", tests[t].name, line, d, n1 > 1 ? "*" : "");
    }

    show("\n");
    save_to_sd();
    show("DONE\n");
    for (;;) {}
}

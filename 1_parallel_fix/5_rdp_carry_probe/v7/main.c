/*
 * rdp_carry_probe v7 - the odd memory colour after fill mode, part 3
 *
 * v6 (NTSC and PAL): after fill or copy mode, in about 3% of runs the first
 * pixel of the next 2-cycle primitive (and only that pixel) gets the memory
 * value read 12 pixels before the last pixel the list reads, counting back
 * across rows. With two 2-cycle primitives after the fill, NTSC put the
 * second one's value into the first one's first pixel (27/512); PAL never
 * did (0/512). 32-bit framebuffers get the constant 000C00E0.
 *
 * v7 asks:
 *  A  is the source "12 reads before the end of the list"? A primitive is
 *     drawn after the probe: 2-cycle without image read, 1-cycle with image
 *     read, a second 2-cycle probe, or a fill rectangle.
 *  B  the NTSC/PAL difference: v6's mid_probe2 again (1024 runs), with the
 *     middle primitive above the probe, and narrower.
 *  C  narrow probes, 1..7 pixels (4 rows), and 2 and 4 pixels with 8 rows:
 *     how many read slots does a short row take?
 *
 * Method as in v6. Results: on screen and in
 * sd:/rdp_carry/rdp_carry_v7_NNN.txt.
 * The RDP is driven directly through the DP registers (no RSP microcode).
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#define VERSION     "7"
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
#define HW_VI_CURRENT   REG(0xA4400010)
#define DPC_CLR_XBUS    0x001
#define DPC_CLR_FREEZE  0x004
#define DPC_CLR_FLUSH   0x010
#define DPC_PENDING     0x600
#define MI_INTR_DP      0x20
#define MI_CLEAR_DP_INT 0x800

/* --- RDP commands -------------------------------------------------------------- */

#define CMD(op)         ((uint64_t)(op) << 56)
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)

#define SOM_1CYC        (0ull << 52)
#define SOM_2CYC        (1ull << 52)
#define SOM_FILL        (3ull << 52)
#define SOM_NO_DITHER   (0xFull << 36)
#define SOM_FORCE_BLEND (1ull << 14)
#define SOM_IMAGE_READ  (1ull << 6)
#define BL(p0, p1, a0, a1, m0, m1, b0, b1) \
    ((uint64_t)(p0) << 30 | (uint64_t)(p1) << 28 | (uint64_t)(a0) << 26 | (uint64_t)(a1) << 24 | \
     (uint64_t)(m0) << 22 | (uint64_t)(m1) << 20 | (uint64_t)(b0) << 18 | (uint64_t)(b1) << 16)
/* 2-cycle: cycle 0 outputs its memory input, cycle 1 passes it through */
#define SOM_PROBE       (SOM_2CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | SOM_IMAGE_READ | BL(1, 0, 3, 3, 1, 0, 2, 2))
/* 1-cycle: outputs the memory colour of the pixel itself */
#define SOM_PROBE1      (SOM_1CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | SOM_IMAGE_READ | BL(1, 1, 3, 3, 1, 1, 2, 2))
/* 2-cycle without image read: writes the combiner colour */
#define SOM_NOREAD2     (SOM_2CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | BL(0, 0, 3, 3, 0, 0, 2, 2))

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

static uint8_t *big;            /* address-coded block (uncached) */
static uint32_t big_size;
static uint8_t *fb;             /* framebuffer inside big */
static int fb16;
static cbuf_t cb;

static void begin(cbuf_t *c)
{
    c->p = c->start;
    emit(c, CMD(0x3F) | (fb16 ? 2ull : 3ull) << 51 | (uint64_t)(FB_W - 1) << 32 | PhysicalAddr(fb));
    emit(c, CMD(0x2D) | FX2(FB_W) << 12 | FX2(FB_H));
    emit(c, combine_prim());
    emit(c, CMD(0x3A) | 0x102030FFu);
}

static void som(cbuf_t *c, uint64_t mode) { emit(c, CMD(0x27)); emit(c, CMD(0x2F) | mode); }

/* 1/2-cycle rectangle: x1, y1 exclusive */
static void rect(cbuf_t *c, int x0, int y0, int x1, int y1)
{
    emit(c, CMD(0x36) | FX2(x1) << 44 | FX2(y1) << 32 | FX2(x0) << 12 | FX2(y0));
}

static uint32_t fill_word(uint32_t rgba32)
{
    if (!fb16) return rgba32;
    uint32_t p = (rgba32 >> 27) << 11 | ((rgba32 >> 19) & 31) << 6 | ((rgba32 >> 11) & 31) << 1 | 1;
    return p << 16 | p;
}

static uint32_t vi_line;

static void run(cbuf_t *c)
{
    emit(c, CMD(0x29));
    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PhysicalAddr(c->start);
    vi_line = (HW_VI_CURRENT & 0x3FF) >> 1;
    HW_DPC_END = PhysicalAddr(c->p);
    uint32_t timeout = 0x2000000;
    while (!(HW_MI_INTR & MI_INTR_DP) && --timeout) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* --- address codes -------------------------------------------------------------- */

/* 32-bit word at physical address a: RGB = a >> 2, alpha byte 0xE0
 * 16-bit halfword at physical address a: address bits 1..15, coverage bit 1 */
static uint32_t code32(uint32_t a) { return ((a >> 2) & 0x3FFFFF) << 8 | 0xE0; }
static uint16_t code16(uint32_t a) { return (uint16_t)(((a >> 1) & 0x7FFF) << 1 | 1); }

static void code_range(uint8_t *p, uint32_t len)
{
    uint32_t a = PhysicalAddr(p);
    if (fb16) for (uint32_t i = 0; i < len; i += 2) *(uint16_t *)(p + i) = code16(a + i);
    else      for (uint32_t i = 0; i < len; i += 4) *(uint32_t *)(p + i) = code32(a + i);
}

static uint32_t read_px(int x, int y)
{
    return fb16 ? ((uint16_t *)fb)[y * FB_W + x] : ((uint32_t *)fb)[y * FB_W + x];
}

/* pixel a value names, as an index from the framebuffer start (16-bit codes
 * are unique for the first 102 rows), or -1 when it is not an address code */
static int32_t source_px(uint32_t v)
{
    if (fb16)
    {
        uint32_t rel = ((((v >> 1) & 0x7FFF) << 1) - (PhysicalAddr(fb) & 0xFFFF)) & 0xFFFF;
        return (int32_t)(rel >> 1);
    }
    if ((v & 0xFF) != 0xE0) return -1;
    int32_t rel = (int32_t)(((v >> 8) << 2) - PhysicalAddr(fb));
    return (rel >= 0 && rel < FB_W * FB_H * 4) ? rel >> 2 : -1;
}

/* --- tests -------------------------------------------------------------------------- */

enum { PRE_FILL, PRE_COPY, PRE_NONE };
/* extra primitives, drawn before (mid) or after (tail) the probe */
enum { X_NONE, X_IR1, X_NOREAD2, X_PROBE2, X_FILL };

typedef struct {
    const char *name;
    int runs;
    int mid, mid_x0, mid_x1, mid_y;     /* primitive between fill and probe */
    int tail;                           /* primitive after the probe, row TAIL_Y */
    int x0, x1, y0, y1;                 /* probe rectangle (x1, y1 exclusive) */
} test_t;

#define PY0 20
#define TAIL_Y 44
#define TAIL_X0 200
#define TAIL_X1 264

static const test_t tests[] = {
    /* name           runs  mid        mx0  mx1  my   tail        x0   x1   y0    y1 */
    { "base",          512, X_NONE,      0,   0,  0, X_NONE,     64, 128, PY0, PY0 + 4 },
    /* A: is the source "12 reads before the end of the list"? */
    { "tail_noread2",  512, X_NONE,      0,   0,  0, X_NOREAD2,  64, 128, PY0, PY0 + 4 },
    { "tail_ir1",      512, X_NONE,      0,   0,  0, X_IR1,      64, 128, PY0, PY0 + 4 },
    { "tail_probe2",   512, X_NONE,      0,   0,  0, X_PROBE2,   64, 128, PY0, PY0 + 4 },
    { "tail_fill",     512, X_NONE,      0,   0,  0, X_FILL,     64, 128, PY0, PY0 + 4 },
    /* B: the NTSC/PAL difference of v6's mid_probe2 */
    { "mid_probe2",   1024, X_PROBE2,  200, 264, 40, X_NONE,     64, 128, PY0, PY0 + 4 },
    { "mid_row10",    1024, X_PROBE2,  200, 264, 10, X_NONE,     64, 128, PY0, PY0 + 4 },
    { "mid_w16",      1024, X_PROBE2,  200, 216, 40, X_NONE,     64, 128, PY0, PY0 + 4 },
    /* C: narrow probes */
    { "w1",            512, X_NONE,      0,   0,  0, X_NONE,     64,  65, PY0, PY0 + 4 },
    { "w2",            512, X_NONE,      0,   0,  0, X_NONE,     64,  66, PY0, PY0 + 4 },
    { "w3",            512, X_NONE,      0,   0,  0, X_NONE,     64,  67, PY0, PY0 + 4 },
    { "w4",            512, X_NONE,      0,   0,  0, X_NONE,     64,  68, PY0, PY0 + 4 },
    { "w5",            512, X_NONE,      0,   0,  0, X_NONE,     64,  69, PY0, PY0 + 4 },
    { "w6",            512, X_NONE,      0,   0,  0, X_NONE,     64,  70, PY0, PY0 + 4 },
    { "w7",            512, X_NONE,      0,   0,  0, X_NONE,     64,  71, PY0, PY0 + 4 },
    { "w2_rows8",      512, X_NONE,      0,   0,  0, X_NONE,     64,  66, PY0, PY0 + 8 },
    { "w4_rows8",      512, X_NONE,      0,   0,  0, X_NONE,     64,  68, PY0, PY0 + 8 },
};
#define N_TESTS ((int)(sizeof tests / sizeof tests[0]))

static int skip_row(const test_t *t, int y)
{
    return y == 50 || y == TAIL_Y || (t->mid && y == t->mid_y) || (y >= t->y0 - 2 && y < t->y1 + 2);
}

static void emit_pre(cbuf_t *c, const test_t *t)
{
    som(c, SOM_FILL);
    for (int y = 0; y < FB_H; y++)
    {
        if (skip_row(t, y)) continue;
        emit(c, CMD(0x27));
        emit(c, CMD(0x37) | fill_word(y & 1 ? 0x0E0DEC1Fu : 0xF1F213E0u));
        emit(c, CMD(0x36) | FX2(FB_W - 1) << 44 | FX2(y) << 32 | FX2(y));
    }
}

static void emit_extra(cbuf_t *c, int kind, int x0, int x1, int y)
{
    switch (kind)
    {
    case X_IR1:     som(c, SOM_PROBE1);  rect(c, x0, y, x1, y + 1); break;
    case X_NOREAD2: som(c, SOM_NOREAD2); rect(c, x0, y, x1, y + 1); break;
    case X_PROBE2:  som(c, SOM_PROBE);   rect(c, x0, y, x1, y + 1); break;
    case X_FILL:
        som(c, SOM_FILL);
        emit(c, CMD(0x37) | fill_word(0x80808080u));
        emit(c, CMD(0x36) | FX2(x1 - 1) << 44 | FX2(y) << 32 | FX2(x0) << 12 | FX2(y));
        break;
    }
}

static void build_and_run(const test_t *t)
{
    begin(&cb);
    som(&cb, SOM_PROBE);
    rect(&cb, 200, 50, 240, 51);                    /* anchor: last read (240,50) */
    emit_pre(&cb, t);
    emit_extra(&cb, t->mid, t->mid_x0, t->mid_x1, t->mid_y);
    som(&cb, SOM_PROBE);
    rect(&cb, t->x0, t->y0, t->x1, t->y1);
    emit_extra(&cb, t->tail, TAIL_X0, TAIL_X1, TAIL_Y);
    run(&cb);
}

/* --- compared area: the probe rectangle and the extra primitives' rows ----------------- */

#define MAX_AREA 1024
typedef struct { int16_t x, y; } pt_t;
static pt_t area[MAX_AREA];
static int n_area;
static uint32_t img[3][MAX_AREA], ref[MAX_AREA], cur[MAX_AREA];

static void set_area(const test_t *t)
{
    n_area = 0;
    for (int y = t->y0; y < t->y1; y++)
        for (int x = t->x0; x < t->x1 + 1 && x < FB_W; x++)    /* one column past the right edge */
            area[n_area++] = (pt_t){ (int16_t)x, (int16_t)y };
    if (t->mid)
        for (int x = t->mid_x0; x <= t->mid_x1; x++)
            area[n_area++] = (pt_t){ (int16_t)x, (int16_t)t->mid_y };
    if (t->tail)
        for (int x = TAIL_X0; x <= TAIL_X1; x++)
            area[n_area++] = (pt_t){ (int16_t)x, (int16_t)TAIL_Y };
}

static void grab(uint32_t *dst)
{
    for (int i = 0; i < n_area; i++) dst[i] = read_px(area[i].x, area[i].y);
}

/* --- output ---------------------------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } text_t;
static text_t out;
#define OUT_CAP     (384 * 1024)

static void tprintf(const char *fmt, ...)
{
    va_list ap;
    if (!out.buf || out.cap - out.len < 512) return;
    va_start(ap, fmt);
    out.len += vsnprintf(out.buf + out.len, out.cap - out.len, fmt, ap);
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
    if (strchr(buf, '\n')) console_render();
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
        snprintf(path, sizeof path, "sd:/rdp_carry/rdp_carry_v7_%03d.txt", n);
        if (stat(path, &st) != 0) break;
    }
    disable_interrupts();
    FILE *f = fopen(path, "w");
    int ok = f && fwrite(out.buf, 1, out.len, f) == out.len;
    if (f && fclose(f) != 0) ok = 0;
    enable_interrupts();
    show(ok ? "SAVED %s\n" : "SD WRITE FAILED %s\n", path);
    debug_close_sdfs();
    return ok;
}

/* one differing pixel: x,y=value(source x,y) */
static void print_diff(int i, uint32_t v)
{
    int32_t s = source_px(v);
    if (s >= 0)
        tprintf("\t%d,%d=%0*lX(%ld,%ld)", area[i].x, area[i].y, fb16 ? 4 : 8, (unsigned long)v,
                (long)(s % FB_W), (long)(s / FB_W));
    else
        tprintf("\t%d,%d=%0*lX(-)", area[i].x, area[i].y, fb16 ? 4 : 8, (unsigned long)v);
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    cb.start = malloc_uncached_aligned(64, 32768);
    out.buf = malloc(OUT_CAP);
    out.cap = out.buf ? OUT_CAP : 0;
    big_size = FB_W * FB_H * 4 + 0x10000;
    big = malloc_uncached_aligned(64, big_size);
    if (!big || !out.buf || !cb.start) { show("OUT OF MEMORY\n"); for (;;) {} }

    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    const char *tv = get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL";
    tprintf("# rdp_carry_probe v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n# memory\t%luMB\n",
            (unsigned long)HW_MI_VERSION, tv, (unsigned long)(get_memory_size() >> 20));
    tprintf("# probe rows %d.., anchor reads (240,50) last; codes: 16-bit = address bits 1..15, 32-bit = (address>>2)<<8 | E0\n", PY0);
    tprintf("# odd lines: test, run, VI line at submission, number of differing pixels, then x,y=value(source pixel)\n");
    tprintf("# normal lines: the first pixels of the normal result, for reference\n");
    show("RDP CARRY PROBE v" VERSION "  %s\n", tv);

    fb = big;
    for (int ti = 0; ti < N_TESTS; ti++)
    {
        const test_t *t = &tests[ti];
        fb16 = 1;
        set_area(t);
        int odd = 0;
        int32_t src_seen[8];
        int src_count[8], n_src = 0;

        /* normal result: majority of three runs */
        for (int r = 0; r < 3; r++)
        {
            code_range(fb, FB_W * FB_H * (fb16 ? 2 : 4));
            build_and_run(t);
            grab(img[r]);
        }
        int pick = (memcmp(img[0], img[1], n_area * 4) == 0 || memcmp(img[0], img[2], n_area * 4) == 0) ? 0 : 1;
        memcpy(ref, img[pick], n_area * 4);
        tprintf("normal\t%s", t->name);
        for (int i = 0; i < n_area && i < 4; i++) print_diff(i, ref[i]);
        for (int i = 0; i < n_area; i++)
            if (area[i].y == t->mid_y && t->mid) { print_diff(i, ref[i]); break; }
        for (int i = 0; i < n_area; i++)
            if (area[i].y == TAIL_Y && t->tail) { print_diff(i, ref[i]); break; }
        tprintf("\n");

        for (int r = 0; r < t->runs; r++)
        {
            code_range(fb, FB_W * FB_H * (fb16 ? 2 : 4));
            wait_ticks((r & 15) * 997);
            build_and_run(t);
            grab(cur);
            int nd = 0;
            for (int i = 0; i < n_area; i++) nd += cur[i] != ref[i];
            if (!nd) continue;
            odd++;
            tprintf("odd\t%s\t%d\t%lu\t%d", t->name, r, (unsigned long)vi_line, nd);
            int shown = 0;
            for (int i = 0; i < n_area && shown < 8; i++)
                if (cur[i] != ref[i])
                {
                    if (!shown)
                    {
                        /* tally the source of the first differing pixel */
                        int32_t s = source_px(cur[i]);
                        int k;
                        for (k = 0; k < n_src && src_seen[k] != s; k++) {}
                        if (k == n_src && n_src < 8) { src_seen[n_src] = s; src_count[n_src++] = 0; }
                        if (k < n_src) src_count[k]++;
                    }
                    print_diff(i, cur[i]);
                    shown++;
                }
            tprintf("\n");
        }

        tprintf("summary\t%s\t%d/%d\n", t->name, odd, t->runs);
        show("%-12s %3d/%d", t->name, odd, t->runs);
        if (n_src)
        {
            /* most common source of the first differing pixel */
            int best = 0;
            for (int k = 1; k < n_src; k++) if (src_count[k] > src_count[best]) best = k;
            if (src_seen[best] >= 0)
                show(" (%ld,%ld)x%d", (long)(src_seen[best] % FB_W), (long)(src_seen[best] / FB_W), src_count[best]);
            else
                show(" const x%d", src_count[best]);
            if (n_src > 1) show("+");
        }
        show("\n");
    }

    show("\n");
    save_to_sd();
    show("DONE\n");
    for (;;) {}
}

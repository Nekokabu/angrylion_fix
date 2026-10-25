/*
 * rdp_carry_probe v3 - where does the odd value after fill mode come from?
 *
 * v2 showed that after fill mode drawing, the next 2-cycle primitive
 * sometimes (3-6%) starts with a constant memory colour instead of the one
 * carried over from before the fill: 000C00E0 on a 32-bit framebuffer, 99C7
 * on a 16-bit one, independent of fill colour, count and the commands in
 * between, and not read from any pixel of the framebuffer.
 *
 * v3 tells apart "the RDP read some other RDRAM address" from "the value
 * comes from inside the RDP":
 *
 *  1. Before anything else, all of RDRAM is searched for the two values and
 *     the addresses are listed (32-bit words / 16-bit halfwords).
 *  2. A large heap block is filled with ADDRESS codes: every 32-bit word
 *     holds its own physical address (RGB = address >> 2), or, for 16-bit
 *     tests, every halfword holds address bits 1..15. The framebuffer lives
 *     inside that block, at two different offsets.
 *  3. The anchor / fill / probe sequence of v2 runs 128 times per test. If an
 *     odd value is an address code, it names the address the RDP read; if it
 *     is still 000C00E0 / 99C7 (or anything else), it did not come from the
 *     coded memory.
 *
 * Results: on screen and in sd:/rdp_carry/rdp_carry_v5_NNN.txt.
 * The RDP is driven directly through the DP registers (no RSP microcode).
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define VERSION     "5"
#define RUNS        256
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
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)

#define SOM_2CYC        (1ull << 52)
#define SOM_FILL        (3ull << 52)
#define SOM_NO_DITHER   (0xFull << 36)
#define SOM_FORCE_BLEND (1ull << 14)
#define SOM_IMAGE_READ  (1ull << 6)
#define BL(p0, p1, a0, a1, m0, m1, b0, b1) \
    ((uint64_t)(p0) << 30 | (uint64_t)(p1) << 28 | (uint64_t)(a0) << 26 | (uint64_t)(a1) << 24 | \
     (uint64_t)(m0) << 22 | (uint64_t)(m1) << 20 | (uint64_t)(b0) << 18 | (uint64_t)(b1) << 16)
#define SOM_PROBE       (SOM_2CYC | SOM_NO_DITHER | SOM_FORCE_BLEND | SOM_IMAGE_READ | BL(1, 0, 3, 3, 1, 0, 2, 2))

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

static void run(cbuf_t *c)
{
    emit(c, CMD(0x29));
    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PhysicalAddr(c->start);
    HW_DPC_END = PhysicalAddr(c->p);
    uint32_t timeout = 0x2000000;
    while (!(HW_MI_INTR & MI_INTR_DP) && --timeout) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* --- address codes -------------------------------------------------------------- */

/* 32-bit word at physical address a: RGB = a >> 2 (22 bits), alpha byte 0xE0 */
static uint32_t code32(uint32_t a) { return ((a >> 2) & 0x3FFFFF) << 8 | 0xE0; }
static uint32_t decode32(uint32_t v) { return (v >> 8) << 2; }
/* 16-bit halfword at physical address a: address bits 1..15, coverage bit 1 */
/* two 16-bit codings: "lo" holds address bits 1..15, "hi" holds bits 8..22;
 * an address seen in both (or one plus the known layout) is fully known */
static int hicode;
static uint16_t code16(uint32_t a)
{
    return hicode ? (uint16_t)(((a >> 8) & 0x7FFF) << 1 | 1) : (uint16_t)(((a >> 1) & 0x7FFF) << 1 | 1);
}
static uint32_t decode16(uint32_t v)
{
    return hicode ? ((v >> 1) & 0x7FFF) << 8 : ((v >> 1) & 0x7FFF) << 1;
}

static void code_range(uint8_t *p, uint32_t len)
{
    uint32_t a = PhysicalAddr(p);
    if (fb16) for (uint32_t i = 0; i < len; i += 2) *(uint16_t *)(p + i) = code16(a + i);
    else      for (uint32_t i = 0; i < len; i += 4) *(uint32_t *)(p + i) = code32(a + i);
}

static uint32_t px_addr(int x, int y) { return PhysicalAddr(fb) + (uint32_t)(y * FB_W + x) * (fb16 ? 2 : 4); }

static uint32_t read_px(int x, int y)
{
    return fb16 ? ((uint16_t *)fb)[y * FB_W + x] : ((uint32_t *)fb)[y * FB_W + x];
}

/* --- tests -------------------------------------------------------------------------- */

/* probe rectangle of the current test: x px0..px1-1, rows py0..py1-1 */
static int px0, px1, py0, py1;
#define PX0 px0
#define PX1 px1
#define PY0 py0
#define PY1 py1

typedef struct {
    const char *name;
    int f16;
    uint32_t fb_offset;     /* framebuffer offset inside big */
    int nfill;              /* fill rows (Sync_Pipe + colour change each, like the demo) */
    int hi;                 /* 16-bit: code address bits 8..22 instead of 1..15 */
    int x0, x1, y0, y1;     /* probe rectangle (x1, y1 exclusive) */
} test_t;

static const test_t tests[] = {
    /* name          f16 off fill hi   x0   x1   y0   y1 */
    { "base_lo",      1, 0, 240, 0,   64, 128, 100, 104 },
    { "base_hi",      1, 0, 240, 1,   64, 128, 100, 104 },
    { "x32_lo",       1, 0, 240, 0,   32,  96, 100, 104 },
    { "x32_hi",       1, 0, 240, 1,   32,  96, 100, 104 },
    { "wide_lo",      1, 0, 240, 0,   64, 192, 100, 104 },
    { "wide_hi",      1, 0, 240, 1,   64, 192, 100, 104 },
    { "rows2_lo",     1, 0, 240, 0,   64, 128, 100, 102 },
    { "rows2_hi",     1, 0, 240, 1,   64, 128, 100, 102 },
    { "rows8_lo",     1, 0, 240, 0,   64, 128, 100, 108 },
    { "rows8_hi",     1, 0, 240, 1,   64, 128, 100, 108 },
    { "y150_lo",      1, 0, 240, 0,   64, 128, 150, 154 },
    { "y150_hi",      1, 0, 240, 1,   64, 128, 150, 154 },
    { "fb32_base",    0, 0, 240, 0,   64, 128, 100, 104 },
    { "fb32_x32",     0, 0, 240, 0,   32,  96, 100, 104 },
    { "fb32_wide",    0, 0, 240, 0,   64, 192, 100, 104 },
};
#define N_TESTS ((int)(sizeof tests / sizeof tests[0]))

static void build_and_run(const test_t *t)
{
    begin(&cb);
    som(&cb, SOM_PROBE);
    rect(&cb, 200, 50, 240, 51);                    /* anchor: last read (240,50) */

    if (t->nfill)
    {
        som(&cb, SOM_FILL);
        int y = 0;
        for (int i = 0; i < t->nfill; i++)
        {
            int yy;
            while ((yy = y % FB_H) == 50 || (yy >= PY0 - 2 && yy < PY1 + 2)) y++;
            emit(&cb, CMD(0x27));
            emit(&cb, CMD(0x37) | fill_word(i & 1 ? 0x0E0DEC1Fu : 0xF1F213E0u));
            emit(&cb, CMD(0x36) | FX2(FB_W - 1) << 44 | FX2(yy) << 32 | FX2(yy));
            y++;
        }
    }

    som(&cb, SOM_PROBE);
    rect(&cb, PX0, PY0, PX1, PY1);
    run(&cb);
}

/* --- output ---------------------------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } text_t;
static text_t out;

/* the buffer is allocated once, before the big block takes the heap */
#define OUT_CAP     (384 * 1024)

static void tprintf(text_t *t, const char *fmt, ...)
{
    va_list ap;
    if (!t->buf || t->cap - t->len < 512)
        return;                         /* full: drop the rest */
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

/* both: screen and file */
static void report(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    show("%s", buf);
    tprintf(&out, "# %s", buf);
}

static void scan_rdram(void)
{
    uint32_t size = get_memory_size();
    int n32 = 0, n16 = 0;
    uint32_t big0 = PhysicalAddr(big), big1 = big0 + big_size;

    report("RDRAM %luMB, CODED BLOCK %06lX-%06lX\n", (unsigned long)(size >> 20),
           (unsigned long)big0, (unsigned long)big1);
    tprintf(&out, "# scan of RDRAM before coding (addresses outside the coded block)\n");
    for (uint32_t a = 0; a < size; a += 2)
    {
        if (a >= big0 && a < big1) continue;
        uint16_t h = *(volatile uint16_t *)(0xA0000000 | a);
        if (h == 0x99C7)
        {
            if (n16 < 64) tprintf(&out, "scan16\t%06lX\n", (unsigned long)a);
            n16++;
        }
        if (!(a & 3) && *(volatile uint32_t *)(0xA0000000 | a) == 0x000C00E0u)
        {
            if (n32 < 64) tprintf(&out, "scan32\t%06lX\n", (unsigned long)a);
            n32++;
        }
    }
    report("SCAN: 000C00E0 x%d, 99C7 x%d\n", n32, n16);

    /* the top 4 KB of RDRAM (stack), for matching against the scan hits */
    tprintf(&out, "# top 4 KB of RDRAM\n");
    for (uint32_t a = size - 4096; a < size; a += 32)
    {
        tprintf(&out, "top\t%06lX", (unsigned long)a);
        for (int k = 0; k < 32; k += 4)
            tprintf(&out, " %08lX", (unsigned long)*(volatile uint32_t *)(0xA0000000 | (a + k)));
        tprintf(&out, "\n");
    }
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
        snprintf(path, sizeof path, "sd:/rdp_carry/rdp_carry_v5_%03d.txt", n);
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

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    cb.start = malloc_uncached_aligned(64, 32768);
    /* the largest block the heap gives us, at least 1.5 MB */
    out.buf = malloc(OUT_CAP);
    out.cap = out.buf ? OUT_CAP : 0;

    /* the largest block the heap gives us, minus 512 KB left free for the
     * SD card code and the C library */
    for (big_size = 6u << 20; big_size >= 0x180000 && !big; big_size -= 0x40000)
        big = malloc_uncached_aligned(64, big_size);
    if (big)
    {
        free_uncached(big);
        big_size += 0x40000 - 0x80000;
        big = malloc_uncached_aligned(64, big_size);
    }
    if (!big || !out.buf) { show("OUT OF MEMORY\n"); for (;;) {} }

    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    const char *tv = get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL";
    tprintf(&out, "# rdp_carry_probe v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n",
            (unsigned long)HW_MI_VERSION, tv);
    show("RDP CARRY PROBE v" VERSION "  %s\n", tv);
    scan_rdram();
    tprintf(&out, "# code32: word at address A = (A>>2)<<8 | E0; code16 lo: halfword = ((A>>1)&7FFF)<<1 | 1; code16 hi: ((A>>8)&7FFF)<<1 | 1\n");
    tprintf(&out, "test\tname\trun\tfb_addr\trow0_raw\trow0_ok\trow0_decoded_addr\trow0_rel_fb\n");

    for (int i = 0; i < N_TESTS; i++)
    {
        const test_t *t = &tests[i];
        int odd = 0;
        uint32_t seen[4] = { 0 };
        int nseen = 0;

        fb16 = t->f16;
        hicode = t->hi;
        px0 = t->x0; px1 = t->x1; py0 = t->y0; py1 = t->y1;
        if (t->fb_offset + FB_W * FB_H * 4 > big_size) { report("%-11s SKIPPED (MEMORY)\n", t->name); continue; }
        fb = big + t->fb_offset;
        code_range(big, big_size);

        for (int r = 0; r < RUNS; r++)
        {
            code_range(fb, FB_W * FB_H * (fb16 ? 2 : 4));
            wait_ticks((r & 15) * 997);
            build_and_run(t);

            uint32_t v = read_px(PX0, PY0);
            uint32_t want = fb16 ? code16(px_addr(240, 50)) : code32(px_addr(240, 50));
            int ok = fb16 ? (v & ~1u) == (want & ~1u) : (v >> 8) == (want >> 8);
            uint32_t addr = fb16 ? decode16(v) : decode32(v);
            int32_t rel = !fb16 ? (int32_t)(addr - PhysicalAddr(fb))
                        : hicode ? (int32_t)(addr - (PhysicalAddr(fb) & ~0xFFu))
                        : (int32_t)((addr - (PhysicalAddr(fb) & 0xFFFF)) & 0xFFFF);
            tprintf(&out, "%d\t%s\t%d\t%06lX\t%08lX\t%d\t%06lX\t%ld\n", i, t->name, r,
                    (unsigned long)PhysicalAddr(fb), (unsigned long)v, ok, (unsigned long)addr, (long)rel);
            if (!ok)
            {
                odd++;
                int k;
                for (k = 0; k < nseen && seen[k] != v; k++) {}
                if (k == nseen && nseen < 4) seen[nseen++] = v;
            }
        }

        /* one line: odd count, most common odd value and its decoded address */
        show("%-10s %3d/%d", t->name, odd, RUNS);
        if (nseen)
        {
            uint32_t a = fb16 ? decode16(seen[0]) : decode32(seen[0]);
            show(fb16 ? " %04lX A%06lX" : " %08lX", (unsigned long)seen[0], (unsigned long)a);
            if (nseen > 1) show("+");
        }
        show("\n");
    }

    show("\n");
    save_to_sd();
    show("DONE\n");
    for (;;) {}
}

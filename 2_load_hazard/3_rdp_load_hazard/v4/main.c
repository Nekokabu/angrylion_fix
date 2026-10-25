/*
 * rdp_load_hazard v4 - idle drain time and the STI alignment rule
 *
 * Model so far (v2/v3, NTSC = PAL):
 *  - the last 15 slots of a load stay queued (1 slot = 8 source bytes;
 *    a Load_Tile row takes max(words + 1, 9) slots)
 *  - every state command retires one slot with the tile state of that moment
 *  - syncs, draws and loads write the whole queue
 *  - with the RDP idle, the queue is written within 10..100 us
 *  - STI reach depends on the RDRAM alignment of the rows
 *
 * Parts:
 *  E  DP status bits and counters (CLOCK, BUFBUSY, PIPEBUSY, TMEM) traced
 *     while the RDP sits idle after a load
 *  D  idle time before the queue is written, in 2 us steps, repeated
 *  F  STI reach for every row width 8..64 and source offset 0..56:
 *     F1 stride = width, F2 stride 128, F3 two rows with independent
 *     alignment (24 and 40 byte rows)
 *
 * Results: sd:/rdp_load_hazard/v4_NNN.txt. The RDP is driven directly.
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#define VERSION     "4"
#define MAX_GAP     14

/* --- hardware ----------------------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_VERSION   REG(0xA4300004)
#define HW_MI_INTR      REG(0xA4300008)
#define HW_SP_STATUS    REG(0xA4040010)
#define DPC_CLR_XBUS    0x001
#define DPC_CLR_FREEZE  0x004
#define DPC_CLR_FLUSH   0x010
#define DPC_PENDING     0x600
#define MI_INTR_DP      0x20
#define MI_CLEAR_DP_INT 0x800

/* --- RDP commands -------------------------------------------------------------- */

#define CMD(op)         ((uint64_t)(op) << 56)
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)

enum { FMT_RGBA = 0, FMT_I = 4 };
enum { SZ_4 = 0, SZ_8 = 1, SZ_16 = 2, SZ_32 = 3 };

typedef struct { uint64_t *p, *start; } cbuf_t;
static inline void emit(cbuf_t *c, uint64_t w) { *c->p++ = w; }
static void syncs(cbuf_t *c) { emit(c, CMD(0x27)); emit(c, CMD(0x26)); emit(c, CMD(0x28)); }

static void set_texture_image(cbuf_t *c, int fmt, int size, int width, uint32_t addr)
{
    emit(c, CMD(0x3D) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr);
}

static void set_tile(cbuf_t *c, int tile, int fmt, int size, int line, int tmem)
{
    emit(c, CMD(0x35) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)line << 41
         | (uint64_t)tmem << 32 | (uint64_t)tile << 24);
}

static void tile_cmd(cbuf_t *c, int op, int tile, int sl, int tl, int sh, int th)
{
    emit(c, CMD(op) | FX2(sl) << 44 | FX2(tl) << 32 | (uint64_t)tile << 24 | FX2(sh) << 12 | FX2(th));
}
#define load_tile(c, t, sl, tl, sh, th)     tile_cmd(c, 0x34, t, sl, tl, sh, th)
#define set_tile_size(c, t, sl, tl, sh, th) tile_cmd(c, 0x32, t, sl, tl, sh, th)

static void load_block(cbuf_t *c, int tile, int texels, int dxt)
{
    emit(c, CMD(0x33) | (uint64_t)tile << 24 | (uint64_t)(texels - 1) << 12 | dxt);
}

static uint64_t combine_tex(void)
{
    const uint64_t a = 15, b = 15, c = 31, d = 1, aa = 7, ab = 7, ac = 7, ad = 1;
    return CMD(0x3C)
         | a << 52 | c << 47 | aa << 44 | ac << 41
         | a << 37 | c << 32 | b << 28 | b << 24
         | aa << 21 | ac << 18 | d << 15 | ab << 12
         | ad << 9 | d << 6 | ab << 3 | ad;
}

#define SOM_READBACK    ((0ull << 52) | (3ull << 42) | (0xFull << 36))

/* --- buffers --------------------------------------------------------------------- */

static uint8_t *src_a, *src_b, *zero4k;
static uint32_t *fb;
static cbuf_t cb;
static uint8_t ref_tmem[4096], test_tmem[4096];
static int timed_out;

/* run a list from RDRAM. split >= 0: submit the first `split` commands, wait
   `wait_us` microseconds, then submit the rest */
static void run_split(cbuf_t *c, int split, int wait_us)
{
    emit(c, CMD(0x29));
    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    uint32_t start = PhysicalAddr(c->start), end = PhysicalAddr(c->p);
    HW_DPC_START = start;
    if (split >= 0)
    {
        HW_DPC_END = start + (uint32_t)split * 8;
        if (wait_us > 0) wait_ticks(TICKS_FROM_US(wait_us));
    }
    HW_DPC_END = end;
    uint32_t timeout = 0x2000000;
    while (!(HW_MI_INTR & MI_INTR_DP) && --timeout) {}
    if (!timeout) timed_out = 1;
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* common list head: clear TMEM, state for the read-back and for draw fillers */
static void list_head(cbuf_t *c)
{
    c->p = c->start;
    set_texture_image(c, FMT_RGBA, SZ_16, 2048, PhysicalAddr(zero4k));
    set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0);
    emit(c, CMD(0x26));
    load_block(c, 7, 2048, 0);
    syncs(c);
    emit(c, CMD(0x3F) | 3ull << 51 | (uint64_t)(64 - 1) << 32 | PhysicalAddr(fb));
    emit(c, CMD(0x2D) | FX2(64) << 12 | FX2(64));
    emit(c, CMD(0x2F) | SOM_READBACK);
    emit(c, combine_tex());
    set_tile(c, 1, FMT_I, SZ_8, 8, 0);
    set_tile_size(c, 1, 0, 0, 63, 63);
    set_tile(c, 6, FMT_RGBA, SZ_16, 1, 0x1C0);    /* 3.5 KiB: second-load filler */
}

/* read-back of all 4 KiB of TMEM through an I8 texture */
static void list_tail_and_run(cbuf_t *c, int split, int wait_us, uint8_t *tmem_out)
{
    syncs(c);
    emit(c, CMD(0x3F) | 3ull << 51 | (uint64_t)(64 - 1) << 32 | PhysicalAddr(fb));
    emit(c, CMD(0x2D) | FX2(64) << 12 | FX2(64));
    emit(c, CMD(0x2F) | SOM_READBACK);
    emit(c, combine_tex());
    set_tile(c, 0, FMT_I, SZ_8, 8, 0);
    set_tile_size(c, 0, 0, 0, 63, 63);
    emit(c, CMD(0x24) | FX2(64) << 44 | FX2(64) << 32 | FX2(0) << 12 | FX2(0));
    emit(c, 0x400ull << 16 | 0x400ull);
    run_split(c, split, wait_us);
    for (int k = 0; k < 4096; k++)
    {
        int row = k >> 6, x = k & 63;
        tmem_out[row * 64 + (x ^ ((row & 1) ? 4 : 0))] = (uint8_t)(fb[k] >> 24);
    }
}

typedef struct { int bytes, words, first, last, ref_nz; } diff_t;
static diff_t compare(void)
{
    diff_t d = { 0, 0, -1, -1, 0 };
    for (int a = 0; a < 4096; a++) d.ref_nz += ref_tmem[a] != 0;
    for (int a = 0; a < 4096; a++)
        if (test_tmem[a] != ref_tmem[a]) { d.bytes++; if (d.first < 0) d.first = a; d.last = a; }
    for (int w = 0; w < 512; w++)
        if (memcmp(test_tmem + w * 8, ref_tmem + w * 8, 8)) d.words++;
    return d;
}

/* --- loads ------------------------------------------------------------------------- */

enum { L_TILE, L_BLOCK };
enum { CH_STI, CH_ST7 };

typedef struct {
    int load;       /* L_TILE / L_BLOCK */
    int change;     /* CH_STI / CH_ST7 */
    int size;       /* SZ_8 / SZ_16 / SZ_32 */
    int rowbytes;   /* tile: bytes per row; block: total bytes */
    int rows;
    int offset;     /* source offset in bytes */
    int stride;     /* image width in bytes (tile) */
} geo_t;

static int bpp(int size) { return size == SZ_8 ? 1 : size == SZ_16 ? 2 : 4; }
static int tile_line(const geo_t *g)
{
    /* 32-bit textures are split over the two TMEM halves: line counts one half */
    return g->size == SZ_32 ? (g->rowbytes / 2 + 7) / 8 : (g->rowbytes + 7) / 8;
}
static int fmt_of(int size) { return size == SZ_8 ? FMT_I : FMT_RGBA; }

static void emit_load(cbuf_t *c, const geo_t *g)
{
    if (g->load == L_TILE)
    {
        set_texture_image(c, fmt_of(g->size), g->size, g->stride / bpp(g->size), PhysicalAddr(src_a) + g->offset);
        set_tile(c, 7, fmt_of(g->size), g->size, tile_line(g), 0);
        emit(c, CMD(0x26));
        load_tile(c, 7, 0, 0, g->rowbytes / bpp(g->size) - 1, g->rows - 1);
    }
    else
    {
        set_texture_image(c, FMT_RGBA, SZ_16, g->rowbytes / 2, PhysicalAddr(src_a));
        set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0);
        emit(c, CMD(0x26));
        load_block(c, 7, g->rowbytes / 2, 0);
    }
}

static void emit_change(cbuf_t *c, const geo_t *g)
{
    if (g->change == CH_STI)
        set_texture_image(c, fmt_of(g->size), g->size, g->stride / bpp(g->size), PhysicalAddr(src_b) + g->offset);
    else if (g->load == L_TILE)
        set_tile(c, 7, fmt_of(g->size), g->size, tile_line(g), 0x80);       /* +1 KiB */
    else
        set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0x80);
}

/* --- output ---------------------------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } text_t;
static text_t out;

static void tprintf(const char *fmt, ...)
{
    va_list ap;
    if (out.cap - out.len < 512) return;
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
    if (strchr(buf, '\n')) console_render();     /* render once per line */
}


/* --- parts ---------------------------------------------------------------------------- */

#define HW_DPC_CLOCK    REG(0xA4100010)
#define HW_DPC_BUFBUSY  REG(0xA4100014)
#define HW_DPC_PIPEBUSY REG(0xA4100018)
#define HW_DPC_TMEM     REG(0xA410001C)
#define DPC_CLR_COUNTERS 0x3C0

static const geo_t g_block128 = { L_BLOCK, CH_ST7, SZ_16, 128, 1, 0, 0 };
static const geo_t g_tile128  = { L_TILE,  CH_ST7, SZ_8,  128, 16, 0, 128 };
static const geo_t g_tile32   = { L_TILE,  CH_ST7, SZ_8,  32, 16, 0, 32 };

/* E: DP status after a load, with the RDP idle (list ends right after the load) */
static void part_e(void)
{
    static const geo_t *const bases[] = { &g_block128, &g_tile128, &g_tile32, &g_block128 };
    static const char *const names[] = { "block128", "tile128", "tile32", "block128+sync_load" };
    tprintf("# part E: DP status trace after a load; the list ends after the load (case 3: plus Sync_Load)\n");
    tprintf("# t_us = time since DPC_END was written; counters are DPC_CLOCK, BUFBUSY, PIPEBUSY, TMEM (cleared at start)\n");
    tprintf("E\tload\tt_us\tstatus\tclock\tbufbusy\tpipebusy\ttmem\n");
    show("E: status trace\n");
    for (int b = 0; b < 4; b++)
    {
        cbuf_t *c = &cb;
        list_head(c);
        run_split(c, -1, 0);                       /* clear TMEM, set state */
        c->p = c->start;
        emit_load(c, bases[b]);
        if (b == 3) emit(c, CMD(0x26));
        while (HW_DPC_STATUS & DPC_PENDING) {}
        HW_DPC_STATUS = DPC_CLR_XBUS;
        HW_DPC_START = PhysicalAddr(c->start);
        HW_DPC_STATUS = DPC_CLR_COUNTERS;
        uint32_t t0 = get_ticks();
        HW_DPC_END = PhysicalAddr(c->p);
        uint32_t last = 0xFFFFFFFF, n = 0;
        uint32_t lim = TICKS_FROM_US(400);
        for (;;)
        {
            uint32_t t = get_ticks() - t0;
            uint32_t st = HW_DPC_STATUS & 0x7FF;
            if (st != last || t >= lim)
            {
                uint32_t clk = HW_DPC_CLOCK & 0xFFFFFF, bb = HW_DPC_BUFBUSY & 0xFFFFFF;
                uint32_t pb = HW_DPC_PIPEBUSY & 0xFFFFFF, tm = HW_DPC_TMEM & 0xFFFFFF;
                tprintf("E\t%s\t%lu.%02lu\t0x%03lX\t%lu\t%lu\t%lu\t%lu\n", names[b],
                        (unsigned long)(t * 10000 / TICKS_FROM_US(100) / 100), (unsigned long)(t * 10000 / TICKS_FROM_US(100) % 100),
                        (unsigned long)st, (unsigned long)clk, (unsigned long)bb, (unsigned long)pb, (unsigned long)tm);
                last = st;
                if (t >= lim || ++n >= 40) break;
            }
        }
        /* finish: let the RDP flush and end the list */
        c->p = c->start;
        syncs(c);
        run_split(c, -1, 0);
        show(" %s done\n", names[b]);
    }
}

/* D: how long the RDP must stay idle before the queue is written */
static void part_d(void)
{
    tprintf("# part D: list in two parts, split right after the load, ST7 after the wait. diff_words / 2 = moved words (15 = none written)\n");
    tprintf("D\tload\twait_us\trep\tdiff_words\n");
    show("D: idle time\n");
    for (int b = 0; b < 2 && !timed_out; b++)
    {
        const geo_t *g = b ? &g_tile128 : &g_block128;
        int w0 = b ? 20 : 10, w1 = b ? 140 : 100, ws = b ? 4 : 2, reps = b ? 2 : 3;
        cbuf_t *c = &cb;
        list_head(c); emit_load(c, g); syncs(c); emit_change(c, g);
        list_tail_and_run(c, -1, 0, ref_tmem);
        int lo = 9999, hi = -1;     /* last wait with an effect, first without */
        for (int w = w0; w <= w1 && !timed_out; w += ws)
            for (int r = 0; r < reps; r++)
            {
                list_head(c); emit_load(c, g);
                int split = (int)(c->p - c->start);
                emit_change(c, g);
                list_tail_and_run(c, split, w, test_tmem);
                diff_t d = compare();
                tprintf("D\t%s\t%d\t%d\t%d\n", b ? "tile128" : "block128", w, r, d.words);
                if (d.words) hi = w; else if (lo == 9999) lo = w;
            }
        show(" %s: last effect %d us, first none %d us\n", b ? "tile128" : "block128", hi, lo);
    }
}

/* F: STI reach versus source alignment */
static int sti_scan(const geo_t *g, char *seq)
{
    cbuf_t *c = &cb;
    list_head(c); emit_load(c, g); syncs(c); emit_change(c, g);
    list_tail_and_run(c, -1, 0, ref_tmem);
    int reach = -1;
    seq[0] = 0;
    for (int gap = 0; gap <= MAX_GAP && !timed_out; gap++)
    {
        list_head(c); emit_load(c, g);
        for (int k = 0; k < gap; k++) emit(c, CMD(0x00));
        emit_change(c, g);
        list_tail_and_run(c, -1, 0, test_tmem);
        diff_t d = compare();
        if (d.bytes) reach = gap;
        sprintf(seq + strlen(seq), gap ? ",%d" : "%d", d.bytes);
    }
    return reach;
}

static void part_f(void)
{
    char seq[128];
    tprintf("# part F: STI after Load_Tile (i8), gaps 0..%d. seq = differing bytes per gap\n", MAX_GAP);
    tprintf("# F1: stride = row width (16 rows); F2: stride 128 (16 rows); F3: 2 rows, row 0 at offset p, row 1 at offset q (mod 64)\n");
    tprintf("F\tgroup\trowbytes\trows\toffset\tstride\tlast_mod64\treach\tseq\n");
    for (int grp = 1; grp <= 2 && !timed_out; grp++)
    {
        show("F%d reach, offset 0..56 ->\n", grp);
        for (int L = 8; L <= 64 && !timed_out; L += 8)
        {
            show("%3d:", L);
            for (int off = 0; off < 64; off += 8)
            {
                int stride = grp == 1 ? L : 128;
                geo_t g = { L_TILE, CH_STI, SZ_8, L, 16, off, stride };
                int r = sti_scan(&g, seq);
                tprintf("F\tF%d\t%d\t16\t%d\t%d\t%d\t%d\t%s\n", grp, L, off, stride, (off + 15 * stride) % 64, r, seq);
                show(" %2d", r);
            }
            show("\n");
        }
    }
    static const int f3w[] = { 24, 40 };
    for (int i = 0; i < 2 && !timed_out; i++)
    {
        int L = f3w[i];
        show("F3 L=%d reach, rows p, cols q ->\n", L);
        for (int p = 0; p < 64 && !timed_out; p += 8)
        {
            show("p%2d:", p);
            for (int q = 0; q < 64; q += 8)
            {
                int stride = 64 + ((q - p) & 63);
                geo_t g = { L_TILE, CH_STI, SZ_8, L, 2, p, stride };
                int r = sti_scan(&g, seq);
                tprintf("F\tF3\t%d\t2\t%d\t%d\t%d\t%d\t%s\n", L, p, stride, (p + stride) % 64, r, seq);
                show(" %2d", r);
            }
            show("\n");
        }
    }
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    src_a = malloc_uncached_aligned(64, 4096);
    src_b = malloc_uncached_aligned(64, 4096);
    zero4k = malloc_uncached_aligned(64, 4096);
    fb = malloc_uncached_aligned(64, 64 * 64 * 4);
    cb.start = malloc_uncached_aligned(64, 8192);
    out.cap = 512 * 1024;
    out.buf = malloc(out.cap);
    for (int i = 0; i < 4096; i++)
    {
        src_a[i] = (uint8_t)(1 + (i * 37 + 11) % 255);
        src_b[i] = (uint8_t)(src_a[i] % 255 + 1);
        zero4k[i] = 0;
    }

    HW_SP_STATUS = 0x2;     /* halt the RSP */
    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    const char *tv = get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL";
    show("RDP LOAD HAZARD v" VERSION "  %s\n", tv);
    tprintf("# rdp_load_hazard v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n# src_a\t0x%08lX\n", (unsigned long)HW_MI_VERSION, tv,
            (unsigned long)PhysicalAddr(src_a));

    part_e();
    part_d();
    if (!timed_out) part_f();
    if (timed_out) { show("RDP TIMEOUT\n"); tprintf("# ABORTED: RDP timeout\n"); }

    char path[64];
    struct stat st;
    if (!debug_init_sdfs("sd:/", -1)) { show("NO SD CARD - NOT SAVED\n"); for (;;) {} }
    mkdir("sd:/rdp_load_hazard", 0777);
    int n;
    for (n = 1; n < 1000; n++)
    {
        snprintf(path, sizeof path, "sd:/rdp_load_hazard/v4_%03d.txt", n);
        if (stat(path, &st) != 0) break;
    }
    disable_interrupts();
    FILE *f = fopen(path, "w");
    int ok = f && fwrite(out.buf, 1, out.len, f) == out.len;
    if (f && fclose(f) != 0) ok = 0;
    enable_interrupts();
    debug_close_sdfs();
    show(ok ? "SAVED %s\nDONE\n" : "SD WRITE FAILED %s\n", path);
    for (;;) {}
}

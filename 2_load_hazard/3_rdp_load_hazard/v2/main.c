/*
 * rdp_load_hazard v2 - timing model of the RDP load hazard
 *
 * v1 showed (identically on NTSC and PAL, deterministic):
 *  - Set_Texture_Image sent shortly after Load_Tile: rows whose DRAM read has
 *    not started yet are read from the new address.
 *  - Set_Tile on the load tile (TMEM address) shortly after Load_Tile,
 *    Load_Block or Load_TLUT: the TMEM writes not done yet (8 bytes each) go
 *    to the new TMEM address.
 *  - a sync, or enough commands in between, prevents both.
 *
 * v2 measures how far the effect reaches, so that it can be modelled:
 *  - gap scan 0..40 no-ops for many load sizes:
 *      STI after Load_Tile, row widths 8..128 bytes (16 rows)
 *      ST7 after Load_Tile, the same widths
 *      ST7 after Load_Block, 128 B .. 4 KiB
 *      ST7 after Load_TLUT, 16..256 entries
 *  - every list is run once from RDRAM and once from RSP DMEM (XBUS), where
 *    commands are fetched faster: if the reach in commands changes, the
 *    effect is a matter of time, not of command count
 *  - RDP cycle counts: each load on its own, and the cost of a no-op from
 *    RDRAM and from DMEM, to convert command counts into cycles
 *
 * For every case the console compares the TMEM read-back with the same load
 * done with a sync before the change, and records how many bytes differ and
 * where. Results: sd:/rdp_load_hazard/v2_NNN.txt (one line per case).
 * The RDP is driven directly through the DP registers (no RSP microcode).
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define VERSION     "2"
#define MAX_GAP     40

/* --- hardware ----------------------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_DPC_CLOCK    REG(0xA4100010)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_VERSION   REG(0xA4300004)
#define HW_MI_INTR      REG(0xA4300008)
#define HW_SP_STATUS    REG(0xA4040010)
#define DMEM            ((volatile uint32_t *)0xA4000000)
#define DPC_CLR_XBUS    0x001
#define DPC_SET_XBUS    0x002
#define DPC_CLR_FREEZE  0x004
#define DPC_CLR_FLUSH   0x010
#define DPC_CLR_COUNTERS 0x3C0
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

static void sync_pipe(cbuf_t *c) { emit(c, CMD(0x27)); }
static void sync_load(cbuf_t *c) { emit(c, CMD(0x26)); }
static void sync_tile(cbuf_t *c) { emit(c, CMD(0x28)); }

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
#define load_tlut(c, t, sl, tl, sh, th)     tile_cmd(c, 0x30, t, sl, tl, sh, th)

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

/* run a list (ending in Sync_Full) from RDRAM or from DMEM; returns DPC_CLOCK */
static uint32_t run(cbuf_t *c, int xbus)
{
    emit(c, CMD(0x29));
    while (HW_DPC_STATUS & DPC_PENDING) {}
    uint32_t start, end;
    if (xbus)
    {
        int words = (int)(c->p - c->start) * 2;
        const uint32_t *src = (const uint32_t *)c->start;
        for (int i = 0; i < words; i++) DMEM[i] = src[i];
        HW_DPC_STATUS = DPC_SET_XBUS;
        start = 0; end = (uint32_t)words * 4;
    }
    else
    {
        HW_DPC_STATUS = DPC_CLR_XBUS;
        start = PhysicalAddr(c->start); end = PhysicalAddr(c->p);
    }
    HW_DPC_START = start;
    HW_DPC_STATUS = DPC_CLR_COUNTERS;
    HW_DPC_END = end;
    uint32_t timeout = 0x2000000;
    while (!(HW_MI_INTR & MI_INTR_DP) && --timeout) {}
    if (!timeout) timed_out = 1;
    uint32_t clk = HW_DPC_CLOCK & 0xFFFFFF;
    HW_MI_MODE = MI_CLEAR_DP_INT;
    HW_DPC_STATUS = DPC_CLR_XBUS;
    return clk;
}

/* --- cases ------------------------------------------------------------------------ */

enum { K_TILE_STI, K_TILE_ST7, K_BLOCK_ST7, K_TLUT_ST7 };
static const char *const kind_names[] = { "tile_sti", "tile_st7", "block_st7", "tlut_st7" };

typedef struct { int kind; int size; } config_t;   /* size: row bytes / block bytes / TLUT entries */

static const config_t configs[] = {
    { K_TILE_STI, 8 }, { K_TILE_STI, 16 }, { K_TILE_STI, 24 }, { K_TILE_STI, 32 },
    { K_TILE_STI, 48 }, { K_TILE_STI, 64 }, { K_TILE_STI, 96 }, { K_TILE_STI, 128 },
    { K_TILE_ST7, 8 }, { K_TILE_ST7, 16 }, { K_TILE_ST7, 24 }, { K_TILE_ST7, 32 },
    { K_TILE_ST7, 48 }, { K_TILE_ST7, 64 }, { K_TILE_ST7, 96 }, { K_TILE_ST7, 128 },
    { K_BLOCK_ST7, 128 }, { K_BLOCK_ST7, 256 }, { K_BLOCK_ST7, 512 }, { K_BLOCK_ST7, 1024 },
    { K_BLOCK_ST7, 2048 }, { K_BLOCK_ST7, 4096 },
    { K_TLUT_ST7, 16 }, { K_TLUT_ST7, 64 }, { K_TLUT_ST7, 256 },
};
#define N_CONFIGS ((int)(sizeof configs / sizeof configs[0]))
#define TILE_ROWS 16

/* gap < 0: a Sync_Load instead of the change hazard (reference) */
static void emit_load(cbuf_t *c, const config_t *k)
{
    switch (k->kind)
    {
    case K_TILE_STI:
    case K_TILE_ST7:
        set_texture_image(c, FMT_I, SZ_8, k->size, PhysicalAddr(src_a));
        set_tile(c, 7, FMT_I, SZ_8, (k->size + 7) / 8, 0);
        sync_load(c);
        load_tile(c, 7, 0, 0, k->size - 1, TILE_ROWS - 1);
        break;
    case K_BLOCK_ST7:
        set_texture_image(c, FMT_RGBA, SZ_16, k->size / 2, PhysicalAddr(src_a));
        set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0);
        sync_load(c);
        load_block(c, 7, k->size / 2, 0);
        break;
    case K_TLUT_ST7:
        set_texture_image(c, FMT_RGBA, SZ_16, k->size, PhysicalAddr(src_a));
        set_tile(c, 7, FMT_I, SZ_4, 0, 0x100);
        sync_load(c);
        load_tlut(c, 7, 0, 0, k->size - 1, 0);
        break;
    }
}

static void emit_change(cbuf_t *c, const config_t *k)
{
    switch (k->kind)
    {
    case K_TILE_STI:  set_texture_image(c, FMT_I, SZ_8, k->size, PhysicalAddr(src_b)); break;
    case K_TILE_ST7:  set_tile(c, 7, FMT_I, SZ_8, (k->size + 7) / 8, 0x80); break;      /* +1 KiB */
    case K_BLOCK_ST7: set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0x80); break;               /* wraps for 4 KiB */
    case K_TLUT_ST7:  set_tile(c, 7, FMT_I, SZ_4, 0, 0x100 + 0x40); break;
    }
}

/* full case: clear, load, gap (or sync), change, read back */
static void run_case(const config_t *k, int gap, int xbus, uint8_t *tmem_out)
{
    cbuf_t *c = &cb;
    c->p = c->start;

    set_texture_image(c, FMT_RGBA, SZ_16, 2048, PhysicalAddr(zero4k));
    set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0);
    sync_load(c);
    load_block(c, 7, 2048, 0);
    sync_pipe(c);
    sync_tile(c);

    emit_load(c, k);
    if (gap < 0) { sync_pipe(c); sync_load(c); sync_tile(c); }
    else for (int i = 0; i < gap; i++) emit(c, CMD(0x00));
    emit_change(c, k);

    sync_pipe(c); sync_load(c); sync_tile(c);
    emit(c, CMD(0x3F) | 3ull << 51 | (uint64_t)(64 - 1) << 32 | PhysicalAddr(fb));
    emit(c, CMD(0x2D) | FX2(64) << 12 | FX2(64));
    emit(c, CMD(0x2F) | SOM_READBACK);
    emit(c, combine_tex());
    set_tile(c, 0, FMT_I, SZ_8, 8, 0);
    set_tile_size(c, 0, 0, 0, 63, 63);
    emit(c, CMD(0x24) | FX2(64) << 44 | FX2(64) << 32 | FX2(0) << 12 | FX2(0));
    emit(c, 0x400ull << 16 | 0x400ull);
    run(c, xbus);

    /* read-back -> linear TMEM (odd rows are read 32-bit word swapped) */
    for (int k2 = 0; k2 < 4096; k2++)
    {
        int row = k2 >> 6, x = k2 & 63;
        tmem_out[row * 64 + (x ^ ((row & 1) ? 4 : 0))] = (uint8_t)(fb[k2] >> 24);
    }
}

/* cycles of a load on its own (minus the same list without the load) */
static uint32_t load_cycles(const config_t *k, int xbus)
{
    cbuf_t *c = &cb;
    uint32_t with = 0, without = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        /* setup (unmeasured) */
        c->p = c->start;
        sync_pipe(c);
        run(c, 0);
        c->p = c->start;
        if (pass == 0) emit_load(c, k);
        else { emit(c, CMD(0x00)); emit(c, CMD(0x00)); emit(c, CMD(0x26)); emit(c, CMD(0x00)); }
        uint32_t clk = run(c, xbus);
        if (pass == 0) with = clk; else without = clk;
    }
    return with - without;
}

static uint32_t noop_cycles(int n, int xbus)
{
    cbuf_t *c = &cb;
    c->p = c->start;
    for (int i = 0; i < n; i++) emit(c, CMD(0x00));
    return run(c, xbus);
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
    console_render();
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
    cb.start = malloc_uncached_aligned(64, 4096);
    out.cap = 512 * 1024;
    out.buf = malloc(out.cap);
    for (int i = 0; i < 4096; i++) { src_a[i] = (uint8_t)(i * 37 + 11); src_b[i] = (uint8_t)(i * 53 + 0x5A); zero4k[i] = 0; }

    HW_SP_STATUS = 0x2;                 /* halt the RSP: DMEM is used as an XBUS command buffer */
    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    const char *tv = get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL";
    show("RDP LOAD HAZARD v" VERSION "  %s\n", tv);
    tprintf("# rdp_load_hazard v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n", (unsigned long)HW_MI_VERSION, tv);

    /* cycle costs */
    tprintf("# cycles: no-op lists (RDRAM / DMEM)\n");
    for (int n = 0; n <= 256; n += 64)
        tprintf("noop\t%d\t%lu\t%lu\n", n, (unsigned long)noop_cycles(n, 0), (unsigned long)noop_cycles(n, 1));
    tprintf("# cycles: load on its own minus the same list without it (RDRAM / DMEM)\n");
    for (int i = 0; i < N_CONFIGS; i++)
        if (configs[i].kind != K_TILE_ST7)
            tprintf("load\t%s\t%d\t%lu\t%lu\n", configs[i].kind == K_TILE_STI ? "tile" : kind_names[configs[i].kind],
                    configs[i].size, (unsigned long)load_cycles(&configs[i], 0), (unsigned long)load_cycles(&configs[i], 1));

    /* gap scans */
    tprintf("# gap scans: bytes of TMEM that differ from the same load with a sync before the change\n");
    tprintf("kind\tsize\tsource\tgap\tdiff_bytes\tfirst\tlast\n");
    for (int i = 0; i < N_CONFIGS && !timed_out; i++)
    {
        const config_t *k = &configs[i];
        run_case(k, -1, 0, ref_tmem);
        show("%-9s %4d:", kind_names[k->kind], k->size);
        for (int xbus = 0; xbus < 2; xbus++)
        {
            int reach = -1;
            for (int gap = 0; gap <= MAX_GAP; gap++)
            {
                run_case(k, gap, xbus, test_tmem);
                int n = 0, first = -1, last = -1;
                for (int a = 0; a < 4096; a++)
                    if (test_tmem[a] != ref_tmem[a]) { n++; if (first < 0) first = a; last = a; }
                if (n) reach = gap;
                tprintf("%s\t%d\t%s\t%d\t%d\t%d\t%d\n", kind_names[k->kind], k->size, xbus ? "dmem" : "rdram",
                        gap, n, first, last);
            }
            show(" %s %2d", xbus ? "D" : "R", reach);
        }
        show("\n");
    }
    if (timed_out) { show("RDP TIMEOUT\n"); tprintf("# ABORTED: RDP timeout\n"); }

    /* save */
    char path[64];
    struct stat st;
    if (!debug_init_sdfs("sd:/", -1)) { show("NO SD CARD - NOT SAVED\n"); for (;;) {} }
    mkdir("sd:/rdp_load_hazard", 0777);
    int n;
    for (n = 1; n < 1000; n++)
    {
        snprintf(path, sizeof path, "sd:/rdp_load_hazard/v2_%03d.txt", n);
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

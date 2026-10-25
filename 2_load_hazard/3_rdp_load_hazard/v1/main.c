/*
 * rdp_load_hazard - what happens when load state changes right after a TMEM
 * load, without a sync?
 *
 * rdp_timing v3 found that a Set_Texture_Image or Set_Tile (on the load
 * tile) sent within a few commands after Load_Tile, with no sync in between,
 * changes the tail of the load: the last bytes come from the new address or
 * land at the new TMEM address. Load_Block was not affected; any sync
 * prevented it. This ROM maps the effect out:
 *
 *  - gap scan: 0..12, 16, 24, 32 no-ops between Load_Tile and the change
 *  - what fills the gap: no-ops, Set_Prim_Color, Set_Env_Color
 *  - load size: 8..64 rows, 16..128 bytes per row, 8/16/32-bit texels
 *  - which command follows: Set_Texture_Image (address, width, size),
 *    Set_Tile of the load tile (TMEM address, line, format), Set_Tile_Size,
 *    Set_Tile of another tile, Set_Other_Modes, Set_Color_Image, ...
 *  - Load_Block and Load_TLUT followed by the same changes
 *
 * Every case: TMEM is cleared (4 KiB of zeros, with syncs), the test
 * sequence runs, then after full syncs all 4 KiB of TMEM is read back as an
 * I8 texture (line 8, 64x64) into a 32-bit framebuffer. The red channel of
 * each pixel is one TMEM byte. Each case runs 4 times.
 *
 * Output on the SD card:
 *   sd:/rdp_load_hazard/NNN/index.txt       one line per case
 *   sd:/rdp_load_hazard/NNN/case_CCC_R.bin  4096 bytes per run (pixel order
 *                                           of the read-back, see index.txt)
 * The RDP is driven directly through the DP registers (no RSP microcode).
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define VERSION     "1"
#define RUNS        4

/* --- hardware ----------------------------------------------------------------- */

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

enum { FMT_RGBA = 0, FMT_CI = 2, FMT_I = 4 };
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

/* Set_Combine_Mode: TEX0 colour and alpha, both cycles */
static uint64_t combine_tex(void)
{
    const uint64_t a = 15, b = 15, c = 31, d = 1, aa = 7, ab = 7, ac = 7, ad = 1;
    return CMD(0x3C)
         | a << 52 | c << 47 | aa << 44 | ac << 41
         | a << 37 | c << 32 | b << 28 | b << 24
         | aa << 21 | ac << 18 | d << 15 | ab << 12
         | ad << 9 | d << 6 | ab << 3 | ad;
}

/* 1-cycle, point sampling, no dither */
#define SOM_READBACK    ((0ull << 52) | (3ull << 42) | (0xFull << 36))

/* --- buffers --------------------------------------------------------------------- */

static uint8_t *src_a;          /* the texture under test: byte i = i*37+11 */
static uint8_t *src_b;          /* the "new" address: byte i = i*53+0x5A */
static uint8_t *zero4k;
static uint32_t *fb;            /* 64x64x32 read-back */
static cbuf_t cb;

static void run(cbuf_t *c)
{
    emit(c, CMD(0x29));         /* Sync_Full */
    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = PhysicalAddr(c->start);
    HW_DPC_END = PhysicalAddr(c->p);
    uint32_t timeout = 0x2000000;
    while (!(HW_MI_INTR & MI_INTR_DP) && --timeout) {}
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

/* --- cases ------------------------------------------------------------------------ */

enum { LD_TILE, LD_BLOCK, LD_TLUT };
enum { G_NOOP, G_PRIM, G_ENV, G_SYNC_PIPE, G_SYNC_LOAD, G_SYNC_TILE };
enum {
    CH_NONE,
    CH_STI_ADDR,        /* Set_Texture_Image: same format, address B */
    CH_STI_WIDTH,       /* Set_Texture_Image: address A, width x2 */
    CH_STI_SIZE,        /* Set_Texture_Image: address A, texel size changed */
    CH_ST7_TMEM,        /* Set_Tile on the load tile: TMEM address +0x40 words (+512 bytes) */
    CH_ST7_LINE,        /* Set_Tile on the load tile: line x2 */
    CH_ST7_FMT,         /* Set_Tile on the load tile: format/size changed */
    CH_STS7,            /* Set_Tile_Size on the load tile */
    CH_ST0,             /* Set_Tile on another tile (0) */
    CH_SOM,             /* Set_Other_Modes */
    CH_SCI,             /* Set_Color_Image */
    CH_PRIM,            /* Set_Prim_Color (control) */
    CH_STI_ST7,         /* Set_Texture_Image B, then Set_Tile TMEM +0x40 */
};
static const char *const ch_names[] = {
    "none", "sti_addr", "sti_width", "sti_size", "st7_tmem", "st7_line", "st7_fmt",
    "sts7", "st0", "som", "sci", "prim", "sti+st7",
};
static const char *const gap_names[] = { "noop", "prim", "env", "sync_pipe", "sync_load", "sync_tile" };

typedef struct {
    int load;           /* LD_* */
    int fmt, size;      /* source format / texel size */
    int width, rows;    /* texels per row, rows (Load_Tile); texels (Load_Block); entries (Load_TLUT) */
    int dxt;            /* Load_Block */
    int gap, gtype;     /* commands between load and change, and what they are */
    int change;
} case_t;

#define MAX_CASES 128
static case_t cases[MAX_CASES];
static int n_cases;

static void add(int load, int fmt, int size, int width, int rows, int dxt, int gap, int gtype, int change)
{
    if (n_cases < MAX_CASES)
        cases[n_cases++] = (case_t){ load, fmt, size, width, rows, dxt, gap, gtype, change };
}

static int bytes_per_texel_x2(int size) { return size == SZ_4 ? 1 : size == SZ_8 ? 2 : size == SZ_16 ? 4 : 8; }

static void build_cases(void)
{
    /* baseline load: I8 32x32 (32 bytes per row, 1 KiB) */
    #define BASE LD_TILE, FMT_I, SZ_8, 32, 32, 0
    add(BASE, 0, G_NOOP, CH_NONE);
    for (int g = 0; g <= 12; g++) add(BASE, g, G_NOOP, CH_STI_ADDR);
    add(BASE, 16, G_NOOP, CH_STI_ADDR); add(BASE, 24, G_NOOP, CH_STI_ADDR); add(BASE, 32, G_NOOP, CH_STI_ADDR);
    for (int g = 0; g <= 12; g++) add(BASE, g, G_NOOP, CH_ST7_TMEM);
    add(BASE, 16, G_NOOP, CH_ST7_TMEM); add(BASE, 24, G_NOOP, CH_ST7_TMEM);

    /* what fills the gap */
    for (int g = 2; g <= 8; g += 2) { add(BASE, g, G_PRIM, CH_STI_ADDR); add(BASE, g, G_ENV, CH_STI_ADDR); }
    add(BASE, 0, G_SYNC_PIPE, CH_STI_ADDR); add(BASE, 0, G_SYNC_LOAD, CH_STI_ADDR); add(BASE, 0, G_SYNC_TILE, CH_STI_ADDR);
    add(BASE, 0, G_SYNC_PIPE, CH_ST7_TMEM); add(BASE, 0, G_SYNC_LOAD, CH_ST7_TMEM); add(BASE, 0, G_SYNC_TILE, CH_ST7_TMEM);

    /* which command follows (gap 0) */
    for (int ch = CH_STI_WIDTH; ch <= CH_STI_ST7; ch++)
        if (ch != CH_ST7_TMEM) add(BASE, 0, G_NOOP, ch);

    /* load size: rows, row width, texel size (change: Set_Texture_Image B, gap 0 and 4) */
    static const int rows[] = { 8, 16, 64 };
    for (int i = 0; i < 3; i++) { add(LD_TILE, FMT_I, SZ_8, 32, rows[i], 0, 0, G_NOOP, CH_STI_ADDR);
                                  add(LD_TILE, FMT_I, SZ_8, 32, rows[i], 0, 4, G_NOOP, CH_STI_ADDR); }
    static const int widths[] = { 16, 64, 128 };
    for (int i = 0; i < 3; i++) { add(LD_TILE, FMT_I, SZ_8, widths[i], 32, 0, 0, G_NOOP, CH_STI_ADDR);
                                  add(LD_TILE, FMT_I, SZ_8, widths[i], 32, 0, 4, G_NOOP, CH_STI_ADDR);
                                  add(LD_TILE, FMT_I, SZ_8, widths[i], 32, 0, 0, G_NOOP, CH_ST7_TMEM); }
    add(LD_TILE, FMT_RGBA, SZ_16, 16, 32, 0, 0, G_NOOP, CH_NONE);
    add(LD_TILE, FMT_RGBA, SZ_16, 16, 32, 0, 0, G_NOOP, CH_STI_ADDR);
    add(LD_TILE, FMT_RGBA, SZ_16, 16, 32, 0, 4, G_NOOP, CH_STI_ADDR);
    add(LD_TILE, FMT_RGBA, SZ_16, 16, 32, 0, 0, G_NOOP, CH_ST7_TMEM);
    add(LD_TILE, FMT_RGBA, SZ_32, 8, 32, 0, 0, G_NOOP, CH_NONE);
    add(LD_TILE, FMT_RGBA, SZ_32, 8, 32, 0, 0, G_NOOP, CH_STI_ADDR);
    add(LD_TILE, FMT_RGBA, SZ_32, 8, 32, 0, 4, G_NOOP, CH_STI_ADDR);
    add(LD_TILE, FMT_RGBA, SZ_32, 8, 32, 0, 0, G_NOOP, CH_ST7_TMEM);

    /* Load_Block (RGBA16, 512 texels = 1 KiB), with and without dxt */
    for (int d = 0; d < 2; d++)
    {
        int dxt = d ? 2048 / 4 : 0;     /* 32-byte rows */
        add(LD_BLOCK, FMT_RGBA, SZ_16, 512, 0, dxt, 0, G_NOOP, CH_NONE);
        add(LD_BLOCK, FMT_RGBA, SZ_16, 512, 0, dxt, 0, G_NOOP, CH_STI_ADDR);
        add(LD_BLOCK, FMT_RGBA, SZ_16, 512, 0, dxt, 0, G_NOOP, CH_ST7_TMEM);
        add(LD_BLOCK, FMT_RGBA, SZ_16, 512, 0, dxt, 0, G_NOOP, CH_STI_ST7);
    }
    add(LD_BLOCK, FMT_RGBA, SZ_16, 2048, 0, 0, 0, G_NOOP, CH_STI_ADDR);    /* 4 KiB */

    /* Load_TLUT (256 entries into the upper half) */
    add(LD_TLUT, FMT_RGBA, SZ_16, 256, 0, 0, 0, G_NOOP, CH_NONE);
    add(LD_TLUT, FMT_RGBA, SZ_16, 256, 0, 0, 0, G_NOOP, CH_STI_ADDR);
    add(LD_TLUT, FMT_RGBA, SZ_16, 256, 0, 0, 0, G_NOOP, CH_ST7_TMEM);
}

/* line (64-bit words per TMEM row) for a Load_Tile of this case */
static int tile_line(const case_t *t)
{
    int bytes = t->width * bytes_per_texel_x2(t->size) / 2;
    if (t->size == SZ_32) bytes /= 2;   /* 32-bit: each half (RG / BA) gets half */
    return (bytes + 7) / 8;
}

static void build(const case_t *t)
{
    cbuf_t *c = &cb;
    c->p = c->start;

    /* clear TMEM: 4 KiB of zeros, with syncs */
    set_texture_image(c, FMT_RGBA, SZ_16, 2048, PhysicalAddr(zero4k));
    set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0);
    sync_load(c);
    load_block(c, 7, 2048, 0);
    sync_pipe(c);
    sync_tile(c);

    /* the load under test */
    int line = 0;
    switch (t->load)
    {
    case LD_TILE:
        line = tile_line(t);
        set_texture_image(c, t->fmt, t->size, t->width, PhysicalAddr(src_a));
        set_tile(c, 7, t->fmt, t->size, line, 0);
        sync_load(c);
        load_tile(c, 7, 0, 0, t->width - 1, t->rows - 1);
        break;
    case LD_BLOCK:
        set_texture_image(c, t->fmt, t->size, t->width, PhysicalAddr(src_a));
        set_tile(c, 7, t->fmt, t->size, 0, 0);
        sync_load(c);
        load_block(c, 7, t->width, t->dxt);
        break;
    case LD_TLUT:
        set_texture_image(c, FMT_RGBA, SZ_16, t->width, PhysicalAddr(src_a));
        set_tile(c, 7, FMT_I, SZ_4, 0, 0x100);          /* TLUT load tile must be 4-bit */
        sync_load(c);
        load_tlut(c, 7, 0, 0, t->width - 1, 0);
        break;
    }

    /* gap */
    for (int i = 0; i < t->gap; i++)
        emit(c, t->gtype == G_PRIM ? CMD(0x3A) | 0x11223344u
              : t->gtype == G_ENV ? CMD(0x3B) | 0x55667788u : CMD(0x00));
    if (t->gtype == G_SYNC_PIPE) sync_pipe(c);
    if (t->gtype == G_SYNC_LOAD) sync_load(c);
    if (t->gtype == G_SYNC_TILE) sync_tile(c);

    /* the change, with no sync before it */
    int lfmt = t->load == LD_TLUT ? FMT_I : t->fmt, lsize = t->load == LD_TLUT ? SZ_4 : t->size;
    int ltmem = t->load == LD_TLUT ? 0x100 : 0;
    switch (t->change)
    {
    case CH_STI_ADDR:  set_texture_image(c, t->load == LD_TLUT ? FMT_RGBA : t->fmt, t->load == LD_TLUT ? SZ_16 : t->size,
                                         t->width, PhysicalAddr(src_b)); break;
    case CH_STI_WIDTH: set_texture_image(c, t->fmt, t->size, t->width * 2, PhysicalAddr(src_a)); break;
    case CH_STI_SIZE:  set_texture_image(c, t->fmt, t->size == SZ_16 ? SZ_8 : SZ_16, t->width, PhysicalAddr(src_a)); break;
    case CH_ST7_TMEM:  set_tile(c, 7, lfmt, lsize, line, ltmem + 0x40); break;
    case CH_ST7_LINE:  set_tile(c, 7, lfmt, lsize, line * 2, ltmem); break;
    case CH_ST7_FMT:   set_tile(c, 7, FMT_RGBA, SZ_16, line, ltmem); break;
    case CH_STS7:      set_tile_size(c, 7, 0, 0, 15, 15); break;
    case CH_ST0:       set_tile(c, 0, FMT_I, SZ_8, 4, 0x100); break;
    case CH_SOM:       emit(c, CMD(0x2F) | SOM_READBACK); break;
    case CH_SCI:       emit(c, CMD(0x3F) | 3ull << 51 | (uint64_t)(64 - 1) << 32 | PhysicalAddr(fb)); break;
    case CH_PRIM:      emit(c, CMD(0x3A) | 0x99AABBCCu); break;
    case CH_STI_ST7:   set_texture_image(c, t->fmt, t->size, t->width, PhysicalAddr(src_b));
                       set_tile(c, 7, lfmt, lsize, line, ltmem + 0x40); break;
    default: break;
    }

    /* read all 4 KiB of TMEM back as I8, 64 bytes per row */
    sync_pipe(c);
    sync_load(c);
    sync_tile(c);
    emit(c, CMD(0x3F) | 3ull << 51 | (uint64_t)(64 - 1) << 32 | PhysicalAddr(fb));   /* Set_Color_Image 32-bit, 64 wide */
    emit(c, CMD(0x2D) | FX2(64) << 12 | FX2(64));                                     /* Set_Scissor */
    emit(c, CMD(0x2F) | SOM_READBACK);
    emit(c, combine_tex());
    set_tile(c, 0, FMT_I, SZ_8, 8, 0);
    set_tile_size(c, 0, 0, 0, 63, 63);
    emit(c, CMD(0x24) | FX2(64) << 44 | FX2(64) << 32 | 0ull << 24 | FX2(0) << 12 | FX2(0));  /* Texture_Rectangle */
    emit(c, 0ull << 48 | 0ull << 32 | 0x400ull << 16 | 0x400ull);                        /* s, t = 0; 1 texel per pixel */
    run(c);
}

/* --- output ---------------------------------------------------------------------------- */

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

static void describe(const case_t *t, char *out, size_t n)
{
    static const char *const ld[] = { "load_tile", "load_block", "load_tlut" };
    static const char *const fm[] = { "rgba", "yuv", "ci", "ia", "i" };
    static const char *const sz[] = { "4", "8", "16", "32" };
    snprintf(out, n, "%s\t%s%s\t%d\t%d\t%d\t%d\t%s\t%s", ld[t->load], fm[t->fmt], sz[t->size],
             t->width, t->rows, t->dxt, t->gap, gap_names[t->gtype], ch_names[t->change]);
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
    for (int i = 0; i < 4096; i++) { src_a[i] = (uint8_t)(i * 37 + 11); src_b[i] = (uint8_t)(i * 53 + 0x5A); zero4k[i] = 0; }

    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    build_cases();
    const char *tv = get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL";
    show("RDP LOAD HAZARD v" VERSION "  %s\n%d CASES x %d RUNS\n", tv, n_cases, RUNS);

    /* run everything first, keep the read-backs in memory */
    uint8_t *results = malloc((size_t)n_cases * RUNS * 4096);
    if (!results) { show("OUT OF MEMORY\n"); for (;;) {} }
    int changed = 0;
    for (int i = 0; i < n_cases; i++)
        for (int r = 0; r < RUNS; r++)
        {
            memset(fb, 0, 64 * 64 * 4);
            build(&cases[i]);
            uint8_t *dst = results + ((size_t)i * RUNS + r) * 4096;
            for (int k = 0; k < 4096; k++) dst[k] = (uint8_t)(fb[k] >> 24);     /* red = TMEM byte */
            if (r && memcmp(dst, dst - 4096, 4096)) changed++;
        }
    show("DONE. RUNS THAT DIFFERED FROM THE PREVIOUS: %d\n", changed);

    /* save */
    char dir[64], path[96];
    struct stat st;
    if (!debug_init_sdfs("sd:/", -1)) { show("NO SD CARD - NOT SAVED\n"); for (;;) {} }
    mkdir("sd:/rdp_load_hazard", 0777);
    int n;
    for (n = 1; n < 1000; n++)
    {
        snprintf(dir, sizeof dir, "sd:/rdp_load_hazard/%03d", n);
        if (stat(dir, &st) != 0) break;
    }
    mkdir(dir, 0777);
    show("SAVING TO %s\n", dir);

    disable_interrupts();
    int ok = 1;
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) ok = 0;
    else
    {
        fprintf(f, "# rdp_load_hazard v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n",
                (unsigned long)HW_MI_VERSION, tv);
        fprintf(f, "# TMEM read back as I8, tile line 8 (64 bytes per row), 64x64; file byte k = texel (k%%64, k/64)\n");
        fprintf(f, "# odd rows read 32-bit word swapped: TMEM byte address = (k/64)*64 + ((k%%64) ^ (4 if (k/64) odd))\n");
        fprintf(f, "# source A byte i = (i*37+11)&255, source B byte i = (i*53+0x5A)&255; TMEM cleared to 0 first\n");
        fprintf(f, "case\tload\tformat\twidth\trows\tdxt\tgap\tgap_type\tchange\tline\n");
        for (int i = 0; i < n_cases; i++)
        {
            char d[128];
            describe(&cases[i], d, sizeof d);
            fprintf(f, "%d\t%s\t%d\n", i, d, cases[i].load == LD_TILE ? tile_line(&cases[i]) : 0);
        }
        if (fclose(f) != 0) ok = 0;
    }
    for (int i = 0; ok && i < n_cases; i++)
        for (int r = 0; ok && r < RUNS; r++)
        {
            snprintf(path, sizeof path, "%s/case_%03d_%d.bin", dir, i, r);
            f = fopen(path, "wb");
            ok = f && fwrite(results + ((size_t)i * RUNS + r) * 4096, 1, 4096, f) == 4096;
            if (f && fclose(f) != 0) ok = 0;
        }
    enable_interrupts();
    debug_close_sdfs();
    show(ok ? "SAVED\nDONE\n" : "SD WRITE FAILED\n");
    for (;;) {}
}

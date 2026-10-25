/*
 * rdp_load_hazard v3 - open questions of the RDP load hazard model
 *
 * v2 (NTSC = PAL, RDRAM = DMEM) gave a model: the last 15 "slots" of a load
 * stay queued; every later command retires one slot with the tile state of
 * that moment; a sync flushes the queue. Load_Tile rows take
 * max(words + 1, 9) slots. Set_Texture_Image takes effect for a row if it
 * comes 4 or more slots before the row's first TMEM write.
 *
 * Open questions, one part each:
 *  A  geometry: why 48 and 96 byte rows reach 4 commands further for STI,
 *     and why 24 byte rows show 2 rows at gap 0. Row width (8..128 incl. 40,
 *     56, 80, 112), source alignment, image stride, row count and texel size
 *     are varied. Every geometry is scanned with STI and with ST7, so the
 *     slot layout (from ST7) can be lined up with the STI reach.
 *  B  what counts as one command: n commands of one type (state commands,
 *     syncs, draws, loads, multi-word commands) between the load and ST7.
 *  C  whether the queue drains while the RDP is idle: the list is
 *     submitted in two parts, with a wait from 0 us to 10 ms between them.
 *
 * Every case is compared on the console with the same list that has a sync
 * before the change. Results: sd:/rdp_load_hazard/v3_NNN.txt.
 * The RDP is driven directly through the DP registers (no RSP microcode).
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#define VERSION     "3"
#define MAX_GAP     20

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

/* --- part B: filler commands ------------------------------------------------------ */

enum {
    F_NOOP, F_PRIM, F_ENV, F_BLEND, F_FOG, F_FILLCOL, F_PRIMDEPTH, F_KEYR, F_KEYGB, F_CONVERT,
    F_SCISSOR, F_SOM, F_COMBINE, F_COLORIMG, F_ZIMG, F_STI_SAME, F_ST1, F_ST7_SAME, F_STS1, F_STS7,
    F_SYNC_PIPE, F_SYNC_TILE, F_SYNC_LOAD,
    F_RECT1, F_RECT64, F_TEXRECT, F_TEXRECT64, F_TRI, F_LOADBLOCK6, F_LOADTILE6,
    N_FILLERS
};
static const char *const filler_names[N_FILLERS] = {
    "noop", "set_prim_color", "set_env_color", "set_blend_color", "set_fog_color", "set_fill_color",
    "set_prim_depth", "set_key_r", "set_key_gb", "set_convert",
    "set_scissor", "set_other_modes", "set_combine", "set_color_image", "set_z_image",
    "sti_same", "set_tile_1", "set_tile_7_same", "set_tile_size_1", "set_tile_size_7",
    "sync_pipe", "sync_tile", "sync_load",
    "fill_rect_1px", "fill_rect_64x64", "tex_rect_2px", "tex_rect_64x64", "triangle", "load_block_t6", "load_tile_t6",
};

static void emit_filler(cbuf_t *c, int f, const geo_t *g)
{
    switch (f)
    {
    case F_NOOP:      emit(c, CMD(0x00)); break;
    case F_PRIM:      emit(c, CMD(0x3A) | 0x11223344); break;
    case F_ENV:       emit(c, CMD(0x3B) | 0x55667788); break;
    case F_BLEND:     emit(c, CMD(0x39) | 0x01020304); break;
    case F_FOG:       emit(c, CMD(0x38) | 0x05060708); break;
    case F_FILLCOL:   emit(c, CMD(0x37) | 0x12345678); break;
    case F_PRIMDEPTH: emit(c, CMD(0x2E)); break;
    case F_KEYR:      emit(c, CMD(0x2B)); break;
    case F_KEYGB:     emit(c, CMD(0x2A)); break;
    case F_CONVERT:   emit(c, CMD(0x2C)); break;
    case F_SCISSOR:   emit(c, CMD(0x2D) | FX2(64) << 12 | FX2(64)); break;
    case F_SOM:       emit(c, CMD(0x2F) | SOM_READBACK); break;
    case F_COMBINE:   emit(c, combine_tex()); break;
    case F_COLORIMG:  emit(c, CMD(0x3F) | 3ull << 51 | (uint64_t)(64 - 1) << 32 | PhysicalAddr(fb)); break;
    case F_ZIMG:      emit(c, CMD(0x3E) | PhysicalAddr(zero4k)); break;
    case F_STI_SAME:
        if (g->load == L_TILE)
            set_texture_image(c, fmt_of(g->size), g->size, g->stride / bpp(g->size), PhysicalAddr(src_a) + g->offset);
        else
            set_texture_image(c, FMT_RGBA, SZ_16, g->rowbytes / 2, PhysicalAddr(src_a));
        break;
    case F_ST1:       set_tile(c, 1, FMT_I, SZ_8, 8, 0); break;
    case F_ST7_SAME:
        if (g->load == L_TILE) set_tile(c, 7, fmt_of(g->size), g->size, tile_line(g), 0);
        else set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0);
        break;
    case F_STS1:      set_tile_size(c, 1, 0, 0, 63, 63); break;
    case F_STS7:      set_tile_size(c, 7, 0, 0, 63, 63); break;
    case F_SYNC_PIPE: emit(c, CMD(0x27)); break;
    case F_SYNC_TILE: emit(c, CMD(0x28)); break;
    case F_SYNC_LOAD: emit(c, CMD(0x26)); break;
    case F_RECT1:     emit(c, CMD(0x36) | FX2(1) << 44 | FX2(1) << 32 | FX2(0) << 12 | FX2(0)); break;
    case F_RECT64:    emit(c, CMD(0x36) | FX2(64) << 44 | FX2(64) << 32 | FX2(0) << 12 | FX2(0)); break;
    case F_TEXRECT:
        emit(c, CMD(0x24) | FX2(2) << 44 | FX2(1) << 32 | 1ull << 24 | FX2(0) << 12 | FX2(0));
        emit(c, 0x400ull << 16 | 0x400ull);
        break;
    case F_TEXRECT64:
        emit(c, CMD(0x24) | FX2(64) << 44 | FX2(64) << 32 | 1ull << 24 | FX2(0) << 12 | FX2(0));
        emit(c, 0x400ull << 16 | 0x400ull);
        break;
    case F_TRI:   /* flat triangle covering x 0..2, y 0..1 */
        emit(c, CMD(0x08) | 1ull << 55 | (uint64_t)(4 & 0x3FFF) << 32 | (uint64_t)(4 & 0x3FFF) << 16 | 0);
        emit(c, (uint64_t)(2u << 16) << 32);    /* XL = 2.0, DxLDy = 0 */
        emit(c, 0);                              /* XH = 0, DxHDy = 0 */
        emit(c, (uint64_t)(2u << 16) << 32);    /* XM = 2.0, DxMDy = 0 */
        break;
    case F_LOADBLOCK6: load_block(c, 6, 8, 0); break;           /* 16 bytes to 3.5 KiB */
    case F_LOADTILE6:  load_tile(c, 6, 0, 0, 7, 0); break;      /* 1 row, 8 or 16 bytes */
    }
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

static const char *size_name(int s) { return s == SZ_8 ? "i8" : s == SZ_16 ? "rgba16" : "rgba32"; }

/* --- parts ---------------------------------------------------------------------------- */

#define MAX_GEOS 96
static geo_t geos[MAX_GEOS];
static int n_geos;

static void add_geo(int size, int rowbytes, int rows, int offset, int stride)
{
    for (int ch = 0; ch < 2; ch++)
        geos[n_geos++] = (geo_t){ L_TILE, ch, size, rowbytes, rows, offset, stride ? stride : rowbytes };
}

static void part_a(void)
{
    static const int widths[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128 };
    for (unsigned i = 0; i < sizeof widths / sizeof widths[0]; i++) add_geo(SZ_8, widths[i], 16, 0, 0);
    static const int offw[] = { 24, 32, 48, 96 }, offs[] = { 8, 16, 32 };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) add_geo(SZ_8, offw[i], 16, offs[j], 0);
    add_geo(SZ_8, 32, 16, 0, 128);
    add_geo(SZ_8, 48, 16, 0, 128);
    add_geo(SZ_8, 48, 16, 0, 64);
    static const int rw[] = { 16, 24, 48 }, rr[] = { 2, 4, 8 };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) add_geo(SZ_8, rw[i], rr[j], 0, 0);
    add_geo(SZ_16, 32, 16, 0, 0);
    add_geo(SZ_16, 48, 16, 0, 0);
    add_geo(SZ_32, 32, 16, 0, 0);
    add_geo(SZ_32, 64, 16, 0, 0);

    tprintf("# part A: gap scans (RDRAM). bytes/words of TMEM that differ from the same list with a sync before the change\n");
    tprintf("A\tchange\tsize\trowbytes\trows\toffset\tstride\tgap\tdiff_bytes\tdiff_words\tfirst\tlast\tref_nonzero\n");
    show("A: geometry (%d scans)\n", n_geos);
    for (int i = 0; i < n_geos && !timed_out; i++)
    {
        const geo_t *g = &geos[i];
        cbuf_t *c = &cb;
        list_head(c); emit_load(c, g); syncs(c); emit_change(c, g);
        list_tail_and_run(c, -1, 0, ref_tmem);
        int reach = -1;
        for (int gap = 0; gap <= MAX_GAP; gap++)
        {
            list_head(c); emit_load(c, g);
            for (int k = 0; k < gap; k++) emit(c, CMD(0x00));
            emit_change(c, g);
            list_tail_and_run(c, -1, 0, test_tmem);
            diff_t d = compare();
            if (d.bytes) reach = gap;
            tprintf("A\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", g->change == CH_STI ? "sti" : "st7",
                    size_name(g->size), g->rowbytes, g->rows, g->offset, g->stride, gap, d.bytes, d.words, d.first, d.last, d.ref_nz);
        }
        if (g->change == CH_STI)
            show("%-6s %3dx%-2d o%-2d s%-3d T%3d", size_name(g->size), g->rowbytes, g->rows, g->offset, g->stride, reach);
        else
            show(" 7%3d\n", reach);
    }
}

static void part_b(void)
{
    static const geo_t bases[] = {
        { L_BLOCK, CH_ST7, SZ_16, 128, 1, 0, 0 },
        { L_TILE,  CH_ST7, SZ_8,  128, 16, 0, 128 },
    };
    static const int counts[] = { 1, 2, 4, 8 };
    tprintf("# part B: n filler commands between the load and ST7. moved words = diff_words / 2; noop baseline is 15 - n\n");
    tprintf("B\tload\tfiller\tn\tdiff_bytes\tdiff_words\tfirst\tlast\tref_nonzero\n");
    show("B: fillers\n");
    for (int b = 0; b < 2 && !timed_out; b++)
    {
        const geo_t *g = &bases[b];
        for (int f = 0; f < N_FILLERS && !timed_out; f++)
        {
            show("%c%-16s", b ? 't' : 'b', filler_names[f]);
            for (int ci = 0; ci < 4; ci++)
            {
                int n = counts[ci];
                cbuf_t *c = &cb;
                list_head(c); emit_load(c, g);
                for (int k = 0; k < n; k++) emit_filler(c, f, g);
                syncs(c); emit_change(c, g);
                list_tail_and_run(c, -1, 0, ref_tmem);
                list_head(c); emit_load(c, g);
                for (int k = 0; k < n; k++) emit_filler(c, f, g);
                emit_change(c, g);
                list_tail_and_run(c, -1, 0, test_tmem);
                diff_t d = compare();
                tprintf("B\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n", b ? "tile128" : "block128", filler_names[f], n,
                        d.bytes, d.words, d.first, d.last, d.ref_nz);
                show(" %2d", d.words / 2);
            }
            show("\n");
        }
    }
}

static void part_c(void)
{
    static const geo_t bases[] = {
        { L_BLOCK, CH_ST7, SZ_16, 128, 1, 0, 0 },
        { L_TILE,  CH_ST7, SZ_8,  128, 16, 0, 128 },
        { L_TILE,  CH_ST7, SZ_8,  32, 16, 0, 32 },
        { L_TILE,  CH_STI, SZ_8,  32, 16, 0, 32 },
    };
    static const char *const base_names[] = { "block128_st7", "tile128_st7", "tile32_st7", "tile32_sti" };
    static const int waits[] = { -1, 0, 1, 10, 100, 1000, 10000 };
    static const int gaps[] = { 0, 4 };
    tprintf("# part C: list submitted in two parts, split after the load and `gap` no-ops, with a wait (us) in between; -1 = one part\n");
    tprintf("C\tload\tgap\twait_us\tdiff_bytes\tdiff_words\tfirst\tlast\tref_nonzero\n");
    show("C: idle wait\n");
    for (int b = 0; b < 4 && !timed_out; b++)
    {
        const geo_t *g = &bases[b];
        cbuf_t *c = &cb;
        list_head(c); emit_load(c, g); syncs(c); emit_change(c, g);
        list_tail_and_run(c, -1, 0, ref_tmem);
        for (int gi = 0; gi < 2; gi++)
        {
            show("%-12s g%d", base_names[b], gaps[gi]);
            for (int w = 0; w < 7; w++)
            {
                list_head(c); emit_load(c, g);
                for (int k = 0; k < gaps[gi]; k++) emit(c, CMD(0x00));
                int split = (int)(c->p - c->start);
                emit_change(c, g);
                list_tail_and_run(c, waits[w] < 0 ? -1 : split, waits[w], test_tmem);
                diff_t d = compare();
                tprintf("C\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", base_names[b], gaps[gi], waits[w], d.bytes, d.words, d.first, d.last, d.ref_nz);
                show(" %3d", d.words);
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
    /* source bytes are never 0 and src_b differs from src_a in every byte */
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
    tprintf("# rdp_load_hazard v" VERSION "\n# mi_version\t0x%08lX\n# tv_type\t%s\n", (unsigned long)HW_MI_VERSION, tv);

    part_a();
    if (!timed_out) part_b();
    if (!timed_out) part_c();
    if (timed_out) { show("RDP TIMEOUT\n"); tprintf("# ABORTED: RDP timeout\n"); }

    char path[64];
    struct stat st;
    if (!debug_init_sdfs("sd:/", -1)) { show("NO SD CARD - NOT SAVED\n"); for (;;) {} }
    mkdir("sd:/rdp_load_hazard", 0777);
    int n;
    for (n = 1; n < 1000; n++)
    {
        snprintf(path, sizeof path, "sd:/rdp_load_hazard/v3_%03d.txt", n);
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

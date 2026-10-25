/*
 * rdp_timing - measure RDP execution time on real hardware
 *
 * Every test case is a small RDP command list that ends in Sync_Full. The
 * list is run several times; for each run the RDP's own cycle counters are
 * cleared just before the list is started and read when the Sync_Full
 * interrupt arrives:
 *
 *   DPC_CLOCK     RCP clock cycles (62.5 MHz) while the RDP clock runs
 *   DPC_BUFBUSY   cycles the command buffer was busy
 *   DPC_PIPEBUSY  cycles the pipeline was busy
 *   DPC_TMEM      cycles TMEM was busy (loads)
 *
 * plus the CPU COUNT register (46.875 MHz) from the DPC_END write until the
 * interrupt was seen, as an independent wall-clock measurement.
 *
 * State setup (modes, texture loads) runs in a separate, unmeasured list
 * first, so a measured list contains only the commands under test. Empty
 * lists ("baseline") are measured too, so the fixed cost of starting a list
 * and of the final Sync_Full can be subtracted.
 *
 * The whole matrix runs twice: once with the VI switched off (no video DMA
 * competing for RDRAM) and once with the VI scanning out a 320x240x16 frame.
 *
 * Results are written as tab-separated text to the flash cart's SD card:
 *   sd:/rdp_timing/rdp_timing_NNN.txt      one line per case (medians etc.)
 *   sd:/rdp_timing/rdp_timing_NNN_raw.txt  one line per run
 * and also sent to the debug log (USB / IS-Viewer) if no SD card is usable.
 *
 * The RDP is driven directly through the DP registers (rdpq is not used),
 * so every command is exactly as listed here.
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>

#define VERSION     "3"
#define RUNS        5           /* runs per case per pass */
#define FB_W        320
#define FB_H        240

/* --- hardware registers ------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_DPC_CLOCK    REG(0xA4100010)
#define HW_DPC_BUFBUSY  REG(0xA4100014)
#define HW_DPC_PIPEBUSY REG(0xA4100018)
#define HW_DPC_TMEM     REG(0xA410001C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_VERSION   REG(0xA4300004)
#define HW_MI_INTR      REG(0xA4300008)
#define HW_VI_CTRL      REG(0xA4400000)
#define HW_VI_V_CURRENT REG(0xA4400010)
#define HW_VI_V_VIDEO   REG(0xA4400028)
#define DMEM            ((volatile uint32_t *)0xA4000000)

/* DPC_STATUS write bits */
#define DPC_CLR_XBUS        0x001
#define DPC_SET_XBUS        0x002
#define DPC_CLR_FREEZE      0x004
#define DPC_CLR_FLUSH       0x010
#define DPC_CLR_COUNTERS    0x3C0   /* TMEM, pipe, cmd (bufbusy) and clock */
/* DPC_STATUS read bits */
#define DPC_PENDING         0x600   /* start or end pending */

#define MI_INTR_DP          0x20
#define MI_CLEAR_DP_INT     0x800

/* --- RDP command encoders ---------------------------------------------- */

#define CMD(op)         ((uint64_t)(op) << 56)
#define FX2(v)          ((uint64_t)((v) * 4) & 0xFFF)  /* 10.2 fixed point */

enum { FMT_RGBA = 0, FMT_CI = 2, FMT_I = 4 };
enum { SZ_4 = 0, SZ_8 = 1, SZ_16 = 2, SZ_32 = 3 };

typedef struct { uint64_t *p, *start; } cbuf_t;

static inline void emit(cbuf_t *c, uint64_t w) { *c->p++ = w; }

static void sync_full(cbuf_t *c) { emit(c, CMD(0x29)); }
static void sync_pipe(cbuf_t *c) { emit(c, CMD(0x27)); }
static void sync_load(cbuf_t *c) { emit(c, CMD(0x26)); }
static void sync_tile(cbuf_t *c) { emit(c, CMD(0x28)); }

static void set_color_image(cbuf_t *c, int fmt, int size, int width, uint32_t addr)
{
    emit(c, CMD(0x3F) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr);
}

static void set_texture_image(cbuf_t *c, int fmt, int size, int width, uint32_t addr)
{
    emit(c, CMD(0x3D) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr);
}

static void set_tile(cbuf_t *c, int tile, int fmt, int size, int line, int tmem,
                     int maskt, int masks, int ct)
{
    emit(c, CMD(0x35) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)line << 41
         | (uint64_t)tmem << 32 | (uint64_t)tile << 24 | (uint64_t)ct << 19
         | (uint64_t)maskt << 14 | (uint64_t)masks << 4);
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

static uint64_t set_scissor_cmd(void)
{
    return CMD(0x2D) | FX2(0) << 44 | FX2(0) << 32 | FX2(FB_W) << 12 | FX2(FB_H);
}

/* Set_Combine_Mode: (A - B) * C + D for colour and alpha, both cycles */
typedef struct { int a, b, c, d, aa, ab, ac, ad; } comb_t;

static uint64_t combine_cmd(comb_t c0, comb_t c1)
{
    return CMD(0x3C)
        | (uint64_t)c0.a << 52 | (uint64_t)c0.c << 47 | (uint64_t)c0.aa << 44 | (uint64_t)c0.ac << 41
        | (uint64_t)c1.a << 37 | (uint64_t)c1.c << 32 | (uint64_t)c0.b << 28 | (uint64_t)c1.b << 24
        | (uint64_t)c1.aa << 21 | (uint64_t)c1.ac << 18 | (uint64_t)c0.d << 15 | (uint64_t)c0.ab << 12
        | (uint64_t)c0.ad << 9 | (uint64_t)c1.d << 6 | (uint64_t)c1.ab << 3 | (uint64_t)c1.ad;
}

/* Inputs: colour A/B 15 = 0, C 31 = 0, D 7 = 0; alpha 7 = 0 */
static const comb_t CC_PRIM     = { 15, 15, 31, 3, 7, 7, 7, 3 };   /* PRIM */
static const comb_t CC_SHADE    = { 15, 15, 31, 4, 7, 7, 7, 4 };   /* SHADE */
static const comb_t CC_TEX      = { 15, 15, 31, 1, 7, 7, 7, 1 };   /* TEX0 */
static const comb_t CC_TEXPRIM  = {  1, 15,  3, 7, 1, 7, 3, 7 };   /* TEX0 * PRIM */
static const comb_t CC_TEXSHADE = {  1, 15,  4, 7, 1, 7, 4, 7 };   /* TEX0 * SHADE */
static const comb_t CC_COMBENV  = {  0, 15,  5, 7, 0, 7, 5, 7 };   /* COMBINED * ENV */

/* Set_Other_Modes fields */
#define OM_1CYC         (0u << 20)
#define OM_2CYC         (1u << 20)
#define OM_COPY         (2u << 20)
#define OM_FILL         (3u << 20)
#define OM_PERSP        (1u << 19)
#define OM_TLUT         (1u << 15)
#define OM_BILERP       (1u << 13)  /* sample_type: 2x2 */
#define OM_BI_LERP01    (3u << 10)
#define OM_NO_DITHER    (0xFu << 4) /* rgb and alpha dither: none */

#define OM_FORCE_BLEND  (1u << 14)
#define OM_ALPHA_CVG    (1u << 13)
#define OM_IMAGE_READ   (1u << 6)
#define OM_Z_UPDATE     (1u << 5)
#define OM_Z_COMPARE    (1u << 4)
#define OM_AA           (1u << 3)
#define OM_Z_PRIM       (1u << 2)
#define OM_B_M2A_MEM    ((1u << 22) | (1u << 20))   /* both cycles: memory colour */
#define OM_B_M2B_MEM    ((1u << 18) | (1u << 16))   /* both cycles: memory coverage */

/* --- buffers --------------------------------------------------------- */

static uint32_t *fb;            /* render target, up to 320x240x32 */
static uint16_t *zb;            /* Z buffer 320x240 */
static uint16_t *tex16;         /* 32x32 RGBA16 */
static uint32_t *tex32;         /* 32x16 RGBA32 */
static uint8_t  *texci8;        /* 32x32 CI8 */
static uint16_t *tlut;          /* 256 x RGBA16 */
static uint8_t  *texi4;         /* 64x64 I4 */
static uint8_t  *texsrc;        /* 8 KiB load source */
static uint8_t  *texseq;        /* 16x16 CI8, indices 0..255 in order */
static uint8_t  *texzero;       /* 2 KiB of zeros (clears TMEM) */
static uint8_t  *texalt;        /* 1 KiB, a different pattern: source of hazard bytes */
static cbuf_t cb_setup, cb_body;

/* --- test cases -------------------------------------------------------- */

enum kind { K_BASELINE, K_FILL, K_COPY, K_RECT, K_TRI, K_LOAD, K_CMD, K_PROBE, K_HAZARD };
enum { TX_NONE = -1, TX_RGBA16, TX_RGBA32, TX_CI8, TX_I4 };
enum { F_BLEND = 1, F_ZPASS = 2, F_AA = 4, F_PERSP = 8, F_ZFAIL = 16 };
#define F_Z (F_ZPASS | F_ZFAIL)
enum { TRI_FLAT, TRI_SHADE, TRI_TEX, TRI_SHADE_TEX_Z, TRI_TEX_PERSP };
/* Diagnostic cases for the CI8/TLUT rendering difference seen on hardware */
enum { PR_CI8_AS_I8, PR_CI8_AS_I8_NOTLUT, PR_TLUT_VIEW, PR_TLUT_VIEW_LINE32 };
/* Load hazard probes: a load, then a gap, then a command that changes the
 * load's parameters while the load may still be running */
enum { HZ_TILE_NONE, HZ_TILE_STI, HZ_TILE_ST7, HZ_TILE_ST6, HZ_BLOCK_STI };
enum { GAP_SYNC_PIPE = -1, GAP_SYNC_LOAD = -2, GAP_SYNC_TILE = -3 };
enum { LD_TILE, LD_BLOCK, LD_TLUT };

typedef struct {
    enum kind kind;
    int cyc;        /* 1, 2 (1/2-cycle), 0 for fill/copy/none */
    int fbbits;     /* 16, 32, 0 */
    int tex;        /* TX_* */
    int bilerp;
    int flags;      /* F_* */
    int comb;       /* combiner: 0 prim, 1 tex, 2 tex*prim */
    int sub;        /* tri type / load type / command index */
    int ldfmt, ldsize;
    int w, h, k;
    int xbus;
    int dump;       /* write the framebuffer to SD in the dump phase */
    int x0;         /* left edge of rectangles */
    int fbw;        /* framebuffer width in pixels */
    int fbfill;     /* framebuffer prefilled with a per-pixel pattern instead of 0 */
    int span;       /* belongs to the "span" group (2-cycle first-pixel cases) */
    int gap;        /* hazard: noops between load and change, or GAP_SYNC_* */
} tcase_t;

static const char *const tex_names[] = { "rgba16", "rgba32", "ci8", "i4" };
static const char *const tri_names[] = { "tri_flat", "tri_shade", "tri_tex", "tri_shade_tex_z", "tri_tex_persp" };
static const char *const load_names[] = { "load_tile", "load_block", "load_tlut" };
static const char *const probe_names[] = {
    "ci8_as_i8", "ci8_as_i8_notlut", "tlut_view", "tlut_view_line32"
};

/* Commands for the per-command overhead tests */
static const struct { const char *name; uint64_t word; } cmd_tests[] = {
    { "noop",            CMD(0x00) },
    { "sync_pipe",       CMD(0x27) },
    { "sync_tile",       CMD(0x28) },
    { "sync_load",       CMD(0x26) },
    { "set_prim_color",  CMD(0x3A) | 0xFF8040FF },
    { "set_env_color",   CMD(0x3B) | 0x40C080FF },
    { "set_fill_color",  CMD(0x37) | 0x7BDE7BDE },
    { "set_other_modes", CMD(0x2F) | (uint64_t)(OM_1CYC | OM_BI_LERP01 | OM_NO_DITHER) << 32 },
    { "set_combine",     0 },   /* filled in at run time */
    { "set_scissor",     0 },
    { "set_tile",        CMD(0x35) | (uint64_t)SZ_16 << 51 | 8ull << 41 | 5 << 14 | 5 << 4 },
    { "set_tile_size",   CMD(0x32) | FX2(31) << 12 | FX2(31) },
    { "set_color_image", 0 },
};
#define N_CMD_TESTS ((int)(sizeof cmd_tests / sizeof cmd_tests[0]))

#define MAX_CASES 512
static tcase_t cases[MAX_CASES];
static int n_cases;

static int clampk(uint32_t pixels)
{
    uint32_t k = pixels ? 65536 / pixels : 32;
    return k < 1 ? 1 : k > 32 ? 32 : (int)k;
}

static tcase_t *add(enum kind kind)
{
    tcase_t *t = &cases[n_cases++];
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->tex = TX_NONE;
    t->fbw = FB_W;
    return t;
}

static void build_case_list(void)
{
    static const int sizes[][2] = {
        { 8, 8 }, { 32, 32 }, { 64, 64 }, { 128, 128 }, { 320, 1 }, { 1, 240 }, { 320, 240 },
    };
    const int n_sizes = sizeof sizes / sizeof sizes[0];

    /* Baselines: empty lists from RDRAM and from DMEM */
    for (int x = 0; x < 2; x++) { tcase_t *t = add(K_BASELINE); t->xbus = x; t->k = 0; }

    /* Fill mode rectangles */
    for (int fbb = 16; fbb <= 32; fbb += 16)
        for (int s = 0; s < n_sizes; s++)
        {
            tcase_t *t = add(K_FILL);
            t->fbbits = fbb; t->w = sizes[s][0]; t->h = sizes[s][1];
            t->k = clampk(t->w * t->h);
        }

    /* Copy mode texture rectangles (16-bit framebuffer only) */
    for (int tx = TX_RGBA16; tx <= TX_CI8; tx += 2)
        for (int s = 0; s < n_sizes; s++)
        {
            tcase_t *t = add(K_COPY);
            t->fbbits = 16; t->tex = tx; t->w = sizes[s][0]; t->h = sizes[s][1];
            t->k = clampk(t->w * t->h);
            if (tx == TX_CI8) t->dump = 1;      /* differed from angrylion in v1 */
        }

    /* 1/2-cycle rectangles */
    static const struct { int tex, bilerp, flags, comb; } rect_modes[] = {
        { TX_NONE,   0, 0,            0 },
        { TX_RGBA16, 0, 0,            1 },
        { TX_RGBA16, 1, 0,            1 },
        { TX_RGBA16, 1, 0,            2 },
        { TX_RGBA32, 0, 0,            1 },
        { TX_CI8,    0, 0,            1 },
        { TX_I4,     0, 0,            1 },
        { TX_NONE,   0, F_BLEND,      0 },
        { TX_NONE,   0, F_ZPASS,      0 },
        { TX_NONE,   0, F_ZFAIL,      0 },
        { TX_NONE,   0, F_AA,         0 },
        { TX_RGBA16, 1, F_BLEND | F_ZPASS, 2 },
    };
    for (int cyc = 1; cyc <= 2; cyc++)
        for (int fbb = 16; fbb <= 32; fbb += 16)
            for (unsigned m = 0; m < sizeof rect_modes / sizeof rect_modes[0]; m++)
                for (int s = 0; s < n_sizes; s++)
                {
                    tcase_t *t = add(K_RECT);
                    t->cyc = cyc; t->fbbits = fbb;
                    t->tex = rect_modes[m].tex; t->bilerp = rect_modes[m].bilerp;
                    t->flags = rect_modes[m].flags; t->comb = rect_modes[m].comb;
                    t->w = sizes[s][0]; t->h = sizes[s][1];
                    t->k = clampk(t->w * t->h);
                    /* Dump the cases that rendered differently from angrylion in
                     * v1 (CI8; 2-cycle full screen with framebuffer reads), and
                     * the matching 1-cycle cases as controls */
                    if (t->tex == TX_CI8) t->dump = 1;
                    if (t->w == 320 && t->h == 240 && (t->flags & (F_BLEND | F_AA))) t->dump = 1;
                }

    /* Triangles (right triangles with legs of s pixels) */
    static const int tri_sizes[] = { 8, 32, 128, 240 };
    static const struct { int type, flags; } tri_modes[] = {
        { TRI_FLAT, 0 }, { TRI_SHADE, 0 }, { TRI_TEX, 0 },
        { TRI_SHADE_TEX_Z, F_ZPASS }, { TRI_SHADE_TEX_Z, F_ZFAIL }, { TRI_TEX_PERSP, F_PERSP },
    };
    for (unsigned m = 0; m < sizeof tri_modes / sizeof tri_modes[0]; m++)
        for (unsigned s = 0; s < sizeof tri_sizes / sizeof tri_sizes[0]; s++)
        {
            int type = tri_modes[m].type;
            tcase_t *t = add(K_TRI);
            t->cyc = 1; t->fbbits = 16; t->sub = type;
            t->tex = (type == TRI_FLAT || type == TRI_SHADE) ? TX_NONE : TX_RGBA16;
            t->bilerp = type == TRI_SHADE_TEX_Z;
            t->flags = tri_modes[m].flags;
            t->w = t->h = tri_sizes[s];
            t->k = clampk(t->w * (t->w + 1) / 2);
        }

    /* TMEM loads */
    static const struct { int type, fmt, size, w, h; } loads[] = {
        { LD_TILE,  FMT_RGBA, SZ_16,  8,  8 }, { LD_TILE,  FMT_RGBA, SZ_16, 16, 16 },
        { LD_TILE,  FMT_RGBA, SZ_16, 32, 32 }, { LD_TILE,  FMT_RGBA, SZ_16, 64, 32 },
        { LD_BLOCK, FMT_RGBA, SZ_16,  8,  8 }, { LD_BLOCK, FMT_RGBA, SZ_16, 16, 16 },
        { LD_BLOCK, FMT_RGBA, SZ_16, 32, 32 }, { LD_BLOCK, FMT_RGBA, SZ_16, 64, 32 },
        { LD_TILE,  FMT_RGBA, SZ_32, 16, 16 }, { LD_TILE,  FMT_RGBA, SZ_32, 32, 16 },
        { LD_TILE,  FMT_CI,   SZ_8,  32, 32 }, { LD_TILE,  FMT_CI,   SZ_8,  64, 64 },
        { LD_TLUT,  FMT_RGBA, SZ_16, 16,  1 }, { LD_TLUT,  FMT_RGBA, SZ_16, 256, 1 },
    };
    for (unsigned i = 0; i < sizeof loads / sizeof loads[0]; i++)
    {
        tcase_t *t = add(K_LOAD);
        t->sub = loads[i].type; t->ldfmt = loads[i].fmt; t->ldsize = loads[i].size;
        t->w = loads[i].w; t->h = loads[i].h; t->k = 8;
    }

    /* Per-command overhead: from RDRAM, and a few from DMEM (XBUS) */
    for (int i = 0; i < N_CMD_TESTS; i++) { tcase_t *t = add(K_CMD); t->sub = i; t->k = 256; }
    static const int xbus_cmds[] = { 0, 1, 4, 7 };
    for (unsigned i = 0; i < sizeof xbus_cmds / sizeof xbus_cmds[0]; i++)
    {
        tcase_t *t = add(K_CMD); t->sub = xbus_cmds[i]; t->k = 256; t->xbus = 1;
    }

    /* CI8 / TLUT probes (1-cycle, 32-bit so values are exact, one rectangle, dumped) */
    for (int i = PR_CI8_AS_I8; i <= PR_TLUT_VIEW_LINE32; i++)
    {
        tcase_t *t = add(K_PROBE);
        t->cyc = 1; t->fbbits = 32; t->sub = i; t->k = 1; t->dump = 1; t->tex = TX_CI8;
        t->w = t->h = i <= PR_CI8_AS_I8_NOTLUT ? 32 : 16;
    }

    /* v3: load hazards. TMEM is read back as I8 (32x64 = the lower 2 KiB) */
    static const struct { int sub, gap; } hazards[] = {
        { HZ_TILE_NONE, 0 },
        { HZ_TILE_STI, 0 }, { HZ_TILE_STI, 1 }, { HZ_TILE_STI, 2 }, { HZ_TILE_STI, 4 },
        { HZ_TILE_STI, 8 }, { HZ_TILE_STI, 16 }, { HZ_TILE_STI, 32 }, { HZ_TILE_STI, 64 },
        { HZ_TILE_STI, GAP_SYNC_PIPE }, { HZ_TILE_STI, GAP_SYNC_LOAD }, { HZ_TILE_STI, GAP_SYNC_TILE },
        { HZ_TILE_ST7, 0 }, { HZ_TILE_ST7, 4 }, { HZ_TILE_ST7, 16 },
        { HZ_TILE_ST7, GAP_SYNC_PIPE }, { HZ_TILE_ST7, GAP_SYNC_TILE },
        { HZ_TILE_ST6, 0 },
        { HZ_BLOCK_STI, 0 }, { HZ_BLOCK_STI, 4 }, { HZ_BLOCK_STI, 16 }, { HZ_BLOCK_STI, GAP_SYNC_PIPE },
    };
    for (unsigned i = 0; i < sizeof hazards / sizeof hazards[0]; i++)
    {
        tcase_t *t = add(K_HAZARD);
        t->cyc = 1; t->fbbits = 32; t->k = 1; t->dump = 1; t->tex = TX_RGBA16;
        t->sub = hazards[i].sub; t->gap = hazards[i].gap;
        t->w = 32; t->h = 64;
    }

    /* v3: 2-cycle first-pixel cases (angrylion differs from hardware at x=0
     * of full-width spans in 2-cycle mode with framebuffer reads) */
    static const struct { int cyc, fbb, flags, x0, w, fbw, fill, h; } spans[] = {
        { 2, 16, F_BLEND,  0, 320, 320, 0, 16 },
        { 2, 16, F_BLEND,  0, 300, 320, 0, 16 },
        { 2, 16, F_BLEND, 20, 300, 320, 0, 16 },
        { 2, 16, F_BLEND, 20, 280, 320, 0, 16 },
        { 2, 16, F_BLEND,  0, 320, 336, 0, 16 },
        { 2, 16, F_BLEND,  0, 320, 320, 1, 16 },
        { 2, 16, F_BLEND, 20, 300, 320, 1, 16 },
        { 2, 16, F_BLEND,  0, 320, 320, 0,  1 },
        { 2, 32, F_BLEND,  0, 320, 320, 0, 16 },
        { 2, 32, F_BLEND, 20, 300, 320, 0, 16 },
        { 2, 16, F_AA,     0, 320, 320, 0, 16 },
        { 2, 16, F_AA,    20, 300, 320, 0, 16 },
        { 1, 16, F_BLEND,  0, 320, 320, 0, 16 },
        { 1, 16, F_BLEND,  0, 320, 320, 1, 16 },
        { 2, 16, 0,        0, 320, 320, 0, 16 },
    };
    for (unsigned i = 0; i < sizeof spans / sizeof spans[0]; i++)
    {
        tcase_t *t = add(K_RECT);
        t->span = 1; t->dump = 1; t->k = 1;
        t->cyc = spans[i].cyc; t->fbbits = spans[i].fbb; t->flags = spans[i].flags;
        t->x0 = spans[i].x0; t->w = spans[i].w; t->fbw = spans[i].fbw;
        t->fbfill = spans[i].fill; t->h = spans[i].h;
    }
}

/* --- description columns -------------------------------------------- */

static uint32_t case_pixels(const tcase_t *t)
{
    switch (t->kind)
    {
    case K_FILL: case K_COPY: case K_RECT: case K_PROBE: case K_HAZARD: return (uint32_t)t->w * t->h;
    case K_TRI:  return (uint32_t)t->w * (t->w + 1) / 2;   /* approximate */
    default:     return 0;
    }
}

static int case_draws(const tcase_t *t)
{
    return t->kind == K_FILL || t->kind == K_COPY || t->kind == K_RECT || t->kind == K_TRI
        || t->kind == K_PROBE || t->kind == K_HAZARD;
}

static const char *case_group(const tcase_t *t)
{
    static const char *const g[] = { "baseline", "fill", "copy", "rect", "tri", "load", "cmd", "probe", "hazard" };
    if (t->span) return "span";
    return g[t->kind];
}

static const char *case_prim(const tcase_t *t)
{
    switch (t->kind)
    {
    case K_BASELINE: return "empty";
    case K_FILL:     return "fill_rect";
    case K_COPY:     return "tex_rect";
    case K_RECT:     return t->tex == TX_NONE ? "fill_rect" : "tex_rect";
    case K_TRI:      return tri_names[t->sub];
    case K_LOAD:     return load_names[t->sub];
    case K_CMD:      return cmd_tests[t->sub].name;
    case K_PROBE:    return probe_names[t->sub];
    case K_HAZARD:
    {
        static const char *const n[] = {
            "load_tile", "load_tile+set_tex_image", "load_tile+set_tile7",
            "load_tile+set_tile6", "load_block+set_tex_image"
        };
        return n[t->sub];
    }
    }
    return "?";
}

static const char *case_cycle(const tcase_t *t)
{
    if (t->kind == K_FILL) return "fill";
    if (t->kind == K_COPY) return "copy";
    if (t->cyc == 1) return "1cyc";
    if (t->cyc == 2) return "2cyc";
    return "-";
}

static const char *case_tex(const tcase_t *t)
{
    if (t->kind == K_PROBE) return t->sub <= PR_CI8_AS_I8_NOTLUT ? "ci8" : "ci8seq";
    if (t->kind == K_HAZARD) return "i8";
    if (t->kind == K_LOAD)
    {
        if (t->sub == LD_TLUT) return "tlut";
        if (t->ldfmt == FMT_CI) return "ci8";
        return t->ldsize == SZ_32 ? "rgba32" : "rgba16";
    }
    return t->tex == TX_NONE ? "-" : tex_names[t->tex];
}

static void case_flags(const tcase_t *t, char *buf, size_t n)
{
    buf[0] = 0;
    if (t->flags & F_BLEND) strlcat(buf, "blend,", n);
    if (t->flags & F_ZPASS) strlcat(buf, "zpass,", n);
    if (t->flags & F_ZFAIL) strlcat(buf, "zfail,", n);
    if (t->flags & F_AA)    strlcat(buf, "aa,", n);
    if (t->flags & F_PERSP) strlcat(buf, "persp,", n);
    if (t->kind == K_HAZARD)
    {
        char g[16];
        if (t->gap == GAP_SYNC_PIPE)      strlcpy(g, "sync_pipe,", sizeof g);
        else if (t->gap == GAP_SYNC_LOAD) strlcpy(g, "sync_load,", sizeof g);
        else if (t->gap == GAP_SYNC_TILE) strlcpy(g, "sync_tile,", sizeof g);
        else snprintf(g, sizeof g, "gap%d,", t->gap);
        strlcat(buf, g, n);
    }
    if (t->span)
    {
        char x[24];
        if (t->x0)          { snprintf(x, sizeof x, "x0=%d,", t->x0); strlcat(buf, x, n); }
        if (t->fbw != FB_W) { snprintf(x, sizeof x, "fbw=%d,", t->fbw); strlcat(buf, x, n); }
        if (t->fbfill) strlcat(buf, "prefill,", n);
    }
    size_t len = strlen(buf);
    if (len) buf[len - 1] = 0; else strlcpy(buf, "-", n);
}

/* --- command list construction -------------------------------------- */

static uint32_t fb_fill_color(int fbbits)
{
    return fbbits == 32 ? 0x804020FFu : 0x7BDE7BDEu;
}

/* Load the case's texture into TMEM and set up render tile 0 */
static void setup_texture(cbuf_t *c, int tex)
{
    switch (tex)
    {
    case TX_RGBA16:
        set_texture_image(c, FMT_RGBA, SZ_16, 32, PhysicalAddr(tex16));
        set_tile(c, 7, FMT_RGBA, SZ_16, 8, 0, 0, 0, 0);
        sync_load(c);
        load_tile(c, 7, 0, 0, 31, 31);
        sync_pipe(c);
        set_tile(c, 0, FMT_RGBA, SZ_16, 8, 0, 5, 5, 0);
        set_tile_size(c, 0, 0, 0, 31, 31);
        break;
    case TX_RGBA32:
        set_texture_image(c, FMT_RGBA, SZ_32, 32, PhysicalAddr(tex32));
        set_tile(c, 7, FMT_RGBA, SZ_32, 8, 0, 0, 0, 0);
        sync_load(c);
        load_tile(c, 7, 0, 0, 31, 15);
        sync_pipe(c);
        set_tile(c, 0, FMT_RGBA, SZ_32, 8, 0, 4, 5, 0);
        set_tile_size(c, 0, 0, 0, 31, 15);
        break;
    case TX_CI8:
        set_texture_image(c, FMT_CI, SZ_8, 32, PhysicalAddr(texci8));
        set_tile(c, 7, FMT_CI, SZ_8, 4, 0, 0, 0, 0);
        sync_load(c);
        load_tile(c, 7, 0, 0, 31, 31);
        sync_pipe(c);           /* v3: let the load finish (v2 lacked this) */
        set_texture_image(c, FMT_RGBA, SZ_16, 256, PhysicalAddr(tlut));
        sync_tile(c);
        set_tile(c, 6, FMT_I, SZ_4, 0, 0x100, 0, 0, 0);  /* TLUT load tile must be 4-bit */
        sync_load(c);
        load_tlut(c, 6, 0, 0, 255, 0);
        sync_pipe(c);
        set_tile(c, 0, FMT_CI, SZ_8, 4, 0, 5, 5, 0);
        set_tile_size(c, 0, 0, 0, 31, 31);
        break;
    case TX_I4:
        /* 64x64 I4 = 2 KiB, loaded as 1024 16-bit words; 4 qwords per row */
        set_texture_image(c, FMT_RGBA, SZ_16, 1024, PhysicalAddr(texi4));
        set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0, 0, 0, 0);
        sync_load(c);
        load_block(c, 7, 1024, 2048 / 4);
        sync_pipe(c);
        set_tile(c, 0, FMT_I, SZ_4, 4, 0, 6, 6, 0);
        set_tile_size(c, 0, 0, 0, 63, 63);
        break;
    }
}

static void build_setup(const tcase_t *t, cbuf_t *c)
{
    c->p = c->start;
    sync_pipe(c);
    if (t->fbbits)
        set_color_image(c, FMT_RGBA, t->fbbits == 32 ? SZ_32 : SZ_16, t->fbw, PhysicalAddr(fb));
    emit(c, CMD(0x3E) | PhysicalAddr(zb));                      /* Set_Z_Image */
    emit(c, set_scissor_cmd());
    emit(c, CMD(0x3A) | 0xC08040FF);                            /* Set_Prim_Color */
    emit(c, CMD(0x3B) | 0x80C0FFFF);                            /* Set_Env_Color */
    emit(c, CMD(0x2E) | 0x0100ull << 16);                       /* Set_Prim_Depth */
    if (t->fbbits) emit(c, CMD(0x37) | fb_fill_color(t->fbbits));

    uint32_t hi = OM_BI_LERP01 | OM_NO_DITHER, lo = 0;
    comb_t c0 = CC_PRIM, c1;

    switch (t->kind)
    {
    case K_FILL:
        hi = OM_FILL;
        break;
    case K_COPY:
        setup_texture(c, t->tex);
        hi = OM_COPY | (t->tex == TX_CI8 ? OM_TLUT : 0);
        break;
    case K_RECT:
    case K_TRI:
        if (t->tex != TX_NONE) setup_texture(c, t->tex);
        hi |= t->cyc == 2 ? OM_2CYC : OM_1CYC;
        if (t->bilerp) hi |= OM_BILERP;
        if (t->tex == TX_CI8) hi |= OM_TLUT;
        if (t->flags & F_PERSP) hi |= OM_PERSP;
        if (t->flags & F_BLEND) lo |= OM_FORCE_BLEND | OM_IMAGE_READ | OM_B_M2A_MEM;
        if (t->flags & F_Z) lo |= OM_Z_COMPARE | OM_Z_UPDATE | (t->kind == K_RECT ? OM_Z_PRIM : 0);
        if (t->flags & F_AA) lo |= OM_AA | OM_IMAGE_READ | OM_ALPHA_CVG | OM_B_M2A_MEM | OM_B_M2B_MEM;
        if (t->kind == K_RECT)
            c0 = t->comb == 0 ? CC_PRIM : t->comb == 1 ? CC_TEX : CC_TEXPRIM;
        else
            c0 = t->sub == TRI_FLAT ? CC_PRIM : t->sub == TRI_SHADE ? CC_SHADE
               : t->sub == TRI_SHADE_TEX_Z ? CC_TEXSHADE : CC_TEX;
        break;
    case K_PROBE:
        hi |= OM_1CYC;
        c0 = CC_TEX;
        if (t->sub <= PR_CI8_AS_I8_NOTLUT)
        {
            /* Load the CI8 texture (and the TLUT), then read TMEM back as I8:
             * each pixel's red channel is the palette index stored there */
            set_texture_image(c, FMT_CI, SZ_8, 32, PhysicalAddr(texci8));
            set_tile(c, 7, FMT_CI, SZ_8, 4, 0, 0, 0, 0);
            sync_load(c);
            load_tile(c, 7, 0, 0, 31, 31);
            sync_pipe(c);       /* v3 */
            if (t->sub == PR_CI8_AS_I8)
            {
                set_texture_image(c, FMT_RGBA, SZ_16, 256, PhysicalAddr(tlut));
                sync_tile(c);
                set_tile(c, 6, FMT_I, SZ_4, 0, 0x100, 0, 0, 0);
                sync_load(c);
                load_tlut(c, 6, 0, 0, 255, 0);
            }
            sync_pipe(c);
            set_tile(c, 0, FMT_I, SZ_8, 4, 0, 5, 5, 0);
            set_tile_size(c, 0, 0, 0, 31, 31);
        }
        else
        {
            /* 16x16 CI8 with indices 0..255 in order: pixel i shows palette entry i */
            set_texture_image(c, FMT_CI, SZ_8, 16, PhysicalAddr(texseq));
            set_tile(c, 7, FMT_CI, SZ_8, 2, 0, 0, 0, 0);
            sync_load(c);
            load_tile(c, 7, 0, 0, 15, 15);
            sync_pipe(c);       /* v3 */
            set_texture_image(c, FMT_RGBA, SZ_16, 256, PhysicalAddr(tlut));
            sync_tile(c);
            set_tile(c, 6, FMT_I, SZ_4, t->sub == PR_TLUT_VIEW_LINE32 ? 32 : 0, 0x100, 0, 0, 0);
            sync_load(c);
            load_tlut(c, 6, 0, 0, 255, 0);
            sync_pipe(c);
            set_tile(c, 0, FMT_CI, SZ_8, 2, 0, 4, 4, 0);
            set_tile_size(c, 0, 0, 0, 15, 15);
            hi |= OM_TLUT;
        }
        break;
    case K_HAZARD:
    {
        hi |= OM_1CYC;
        c0 = CC_TEX;
        int block = t->sub == HZ_BLOCK_STI;
        /* Clear the lower 2 KiB of TMEM first (with proper syncs) */
        set_texture_image(c, FMT_RGBA, SZ_16, 1024, PhysicalAddr(texzero));
        set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0, 0, 0, 0);
        sync_load(c);
        load_block(c, 7, 1024, 0);
        sync_pipe(c);
        sync_tile(c);
        /* The load under test: 32x32 bytes from texci8 into TMEM 0..0x3FF */
        if (block)
        {
            set_texture_image(c, FMT_RGBA, SZ_16, 512, PhysicalAddr(texci8));
            set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0, 0, 0, 0);
            sync_load(c);
            load_block(c, 7, 512, 2048 / 4);
        }
        else
        {
            set_texture_image(c, FMT_I, SZ_8, 32, PhysicalAddr(texci8));
            set_tile(c, 7, FMT_I, SZ_8, 4, 0, 0, 0, 0);
            sync_load(c);
            load_tile(c, 7, 0, 0, 31, 31);
        }
        /* Gap */
        if (t->gap == GAP_SYNC_PIPE)      sync_pipe(c);
        else if (t->gap == GAP_SYNC_LOAD) sync_load(c);
        else if (t->gap == GAP_SYNC_TILE) sync_tile(c);
        else for (int i = 0; i < t->gap; i++) emit(c, CMD(0x00));
        /* The change, with no sync before it */
        switch (t->sub)
        {
        case HZ_TILE_STI:  set_texture_image(c, FMT_I, SZ_8, 32, PhysicalAddr(texalt)); break;
        case HZ_BLOCK_STI: set_texture_image(c, FMT_RGBA, SZ_16, 512, PhysicalAddr(texalt)); break;
        case HZ_TILE_ST7:  set_tile(c, 7, FMT_I, SZ_8, 4, 0x40, 0, 0, 0); break;
        case HZ_TILE_ST6:  set_tile(c, 6, FMT_I, SZ_8, 4, 0x40, 0, 0, 0); break;
        }
        sync_pipe(c);
        sync_tile(c);
        set_tile(c, 0, FMT_I, SZ_8, 4, 0, 6, 5, 0);     /* read TMEM 0..0x7FF as I8 */
        set_tile_size(c, 0, 0, 0, 31, 63);
        break;
    }
    case K_LOAD:
        if (t->sub == LD_TLUT)
        {
            set_texture_image(c, FMT_RGBA, SZ_16, 256, PhysicalAddr(texsrc));
            set_tile(c, 7, FMT_I, SZ_4, 0, 0x100, 0, 0, 0);  /* TLUT load tile must be 4-bit */
        }
        else
        {
            int bits = t->ldsize == SZ_32 ? 16 : t->ldsize == SZ_16 ? 16 : 8; /* 32-bit: split TMEM */
            int line = (t->w * bits + 63) / 64;
            set_texture_image(c, t->ldfmt, t->ldsize, t->w, PhysicalAddr(texsrc));
            set_tile(c, 7, t->ldfmt, t->ldsize, t->sub == LD_BLOCK ? 0 : line, 0, 0, 0, 0);
        }
        break;
    default:
        break;
    }

    c1 = t->cyc == 2 ? CC_COMBENV : c0;
    emit(c, CMD(0x2F) | (uint64_t)hi << 32 | lo);
    emit(c, combine_cmd(c0, c1));
    sync_full(c);
}

/* Depth of the i-th primitive: decreasing, so with F_ZPASS every primitive
 * passes the Z test (buffer cleared to far); with F_ZFAIL the buffer is
 * cleared to near and every primitive fails */
static uint32_t prim_z(int i) { return 0x7000 - i * 0x100; }

static void emit_rect(cbuf_t *c, const tcase_t *t, int i)
{
    int incl = t->kind == K_FILL || t->kind == K_COPY;  /* fill/copy: inclusive */
    if (t->flags & F_Z)
        emit(c, CMD(0x2E) | (uint64_t)prim_z(i) << 16);                 /* Set_Prim_Depth */
    uint64_t xl = FX2(t->x0 + t->w - incl), yl = FX2(t->h - incl), xh = FX2(t->x0);
    if (t->tex == TX_NONE)
        emit(c, CMD(0x36) | xl << 44 | yl << 32 | xh << 12);            /* Fill_Rectangle */
    else
    {
        emit(c, CMD(0x24) | xl << 44 | yl << 32 | xh << 12);            /* Texture_Rectangle, tile 0 */
        uint64_t dsdx = t->kind == K_COPY ? 4 << 10 : 1 << 10;           /* s5.10 */
        emit(c, dsdx << 16 | (1 << 10));
    }
}

/* Right triangle (0,0) (s,0) (0,s): left edge vertical, hypotenuse slope -1 */
static void emit_tri(cbuf_t *c, const tcase_t *t, int i)
{
    int s = t->w;
    int shade = t->sub == TRI_SHADE || t->sub == TRI_SHADE_TEX_Z;
    int tex = t->tex != TX_NONE;
    int z = (t->flags & F_Z) != 0;
    int op = 0x08 | shade << 2 | tex << 1 | z;

    emit(c, CMD(op) | 1ull << 55 | (uint64_t)((s * 4) & 0x3FFF) << 32);   /* lft, YL; YM = YH = 0 */
    emit(c, (uint64_t)(s << 16) << 32 | 0xFFFF0000u);                       /* XL, DxLDy = -1 */
    emit(c, 0);                                                              /* XH = 0, DxHDy = 0 */
    emit(c, (uint64_t)(s << 16) << 32);                                      /* XM, DxMDy = 0 */
    if (shade)
    {
        emit(c, 0x00FF00FF00FF00FFull);        /* R G B A = 255, no gradients */
        for (int i = 0; i < 7; i++) emit(c, 0);
    }
    if (tex)
    {
        uint64_t w = (t->flags & F_PERSP) ? 0x4000 : 0x7FFF;
        emit(c, w << 16);                      /* S = T = 0, W */
        emit(c, 32ull << 48);                  /* DsDx = 1 texel (s10.5) */
        emit(c, 0);
        emit(c, 0);
        emit(c, 32ull << 32);                  /* DtDe = 1 texel */
        emit(c, 32ull << 32);                  /* DtDy = 1 texel */
        emit(c, 0);
        emit(c, 0);
    }
    if (z)
    {
        emit(c, (uint64_t)prim_z(i) << 48);    /* Z (s15.16 integer part), DzDx = 0 */
        emit(c, 0);                            /* DzDe = DzDy = 0 */
    }
}

static void emit_load(cbuf_t *c, const tcase_t *t)
{
    sync_load(c);
    switch (t->sub)
    {
    case LD_TILE:  load_tile(c, 7, 0, 0, t->w - 1, t->h - 1); break;
    case LD_TLUT:  load_tlut(c, 7, 0, 0, t->w - 1, 0); break;
    case LD_BLOCK:
    {
        int line = t->w * 16 / 64;          /* qwords per row (16-bit texels) */
        load_block(c, 7, t->w * t->h, (2048 + line - 1) / line);
        break;
    }
    }
}

static uint64_t cmd_word(int i)
{
    if (!strcmp(cmd_tests[i].name, "set_combine"))     return combine_cmd(CC_TEX, CC_TEX);
    if (!strcmp(cmd_tests[i].name, "set_scissor"))     return set_scissor_cmd();
    if (!strcmp(cmd_tests[i].name, "set_color_image"))
        return CMD(0x3F) | (uint64_t)SZ_16 << 51 | (uint64_t)(FB_W - 1) << 32 | PhysicalAddr(fb);
    return cmd_tests[i].word;
}

static void build_body(const tcase_t *t, cbuf_t *c)
{
    c->p = c->start;
    for (int i = 0; i < t->k; i++)
    {
        switch (t->kind)
        {
        case K_FILL: case K_COPY: case K_RECT: case K_PROBE: case K_HAZARD: emit_rect(c, t, i); break;
        case K_TRI:  emit_tri(c, t, i); break;
        case K_LOAD: emit_load(c, t); break;
        case K_CMD:  emit(c, cmd_word(t->sub)); break;
        default: break;
        }
    }
    sync_full(c);
}

/* --- running and measuring ------------------------------------------- */

typedef struct { uint32_t clock, bufbusy, pipebusy, tmem, count; } meas_t;

static int timed_out;

/* Run a list that ends in Sync_Full; start/end are physical or DMEM addresses */
static void dp_run(uint32_t start, uint32_t end, int xbus, meas_t *m)
{
    uint32_t t0, t1, timeout = 0x4000000;

    while (HW_DPC_STATUS & DPC_PENDING) {}
    HW_DPC_STATUS = xbus ? DPC_SET_XBUS : DPC_CLR_XBUS;
    HW_DPC_START = start;
    HW_DPC_STATUS = DPC_CLR_COUNTERS;
    t0 = C0_COUNT();
    HW_DPC_END = end;
    while (!(HW_MI_INTR & MI_INTR_DP))
        if (--timeout == 0) { timed_out = 1; break; }
    t1 = C0_COUNT();
    if (m)
    {
        m->clock    = HW_DPC_CLOCK & 0xFFFFFF;
        m->bufbusy  = HW_DPC_BUFBUSY & 0xFFFFFF;
        m->pipebusy = HW_DPC_PIPEBUSY & 0xFFFFFF;
        m->tmem     = HW_DPC_TMEM & 0xFFFFFF;
        m->count    = t1 - t0;
    }
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

static void run_cbuf(const cbuf_t *c, meas_t *m)
{
    dp_run(PhysicalAddr(c->start), PhysicalAddr(c->p), 0, m);
}

static void run_body_xbus(const cbuf_t *c, meas_t *m)
{
    int words = (c->p - c->start) * 2;
    const uint32_t *src = (const uint32_t *)c->start;
    for (int i = 0; i < words; i++) DMEM[i] = src[i];
    dp_run(0, words * 4, 1, m);
}

static void clear_z(uint16_t value)
{
    uint64_t v = value * 0x0001000100010001ull;
    uint64_t *p = (uint64_t *)zb;
    for (int i = 0; i < FB_W * FB_H / 4; i++) p[i] = v;
}

static void clear_z_for(const tcase_t *t)
{
    if (t->flags & F_ZPASS) clear_z(0xFFFC);    /* far: every primitive passes */
    if (t->flags & F_ZFAIL) clear_z(0x0000);    /* near: every primitive fails */
}

/* VI phase: in the vi_on pass every run starts on the same VI line, so the
 * amount of video DMA a run competes with is the same every time */
static uint32_t vi_sync_line;       /* VI half-line, 0 = no sync */
static uint32_t vi_sync_misses;

static void vi_wait_line(void)
{
    uint32_t start = C0_COUNT(), limit = 2 * 46875000 / 50;   /* two PAL frames */
    while ((HW_VI_V_CURRENT & 0x3FE) == vi_sync_line)
        if (C0_COUNT() - start > limit) { vi_sync_misses++; return; }
    while ((HW_VI_V_CURRENT & 0x3FE) != vi_sync_line)
        if (C0_COUNT() - start > limit) { vi_sync_misses++; return; }
}

static void clear_fb(void)
{
    uint64_t *p = (uint64_t *)fb;
    for (int i = 0; i < FB_W * FB_H / 2; i++) p[i] = 0;
}

/* Clear, or for "prefill" cases write a pattern where neighbouring pixels
 * differ, so the memory value each blended pixel read can be told apart */
static void clear_fb_for(const tcase_t *t)
{
    clear_fb();
    if (!t->fbfill) return;
    for (int y = 0; y < t->h + 1; y++)
        for (int x = 0; x < t->fbw; x++)
        {
            uint32_t r = (x * 7 + y * 13) & 0xFF, g = (x * 3 + y * 5) & 0xFF, b = (x + y * 11) & 0xFF;
            if (t->fbbits == 32)
                fb[y * t->fbw + x] = r << 24 | g << 16 | b << 8 | 0xFF;
            else
                ((uint16_t *)fb)[y * t->fbw + x] = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1;
        }
}

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
        crc_table[i] = c;
    }
}

static uint32_t crc32_buf(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t crc = ~0u;
    while (len--) crc = crc >> 8 ^ crc_table[(crc ^ *p++) & 0xFF];
    return ~crc;
}

/* CRC and number of non-zero pixels of the rendered framebuffer, read
 * through the cached alias */
static uint32_t fb_stats(int fbbits, uint32_t *drawn)
{
    size_t size = FB_W * FB_H * (fbbits / 8);
    void *cached = CachedAddr(fb);
    data_cache_hit_invalidate(cached, size);
    uint32_t n = 0;
    if (fbbits == 32)
        for (int i = 0; i < FB_W * FB_H; i++) n += ((uint32_t *)cached)[i] != 0;
    else
        for (int i = 0; i < FB_W * FB_H; i++) n += ((uint16_t *)cached)[i] != 0;
    *drawn = n;
    return crc32_buf(cached, size);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t median(uint32_t *v, int n)
{
    qsort(v, n, sizeof *v, cmp_u32);
    return v[n / 2];
}

/* --- text output -------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } text_t;
static text_t sum_txt, raw_txt;

static void tprintf(text_t *t, const char *fmt, ...)
{
    va_list ap;
    if (t->cap - t->len < 512)
    {
        size_t cap = t->cap ? t->cap * 2 : 64 * 1024;
        char *nb = realloc(t->buf, cap);
        if (!nb) return;
        t->buf = nb; t->cap = cap;
    }
    va_start(ap, fmt);
    int n = vsnprintf(t->buf + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    if (n > 0) t->len += (size_t)n < t->cap - t->len ? (size_t)n : t->cap - t->len - 1;
}

static void show(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    console_render();
}

extern int cart_type;   /* libcart (bundled in libdragon) */

static const char *cart_name(void)
{
    static const char *const names[] = { "64drive", "everdrive64_x", "everdrive64", "summercart64" };
    return cart_type >= 0 && cart_type < 4 ? names[cart_type] : "none";
}

static void write_header(text_t *t, const char *what)
{
    static const char *const tv[] = { "PAL", "NTSC", "MPAL" };
    tprintf(t, "# rdp_timing v" VERSION " - %s\n", what);
    tprintf(t, "# mi_version\t0x%08" PRIX32 "\n", HW_MI_VERSION);
    tprintf(t, "# tv_type\t%s\n", tv[get_tv_type() % 3]);
    tprintf(t, "# memory_size\t%d\n", get_memory_size());
    tprintf(t, "# runs_per_case\t%d\n", RUNS);
    tprintf(t, "# fb_size\t%dx%d\n", FB_W, FB_H);
    tprintf(t, "# clock/bufbusy/pipebusy/tmem: DPC counters (RCP cycles, 62.5 MHz), cleared just before DPC_END\n");
    tprintf(t, "# count: CPU COUNT ticks (46.875 MHz) from DPC_END write to the Sync_Full interrupt\n");
    tprintf(t, "# k: primitives/commands in the list; pixels: pixels per primitive (tri: approx s*(s+1)/2)\n");
    tprintf(t, "# drawn/crc32: non-zero pixels and CRC32 of the framebuffer after all runs (cleared before the first)\n");
    tprintf(t, "# pass vi_off: VI disabled (no video DMA); vi_on: VI scanning out 320x240x16,\n");
    tprintf(t, "#   every vi_on run started when VI_V_CURRENT reached vi_sync_line (first active half-line)\n");
    tprintf(t, "# zpass: depth decreases per primitive, Z buffer cleared to far (all pass and write);\n");
    tprintf(t, "#   zfail: Z buffer cleared to near (all fail); both include one Set_Prim_Depth per rectangle\n");
}

/* --- main ---------------------------------------------------------------- */

static void run_case(int id, const tcase_t *t, const char *pass, int want_crc, int vi_sync)
{
    meas_t m[RUNS];
    uint32_t v[RUNS];
    char flags[48];

    build_setup(t, &cb_setup);
    run_cbuf(&cb_setup, NULL);
    build_body(t, &cb_body);

    if (case_draws(t)) clear_fb_for(t);
    for (int r = 0; r < RUNS && !timed_out; r++)
    {
        clear_z_for(t);
        if (vi_sync) vi_wait_line();
        if (t->xbus) run_body_xbus(&cb_body, &m[r]);
        else         run_cbuf(&cb_body, &m[r]);
    }
    if (timed_out) return;

    uint32_t drawn = 0;
    uint32_t crc = (want_crc && case_draws(t)) ? fb_stats(t->fbbits, &drawn) : 0;

    case_flags(t, flags, sizeof flags);
    tprintf(&sum_txt, "%d\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%d\t%d\t%d\t%" PRIu32,
            id, pass, t->xbus ? "xbus" : "rdram", case_group(t), case_prim(t), case_cycle(t),
            t->fbbits, case_tex(t), t->tex != TX_NONE ? (t->bilerp ? "bilinear" : "point") : "-",
            flags, t->w, t->h, t->k, case_pixels(t));

    #define FIELD_STATS(field, all) do { \
        for (int r = 0; r < RUNS; r++) v[r] = m[r].field; \
        uint32_t med = median(v, RUNS); \
        if (all) tprintf(&sum_txt, "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32, med, v[0], v[RUNS - 1]); \
        else     tprintf(&sum_txt, "\t%" PRIu32, med); \
    } while (0)
    FIELD_STATS(clock, 1);
    FIELD_STATS(bufbusy, 0);
    FIELD_STATS(pipebusy, 0);
    FIELD_STATS(tmem, 0);
    FIELD_STATS(count, 1);
    if (case_draws(t) && want_crc) tprintf(&sum_txt, "\t%" PRIu32 "\t%08" PRIX32 "\n", drawn, crc);
    else                           tprintf(&sum_txt, "\t-\t-\n");

    for (int r = 0; r < RUNS; r++)
        tprintf(&raw_txt, "%d\t%s\t%d\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\n",
                id, pass, r, m[r].clock, m[r].bufbusy, m[r].pipebusy, m[r].tmem, m[r].count);
}

static void run_pass(const char *pass, int vi_off)
{
    const char *last = "";
    uint32_t vi_ctrl = 0;

    if (vi_off)
    {
        show("PASS %s (SCREEN OFF)...\n", pass);
        wait_ms(100);
        disable_interrupts();
        vi_ctrl = HW_VI_CTRL;
        HW_VI_CTRL = vi_ctrl & ~3u;         /* blank: no framebuffer fetch */
    }
    else
    {
        vi_sync_line = (HW_VI_V_VIDEO >> 16) & 0x3FE;       /* first active line */
        show("PASS %s (RUNS START AT VI LINE %" PRIu32 ")\n", pass, vi_sync_line);
    }

    for (int i = 0; i < n_cases && !timed_out; i++)
    {
        if (!vi_off && strcmp(last, case_group(&cases[i])))
        {
            last = case_group(&cases[i]);
            show("  %s\n", last);
        }
        if (!vi_off) disable_interrupts();
        run_case(i, &cases[i], pass, 1, !vi_off && vi_sync_line);
        if (!vi_off) enable_interrupts();
    }

    if (vi_off)
    {
        HW_VI_CTRL = vi_ctrl;
        enable_interrupts();
    }
}

static int write_file(const char *path, const text_t *t)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    size_t done = fwrite(t->buf, 1, t->len, f);
    return fclose(f) == 0 && done == t->len;
}

/* Re-render each case marked for dumping once and write the drawn area
 * (w x h from the top left, big-endian pixels, row-major) to
 * sd:/rdp_timing/rdp_timing_NNN_fb/case_III.bin, plus an index. */
static void dump_framebuffers(int n)
{
    char dir[64], path[96];
    text_t idx = { 0 };
    int files = 0, failed = 0;

    snprintf(dir, sizeof dir, "sd:/rdp_timing/rdp_timing_%03d_fb", n);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) { show("CANNOT CREATE %s\n", dir); return; }
    show("WRITING FRAMEBUFFER DUMPS...\n");

    tprintf(&idx, "# rdp_timing v" VERSION " framebuffer dumps: drawn area (w x h from the top left),\n");
    tprintf(&idx, "# big-endian pixels, row-major; bpp 16 = RGBA5551, 32 = RGBA8888;\n");
    tprintf(&idx, "# span cases: whole framebuffer rows (w = fbw), the rectangle starts at x0\n");
    tprintf(&idx, "id\tfile\tgroup\tprim\tcycle\tfb\ttex\tfilter\tflags\tw\th\tbpp\tx0\tfbw\n");

    /* Interrupts stay off for the whole phase: nothing here needs them, and
     * it keeps the vblank handler from running in the middle of SD writes */
    disable_interrupts();
    for (int i = 0; i < n_cases; i++)
    {
        const tcase_t *t = &cases[i];
        char flags[48];
        if (!t->dump) continue;

        build_setup(t, &cb_setup);
        run_cbuf(&cb_setup, NULL);
        build_body(t, &cb_body);
        clear_fb_for(t);
        clear_z_for(t);
        run_cbuf(&cb_body, NULL);
        if (timed_out) break;

        int bpp = t->fbbits / 8;
        /* span cases: whole rows (including pixels outside the rectangle) */
        int dw = t->span ? t->fbw : t->w;
        uint8_t *cached = CachedAddr(fb);
        data_cache_hit_invalidate(cached, FB_W * FB_H * 4);

        snprintf(path, sizeof path, "%s/case_%03d.bin", dir, i);
        int ok = 0;
        for (int attempt = 0; attempt < 2 && !ok; attempt++)    /* one retry */
        {
            FILE *f = fopen(path, "wb");
            ok = f != NULL;
            for (int y = 0; ok && y < t->h; y++)
                ok = fwrite(cached + y * t->fbw * bpp, bpp, dw, f) == (size_t)dw;
            if (f && fclose(f) != 0) ok = 0;
        }
        if (!ok) { failed++; continue; }
        files++;

        case_flags(t, flags, sizeof flags);
        tprintf(&idx, "%d\tcase_%03d.bin\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n",
                i, i, case_group(t), case_prim(t), case_cycle(t), t->fbbits, case_tex(t),
                t->tex != TX_NONE ? (t->bilerp ? "bilinear" : "point") : "-", flags,
                t->span ? t->fbw : t->w, t->h, t->fbbits, t->x0, t->fbw);
    }

    snprintf(path, sizeof path, "%s/index.txt", dir);
    if (!write_file(path, &idx)) failed++;
    enable_interrupts();
    free(idx.buf);
    show("  %d DUMPS WRITTEN%s\n", files, failed ? ", SOME FAILED" : "");
}

static int save_to_sd(void)
{
    char sum_path[64], raw_path[64];
    struct stat st;
    int n;

    show("\nSAVING TO SD CARD...\n");
    if (!debug_init_sdfs("sd:/", -1)) { show("NO SUPPORTED FLASH CART / SD CARD\n"); return 0; }
    show("CART: %s\n", cart_name());
    if (mkdir("sd:/rdp_timing", 0777) != 0 && errno != EEXIST)
    {
        show("CANNOT CREATE sd:/rdp_timing (ERRNO %d)\n", errno);
        debug_close_sdfs();
        return 0;
    }
    for (n = 1; n < 1000; n++)
    {
        snprintf(sum_path, sizeof sum_path, "sd:/rdp_timing/rdp_timing_%03d.txt", n);
        if (stat(sum_path, &st) != 0) break;
    }
    snprintf(raw_path, sizeof raw_path, "sd:/rdp_timing/rdp_timing_%03d_raw.txt", n);

    int ok = write_file(sum_path, &sum_txt) && write_file(raw_path, &raw_txt);
    if (ok) show("SAVED %s\n  AND rdp_timing_%03d_raw.txt\n", sum_path, n);
    else    show("SD WRITE FAILED\n");
    if (ok) dump_framebuffers(n);
    debug_close_sdfs();
    return ok;
}

/* Fallback: dump the summary to the debug log (USB / IS-Viewer) */
static void log_text(const text_t *t)
{
    size_t pos = 0;
    while (pos < t->len)
    {
        size_t end = pos;
        while (end < t->len && t->buf[end] != '\n') end++;
        debugf("%.*s\n", (int)(end - pos), t->buf + pos);
        pos = end + 1;
    }
}

static void init_textures(void)
{
    for (int i = 0; i < 32 * 32; i++) tex16[i] = (uint16_t)(i * 0x9E37 | 1);
    for (int i = 0; i < 32 * 16; i++) tex32[i] = i * 0x9E3779B1u | 0xFF;
    for (int i = 0; i < 32 * 32; i++) texci8[i] = (uint8_t)(i * 37);
    for (int i = 0; i < 256; i++)     tlut[i] = (uint16_t)(i * 0x0842 | 1);
    for (int i = 0; i < 64 * 64 / 2; i++) texi4[i] = (uint8_t)(i * 29);
    for (int i = 0; i < 8192; i++)    texsrc[i] = (uint8_t)(i * 13);
    for (int i = 0; i < 256; i++)     texseq[i] = (uint8_t)i;
    for (int i = 0; i < 2048; i++)    texzero[i] = 0;
    for (int i = 0; i < 1024; i++)    texalt[i] = (uint8_t)(i * 53 + 0x5A);
}

int main(void)
{
    debug_init_usblog();
    debug_init_isviewer();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_render_mode(RENDER_MANUAL);
    show("RDP TIMING v" VERSION "\n\n");

    fb     = malloc_uncached_aligned(64, FB_W * FB_H * 4);
    zb     = malloc_uncached_aligned(64, FB_W * FB_H * 2);
    tex16  = malloc_uncached_aligned(64, 32 * 32 * 2);
    tex32  = malloc_uncached_aligned(64, 32 * 16 * 4);
    texci8 = malloc_uncached_aligned(64, 32 * 32);
    tlut   = malloc_uncached_aligned(64, 256 * 2);
    texi4  = malloc_uncached_aligned(64, 64 * 64 / 2);
    texsrc = malloc_uncached_aligned(64, 8192);
    texseq = malloc_uncached_aligned(64, 256);
    texzero = malloc_uncached_aligned(64, 2048);
    texalt = malloc_uncached_aligned(64, 1024);
    cb_setup.start = malloc_uncached_aligned(64, 4096);
    cb_body.start  = malloc_uncached_aligned(64, 32768);
    if (!fb || !zb || !tex16 || !tex32 || !texci8 || !tlut || !texi4 || !texsrc || !texseq || !texzero || !texalt
        || !cb_setup.start || !cb_body.start)
    {
        show("OUT OF MEMORY\n");
        for (;;) {}
    }

    init_textures();
    crc_init();
    build_case_list();

    /* Reset the command interface and any pending DP interrupt */
    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    write_header(&sum_txt, "summary, one line per case and pass");
    tprintf(&sum_txt, "id\tpass\tsource\tgroup\tprim\tcycle\tfb\ttex\tfilter\tflags\tw\th\tk\tpixels"
            "\tclock_med\tclock_min\tclock_max\tbufbusy_med\tpipebusy_med\ttmem_med"
            "\tcount_med\tcount_min\tcount_max\tdrawn\tcrc32\n");
    write_header(&raw_txt, "raw, one line per run");
    tprintf(&raw_txt, "id\tpass\trun\tclock\tbufbusy\tpipebusy\ttmem\tcount\n");

    show("%d CASES x %d RUNS x 2 PASSES\n\n", n_cases, RUNS);
#ifndef DUMP_ONLY  /* debug builds: skip the timing passes */
    run_pass("vi_off", 1);
    if (!timed_out) run_pass("vi_on", 0);
#endif
    tprintf(&sum_txt, "# vi_sync_line\t%" PRIu32 "\n# vi_sync_misses\t%" PRIu32 "\n",
            vi_sync_line, vi_sync_misses);

    if (timed_out)
    {
        show("\nRDP TIMEOUT - RESULTS ARE INCOMPLETE\n");
        tprintf(&sum_txt, "# ABORTED: RDP timeout\n");
    }

    if (!save_to_sd())
    {
        show("WRITING RESULTS TO DEBUG LOG\n(USB / IS-VIEWER) INSTEAD\n");
        log_text(&sum_txt);
    }

    show("DONE!\n");
    for (;;) {}
}

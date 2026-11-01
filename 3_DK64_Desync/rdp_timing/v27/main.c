/*
 * rdp_timing v9 - real game display lists, the CPU under RDP load (part 2), SP DMA
 *
 * The ares RDP model built from v3-v8 still leaves Donkey Kong 64 and Beetle Adventure
 * Racing pulling in opposite directions. Three things decide where the remaining error is:
 *
 *   R  replay complete display lists captured from both games (one frame each, with the
 *      depth buffer they started from) and read DPC_CLOCK: the RDP cost of a real frame,
 *      with no model in between. VI off, VI on, and VI on with the CPU chasing pointers
 *      at the same time (both the RDP slowdown and the CPU slowdown of a real frame).
 *      Needs 8 MiB (the lists address up to 6 MiB; they are moved up by a multiple of
 *      1 MiB when they would overlap this program, which keeps the 64-byte and 2 KiB
 *      phases of every address).
 *   F  what v8 could not measure: dependent (random) cache fills while the RDP draws,
 *      random cached writes, a game-like mixed loop, and RDP loads that v8 lacked
 *      (texture loading, 2-cycle mode).
 *   S  SP DMA: RDRAM->DMEM and DMEM->RDRAM, 8 B .. 4 KiB, idle and while the RDP draws.
 *
 * VI on (progress on screen) unless a line says otherwise, 3 runs per case, smallest kept.
 * Results: sd:/rdp_timing/rdp_timing_v9_NNN.txt
 * Buttons at power-on: A = R only, B = F only, Z = S only, nothing = R, F, S
 */

#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERSION     "27"
#define RUNS        3
#define FB_W        320
#define FB_H        240
#define ZB_DEFAULT  0x4B000     /* default depth buffer offset: after two 320x240x16 colour buffers */

/* --- hardware registers ------------------------------------------------- */

#define REG(a)          (*(volatile uint32_t *)(a))
#define HW_SP_MEM_ADDR  REG(0xA4040000)
#define HW_SP_DRAM_ADDR REG(0xA4040004)
#define HW_SP_RD_LEN    REG(0xA4040008)
#define HW_SP_WR_LEN    REG(0xA404000C)
#define HW_SP_STATUS    REG(0xA4040010)
#define HW_DPC_START    REG(0xA4100000)
#define HW_DPC_END      REG(0xA4100004)
#define HW_DPC_CURRENT  REG(0xA4100008)
#define HW_DPC_STATUS   REG(0xA410000C)
#define HW_DPC_CLOCK    REG(0xA4100010)
#define HW_DPC_BUFBUSY  REG(0xA4100014)
#define HW_DPC_PIPEBUSY REG(0xA4100018)
#define HW_DPC_TMEM     REG(0xA410001C)
#define HW_MI_MODE      REG(0xA4300000)
#define HW_MI_VERSION   REG(0xA4300004)
#define HW_MI_INTR      REG(0xA4300008)
#define HW_VI_CTRL      REG(0xA4400000)

#define DPC_CLR_XBUS        0x001
#define DPC_CLR_FREEZE      0x004
#define DPC_CLR_FLUSH       0x010
#define DPC_CLR_COUNTERS    0x3C0
#define DPC_PENDING         0x600
#define MI_INTR_DP          0x20
#define MI_CLEAR_DP_INT     0x800
#define SPSTAT_DMA_BUSY     0x00C

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

static void set_color_image(cbuf_t *c, int size, int width, uint32_t addr)
{
    emit(c, CMD(0x3F) | (uint64_t)FMT_RGBA << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr);
}
static void set_mask_image(cbuf_t *c, uint32_t addr) { emit(c, CMD(0x3E) | addr); }
static void set_texture_image(cbuf_t *c, int fmt, int size, int width, uint32_t addr)
{
    emit(c, CMD(0x3D) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr);
}
static void set_tile(cbuf_t *c, int tile, int fmt, int size, int line, int tmem, int mask_t, int mask_s)
{
    emit(c, CMD(0x35) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)line << 41
         | (uint64_t)tmem << 32 | (uint64_t)tile << 24 | (uint64_t)mask_t << 14 | (uint64_t)mask_s << 4);
}
static void tile_cmd(cbuf_t *c, int op, int tile, int sl, int tl, int sh, int th)
{
    emit(c, CMD(op) | FX2(sl) << 44 | FX2(tl) << 32 | (uint64_t)tile << 24 | FX2(sh) << 12 | FX2(th));
}
#define load_tile(c, t, sl, tl, sh, th)     tile_cmd(c, 0x34, t, sl, tl, sh, th)
#define set_tile_size(c, t, sl, tl, sh, th) tile_cmd(c, 0x32, t, sl, tl, sh, th)
static void load_block(cbuf_t *c, int tile, int texels, int dxt)
{
    emit(c, CMD(0x33) | (uint64_t)tile << 24 | (uint64_t)(texels - 1) << 12 | (uint64_t)dxt);
}
static void set_scissor(cbuf_t *c) { emit(c, CMD(0x2D) | FX2(FB_W) << 12 | FX2(FB_H)); }
static void set_prim_depth(cbuf_t *c, int z) { emit(c, CMD(0x2E) | (uint64_t)z << 16); }

typedef struct { int a, b, c, d, aa, ab, ac, ad; } comb_t;
static uint64_t combine_cmd(comb_t c0, comb_t c1)
{
    return CMD(0x3C)
        | (uint64_t)c0.a << 52 | (uint64_t)c0.c << 47 | (uint64_t)c0.aa << 44 | (uint64_t)c0.ac << 41
        | (uint64_t)c1.a << 37 | (uint64_t)c1.c << 32 | (uint64_t)c0.b << 28 | (uint64_t)c1.b << 24
        | (uint64_t)c1.aa << 21 | (uint64_t)c1.ac << 18 | (uint64_t)c0.d << 15 | (uint64_t)c0.ab << 12
        | (uint64_t)c0.ad << 9 | (uint64_t)c1.d << 6 | (uint64_t)c1.ab << 3 | (uint64_t)c1.ad;
}
static const comb_t CC_PRIM     = { 15, 15, 31, 3, 7, 7, 7, 3 };
static const comb_t CC_TEXPRIM  = {  1, 15,  3, 7, 1, 7, 3, 7 };
static const comb_t CC_COMBENV  = {  0, 15,  5, 7, 0, 7, 5, 7 };

#define OM_1CYC         (0u << 20)
#define OM_2CYC         (1u << 20)
#define OM_COPY         (2u << 20)
#define OM_FILL         (3u << 20)
#define OM_BILERP       (1u << 13)
#define OM_BI_LERP01    (3u << 10)
#define OM_NO_DITHER    (0xFu << 4)
#define OM_FORCE_BLEND  (1u << 14)
#define OM_IMAGE_READ   (1u << 6)
#define OM_Z_UPDATE     (1u << 5)
#define OM_Z_COMPARE    (1u << 4)
#define OM_Z_PRIM       (1u << 2)
#define OM_B_M2A_MEM    ((1u << 22) | (1u << 20))

enum { M_FILL16, M_1CYC16, M_1CYC16_BLEND_ZPASS, M_2CYC16_BLEND_ZPASS, M_1CYC16_TEX, M_1CYC16_TEX_BLEND_ZPASS, N_MODES };
enum { F_BLEND = 1, F_Z = 2, F_TEX = 4 };
typedef struct { const char *name; int cyc; int flags; } mode_t_;   /* cyc: 0 fill, 1, 2 */
static const mode_t_ modes[N_MODES] = {
    { "fill16",                 0, 0 },
    { "1cyc16",                 1, 0 },
    { "1cyc16_blend_zpass",     1, F_BLEND | F_Z },
    { "2cyc16_blend_zpass",     2, F_BLEND | F_Z },
    { "1cyc16_tex",             1, F_TEX },
    { "1cyc16_tex_blend_zpass", 1, F_TEX | F_BLEND | F_Z },
};

/* --- buffers --------------------------------------------------------- */

static uint8_t  *arena;             /* colour + depth buffers, 2 KiB aligned */
static size_t    arena_size;
static uint16_t *fb;
static uint16_t *zb;
static int       fb_width = FB_W;
static uint16_t *tex16;             /* 32x32 RGBA16 texture / load source */
static uint8_t  *dma_buf;           /* 64 KiB, SP DMA source/destination */
static cbuf_t    cb_setup, cb_body;
static int       mem_8mb;

/* --- running and measuring ------------------------------------------- */

typedef struct { uint32_t clock, pipebusy, tmem; } meas_t;
static void show(const char *fmt, ...);
static int save_to_sd(void);
static int timed_out;            /* the last list did not finish */
static int rdp_dead;             /* the RDP could not be recovered: skip everything */
static int timeouts;
static uint32_t start_ticks, last_ticks;

static void dp_start_at(uint32_t start, uint32_t end)
{
    uint32_t timeout = 0x400000;
    while (HW_DPC_STATUS & DPC_PENDING)
        if (--timeout == 0) { timed_out = 1; break; }
    HW_DPC_STATUS = DPC_CLR_XBUS;
    HW_DPC_START = start;
    HW_DPC_STATUS = DPC_CLR_COUNTERS;
    start_ticks = get_ticks();
    HW_DPC_END = end;
}
static void dp_start(const cbuf_t *c) { dp_start_at(PhysicalAddr(c->start), PhysicalAddr(c->p)); }

static void dp_finish(meas_t *m)
{
    uint32_t timeout = 0x1000000;    /* ~2 s */
    while (!(HW_MI_INTR & MI_INTR_DP))
        if (--timeout == 0) { timed_out = 1; break; }
    last_ticks = get_ticks() - start_ticks;
    if (m)
    {
        m->clock    = HW_DPC_CLOCK & 0xFFFFFF;
        m->pipebusy = HW_DPC_PIPEBUSY & 0xFFFFFF;
        m->tmem     = HW_DPC_TMEM & 0xFFFFFF;
    }
    HW_MI_MODE = MI_CLEAR_DP_INT;
}

static int save_to_sd_quiet(void);
static void tprintf(const char *fmt, ...);
static void rdp_recover(void)
{
    timeouts++;
    rdp_dead = 1;
    /* no console output from here on: with the RDP hung, libdragon's display_get() would crash */
    tprintf("# RDP TIMEOUT: DP_STATUS 0x%03lX START %06lX END %06lX CURRENT %06lX - stopping, results saved up to here\n",
            (unsigned long)(HW_DPC_STATUS & 0x7FF), (unsigned long)HW_DPC_START, (unsigned long)HW_DPC_END, (unsigned long)HW_DPC_CURRENT);
    fputs("RDP TIMEOUT - STOPPING, SAVING WHAT WE HAVE\n", stdout);
    enable_interrupts(); save_to_sd_quiet(); disable_interrupts();
}

static void dp_run(const cbuf_t *c, meas_t *m)
{
    if (rdp_dead) { timed_out = 1; if (m) m->clock = m->pipebusy = m->tmem = 0; return; }
    dp_start(c);
    dp_finish(m);
    if (timed_out) rdp_recover();
}

static void clear_z(uint16_t value, int rows)
{
    uint64_t v = value * 0x0001000100010001ull;
    uint64_t *p = (uint64_t *)zb;
    for (int i = 0; i < fb_width * rows / 4; i++) p[i] = v;
}

/* --- mode setup -------------------------------------------------------- */

static uint64_t other_modes_word(int mode)
{
    const mode_t_ *m = &modes[mode];
    uint32_t hi = OM_BI_LERP01 | OM_NO_DITHER, lo = 0;
    switch (m->cyc)
    {
    case 0: hi = OM_FILL; break;
    case 2: hi |= OM_2CYC; break;
    default: hi |= OM_1CYC; break;
    }
    if (m->flags & F_TEX) hi |= OM_BILERP;
    if (m->flags & F_BLEND) lo |= OM_FORCE_BLEND | OM_IMAGE_READ | OM_B_M2A_MEM;
    if (m->flags & F_Z) lo |= OM_Z_COMPARE | OM_Z_UPDATE | OM_Z_PRIM;
    return CMD(0x2F) | (uint64_t)hi << 32 | lo;
}

static uint64_t combine_word(int mode)
{
    const mode_t_ *m = &modes[mode];
    comb_t c0 = (m->flags & F_TEX) ? CC_TEXPRIM : CC_PRIM;
    comb_t c1 = m->cyc == 2 ? CC_COMBENV : c0;
    return combine_cmd(c0, c1);
}

static void emit_texture_setup(cbuf_t *c)
{
    set_texture_image(c, FMT_RGBA, SZ_16, 32, PhysicalAddr(tex16));
    set_tile(c, 7, FMT_RGBA, SZ_16, 8, 0, 0, 0);
    sync_load(c);
    load_tile(c, 7, 0, 0, 31, 31);
    sync_pipe(c);
    set_tile(c, 0, FMT_RGBA, SZ_16, 8, 0, 5, 5);
    set_tile_size(c, 0, 0, 0, 31, 31);
}

static void run_setup(int mode)
{
    cbuf_t *c = &cb_setup;
    c->p = c->start;
    sync_pipe(c);
    set_color_image(c, SZ_16, fb_width, PhysicalAddr(fb));
    set_mask_image(c, PhysicalAddr(zb));
    set_scissor(c);
    emit(c, CMD(0x3A) | 0xC08040FF);            /* prim color */
    emit(c, CMD(0x3B) | 0x80C0FFFF);            /* env color  */
    set_prim_depth(c, 0x7000);
    emit(c, CMD(0x37) | 0x7BDE7BDEu);           /* fill color */
    if (modes[mode].flags & F_TEX) emit_texture_setup(c);
    else
    {
        set_texture_image(c, FMT_RGBA, SZ_16, 32, PhysicalAddr(tex16));
        set_tile(c, 7, FMT_RGBA, SZ_16, 0, 0, 0, 0);
    }
    emit(c, other_modes_word(mode));
    emit(c, combine_word(mode));
    sync_full(c);
    timed_out = 0;
    dp_run(c, NULL);
}

static void emit_rect(cbuf_t *c, int mode, int x0, int y0, int w, int h)
{
    const mode_t_ *m = &modes[mode];
    int incl = m->cyc == 0;
    uint64_t xl = FX2(x0 + w - incl), yl = FX2(y0 + h - incl), xh = FX2(x0), yh = FX2(y0);
    if (!(m->flags & F_TEX))
        emit(c, CMD(0x36) | xl << 44 | yl << 32 | xh << 12 | yh);
    else
    {
        emit(c, CMD(0x24) | xl << 44 | yl << 32 | xh << 12 | yh);
        emit(c, (uint64_t)(1 << 10) << 16 | (1 << 10));
    }
}

/* --- text output -------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } text_t;
static text_t out;

static void tprintf(const char *fmt, ...)
{
    va_list ap;
    if (out.cap - out.len < 1024) return;
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

static void progress(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_clear();
    printf("RDP TIMING v" VERSION "\n\n%s\n", buf);
    enable_interrupts();
    console_render();
    disable_interrupts();
}

/* --- generic measurement ----------------------------------------------- */


/* ======================================================================= */
/* R: replay of captured game display lists                                 */
/* ======================================================================= */

/* on 8 MiB consoles the big buffers of this program sit at fixed addresses near the top of RDRAM
 * (below libdragon's 64 KiB stack), so that everything from the heap top up to LIST_BUF_PHYS is
 * free for the replayed lists' colour/depth buffers and textures */
#define LIST_BUF_PHYS   0x700000u       /* the replayed list (prefix + commands) */
#define LIST_BUF_MAX    0x80000u
#define ZCOPY_PHYS      0x780000u       /* copy of the list's initial depth buffer (192 KiB) */
#define OUT_PHYS        0x7B0000u       /* result text (128 KiB) */
#define DMA_PHYS        0x7D0000u       /* SP DMA buffer / pointer chase ring (64 KiB) */
#define RELOC_HIGH      LIST_BUF_PHYS   /* lists' data must end below here ... */
static uint32_t reloc_low;              /* ... and start at or above here (heap top rounded up + margin) */

typedef struct { const char *name; uint32_t vi_ctrl; } rlist_t;
static const rlist_t rlists[] = {
    { "bar_i2274.bin", 0x13012 },   /* the light BAR intro frame (43% of its pixels in spans 128-320 px wide) */
};
#define N_RLISTS ((int)(sizeof rlists / sizeof rlists[0]))

static uint8_t *zcopy;              /* the depth buffer the list starts from (restored before every run) */
static uint32_t zcopy_bytes, zcopy_addr; static uint8_t *zcopy_cur;
#define ZBIG_PHYS 0x500000u
static uint8_t  stage[16384] __attribute__((aligned(16)));

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* pointer chase for R (built once, over dma_buf's 64 KiB: 4096 nodes of 16 B) */
#define RCHASE_NODES 4096
static uint32_t *rchase;
static void build_rchase(void)
{
    static uint16_t perm[RCHASE_NODES];
    rchase = (uint32_t *)dma_buf;    /* dma_buf is uncached: use the cached alias */
    rchase = (uint32_t *)CachedAddr(rchase);
    for (int i = 0; i < RCHASE_NODES; i++) perm[i] = i;
    uint32_t rnd = 0xC0FFEE11u;
    for (int i = RCHASE_NODES - 1; i > 0; i--)
    {
        rnd = rnd * 1664525u + 1013904223u;
        int j = (rnd >> 8) % i;
        uint16_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (int i = 0; i < RCHASE_NODES; i++) rchase[perm[i] * 4] = perm[(i + 1) % RCHASE_NODES];
    data_cache_hit_writeback_invalidate(rchase, RCHASE_NODES * 16);
}

/* load one list file: returns 0 on failure; fills first/last (physical) and the relocation offset */
/* variants: command classes replaced by NOPs (every word of a multi-word command) */
#define V_SYNC   (1u << 0)   /* Sync_Load / Sync_Pipe / Sync_Tile */
#define V_LOAD   (1u << 1)   /* Load_TLUT / Load_Block / Load_Tile */
#define V_TRI    (1u << 2)   /* triangles 08-0F */
#define V_RECT   (1u << 3)   /* Texture_Rectangle(_Flip), Fill_Rectangle */
#define V_STATE  (1u << 4)   /* Set_* (everything else except Sync_Full and NOP) */
/* other-modes patches (applied to every Set_Other_Modes, the carried-over prefix included) */
enum { P_NONE, P_NO_ZCMP, P_NO_AA, P_NO_PERSP, P_NO_LOD, P_1CYC, P_NO_DITHER, P_POINT, P_NO_TLUT, P_NO_CVG, P_NO_IMRD, P_WLIKE, P_NO_ZUPD, P_NO_ATOMIC };
enum { TRI_NONE, TRI_FLAT, TRI_NOSHADE, TRI_NOTEX, TRI_EDGEZ };
/* zpre: the fill that clears the depth buffer is removed and the depth buffer preset: 1 = 0 (nearest: every depth test fails), 2 = 0xFFFC (farthest: every first
 * draw passes, only the list's own overdraw fails) ... v23's z_near did not remove the fill (bug), so it was the same as full */
/* img: 1 = the colour image moved to COLOR_LOW (below the relocated lists' data, in the first RDRAM megabyte), 2 = the depth image moved up by 1 MiB (BAR only) */
typedef struct { const char *name; uint32_t mask; int om; int tri; int zpre; int img; } variant_t;
#define COLOR_LOW 0x0F5000u
static const variant_t variants[] = {
    { "full",          0,               P_NONE, TRI_NONE, 0, 0 },
    { "tri_only",      V_LOAD | V_RECT, P_NONE, TRI_NONE, 0, 0 },
    { "no_tri",        V_TRI,           P_NONE, TRI_NONE, 0, 0 },
};
#define N_VARIANTS ((int)(sizeof variants / sizeof variants[0]))
static uint64_t patch_om(uint64_t w, int om)
{
    uint32_t hi = (uint32_t)(w >> 32) & 0xFFFFFF, lo = (uint32_t)w;
    switch (om)
    {
    case P_NO_ZCMP:   lo &= ~(1u << 4); break;
    case P_NO_ZUPD:   lo &= ~(1u << 5); break;
    case P_NO_AA:     lo &= ~(1u << 3); break;
    case P_NO_PERSP:  hi &= ~(1u << 19); break;
    case P_NO_LOD:    hi &= ~(7u << 16); break;
    case P_1CYC:      if (((hi >> 20) & 3) == 1) hi &= ~(3u << 20); break;
    case P_NO_DITHER: hi |= 0xFu << 4; break;
    case P_POINT:     hi &= ~(1u << 13); break;
    case P_NO_TLUT:   hi &= ~(3u << 14); break;
    case P_NO_CVG:    lo &= ~((3u << 12) | (3u << 8) | (1u << 7) | 3u); break;
    case P_NO_IMRD:   lo &= ~(1u << 6); break;
    case P_NO_ATOMIC: hi &= ~(1u << 23); break;
    case P_WLIKE:     hi = (hi & (3u << 20)) | OM_BILERP | OM_BI_LERP01 | OM_NO_DITHER;
                       lo &= 0xFFFF0000u | OM_FORCE_BLEND | OM_IMAGE_READ | OM_Z_UPDATE | OM_Z_COMPARE; break;
    default: break;
    }
    return (w & 0xFF00000000000000ull) | (uint64_t)hi << 32 | lo;
}
/* n = the command's words; returns the words to write (always n: dropped words become NOPs) */
static void patch_tri(uint64_t *cmd, int n, int tri)
{
    int code = (int)(cmd[0] >> 56) & 0x3F;
    if (code != 0x0F) return;        /* only the full shade+texture+z triangle (what both games use) */
    if (tri == TRI_FLAT)
    {
        for (int i = 4; i < 12; i++) if (i != 4 && i != 6) cmd[i] = 0;            /* shade: keep R G B A int and frac */
        for (int i = 12; i < 20; i++) if (i != 12 && i != 14) cmd[i] = 0;         /* texture: keep S T W int and frac */
        cmd[20] &= 0xFFFFFFFF00000000ull; cmd[21] = 0;                            /* depth: keep Z, no slopes */
        return;
    }
    uint64_t t[22]; memcpy(t, cmd, sizeof t);
    int k = 4;
    if (tri == TRI_NOSHADE) { cmd[0] = (cmd[0] & ~CMD(0x3F)) | CMD(0x0B); memcpy(cmd + k, t + 12, 8 * 8); k += 8; memcpy(cmd + k, t + 20, 2 * 8); k += 2; }
    if (tri == TRI_NOTEX)   { cmd[0] = (cmd[0] & ~CMD(0x3F)) | CMD(0x0D); memcpy(cmd + k, t + 4, 8 * 8); k += 8; memcpy(cmd + k, t + 20, 2 * 8); k += 2; }
    if (tri == TRI_EDGEZ)   { cmd[0] = (cmd[0] & ~CMD(0x3F)) | CMD(0x09); memcpy(cmd + k, t + 20, 2 * 8); k += 2; }
    for (; k < n; k++) cmd[k] = 0;
}
static int nop_class(int code)
{
    if (code >= 0x08 && code <= 0x0F) return V_TRI;
    if (code == 0x24 || code == 0x25 || code == 0x36) return V_RECT;
    if (code == 0x26 || code == 0x27 || code == 0x28) return V_SYNC;
    if (code == 0x30 || code == 0x33 || code == 0x34) return V_LOAD;
    if (code == 0x29 || code == 0x00) return 0;
    return V_STATE;
}

static int load_list(const char *name, const variant_t *var, uint32_t *first, uint32_t *last, uint32_t *off_out, uint32_t *model, char *info, size_t infolen)
{
    uint32_t variant_mask = var->mask;
    char path[64];
    snprintf(path, sizeof path, "rom:/%s", name);
    int fd = dfs_open(path + 5);     /* dfs_open wants the path without "rom:/" */
    if (fd < 0) { snprintf(info, infolen, "open failed %d", fd); return 0; }
    uint8_t hdr[64];
    if (dfs_read(hdr, 1, 64, fd) != 64 || memcmp(hdr, "RDPL", 4) || be32(hdr + 4) != 2) { dfs_close(fd); snprintf(info, infolen, "bad header"); return 0; }
    uint32_t list_bytes = be32(hdr + 8), color = be32(hdr + 12), zaddr = be32(hdr + 20), zbytes = be32(hdr + 24);
    uint32_t nreg = be32(hdr + 28), cyc = be32(hdr + 32), prefix = be32(hdr + 56);
    (void)color;
    uint32_t total = prefix * 8 + list_bytes;
    if (total > LIST_BUF_MAX || zbytes > 0x96000) { dfs_close(fd); snprintf(info, infolen, "too big"); return 0; }

    /* the regions come after the list and the depth buffer: read them first to pick the offset */
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    static uint32_t regs[2048][2]; uint32_t nregs = 0;
    {
        dfs_seek(fd, 64 + total + zbytes, SEEK_SET);
        for (uint32_t r = 0; r < nreg; r++)
        {
            uint8_t e[12];
            if (dfs_read(e, 1, 12, fd) != 12) break;
            uint32_t a = be32(e), b = be32(e + 4), k = be32(e + 8);
            if (k == 2) continue;                       /* generous texture image bound: not a real access */
            if (a < lo) lo = a;
            if (a + b > hi) hi = a + b;
            if (nregs < 2048) { regs[nregs][0] = a; regs[nregs][1] = a + b; nregs++; }
        }
    }
    uint32_t off = 0;
    while (lo + off < reloc_low) off += 0x100000;
    if (hi + off > RELOC_HIGH) { dfs_close(fd); snprintf(info, infolen, "no room: %06lX-%06lX", (unsigned long)lo, (unsigned long)hi); return 0; }
    if (zbytes > 0x30000)
        for (uint32_t r = 0; r < nregs; r++)
            if (regs[r][0] + off < ZBIG_PHYS + zbytes && regs[r][1] + off > ZBIG_PHYS)
            { dfs_close(fd); snprintf(info, infolen, "no room for the depth copy: %06lX-%06lX", (unsigned long)(regs[r][0] + off), (unsigned long)(regs[r][1] + off)); return 0; }

    /* list (prefix + commands) into the list buffer, relocating image addresses */
    uint64_t *dst = (uint64_t *)(0xA0000000u | LIST_BUF_PHYS);
    dfs_seek(fd, 64, SEEK_SET);
    uint32_t done = 0, pending = 0, nopping = 0, prefix_left = prefix;     /* 'pending' = remaining words of a multi-word command */
    uint32_t cur_color = 0xFFFFFFFF, cur_mask = zaddr;                       /* to tell depth-clearing fills from other rectangles */
    static const uint8_t cmdlen[64] = {
        1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    };
    uint64_t cmd[22]; int cmdn = 0, cmdcode = 0;
    while (done < total)
    {
        uint32_t n = total - done; if (n > sizeof stage) n = sizeof stage;
        if (dfs_read(stage, 1, n, fd) != (int)n) { dfs_close(fd); snprintf(info, infolen, "read failed"); return 0; }
        for (uint32_t i = 0; i < n; i += 8)
        {
            uint64_t w = (uint64_t)be32(stage + i) << 32 | be32(stage + i + 4);
            if (pending == 0)
            {
                int code = stage[i] & 0x3F;
                pending = cmdlen[code]; cmdcode = code; cmdn = 0;
                if (code == 0x3D || code == 0x3E || code == 0x3F)
                    w = (w & ~0x3FFFFFFull) | ((w + off) & 0x3FFFFFF);
                if (code == 0x3F) cur_color = w & 0x3FFFFFF;        /* after relocation */
                if (code == 0x3E) cur_mask = w & 0x3FFFFFF;
                if (var->img == 1 && code == 0x3F && cur_color != zaddr + off) w = (w & ~0x3FFFFFFull) | COLOR_LOW;      /* the drawn colour buffer only */
                if (var->img == 2 && (code == 0x3E || (code == 0x3F && cur_color == zaddr + off))) w = (w & ~0x3FFFFFFull) | ((w + 0x100000) & 0x3FFFFFF);
                if (code == 0x2F && var->om != P_NONE) w = patch_om(w, var->om);
                int cls = nop_class(code);
                int drop = (cls & variant_mask) != 0;
                if (cls == V_RECT && code == 0x36 && cur_color == cur_mask) drop = var->zpre != 0;   /* a fill into the depth buffer: keep (z_far/z_near: remove) */
                nopping = prefix_left == 0 && drop;                   /* the prefix (carried-over state) always stays */
                if (prefix_left) prefix_left--;
            }
            pending--;
            cmd[cmdn++] = nopping ? 0 : w;
            if (pending == 0)
            {
                if (cmdcode >= 0x08 && cmdcode <= 0x0F && var->tri != TRI_NONE && !nopping) patch_tri(cmd, cmdn, var->tri);
                for (int q = 0; q < cmdn; q++) *dst++ = cmd[q];
            }
        }
        done += n;
    }
    /* depth buffer into its (relocated) place and a copy for the following runs */
    zcopy_addr = zaddr + off; zcopy_bytes = zbytes;
    /* a 640x480 depth buffer (600 KiB) does not fit the copy area near the top: it goes into the gap between the
     * list's own depth and colour buffers (MGC: 0x4EA200-0x600000), which the list does not touch */
    zcopy_cur = zbytes <= 0x30000 ? zcopy : (uint8_t *)(0x80000000u | ZBIG_PHYS);
    if (dfs_read(zcopy_cur, 1, zbytes, fd) != (int)zbytes) { dfs_close(fd); snprintf(info, infolen, "z read failed"); return 0; }
    if (var->zpre == 1) memset(zcopy_cur, 0, zbytes);          /* nearest: every depth test fails */
    if (var->zpre == 2) memset(zcopy_cur, 0xFF, zbytes);       /* farthest (0xFFFF: dz max, z max) */
    if (var->img == 2) zcopy_addr += 0x100000;
    dfs_close(fd);
    *first = LIST_BUF_PHYS; *last = LIST_BUF_PHYS + total; *off_out = off; *model = cyc;
    snprintf(info, infolen, "list %lu B, prefix %lu, z %06lX+%lu, regions %06lX-%06lX, offset +%06lX", (unsigned long)list_bytes,
             (unsigned long)prefix, (unsigned long)zaddr, (unsigned long)zbytes, (unsigned long)lo, (unsigned long)hi, (unsigned long)off);
    return 1;
}

static void restore_z(void)
{
    memcpy((void *)(0xA0000000u | zcopy_addr), zcopy_cur, zcopy_bytes);
}

/* VI modes: the console display of this ROM uses AA_MODE 3 (replicate, 1 line fetched per output line);
 * games use AA_MODE 0 + divot + dedither, which fetches 3 framebuffer lines per output line */
typedef struct { const char *name; uint32_t ctrl; } vimode_t;
static const vimode_t vimodes[] = {
    { "vi_off",                 0 },
    { "aa3_replicate(rom)",     0x00003302 },   /* what this ROM's console mode sets */
    { "aa2_resample",           0x00003202 },
    { "aa1_aa_if_needed+divot", 0x00003112 },
    { "aa0_aa_always+divot",    0x00003012 },
    { "bar_0x13012",            0x00013012 },   /* Beetle Adventure Racing (J): aa0 + divot + dedither */
    { "dk64_0x13016",           0x00013016 },   /* Donkey Kong 64 (J): + gamma dither */
    { "hires_640x480i_0x13052", 0x00013052 },   /* 640x480 interlaced (serrate) 16-bit, aa0 + divot + dedither; VI_WIDTH 640 */
    { "mgc2011_0x3242_w1280",   0x00003242 },   /* MGC 2011 demo: 640x480 interlaced, aa2 (resample only), VI_WIDTH 1280 */
};
#define N_VIMODES ((int)(sizeof vimodes / sizeof vimodes[0]))

/* CPU loads run beside the list (as in v10) */
enum { C_NONE, C_CHASE, C_ICACHE, C_RWRITE, C_MIXED, C_UCWRITE, C_DMA64, C_DMA512, N_COND };
static const char *const cnames[N_COND] = {
    "none", "cpu_chase(dependent fills)", "cpu_icache(32B fills)", "cpu_random_write(fill+writeback)",
    "cpu_mixed(load+store+alu+fpu)", "cpu_uncached_write32", "sp_dma_64B", "sp_dma_512B",
};

__attribute__((noinline)) static void big_code(void)
{
    asm volatile(".rept 8192\n nop\n .endr");
}

static uint32_t load_chunk(int cond, uint32_t *state)
{
    volatile uint32_t *ch = rchase;
    volatile uint32_t *uc = (volatile uint32_t *)dma_buf;
    uint32_t n = state[0], rnd = state[1], sink = state[2];
    switch (cond)
    {
    case C_CHASE:   for (int i = 0; i < 256; i++) n = ch[n * 4]; state[0] = n; return 256;
    case C_ICACHE:  big_code(); return 1024;
    case C_RWRITE:  for (int i = 0; i < 256; i++) { rnd = rnd * 1664525u + 1013904223u; ch[(rnd >> 7) & 0x3FFF & ~3u] = i; } state[1] = rnd; return 256;
    case C_MIXED:
        for (int i = 0; i < 256; i++)
        {
            rnd = rnd * 1664525u + 1013904223u;
            uint32_t a = ch[(rnd >> 7) & 0x3FFF];
            sink = sink * 7 + a;
            ch[((rnd >> 3) * 97) & 0x3FFF] = sink;
            asm volatile("" : "+r"(sink));
        }
        state[1] = rnd; state[2] = sink; return 256;
    case C_UCWRITE: for (int i = 0; i < 256; i++) uc[(n + i * 61) & 0x3FFF] = i; state[0] = n + 256 * 61; return 256;
    case C_DMA64: case C_DMA512:
    {
        int size = cond == C_DMA64 ? 64 : 512;
        if (HW_SP_STATUS & SPSTAT_DMA_BUSY) return 0;
        HW_SP_MEM_ADDR  = 0;
        HW_SP_DRAM_ADDR = PhysicalAddr(dma_buf) + ((n * size) & 0xFFFF & ~(size - 1));
        HW_SP_RD_LEN    = size - 1;
        state[0] = n + 1; return 1;
    }
    default: return 0;
    }
}

#define HW_VI_WIDTH     REG(0xA4400008)
#define HW_VI_V_SYNC    REG(0xA4400018)
#define HW_VI_X_SCALE   REG(0xA4400030)
static uint32_t vi_ctrl_console, vi_width_console, vi_vsync_console, vi_xscale_console;   /* what libdragon's console display uses */
static void set_vi(uint32_t ctrl)
{
    wait_ms(40);
    if (ctrl == 0x3242) { HW_VI_WIDTH = 1280; HW_VI_X_SCALE = 0x400; }  /* the demo's stride */
    else if (ctrl & 0x40) { HW_VI_WIDTH = 640; HW_VI_X_SCALE = 0x400; }   /* hires: 1280-byte lines, 1:1 scale */
    else if (ctrl == 0)   { }
    else             { HW_VI_WIDTH = 320; HW_VI_X_SCALE = 0x200; }        /* the games' 320x240 framebuffer (the console display of this ROM uses 640) */
    /* interlaced (serrate) modes need the interlaced line count, or the VI/interrupt timing goes wrong (v12 on NTSC hung here) */
    if (ctrl & 0x40) HW_VI_V_SYNC = vi_vsync_console & ~1u; else HW_VI_V_SYNC = vi_vsync_console;
    HW_VI_CTRL = ctrl;
    wait_ms(40);     /* two fields: the VI settles */
}
/* back to the console display (all four registers), before anything renders text */
static void vi_console(void)
{
    HW_VI_CTRL = 0; wait_ms(20);
    HW_VI_WIDTH = vi_width_console; HW_VI_X_SCALE = vi_xscale_console; HW_VI_V_SYNC = vi_vsync_console;
    HW_VI_CTRL = vi_ctrl_console;
    wait_ms(40);
}

/* the load alone (RDP idle) under a VI mode: ticks per operation */
static void cpu_alone(uint32_t vi_ctrl_on)
{
    tprintf("# VA lines: vi mode, load, ticks, operations, ticks per operation (RDP idle)\n");
    for (int vi = 0; vi < N_VIMODES; vi++)
    {
        for (int cond = C_CHASE; cond < N_COND; cond++)
        {
            HW_VI_CTRL = vi_ctrl_on; wait_ms(40);        /* the console display needs the VI on while progress() renders */
            progress("VA  %s  %s", vimodes[vi].name, cnames[cond]);
            set_vi(vimodes[vi].ctrl ? vimodes[vi].ctrl : (vi_ctrl_on & ~3u));
            uint32_t best = 0xFFFFFFFF, bops = 0;
            for (int r = 0; r < RUNS; r++)
            {
                uint32_t state[3] = { 0, 0x2545F491u, 0 }, ops = 0;
                if (cond == C_CHASE) build_rchase();
                uint32_t t0 = get_ticks();
                while (ops < 8192 || (cond >= C_DMA64 && ops < 256)) ops += load_chunk(cond, state);
                uint32_t timeout = 0x100000;
                while ((HW_SP_STATUS & SPSTAT_DMA_BUSY) && --timeout) {}
                uint32_t t = get_ticks() - t0;
                data_cache_hit_writeback_invalidate(rchase, 65536);
                if (t < best) { best = t; bops = ops; }
            }
            tprintf("VA\t%s\t%s\t%lu\t%lu\t%.2f\n", vimodes[vi].name, cnames[cond], (unsigned long)best, (unsigned long)bops, (double)best / bops);
        }
    }
    set_vi(vi_ctrl_on);
}

/* 4096 dependent line fills with the RDP idle: the VI's framebuffer fetch slows them by 7% (v12), so the ticks say whether the VI is really on */
static uint32_t vi_probe(void)
{
    uint32_t state[3] = { 0, 0x2545F491u, 0 }, ops = 0;
    build_rchase();
    uint32_t t0 = get_ticks();
    while (ops < 4096) ops += load_chunk(C_CHASE, state);
    uint32_t t = get_ticks() - t0;
    data_cache_hit_writeback_invalidate(rchase, 65536);
    return t;
}

static void series_x(uint32_t vi_ctrl_on)
{
    (void)vi_ctrl_on;
    tprintf("# X lines: list, variant, vi mode (vi_off), clock (smallest of %d), ticks, model (ares tables of the unpatched full list), VI_CTRL read back, VI_WIDTH, VI probe\n", RUNS);
    tprintf("# v24 variants: z_far / z_near = the depth-clearing fill removed and the depth buffer preset farthest / nearest (every first draw passes / every test fails); om_no_atomic = ATOMIC_PRIM off; color_low = the drawn colour buffer moved to 0x0F5000; z_up1m = the depth buffer 1 MiB higher (BAR only); tri_only_zfar / znear = the same on tri_only\n");
    tprintf("# XI lines: list, info (placement in RDRAM)\n");
    tprintf("# variants: no_tri / tri_only as v22 (NOPs replace the class); om_* patch every Set_Other_Modes (no_zcmp: Z_COMPARE off = all pass; no_zupd: Z_UPDATE off; no_aa: AA_EN off; no_persp: TEX_PERSP off; no_lod: TEX_LOD/SHARPEN/DETAIL off; 1cyc: 2-cycle lists as 1-cycle; no_dither: dither off; point: point sampling; no_tlut: EN_TLUT off; no_cvg: ALPHA_CVG_SEL/CVG_X_ALPHA/CVG_DST/COLOR_ON_CVG/ALPHA_COMPARE off; no_imrd: IMAGE_READ off; wlike: v22 W's mode bits with the list's cycle type, blender mux, Z and IMAGE_READ)\n");
    tprintf("# z_near: the depth-clearing fill removed and the depth buffer preset to 0 (every depth test fails); tri_flat: all shade/texture/depth slopes of the 0F triangles zeroed; tri_noshade/notex/edgez: 0F -> 0B / 0D / 09 (dropped words become NOPs)\n");
    if (!mem_8mb) { tprintf("# X skipped: needs 8 MiB\n"); return; }
    if (dfs_init(DFS_DEFAULT_LOCATION) != DFS_ESUCCESS) { tprintf("# X skipped: no filesystem\n"); return; }
    for (int li = 0; li < N_RLISTS && !rdp_dead; li++)
    {
        for (int vi = 0; vi < N_VARIANTS && !rdp_dead; vi++)
        {
            uint32_t first, last, off, model; char info[160];
            if (variants[vi].img == 2 && strncmp(rlists[li].name, "bar_", 4) != 0) continue;   /* z_up1m: BAR only (DK64's depth + 1 MiB would hit this program) */
            vi_console();
            progress("X  %s  %s", rlists[li].name, variants[vi].name);
            enable_interrupts();
            int ok = load_list(rlists[li].name, &variants[vi], &first, &last, &off, &model, info, sizeof info);
            disable_interrupts();
            if (vi == 0) tprintf("XI\t%s\t%s\n", rlists[li].name, info);
            if (!ok) { if (vi == 0) break; continue; }
            /* VI off always; the game's VI mode for full and tri_only (the VI stopped fetching on the NTSC console in v22 after the first case: the probe tells) */
            int nmodes = 1;
            for (int m = 0; m < nmodes && !rdp_dead; m++)
            {
                uint32_t ctrl = m == 0 ? 0 : rlists[li].vi_ctrl;
                vi_console();
                progress("X  %s  %s  %s", rlists[li].name, variants[vi].name, m == 0 ? "vi_off" : "game");
                set_vi(ctrl);
                uint32_t best = 0xFFFFFFFF, bticks = 0;
                for (int r = 0; r < RUNS && !rdp_dead; r++)
                {
                    restore_z();
                    meas_t m2;
                    timed_out = 0;
                    dp_start_at(first, last);
                    dp_finish(&m2);
                    if (timed_out) { rdp_recover(); best = 0; break; }
                    if (m2.clock < best) { best = m2.clock; bticks = last_ticks; }
                }
                uint32_t probe = vi_probe();
                tprintf("X\t%s\t%s\t%s\t%lu\t%lu\t%lu\t%lX\t%lu\t%lu\n", rlists[li].name, variants[vi].name, m == 0 ? "vi_off" : "game",
                        (unsigned long)best, (unsigned long)bticks, (unsigned long)model, (unsigned long)(HW_VI_CTRL & 0xFFFF), (unsigned long)HW_VI_WIDTH, (unsigned long)probe);
            }
            if (rdp_dead) { enable_interrupts(); save_to_sd_quiet(); disable_interrupts(); return; }
        }
        vi_console();
        enable_interrupts(); save_to_sd(); disable_interrupts();
    }
}


/* ======================================================================= */
/* Y: synthetic triangles, one property of the games' triangles at a time, on top of v22's W triangles         */
/*    (2cyc_tex_blend_z, random positions, H spans of w pixels). v22 found the game lists 10-20% dearer per span */
/*    than these; the candidates are the mode bits (AA, perspective, LOD, dither, TLUT), slanted edges (the edge */
/*    walker visits floor(xmin)..floor(xmax) over 4 sub-scanlines), attribute slopes, strips, interleaved state. */
/* ======================================================================= */

#define OM_TEX_2CYC (OM_2CYC | OM_BILERP | OM_BI_LERP01 | OM_NO_DITHER)
#define OM_TEX_1CYC (OM_1CYC | OM_BILERP | OM_BI_LERP01 | OM_NO_DITHER)
#define OM_BLENDL   (OM_FORCE_BLEND | OM_IMAGE_READ | OM_B_M2A_MEM)
#define OM_ZL       (OM_Z_COMPARE | OM_Z_UPDATE)
#define Y_SPANS 4096
#define Y_H     8
enum { SH_RECT, SH_SHEAR_HALF, SH_SHEAR_TWO, SH_APEX, SH_RECT_FRAC };
enum { PL_RANDOM, PL_STRIP, PL_SAME, PL_RANDOM_Y, PL_RANDOM_X };
enum { IL_NONE, IL_STATE1, IL_STATE8, IL_TILE1, IL_LOAD8, IL_SYNCPIPE1 };
enum { TX_RGBA16, TX_CI4, TX_RGBA32, TX_I8 };
typedef struct {
    const char *name;
    uint32_t om_hi, om_lo;      /* other modes (hi = bits 32..55) */
    int shape, place, inter, tex, op; /* op: triangle opcode 0x08..0x0F */
    int slopes;                 /* 1: shade/texture/depth slopes nonzero */
    int lod;                    /* 1: tile 1 defined too (for TEX_LOD) */
    uint32_t fb_phys, zb_phys;  /* 0 = the default arena; else absolute RDRAM addresses of the colour / depth buffers */
    int zclear;                 /* 0 = far (all pass), 1 = near (all fail) */
} ycase_t;
#define BAR_HI  0x182c0fu
#define BAR_LO  0x0c182078u
#define DK_HI   0x882c0fu
#define DK_LO   0x00552078u
#define W_HI    (OM_TEX_2CYC)
#define W_LO    (OM_BLENDL | OM_ZL)
#define W1_HI   (OM_TEX_1CYC)
#define YB      SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 0, 0
static const ycase_t ycases[] = {
    /* wide spans with real texture stepping (v26 stepped 1/32 texel per pixel and came out 25-40% below the tables for w >= 64) */
    { "wide_tbz_1tx",     W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },   /* 1 texel per pixel */
    { "wide_tbz_4tx",     W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 2, 0, 0x300000, 0x4DA800, 0 },   /* 4 texels per pixel (minified) */
    { "wide_tbz_qtx",     W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 4, 0, 0x300000, 0x4DA800, 0 },   /* 1/4 texel per pixel (magnified) */
    { "wide_tbz_persp",   W_HI | (1u << 19), W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 3, 0, 0x300000, 0x4DA800, 0 },  /* TEX_PERSP with W varying */
    { "wide_bar_1tx",     BAR_HI, BAR_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },
    { "wide_bar_persp",   BAR_HI, BAR_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 3, 0, 0x300000, 0x4DA800, 0 },
    { "wide_tb_1tx",      W_HI, W_LO & ~OM_ZL, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },
    { "wide_tz_1tx",      W_HI, W_LO & ~(OM_FORCE_BLEND | OM_IMAGE_READ), SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },
    { "wide_1cyc_1tx",    W1_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },
    { "wide_notex_sz",    W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0D, 1, 0, 0x300000, 0x4DA800, 0 },   /* shade+z, no texture words */
    { "wide_tbz_i8_1tx",  W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_I8, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },
    { "wide_tbz_rgba32",  W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA32, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },
    { "base_1tx",         W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 1, 0, 0x300000, 0x4DA800, 0 },   /* v26 wide_2cyc_tbz again, 1/32 texel per pixel below */
    { "base_1_32",        W_HI, W_LO, SH_RECT, PL_RANDOM, IL_NONE, TX_RGBA16, 0x0F, 0, 0, 0x300000, 0x4DA800, 0 },
};
#define N_YCASES ((int)(sizeof ycases / sizeof ycases[0]))

/* one triangle of H rows whose spans average w pixels, starting at x0 (left edge of the top row), y0 */
static void emit_tri_y(cbuf_t *c, const ycase_t *y, int x0, int y0, int w, int H, uint32_t rnd)
{
    int op = y->op;
    int64_t xh, xm, xl, dxh = 0, dxm = 0, dxl = 0; int ym;
    int64_t X0 = (int64_t)x0 << 16, W = (int64_t)w << 16;
    switch (y->shape)
    {
    default:
    case SH_RECT:      xh = X0; xm = xl = X0 + W; ym = y0 + H; break;
    case SH_RECT_FRAC: xh = X0 + 0x8000; xm = xl = X0 + W + 0x8000; ym = y0 + H; break;
    case SH_SHEAR_HALF: xh = X0; xm = xl = X0 + W; dxh = dxm = dxl = 0x8000; ym = y0 + H; break;      /* all edges +0.5 px per row */
    case SH_SHEAR_TWO:  xh = X0; xm = xl = X0 + W; dxh = dxm = dxl = 0x20000; ym = y0 + H; break;     /* +2 px per row */
    case SH_APEX:                                                                                      /* apex at the top centre, base 2w wide: spans average w */
        xh = xm = xl = X0 + W; dxh = -(W / H); dxm = dxl = W / H; ym = y0 + H; break;
    }
    /* lft (bit 55): major edge on the left when the major edge is the one with the smaller x */
    uint64_t lft = (y->shape == SH_APEX || dxh <= dxm) ? 1ull << 55 : 0;
    uint64_t yl = (uint64_t)((y0 + H) * 4) & 0x3FFF, ymw = (uint64_t)(ym * 4) & 0x3FFF, yhw = (uint64_t)(y0 * 4) & 0x3FFF;
    emit(c, CMD(op) | lft | yl << 32 | ymw << 16 | yhw);
    emit(c, (uint64_t)(uint32_t)xl << 32 | (uint32_t)dxl);
    emit(c, (uint64_t)(uint32_t)xh << 32 | (uint32_t)dxh);
    emit(c, (uint64_t)(uint32_t)xm << 32 | (uint32_t)dxm);
    int shade = op & 4, tex = op & 2, z = op & 1;
    if (shade)
    {
        emit(c, 0x00FF00C000800040ull);
        if (y->slopes) { emit(c, 0x0001000200030004ull); emit(c, 0); emit(c, 0); emit(c, 0xFFFF0001FFFF0001ull); emit(c, 0x0001FFFF0001FFFFull); emit(c, 0); emit(c, 0); }
        else { emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); }
    }
    if (tex)
    {
        /* S, T start at a random texel (10.5), W = 1; DsDx = 1/32 texel (v22) or one texel per pixel with slopes */
        uint64_t s0 = (uint64_t)((rnd >> 3) & 0x3FF) << 48 | (uint64_t)((rnd >> 13) & 0x3FF) << 32;
        emit(c, s0 | 0x00010000ull);                                                  /* S.i T.i | W.i 0 */
        if (y->slopes) {
            uint64_t ds = y->slopes == 2 ? 0x0080 : y->slopes == 4 ? 0x0008 : 0x0020;      /* texels per pixel: 4, 1/4, 1 (s10.5) */
            uint64_t dw = y->slopes == 3 ? 0x00000100ull : 0;                              /* W slope: 1/256 per pixel (W starts at 1) */
            emit(c, ds << 48 | (uint64_t)0x0008 << 32 | dw); emit(c, 0); emit(c, 0); emit(c, (uint64_t)0x0008 << 48 | ds << 32); emit(c, (uint64_t)0x0010 << 48 | (uint64_t)0x0010 << 32); emit(c, 0); emit(c, 0);
        }
        else { emit(c, (uint64_t)(1u << 16) << 32); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, (uint64_t)(1u << 16)); emit(c, 0); emit(c, 0); }
    }
    if (z)
    {
        if (y->slopes) { emit(c, (uint64_t)0x40000000ull << 32 | 0x00001000u); emit(c, (uint64_t)0x00001000ull << 32 | 0x00001000u); }
        else { emit(c, (uint64_t)0x40000000ull << 32); emit(c, 0); }
    }
}

static void y_texture_setup(cbuf_t *c, const ycase_t *y)
{
    switch (y->tex)
    {
    default:
    case TX_RGBA16:
        set_texture_image(c, FMT_RGBA, SZ_16, 32, PhysicalAddr(tex16));
        set_tile(c, 7, FMT_RGBA, SZ_16, 8, 0, 0, 0); sync_load(c); load_tile(c, 7, 0, 0, 31, 31); sync_pipe(c);
        set_tile(c, 0, FMT_RGBA, SZ_16, 8, 0, 5, 5); set_tile_size(c, 0, 0, 0, 31, 31);
        if (y->lod) { set_tile(c, 1, FMT_RGBA, SZ_16, 8, 0, 5, 5); set_tile_size(c, 1, 0, 0, 31, 31); }
        break;
    case TX_RGBA32:
        set_texture_image(c, FMT_RGBA, SZ_32, 32, PhysicalAddr(tex16));                  /* 32x16 of the 2 KiB buffer, read as 32-bit */
        set_tile(c, 7, FMT_RGBA, SZ_32, 8, 0, 0, 0); sync_load(c); load_tile(c, 7, 0, 0, 31, 15); sync_pipe(c);
        set_tile(c, 0, FMT_RGBA, SZ_32, 8, 0, 4, 5); set_tile_size(c, 0, 0, 0, 31, 15);
        break;
    case TX_I8:
        set_texture_image(c, FMT_I, SZ_8, 32, PhysicalAddr(tex16));
        set_tile(c, 7, FMT_I, SZ_8, 4, 0, 0, 0); sync_load(c); load_tile(c, 7, 0, 0, 31, 31); sync_pipe(c);
        set_tile(c, 0, FMT_I, SZ_8, 4, 0, 5, 5); set_tile_size(c, 0, 0, 0, 31, 31);
        break;
    case TX_CI4:
        set_texture_image(c, FMT_RGBA, SZ_16, 16, PhysicalAddr(tex16));                  /* the palette: 16 entries */
        set_tile(c, 5, FMT_RGBA, SZ_16, 0, 256, 0, 0); sync_load(c); tile_cmd(c, 0x30, 5, 0, 0, 15, 0); sync_pipe(c);
        set_texture_image(c, FMT_CI, SZ_4, 32, PhysicalAddr(tex16) + 64);
        set_tile(c, 7, FMT_CI, SZ_4, 2, 0, 0, 0); sync_load(c); load_tile(c, 7, 0, 0, 31, 31); sync_pipe(c);
        set_tile(c, 0, FMT_CI, SZ_4, 2, 0, 5, 5); set_tile_size(c, 0, 0, 0, 31, 31);
        break;
    }
}

static void y_setup(const ycase_t *y)
{
    cbuf_t *c = &cb_setup;
    c->p = c->start;
    sync_pipe(c);
    set_color_image(c, SZ_16, fb_width, PhysicalAddr(fb));
    set_mask_image(c, PhysicalAddr(zb));
    set_scissor(c);
    emit(c, CMD(0x3A) | 0xC08040FF);
    emit(c, CMD(0x3B) | 0x80C0FFFF);
    set_prim_depth(c, 0x7000);
    emit(c, CMD(0x37) | 0x7BDE7BDEu);
    emit(c, CMD(0x39) | 0x808080FF);            /* blend color */
    y_texture_setup(c, y);
    emit(c, CMD(0x2F) | (uint64_t)y->om_hi << 32 | y->om_lo);
    int cyc2 = ((y->om_hi >> 20) & 3) == 1;
    comb_t c0 = (y->op & 2) ? CC_TEXPRIM : CC_PRIM;
    emit(c, cyc2 ? combine_cmd(c0, CC_COMBENV) : combine_cmd(c0, c0));
    sync_full(c);
    timed_out = 0;
    dp_run(c, NULL);
}

static void series_y(uint32_t vi_ctrl_on)
{
    (void)vi_ctrl_on;
    static const int widths[] = { 8, 32, 128, 318 };
    tprintf("# Y lines: case, w, H, triangles, spans, clock (smallest of %d), ticks, clocks/span, clocks/triangle, VI probe. VI off, depth cleared far (all pass), 2cyc_tex_blend_z unless the case says otherwise\n", RUNS);
    tprintf("# v24 cases: zfail = depth cleared nearest; no_zcmp / no_zupd / no_imrd = that mode bit off; fbzb_* = colour / depth buffers at those RDRAM addresses (adjacent = depth right after colour; bar = 0x200000 / 0x3DA800 as the game; bar_reloc / dk_reloc = as replayed in X); random_rows / random_cols = only the row / only the column random; 1cyc_atomic / 2cyc_atomic / dk_om_noatomic = ATOMIC_PRIM\n");
    tprintf("# Y cases: base = v22's W triangle (vertical edges at integer x, random position, 1/32 texel per pixel, constant shade/z); frac = edges at x+0.5; shear_half/two = all edges slanted 0.5/2 px per row (spans stay w wide); apex = real triangle, apex on top, base 2w (spans average w)\n");
    tprintf("#   aa = AA_EN; persp = TEX_PERSP; lod = TEX_LOD (tile 1 = tile 0); dither = rgb/alpha dither on; point = point sampling; cvg = ALPHA_CVG_SEL + CVG_DST full; bar_om = BAR's other modes 0x182c0f/0x0c182078; dk_om = DK64's 0x882c0f/0x00552078 (1-cycle)\n");
    tprintf("#   slopes = shade/texture (1 texel per pixel)/depth slopes nonzero; strip = triangles side by side along rows; same_place = every triangle at the same spot; state_every1/8 = Set_Other_Modes + Set_Combine before every / every 8th triangle; syncpipe_every1 = Sync_Pipe before every triangle; tile_every1 = Set_Tile + Set_Tile_Size + Sync_Tile before every triangle; load_every8 = Sync_Load + Load_Block (64 B) + Sync_Tile before every 8th\n");
    tprintf("#   ci4_tlut / rgba32_tex / i8_tex = texture format; op_0b/0d/09/08 = triangle command type (texture+z 14 words / shade+z 14 / z 6 / plain 4; the combiner reads the missing attribute from the previous one)\n");
    for (int yi = 0; yi < N_YCASES && !rdp_dead; yi++)
    {
        const ycase_t *y = &ycases[yi];
        for (int wi = 0; wi < (int)(sizeof widths / sizeof widths[0]) && !rdp_dead; wi++)
        {
            int w = widths[wi], H = Y_H;
            if ((y->shape == SH_APEX ? 2 * w : w + 1) >= FB_W) continue;   /* does not fit the buffer */
            vi_console();
            progress("Y  %s  w%d", y->name, w);
            if (y->fb_phys) { fb = (uint16_t *)(0xA0000000u | y->fb_phys); zb = (uint16_t *)(0xA0000000u | y->zb_phys); }
            else { fb = (uint16_t *)arena; zb = (uint16_t *)(arena + ZB_DEFAULT); }
            set_vi(0);
            cbuf_t *c = &cb_body; c->p = c->start;
            int ntri = Y_SPANS / H, spans = ntri * H;
            uint32_t rnd = 0x1234567u;
            int sx = 0, sy = 0;
            for (int t = 0; t < ntri; t++)
            {
                int x0, y0, wmax = y->shape == SH_APEX ? 2 * w : w + (y->shape == SH_SHEAR_TWO ? 2 * H : y->shape == SH_SHEAR_HALF ? H / 2 + 1 : 1);
                rnd = rnd * 1664525u + 1013904223u;
                if (y->place == PL_STRIP)
                {
                    x0 = sx; y0 = sy; sx += w; if (sx + wmax >= FB_W) { sx = 0; sy += H; if (sy + H >= FB_H) sy = 0; }
                }
                else if (y->place == PL_SAME) { x0 = 100; y0 = 100; }
                else if (y->place == PL_RANDOM_Y) { x0 = 100; y0 = (int)((rnd >> 20) % (uint32_t)(FB_H - H)); }
                else if (y->place == PL_RANDOM_X) { x0 = (int)((rnd >> 8) % (uint32_t)(FB_W - wmax)); y0 = 100; }
                else { x0 = (int)((rnd >> 8) % (uint32_t)(FB_W - wmax)); y0 = (int)((rnd >> 20) % (uint32_t)(FB_H - H)); }
                switch (y->inter)
                {
                case IL_STATE1: emit(c, CMD(0x2F) | (uint64_t)y->om_hi << 32 | y->om_lo); emit(c, combine_cmd(CC_TEXPRIM, CC_COMBENV)); break;
                case IL_STATE8: if ((t & 7) == 0) { emit(c, CMD(0x2F) | (uint64_t)y->om_hi << 32 | y->om_lo); emit(c, combine_cmd(CC_TEXPRIM, CC_COMBENV)); } break;
                case IL_SYNCPIPE1: sync_pipe(c); break;
                case IL_TILE1: set_tile(c, 0, FMT_RGBA, SZ_16, 8, 0, 5, 5); set_tile_size(c, 0, 0, 0, 31, 31); emit(c, CMD(0x28)); break;
                case IL_LOAD8: if ((t & 7) == 0) { sync_load(c); set_tile(c, 7, FMT_RGBA, SZ_16, 8, 0, 0, 0); load_block(c, 7, 32, 0); emit(c, CMD(0x28)); } break;
                default: break;
                }
                emit_tri_y(c, y, x0, y0, w, H, rnd);
            }
            sync_full(c);
            uint32_t best = 0xFFFFFFFF, bticks = 0;
            for (int r = 0; r < RUNS && !rdp_dead; r++)
            {
                y_setup(y);
                clear_z(y->zclear ? 0x0000 : 0xFFFC, FB_H);
                meas_t m2; timed_out = 0;
                dp_run(c, &m2);
                if (timed_out) { best = 0; break; }
                if (m2.clock < best) { best = m2.clock; bticks = last_ticks; }
            }
            uint32_t probe = vi_probe();
            tprintf("Y\t%s\t%d\t%d\t%d\t%d\t%lu\t%lu\t%.2f\t%.1f\t%lu\n", y->name, w, H, ntri, spans,
                    (unsigned long)best, (unsigned long)bticks, (double)best / spans, (double)best / ntri, (unsigned long)probe);
            if (rdp_dead) { enable_interrupts(); save_to_sd_quiet(); disable_interrupts(); return; }
        }
        vi_console(); enable_interrupts(); save_to_sd(); disable_interrupts();
    }
    vi_console();
}

/* ======================================================================= */

static char save_path[64];
static int  sd_ok = -1;

static int save_to_sd(void)
{
    if (sd_ok < 0)
    {
        struct stat st;
        sd_ok = 0;
        if (!debug_init_sdfs("sd:/", -1)) { show("NO SD CARD - NOT SAVED\n"); return 0; }
        if (mkdir("sd:/rdp_timing", 0777) != 0 && errno != EEXIST) { show("CANNOT CREATE sd:/rdp_timing\n"); return 0; }
        for (int n = 1; n < 1000; n++)
        {
            snprintf(save_path, sizeof save_path, "sd:/rdp_timing/rdp_timing_v27_%03d.txt", n);
            if (stat(save_path, &st) != 0) break;
        }
        sd_ok = 1;
    }
    if (!sd_ok) return 0;
    disable_interrupts();
    FILE *f = fopen(save_path, "w");
    int ok = f && fwrite(out.buf, 1, out.len, f) == out.len;
    if (f && fclose(f) != 0) ok = 0;
    enable_interrupts();
    show(ok ? "SAVED %s (%u bytes)\n" : "SD WRITE FAILED %s\n", save_path, (unsigned)out.len);
    return ok;
}

static int save_to_sd_quiet(void)
{
    if (sd_ok < 0) { if (!debug_init_sdfs("sd:/", -1)) { sd_ok = 0; return 0; } mkdir("sd:/rdp_timing", 0777); struct stat st; for (int n = 1; n < 1000; n++) { snprintf(save_path, sizeof save_path, "sd:/rdp_timing/rdp_timing_v27_%03d.txt", n); if (stat(save_path, &st) != 0) break; } sd_ok = 1; }
    if (!sd_ok) return 0;
    FILE *f = fopen(save_path, "w");
    int ok = f && fwrite(out.buf, 1, out.len, f) == out.len;
    if (f && fclose(f) != 0) ok = 0;
    return ok;
}

static void checkpoint(const char *done, const char *next)
{
    enable_interrupts();
    show("SERIES %s DONE (%u bytes), NEXT: %s\n", done, (unsigned)out.len, next);
    save_to_sd();
    wait_ms(500);
    disable_interrupts();
}

int main(void)
{
    debug_init_usblog();
    debug_init_isviewer();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_render_mode(RENDER_MANUAL);
    mem_8mb = get_memory_size() >= 0x800000;

    /* small allocations only before R: the replayed lists need everything from the heap top up */
    tex16  = malloc_uncached_aligned(64, 32 * 32 * 2);
    cb_setup.start = malloc_uncached_aligned(64, 4096);
    out.cap = 128 * 1024;
    if (mem_8mb)
    {
        dma_buf = (uint8_t *)(0xA0000000u | DMA_PHYS);
        zcopy   = (uint8_t *)(0x80000000u | ZCOPY_PHYS);
        out.buf = (char *)(0x80000000u | OUT_PHYS);
    }
    else
    {
        dma_buf = malloc_uncached_aligned(64, 64 * 1024);
        zcopy   = malloc(0x30000);
        out.buf = malloc(out.cap);
    }
    if (!tex16 || !cb_setup.start || !dma_buf || !zcopy || !out.buf)
    {
        show("OUT OF MEMORY\n");
        for (;;) {}
    }
    for (int i = 0; i < 32 * 32; i++) tex16[i] = (uint16_t)(i * 0x9E37 | 1);
    uint32_t heap_top = PhysicalAddr(sbrk(0));
    reloc_low = ((heap_top + 0xFFFF) & ~0xFFFFu) + 0x10000;
    uint32_t fb_phys = 0;
    { surface_t *d = display_get(); fb_phys = PhysicalAddr(d->buffer); display_show(d); }

    HW_DPC_STATUS = DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH;
    HW_MI_MODE = MI_CLEAR_DP_INT;

    static const char *const tv[] = { "PAL", "NTSC", "MPAL" };
    show("RDP TIMING v" VERSION "  %s  %d MiB\n", tv[get_tv_type() % 3], get_memory_size() >> 20);
    tprintf("# rdp_timing v" VERSION " - wide spans with real texture stepping\n");
    tprintf("# mi_version\t0x%08" PRIX32 "\n# tv_type\t%s\n# memory_size\t%d\n# runs_per_case\t%d\n",
            HW_MI_VERSION, tv[get_tv_type() % 3], get_memory_size(), RUNS);
    tprintf("# texture at 0x%06lX; dma buffer at 0x%06lX; heap top 0x%06lX; display buffer at 0x%06lX; replayed lists at 0x%06lX, their data moved up to at least 0x%06lX\n",
            (unsigned long)PhysicalAddr(tex16), (unsigned long)PhysicalAddr(dma_buf),
            (unsigned long)heap_top, (unsigned long)fb_phys, (unsigned long)LIST_BUF_PHYS, (unsigned long)reloc_low);
    tprintf("# values: smallest DPC_CLOCK of %d runs, including the list start and Sync_Full; ticks are COP0 count (46.875 MHz, x2 = CPU cycles, x4/3 = RDP clocks)\n", RUNS);

    wait_ms(100);
    disable_interrupts();
    uint32_t vi_ctrl = HW_VI_CTRL;
    vi_ctrl_console = vi_ctrl; vi_width_console = HW_VI_WIDTH; vi_vsync_console = HW_VI_V_SYNC; vi_xscale_console = HW_VI_X_SCALE;
    tprintf("# console VI: ctrl 0x%lX width %lu v_sync 0x%lX x_scale 0x%lX\n", (unsigned long)vi_ctrl, (unsigned long)vi_width_console, (unsigned long)vi_vsync_console, (unsigned long)vi_xscale_console);

    {
        cb_setup.p = cb_setup.start;
        sync_full(&cb_setup);
        uint32_t best = 0xFFFFFFFF;
        for (int r = 0; r < RUNS; r++) { meas_t m; dp_run(&cb_setup, &m); if (m.clock < best) best = m.clock; }
        tprintf("B0\t%lu\n", (unsigned long)best);
    }
    checkpoint("init", "X (in-game lists with patched modes / triangles)");
    series_x(vi_ctrl); checkpoint("X", "Y (synthetic triangles, one property at a time)");
    {
        arena_size = 0x4B000 + 0x25800 + 0x800;
        arena = malloc_uncached_aligned(2048, arena_size);
        cb_body.start = (uint64_t *)(0xA0000000u | LIST_BUF_PHYS);   /* the replayed lists' buffer (X is over): keeps the heap below 0x200000 for the fbzb_* cases */
        if (!arena || !cb_body.start) { enable_interrupts(); show("OUT OF MEMORY\n"); for (;;) {} }
        memset(arena, 0, arena_size);
        fb = (uint16_t *)arena;
        zb = (uint16_t *)(arena + ZB_DEFAULT);
        fb_width = FB_W;
        series_y(vi_ctrl); checkpoint("Y", "end");
    }
    if (timeouts) tprintf("# %d lists timed out (clock 0)\n", timeouts);

    vi_console();
    enable_interrupts();
    if (rdp_dead)
    {
        tprintf("# ABORTED: the RDP stopped responding (%d timeouts)\n", timeouts);
        save_to_sd_quiet();
        fputs("---- RESULTS ----\n", stdout); fwrite(out.buf, 1, out.len, stdout); fputs("---- END ----\n", stdout);
        for (;;) {}      /* the screen keeps the last progress line; the SD file has everything up to the hang */
    }
    if (out.cap - out.len < 1024) tprintf("# OUTPUT TRUNCATED\n");
    show("MEASURED, %u BYTES OF RESULTS\n", (unsigned)out.len);
    sd_ok = sd_ok > 0 ? 1 : -1;
    if (!save_to_sd())
    {
        /* no SD card (an emulator, say): the results go to the debug output instead */
        fputs("---- RESULTS ----\n", stdout);
        fwrite(out.buf, 1, out.len, stdout);
        fputs("---- END ----\n", stdout);
    }
    show("DONE\n");
    for (;;) {}
}

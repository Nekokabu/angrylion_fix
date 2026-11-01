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

#define VERSION     "22"
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
    /* in-game intro frames of Beetle Adventure Racing (J), dumped from ares with the bar_tasklog ROM running (Sync_Full index
     * = tasklog frame + 2254): light frames (console RDP +6.7% over ares when the CPU is parked) and heavy ones (+2.7%) */
    { "bar_i2274.bin", 0x13012 }, { "bar_i2279.bin", 0x13012 }, { "bar_i2284.bin", 0x13012 },   /* frames 20, 25, 30: light (0.62-0.65 VI in ares) */
    { "bar_i2314.bin", 0x13012 }, { "bar_i2338.bin", 0x13012 },                                 /* frames 60, 84: heavy, console +0.06/+0.09 VI */
    { "bar_i2404.bin", 0x13012 }, { "bar_i2410.bin", 0x13012 },                                 /* frames 150, 156: heavy, console +0.00/+0.01 VI */
    { "bar_2316.bin",  0x13012 },                                                               /* v15's list, for continuity */
    { "dk64_797.bin",  0x13016 },
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
typedef struct { const char *name; uint32_t mask; } variant_t;
/* syncs are never removed (v15 run 1: a BAR list without its Sync_Pipe/Sync_Tile hung the RDP); fill rectangles
 * that clear the depth buffer stay too, or everything drawn afterwards changes its Z-pass behaviour */
static const variant_t variants[] = {
    { "full",          0 },
    { "no_tri",        V_TRI },
    { "no_load",       V_LOAD },
    { "no_rect",       V_RECT },
    { "state_only",    V_TRI | V_RECT | V_LOAD },
    { "tri_only",      V_LOAD | V_RECT },
    { "fetch_only",    V_TRI | V_RECT | V_LOAD | V_SYNC | V_STATE },
};
#define N_VARIANTS ((int)(sizeof variants / sizeof variants[0]))
static int nop_class(int code)
{
    if (code >= 0x08 && code <= 0x0F) return V_TRI;
    if (code == 0x24 || code == 0x25 || code == 0x36) return V_RECT;
    if (code == 0x26 || code == 0x27 || code == 0x28) return V_SYNC;
    if (code == 0x30 || code == 0x33 || code == 0x34) return V_LOAD;
    if (code == 0x29 || code == 0x00) return 0;
    return V_STATE;
}

static int load_list(const char *name, uint32_t variant_mask, uint32_t *first, uint32_t *last, uint32_t *off_out, uint32_t *model, char *info, size_t infolen)
{
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
                pending = cmdlen[code];
                if (code == 0x3D || code == 0x3E || code == 0x3F)
                    w = (w & ~0x3FFFFFFull) | ((w + off) & 0x3FFFFFF);
                if (code == 0x3F) cur_color = w & 0x3FFFFFF;        /* before relocation */
                if (code == 0x3E) cur_mask = w & 0x3FFFFFF;
                int cls = nop_class(code);
                if (cls == V_RECT && code == 0x36 && cur_color == cur_mask) cls = 0;   /* a fill into the depth buffer: keep */
                nopping = prefix_left == 0 && (cls & variant_mask);   /* the prefix (carried-over state) always stays */
                if (prefix_left) prefix_left--;
            }
            pending--;
            *dst++ = nopping ? 0 : w;
        }
        done += n;
    }
    /* depth buffer into its (relocated) place and a copy for the following runs */
    zcopy_addr = zaddr + off; zcopy_bytes = zbytes;
    /* a 640x480 depth buffer (600 KiB) does not fit the copy area near the top: it goes into the gap between the
     * list's own depth and colour buffers (MGC: 0x4EA200-0x600000), which the list does not touch */
    zcopy_cur = zbytes <= 0x30000 ? zcopy : (uint8_t *)(0x80000000u | ZBIG_PHYS);
    if (dfs_read(zcopy_cur, 1, zbytes, fd) != (int)zbytes) { dfs_close(fd); snprintf(info, infolen, "z read failed"); return 0; }
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

static void series_v(uint32_t vi_ctrl_on)
{
    tprintf("# V lines: list, variant, vi mode (game = the game's VI_CTRL with VI_WIDTH 320, vi_off), clock (smallest of %d), ticks (CPU wall time), model of the full list (ares tables only), VI_CTRL read back after the runs, VI_WIDTH, VI probe (ticks of 4096 dependent line fills with the RDP idle, right after the runs: about 93K with the VI off, 100K in the games' AA mode - tells whether the VI really fetched during the runs)\n", RUNS);
    tprintf("# VI lines: list, info (placement in RDRAM)\n");
    tprintf("# variants: NOPs replace a class of commands (every word of the command): sync = Sync_Load/Pipe/Tile, load = Load_TLUT/Block/Tile, tri = 08-0F, rect = 24/25/36, state = the other Set_* (Sync_Full and the carried-over prefix always stay)\n");
    if (!mem_8mb) { tprintf("# V skipped: needs 8 MiB\n"); return; }
    if (dfs_init(DFS_DEFAULT_LOCATION) != DFS_ESUCCESS) { tprintf("# V skipped: no filesystem\n"); return; }
    for (int li = 0; li < N_RLISTS && !rdp_dead; li++)
    {
        for (int vi = 0; vi < N_VARIANTS && !rdp_dead; vi++)
        {
            uint32_t first, last, off, model; char info[160];
            vi_console();     /* the console display needs its VI settings back while progress() renders */
            progress("V  %s  %s", rlists[li].name, variants[vi].name);
            enable_interrupts();
            int ok = load_list(rlists[li].name, variants[vi].mask, &first, &last, &off, &model, info, sizeof info);
            disable_interrupts();
            if (vi == 0) tprintf("VI\t%s\t%s\n", rlists[li].name, info);
            if (!ok) { if (vi == 0) break; continue; }
            /* the game's VI mode; the full list also with the VI off (cross-check against v9/v12) and under the other games' modes */
            static const struct { const char *name; uint32_t ctrl; } fullmodes[] = {
                { "game", 0 }, { "vi_off", 0 }, { "rom_aa3_0x3302", 0x3302 },
            };
            int nmodes = vi == 0 ? 3 : 2;
            for (int m = 0; m < nmodes && !rdp_dead; m++)
            {
                uint32_t ctrl = m == 0 ? rlists[li].vi_ctrl : fullmodes[m].ctrl;
                vi_console();
                progress("V  %s  %s  %s", rlists[li].name, variants[vi].name, fullmodes[m].name);
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
                tprintf("V\t%s\t%s\t%s\t%lu\t%lu\t%lu\t%lX\t%lu\t%lu\n", rlists[li].name, variants[vi].name, fullmodes[m].name,
                        (unsigned long)best, (unsigned long)bticks, (unsigned long)model, (unsigned long)(HW_VI_CTRL & 0xFFFF), (unsigned long)HW_VI_WIDTH, (unsigned long)probe);
            }
            if (rdp_dead) { enable_interrupts(); save_to_sd_quiet(); disable_interrupts(); return; }
            vi_console();
            enable_interrupts(); save_to_sd(); disable_interrupts();
        }
    }
}


/* ======================================================================= */
/* W: narrow triangles - the games' triangles have 2-11 pixels per span, so the cost per span (and per triangle)  */
/*    matters more than the cost per pixel. Shade+texture+Z triangles (22 words) whose spans are all w pixels      */
/*    wide, H spans each, placed at random positions of a 320x240 16-bit buffer, under the game's modes.           */
/* ======================================================================= */

typedef struct { const char *name; uint32_t om_hi, om_lo; int z, cyc; } wmode_t;
#define OM_TEX_2CYC (OM_2CYC | OM_BILERP | OM_BI_LERP01 | OM_NO_DITHER)
#define OM_TEX_1CYC (OM_1CYC | OM_BILERP | OM_BI_LERP01 | OM_NO_DITHER)
#define OM_BLENDL   (OM_FORCE_BLEND | OM_IMAGE_READ | OM_B_M2A_MEM)
#define OM_ZL       (OM_Z_COMPARE | OM_Z_UPDATE)
static const wmode_t wmodes[] = {
    { "2cyc_tex_blend_z", OM_TEX_2CYC, OM_BLENDL | OM_ZL, 1, 2 },   /* BAR's main mode */
    { "2cyc_tex_blend",   OM_TEX_2CYC, OM_BLENDL,         0, 2 },
    { "2cyc_tex_z",       OM_TEX_2CYC, OM_ZL,             1, 2 },
    { "2cyc_tex",         OM_TEX_2CYC, 0,                 0, 2 },
    { "1cyc_tex_blend_z", OM_TEX_1CYC, OM_BLENDL | OM_ZL, 1, 1 },   /* DK64's main mode */
    { "1cyc_tex_z",       OM_TEX_1CYC, OM_ZL,             1, 1 },
};
#define N_WMODES ((int)(sizeof wmodes / sizeof wmodes[0]))
#define W_SPANS 4096

/* a 'triangle' whose three edges are vertical: H spans of exactly w pixels from x0 (the major edge is on the left) */
static void emit_tri_rect(cbuf_t *c, int x0, int y0, int w, int h, uint32_t zint, int tex_step)
{
    uint64_t yl = (uint64_t)((y0 + h) * 4) & 0x3FFF, ym = yl, yh = (uint64_t)(y0 * 4) & 0x3FFF;
    emit(c, CMD(0x0F) | (1ull << 55) | yl << 32 | ym << 16 | yh);
    emit(c, (uint64_t)(uint32_t)((x0 + w) << 16) << 32);   /* XL, DxLDy = 0 */
    emit(c, (uint64_t)(uint32_t)(x0 << 16) << 32);         /* XH, DxHDy = 0 */
    emit(c, (uint64_t)(uint32_t)((x0 + w) << 16) << 32);   /* XM, DxMDy = 0 */
    /* shade: constant colour */
    emit(c, 0x00FF00C000800040ull); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0); emit(c, 0);
    /* texture: S, T from 0, one texel per pixel along the span (DsDx), W = 1 */
    emit(c, (uint64_t)0 << 32 | 0); emit(c, (uint64_t)(tex_step << 16) << 32 | 0);   /* S,T int | DsDx,DtDx int */
    emit(c, (uint64_t)0x00010000 << 32); emit(c, 0);                                 /* W int (fraction words) */
    emit(c, 0); emit(c, (uint64_t)(tex_step << 16)); emit(c, 0); emit(c, 0);           /* DsDe.., DsDy (one texel per line) */
    /* z: constant depth */
    emit(c, (uint64_t)(zint << 16) << 32 | 0); emit(c, 0);
}

static void w_setup(const wmode_t *m)
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
    emit_texture_setup(c);
    emit(c, CMD(0x2F) | (uint64_t)m->om_hi << 32 | m->om_lo);
    emit(c, m->cyc == 2 ? combine_cmd(CC_TEXPRIM, CC_COMBENV) : combine_cmd(CC_TEXPRIM, CC_TEXPRIM));
    sync_full(c);
    timed_out = 0;
    dp_run(c, NULL);
}

/* zcase: 0 = all pass (depth cleared far), 1 = all fail (cleared near), 2 = pairs at the same place, the second one behind the first */
static void series_w(uint32_t vi_ctrl_on)
{
    tprintf("# W lines: vi mode, mode, span width w, spans per triangle H, z case (pass / fail / half: pairs, second behind), triangles, spans, DPC_CLOCK (smallest of %d), ticks, clocks per span, clocks per triangle, VI probe\n", RUNS);
    tprintf("# W: shade+texture+z triangles (22 words) with vertical edges, random positions in a 320x240 16-bit buffer, 1 texel/pixel; %d spans per run\n", W_SPANS);
    static const int widths[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
    static const int heights[] = { 1, 2, 3, 4, 6, 8, 16 };
    static const char *const zcases[] = { "pass", "fail", "half" };
    for (int vi = 0; vi < 2 && !rdp_dead; vi++)
    {
        uint32_t ctrl = vi == 0 ? 0 : 0x13012;
        const char *vname = vi == 0 ? "vi_off" : "bar_0x13012";
        for (int mi = 0; mi < N_WMODES && !rdp_dead; mi++)
        {
            const wmode_t *m = &wmodes[mi];
            for (int hi = 0; hi < (int)(sizeof heights / sizeof heights[0]) && !rdp_dead; hi++)
            {
                int H = heights[hi];
                for (int wi = 0; wi < (int)(sizeof widths / sizeof widths[0]) && !rdp_dead; wi++)
                {
                    int w = widths[wi];
                    /* the full width/height grid only for the two main modes at H 1,2,4,16; the others at H=3 (BAR's typical) and w 2,4,8,32 */
                    int main_mode = mi == 0 || mi == 4;
                    if (!main_mode && !(H == 3 && (w == 2 || w == 4 || w == 8 || w == 32))) continue;
                    if (main_mode && !(H == 1 || H == 2 || H == 3 || H == 4 || H == 16)) continue;
                    int nz = m->z ? 3 : 1;
                    for (int zc = 0; zc < nz && !rdp_dead; zc++)
                    {
                        if (zc > 0 && !(main_mode && (H == 3 || H == 1) && (w == 2 || w == 4 || w == 8 || w == 32))) continue;
                        vi_console();
                        progress("W  %s  %s  w%d H%d %s", vname, m->name, w, H, zcases[zc]);
                        set_vi(ctrl);
                        /* build the list */
                        cbuf_t *c = &cb_body; c->p = c->start;
                        int ntri = W_SPANS / H, spans = ntri * H;
                        uint32_t rnd = 0x1234567u;
                        for (int t = 0; t < ntri; t++)
                        {
                            int x0, y0;
                            if (zc == 2 && (t & 1)) { x0 = ((int *)stage)[0]; y0 = ((int *)stage)[1]; }   /* same place as the previous one */
                            else
                            {
                                rnd = rnd * 1664525u + 1013904223u;
                                x0 = (int)((rnd >> 8) % (uint32_t)(FB_W - w)); y0 = (int)((rnd >> 20) % (uint32_t)(FB_H - H));
                                ((int *)stage)[0] = x0; ((int *)stage)[1] = y0;
                            }
                            uint32_t zint = zc == 2 ? ((t & 1) ? 0x6000 : 0x2000) : 0x4000;
                            emit_tri_rect(c, x0, y0, w, H, zint, 1);
                        }
                        sync_full(c);
                        uint32_t best = 0xFFFFFFFF, bticks = 0;
                        for (int r = 0; r < RUNS && !rdp_dead; r++)
                        {
                            w_setup(m);
                            clear_z(zc == 1 ? 0x0000 : 0xFFFC, FB_H);
                            meas_t m2; timed_out = 0;
                            dp_run(c, &m2);
                            if (timed_out) { best = 0; break; }
                            if (m2.clock < best) { best = m2.clock; bticks = last_ticks; }
                        }
                        uint32_t probe = vi_probe();
                        tprintf("W\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%lu\t%lu\t%.2f\t%.1f\t%lu\n", vname, m->name, w, H, zcases[zc], ntri, spans,
                                (unsigned long)best, (unsigned long)bticks, (double)best / spans, (double)best / ntri, (unsigned long)probe);
                    }
                }
            }
            vi_console();
            enable_interrupts(); save_to_sd(); disable_interrupts();
        }
    }
}

/* ======================================================================= */
/* Q: how DPC_CURRENT advances while a game list runs (the RSP's FIFO wait depends on it)                            */
/* ======================================================================= */
static void series_q(void)
{
    tprintf("# Q lines: list, vi mode, reads of DPC_CURRENT during the list, changes seen, histogram of the step in bytes (<=8, 16, 32, 64, 128, 256, 512, 1024, more), largest step, list bytes, ticks\n");
    if (!mem_8mb) return;
    for (int li = 0; li < 2 && !rdp_dead; li++)
    {
        const char *name = li == 0 ? "bar_2316.bin" : "bar_i2314.bin";
        uint32_t first, last, off, model; char info[160];
        vi_console();
        progress("Q  %s", name);
        enable_interrupts();
        int ok = load_list(name, 0, &first, &last, &off, &model, info, sizeof info);
        disable_interrupts();
        if (!ok) { tprintf("# Q %s: %s\n", name, info); continue; }
        for (int vi = 0; vi < 2 && !rdp_dead; vi++)
        {
            set_vi(vi == 0 ? 0 : 0x13012);
            restore_z();
            uint32_t hist[9] = { 0 }, reads = 0, changes = 0, maxstep = 0;
            timed_out = 0;
            dp_start_at(first, last);
            uint32_t prev = HW_DPC_CURRENT, timeout = 0x1000000;
            while (!(HW_MI_INTR & MI_INTR_DP))
            {
                uint32_t cur = HW_DPC_CURRENT; reads++;
                if (cur != prev)
                {
                    uint32_t d = cur - prev; changes++;
                    if (d > maxstep) maxstep = d;
                    int b = d <= 8 ? 0 : d <= 16 ? 1 : d <= 32 ? 2 : d <= 64 ? 3 : d <= 128 ? 4 : d <= 256 ? 5 : d <= 512 ? 6 : d <= 1024 ? 7 : 8;
                    hist[b]++; prev = cur;
                }
                if (--timeout == 0) { timed_out = 1; break; }
            }
            last_ticks = get_ticks() - start_ticks;
            HW_MI_MODE = MI_CLEAR_DP_INT;
            if (timed_out) { rdp_recover(); break; }
            tprintf("Q\t%s\t%s\t%lu\t%lu\t%lu %lu %lu %lu %lu %lu %lu %lu %lu\t%lu\t%lu\t%lu\n", name, vi == 0 ? "vi_off" : "bar_0x13012",
                    (unsigned long)reads, (unsigned long)changes, (unsigned long)hist[0], (unsigned long)hist[1], (unsigned long)hist[2], (unsigned long)hist[3],
                    (unsigned long)hist[4], (unsigned long)hist[5], (unsigned long)hist[6], (unsigned long)hist[7], (unsigned long)hist[8],
                    (unsigned long)maxstep, (unsigned long)(last - first), (unsigned long)last_ticks);
        }
    }
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
            snprintf(save_path, sizeof save_path, "sd:/rdp_timing/rdp_timing_v22_%03d.txt", n);
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
    if (sd_ok < 0) { if (!debug_init_sdfs("sd:/", -1)) { sd_ok = 0; return 0; } mkdir("sd:/rdp_timing", 0777); struct stat st; for (int n = 1; n < 1000; n++) { snprintf(save_path, sizeof save_path, "sd:/rdp_timing/rdp_timing_v22_%03d.txt", n); if (stat(save_path, &st) != 0) break; } sd_ok = 1; }
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

    enum { SEL_R = 1, SEL_F = 2, SEL_S = 4 };
    int sel = SEL_R;
    const char *sel_name = "V (replay of the in-game intro lists with command classes removed), Q, W";
    joypad_init();
    for (int i = 0; i < 3; i++) { joypad_poll(); wait_ms(20); }
    joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
    (void)held;

    static const char *const tv[] = { "PAL", "NTSC", "MPAL" };
    show("RDP TIMING v" VERSION "  %s  %d MiB\n", tv[get_tv_type() % 3], get_memory_size() >> 20);
    tprintf("# rdp_timing v" VERSION " - replay of in-game BAR intro lists with command classes removed (V), DPC_CURRENT steps (Q), narrow shade+texture+z triangles per span/triangle cost (W)\n");
    tprintf("# mi_version\t0x%08" PRIX32 "\n# tv_type\t%s\n# memory_size\t%d\n# runs_per_case\t%d\n",
            HW_MI_VERSION, tv[get_tv_type() % 3], get_memory_size(), RUNS);
    tprintf("# VI on (320x240 16-bit console display) unless a line says otherwise\n");
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
    tprintf("# series run: %s\n", sel_name);
    checkpoint("init", sel_name);
    if (sel & SEL_R) { series_v(vi_ctrl); checkpoint("V", "Q (DPC_CURRENT steps)"); }
    series_q(); checkpoint("Q", "W (narrow triangles)");
    {
        /* the work buffers for W (above the replayed lists' floor: V and Q are over) */
        arena_size = 0x4B000 + 0x25800 + 0x800;
        arena = malloc_uncached_aligned(2048, arena_size);
        cb_body.start = malloc_uncached_aligned(64, 1024 * 1024);
        if (!arena || !cb_body.start) { enable_interrupts(); show("OUT OF MEMORY\n"); for (;;) {} }
        memset(arena, 0, arena_size);
        fb = (uint16_t *)arena;
        zb = (uint16_t *)(arena + ZB_DEFAULT);
        fb_width = FB_W;
        series_w(vi_ctrl); checkpoint("W", "end");
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

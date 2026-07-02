/* gu_draw.c — GU-state translation + citro2d draw backend.
 *
 * Texture path: the game stores sprites as 4/8-bit indexed pixels with
 * 16-bit 5551 palettes (PSP CLUT). The PICA200 has no palette textures,
 * so each (pixels, palette) pair is expanded once into a native RGB5A1
 * C3D_Tex, Morton-tiled, and cached. Palette content is checksummed so
 * palette animation (character colors, flashes) invalidates correctly.
 */
#include <pspshim.h>
#include "ctr/gu_draw.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void debug_print(const char *fmt, ...);

/* ----------------------------------------------------- bound GU state -- */

typedef struct {
    const void *tex_ptr;
    int tex_w, tex_h, tex_stride;
    int format;   /* GU_PSM_* */
    int swizzled; /* PSP block swizzle */
    const void *clut_ptr;
    int texture_2d_enabled;
} GuState;

static GuState s_gu;

/* ------------------------------------------------------ texture cache -- */

#define TEXCACHE_SIZE 256 /* must exceed a scene's per-frame texture working
                           * set or the cache thrashes (miss every frame).
                           * Detailed animated stages (Remy's clock temple)
                           * exceed ~128 → ~10 textures/frame thrashed → 27fps.
                           * Entries allocate lazily, so the cap only bounds
                           * how many coexist, not RAM used. See pal_reuse note. */

typedef struct {
    const void *tex_ptr;
    const void *clut_ptr;
    uint32_t clut_sum;
    /* Per-entry palette LUT cache. The index->RGBA5551 table depends only on the
     * palette, so for an animated sheet (reconverted every frame with the palette
     * unchanged) we rebuild it only when (clut_ptr, clut_sum) changes rather than
     * on every reconvert. Correct across eviction too: a reused entry whose new
     * palette matches (ptr+sum) can keep the LUT, since the LUT is palette-only. */
    uint16_t lut[256];
    const void *lut_clut;
    uint32_t lut_sum;
    uint32_t data_sum; /* full content checksum — detects CPU-side melt writes */
    uint32_t check_frame;
    /* Dirty TILE list written by melt hooks. Animation tiles scatter across
     * the sheet, so a bounding box covers everything — we track each changed
     * tile and reconvert only those. dirty_n == -1 means whole sheet dirty
     * (overflow or stream-in); 0 means clean. */
#define MAX_DIRTY_TILES 512
    int dirty_n;
    struct { int16_t x, y, w, h; } dirty[MAX_DIRTY_TILES];
    uint32_t drawn_frame;          /* draws are deferred to frame end — never mutate
                                      a texture already referenced this frame */
    int w, h, format, swizzled;
    int valid;
    int tex_alive; /* C3D_Tex allocated (reused across evictions of same dims) */
    uint32_t last_use;
    C3D_Tex tex;
} TexCacheEntry;

static TexCacheEntry s_cache[TEXCACHE_SIZE];
static uint32_t s_use_counter;
static C2D_ImageTint s_tint;

/* fast-path + palette-checksum memo (declared here so a full cache
 * invalidate can reset them — otherwise they keep stale references and
 * cause wrong/missing textures when content is swapped into reused buffers,
 * e.g. the opening's fast image sequence). */
static TexCacheEntry *s_last_resolved;
static const void *s_memo_clut;
static uint32_t s_memo_sum, s_memo_frame = ~0u;

/* Value LUTs for direct 16-bit formats: the PS2->PICA channel swap is a pure
 * function of the pixel value (palette-independent), so a one-time 64Ki-entry
 * table turns per-pixel bit math into a single load. Built lazily on first
 * direct-format conversion. 256KB total — negligible vs the RAM budget. */
static uint16_t *s_lut5551;
static uint16_t *s_lut4444;

void ctrGuInit(void) {
    memset(&s_gu, 0, sizeof(s_gu));
    memset(s_cache, 0, sizeof(s_cache));
    s_gu.texture_2d_enabled = 1;
    C2D_SetTintMode(C2D_TintMult);
}

static uint32_t s_frame_id;

/* --- lightweight profiler (GU_PROFILE) --------------------------------- */
#define GU_PROFILE 0 /* off for release: removes per-quad timing overhead + log spam */
#if GU_PROFILE
static uint32_t prof_frames;
static uint32_t prof_conv, prof_rows, prof_quads, prof_csum, prof_misses;
static uint64_t prof_conv_ticks, prof_csum_ticks;
static uint32_t prof_fmt[8];      /* conversions per GU_PSM_* */
static uint32_t prof_dim_w, prof_dim_h; /* last converted dims */
static uint32_t prof_texsolid, prof_colorspr, prof_colortri; /* draw-path mix */
static uint32_t prof_cfull, prof_ctiles, prof_cpal, prof_ctilecnt; /* conv sources */
static uint64_t prof_frame_ticks, prof_frame_max, prof_last_tick; /* total frame period */
static uint64_t prof_draw_ticks; /* time in C2D draw submission */
static uint64_t prof_conv_prev, prof_conv_fr_max; /* worst single-frame conv (burst) */
#define PROF_ADD(var, n) (prof_##var += (n))
#define PROF_TICK_START() uint64_t _t0 = svcGetSystemTick()
#define PROF_TICK_END(acc) (prof_##acc += svcGetSystemTick() - _t0)
#else
#define PROF_ADD(var, n) ((void)0)
#define PROF_TICK_START() ((void)0)
#define PROF_TICK_END(acc) ((void)0)
#endif

/* --- depth ordering -----------------------------------------------------
 * The game gives every sprite a Z/priority (PS2/PSP depth-tested GU_LEQUAL,
 * small z = front). citro2d depth-tests GEQUAL (higher depth = on top) and
 * clears depth to 0 via C2D_TargetClear. We map the game's z so that the
 * frontmost (smallest z) gets the highest C2D depth. The z range is unknown
 * and scene-dependent, so we self-normalize using the PREVIOUS frame's
 * observed [min,max] (stable frame-to-frame) and accumulate the current
 * frame's range as we draw. Applied uniformly to every path so fades and
 * masks land in their correct layer just like on hardware. */
static float s_zmin, s_zmax;             /* previous frame range (for mapping) */
static float s_zmin_cur = 1e30f, s_zmax_cur = -1e30f; /* this frame, accumulating */
static float s_quad_z;                   /* z for the next ctrGuDrawTexQuad */
static int s_quad_z_set;                 /* 0 => caller is an overlay (draw on top) */

static float map_depth(float z) {
    if (z < s_zmin_cur) s_zmin_cur = z;
    if (z > s_zmax_cur) s_zmax_cur = z;
    float range = s_zmax - s_zmin;
    if (range < 1e-6f)
        return 0.5f; /* unknown/flat range -> rely on submission order */
    float t = (s_zmax - z) / range; /* small z (front) -> ~1.0 */
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* --- blend modes --------------------------------------------------------
 * The game sets GPU blend via flSetRenderState(FLRENDER_ALPHABLENDMODE,v):
 * low nibble = src factor, high nibble = dst factor (PS2/PSP encoding).
 * 0x32 = standard alpha (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) = the common case
 * and what citro2d already does. Effects (clash flash, sparks) use other
 * values — typically additive — which previously rendered as opaque blocks
 * because we ignored the mode. We switch the C3D blend equation on change,
 * flushing C2D's batch first so prior draws keep their blend. */
static unsigned int s_blend_mode = 0x32;
static unsigned int s_blend_op = 0;
static int s_blend_applied = -1; /* 0=alpha, 1=additive, -1=unset */

void ctrGuSetBlendMode(unsigned int mode) {
    s_blend_mode = mode;
#if GU_PROFILE
    static unsigned int seen[16];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == mode) known = 1;
    if (!known && nseen < 16) {
        seen[nseen++] = mode;
        debug_print("blend: ALPHABLENDMODE 0x%x", mode);
    }
#endif
}

void ctrGuSetBlendOp(unsigned int op) { s_blend_op = op; }

static void apply_blend(void) {
    /* 0x32 is standard alpha; treat every other mode as additive (the only
     * other family the game uses for visible fx). Refined per logs if a
     * non-0x32 alpha case turns up. */
    int additive = (s_blend_mode != 0x32);
    if (additive == s_blend_applied)
        return;
    C2D_Flush(); /* emit queued draws under the previous blend */
    if (additive)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA,
                       GPU_ONE);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    s_blend_applied = additive;
}

void ctrGuFrameBegin(void) {
    /* Dynamic sheets (texcash melts) are detected by content checksum at
     * first bind each frame — see resolve_texture.
     * TODO(old-3ds): replace checksums with dirty hooks in the melt path. */
    s_frame_id++;

    /* commit the range observed last frame, reset the accumulator */
    s_zmin = s_zmin_cur;
    s_zmax = s_zmax_cur;
    s_zmin_cur = 1e30f;
    s_zmax_cur = -1e30f;
#if GU_PROFILE
    {
        uint64_t now = svcGetSystemTick();
        if (prof_last_tick) {
            uint64_t dt = now - prof_last_tick;
            prof_frame_ticks += dt;
            if (dt > prof_frame_max) prof_frame_max = dt;
        }
        prof_last_tick = now;
    }
    /* worst single-frame conversion time in the window (catches super-art bursts) */
    { uint64_t cf = prof_conv_ticks - prof_conv_prev; prof_conv_prev = prof_conv_ticks;
      if (cf > prof_conv_fr_max) prof_conv_fr_max = cf; }
    if (++prof_frames >= 60) {
        uint64_t cps = (uint64_t)CPU_TICKS_PER_USEC;
        debug_print("prof/60f: FRAME avg=%lu max=%lu | conv=%lu(%lu us frmax=%lu us) csum=%lu(%lu us) draw=%lu us quads=%lu miss=%lu",
                    (unsigned long)(prof_frame_ticks / cps / 60), (unsigned long)(prof_frame_max / cps),
                    (unsigned long)prof_conv,
                    (unsigned long)(prof_conv_ticks / cps), (unsigned long)(prof_conv_fr_max / cps), (unsigned long)prof_csum,
                    (unsigned long)(prof_csum_ticks / cps), (unsigned long)(prof_draw_ticks / cps),
                    (unsigned long)prof_quads, (unsigned long)prof_misses);
        prof_draw_ticks = 0;
        if (prof_texsolid || prof_colorspr || prof_colortri)
            debug_print("   paths: texAsSolid=%lu colorSpr=%lu colorTri=%lu",
                        (unsigned long)prof_texsolid, (unsigned long)prof_colorspr,
                        (unsigned long)prof_colortri);
        prof_texsolid = prof_colorspr = prof_colortri = 0;
        if (prof_conv)
            debug_print("   convsrc: full=%lu pal=%lu tileconv=%lu (tiles=%lu) miss=%lu",
                        (unsigned long)prof_cfull, (unsigned long)prof_cpal,
                        (unsigned long)prof_ctiles, (unsigned long)prof_ctilecnt,
                        (unsigned long)prof_misses);
        prof_cfull = prof_ctiles = prof_cpal = prof_ctilecnt = 0;
        if (prof_conv)
            debug_print("   fmt T4=%lu T8=%lu 5551=%lu 4444=%lu 8888=%lu other=%lu  last=%lux%lu",
                        (unsigned long)prof_fmt[GU_PSM_T4], (unsigned long)prof_fmt[GU_PSM_T8],
                        (unsigned long)prof_fmt[GU_PSM_5551], (unsigned long)prof_fmt[GU_PSM_4444],
                        (unsigned long)prof_fmt[GU_PSM_8888],
                        (unsigned long)(prof_fmt[0] + prof_fmt[GU_PSM_T16]),
                        (unsigned long)prof_dim_w, (unsigned long)prof_dim_h);
        prof_frames = prof_conv = prof_rows = prof_quads = prof_csum = prof_misses = 0;
        prof_conv_ticks = prof_csum_ticks = 0;
        prof_conv_prev = prof_conv_fr_max = 0;
        prof_frame_ticks = prof_frame_max = 0;
        memset(prof_fmt, 0, sizeof(prof_fmt));
    }
#endif
}

static uint32_t data_checksum(const void *p, size_t bytes) {
    PROF_TICK_START();
    const uint32_t *u = (const uint32_t *)p;
    size_t n = bytes / 4;
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        a ^= u[i];
        b += u[i];
    }
    PROF_ADD(csum, 1);
    PROF_TICK_END(csum_ticks);
    return a ^ (b << 1);
}

static size_t src_bytes(int format, int w, int h) {
    switch (format) {
    case GU_PSM_T4: return (size_t)w * h / 2;
    case GU_PSM_T8: return (size_t)w * h;
    case GU_PSM_8888: return (size_t)w * h * 4;
    default: return (size_t)w * h * 2;
    }
}

void ctrGuTexcacheInvalidate(const void *src) {
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        if (!s_cache[i].valid)
            continue;
        if (src == NULL || s_cache[i].tex_ptr == src || s_cache[i].clut_ptr == src) {
            if (s_cache[i].tex_alive) {
                C3D_TexDelete(&s_cache[i].tex);
                s_cache[i].tex_alive = 0;
            }
            s_cache[i].valid = 0;
        }
    }
    /* reset fast-path + palette memo so they don't reference cleared state */
    s_last_resolved = NULL;
    s_memo_frame = ~0u;
}

static void add_dirty_tile(TexCacheEntry *e, int x, int y, int w, int h) {
    if (e->dirty_n < 0)
        return; /* already whole-sheet dirty */
    if (e->dirty_n >= MAX_DIRTY_TILES) {
        e->dirty_n = -1; /* overflow → fall back to full reconvert */
        return;
    }
    int i = e->dirty_n++;
    e->dirty[i].x = (int16_t)x;
    e->dirty[i].y = (int16_t)y;
    e->dirty[i].w = (int16_t)w;
    e->dirty[i].h = (int16_t)h;
}

/* Per-tile melt notification (ppgRenewDotDataSeqs): a tile of tw x th pixels
 * was written at byte offset `boff` within the sheet. Convert the byte offset
 * to a texel position using the cached sheet's width/format, and expand the
 * dirty rect — so the next bind reconverts just the changed tiles, not the
 * full-width rows spanning them (the big fight-perf win). */
void ctrGuTexcacheNotifyTile(const void *sheet_base, uint32_t boff, int tw, int th) {
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        TexCacheEntry *e = &s_cache[i];
        if (!e->valid || e->tex_ptr != sheet_base)
            continue;
        uint32_t row_bytes = (e->format == GU_PSM_T4) ? (uint32_t)e->w / 2
                             : (e->format == GU_PSM_T8) ? (uint32_t)e->w
                                                        : (uint32_t)e->w * 2;
        if (!row_bytes) continue;
        int y = (int)(boff / row_bytes);
        uint32_t xb = boff % row_bytes;
        int x = (e->format == GU_PSM_T4) ? (int)(xb * 2)
                : (e->format == GU_PSM_T8) ? (int)xb
                                           : (int)(xb / 2);
        add_dirty_tile(e, x, y, tw, th);
    }
}

/* Whole-sheet invalidation (texcash stream-in, ppgRenewTexChunkSeqs). */
void ctrGuTexcacheNotifyWrite(const void *sheet_base, uint32_t start, uint32_t end) {
    (void)start; (void)end;
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        TexCacheEntry *e = &s_cache[i];
        if (e->valid && e->tex_ptr == sheet_base)
            e->dirty_n = -1; /* whole sheet */
    }
}

/* 16 colors for T4, 256 for T8 */
static uint32_t clut_checksum(const uint16_t *pal, int count) {
    uint32_t sum = 2166136261u;
    for (int i = 0; i < count; i++)
        sum = (sum ^ pal[i]) * 16777619u;
    return sum;
}

/* Source is PS2 PSMCT16: R in bits 0-4, G 5-9, B 10-14, A bit 15.
 * Target PICA RGBA5551: R5 G5 B5 A1 from MSB. (No R/B swap — the earlier
 * swap produced inverted colors.) */
static inline uint16_t conv5551(uint16_t c) {
    uint16_t r = c & 0x1F;
    uint16_t g = (c >> 5) & 0x1F;
    uint16_t b = (c >> 10) & 0x1F;
    uint16_t a = (c >> 15) & 1;
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | a);
}

/* Source 4444 (PS2 order, R nibble low) → RGB5A1 with 4->5 bit expand */
static inline uint16_t conv4444(uint16_t c) {
    uint16_t r = c & 0xF;
    uint16_t g = (c >> 4) & 0xF;
    uint16_t b = (c >> 8) & 0xF;
    uint16_t a = (c >> 12) ? 1 : 0;
    r = (r << 1) | (r >> 3);
    g = (g << 1) | (g >> 3);
    b = (b << 1) | (b >> 3);
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | a);
}

/* Direct (non-paletted) 5551 textures — e.g. the title logo — are stored as
 * ARGB1555 (A bit15, R 14-10, G 9-5, B 4-0) byte-swapped relative to the PS2
 * PSMCT16 palette layout. Swap endianness, then read A,R,G,B from the MSB.
 * NOTE: kept separate from conv5551 so the T8/T4 CLUTs (still PSMCT16) are
 * unaffected. */
static inline uint16_t conv5551_direct(uint16_t c) {
    uint16_t a = (c >> 15) & 1;
    uint16_t r = (c >> 10) & 0x1F;
    uint16_t g = (c >> 5) & 0x1F;
    uint16_t b = c & 0x1F;
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | a);
}

static inline uint32_t morton7(uint32_t x, uint32_t y) {
    return ((x & 1)) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

/* undo PSP block swizzle (16-byte x 8-row blocks) */
static void psp_unswizzle(uint8_t *out, const uint8_t *in, uint32_t width_bytes, uint32_t height) {
    uint32_t bx = width_bytes / 16;
    uint32_t by = height / 8;
    const uint8_t *src = in;
    for (uint32_t j = 0; j < by; j++) {
        for (uint32_t i = 0; i < bx; i++) {
            for (uint32_t r = 0; r < 8; r++) {
                memcpy(out + ((j * 8 + r) * width_bytes) + i * 16, src, 16);
                src += 16;
            }
        }
    }
}

/* Morton offsets within an 8x8 tile: off(x,y) = mx[x&7] + my[y&7].
 * Even x and x+1 map to adjacent offsets → 2-texel u32 stores. */
static const uint16_t s_mx[8] = {0, 1, 4, 5, 16, 17, 20, 21};
static const uint16_t s_my[8] = {0, 2, 8, 10, 32, 34, 40, 42};

/* prebuilt per-sheet conversion state, set once before converting tiles */
typedef struct {
    const uint8_t *src;
    uint32_t row_bytes, tiles_w;
    uint16_t *dst;
    int format;
    const uint16_t *pal5551;  /* T8 / fallback */
    const uint32_t *t4lut;    /* T4 pair-LUT */
    const uint16_t *dlut;     /* direct 5551/4444 value LUT */
} ConvCtx;

/* convert one tile-aligned rect using prebuilt ctx (no per-call setup) */
static void convert_rect(const ConvCtx *c, int w, int h, int x0, int y0, int x1, int y1) {
    x0 &= ~7; y0 &= ~7;
    x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x0 >= x1 || y0 >= y1) return;
    PROF_ADD(rows, ((y1 - y0) * (x1 - x0)) / (w ? w : 1));

    /* Inner loop is unrolled by an 8-pixel Morton tile-column: x0/x1/y0/y1 are
     * tile-aligned (mult. of 8), so per column we compute one base index and emit
     * the 4 fixed 2-texel stores at Morton offsets {0,4,16,20}>>1 = {0,2,8,10}.
     * yb and (x>>3)*64 are even, so the >>1 distributes across the terms. */
    uint32_t *dst32 = (uint32_t *)c->dst;
    if (c->format == GU_PSM_T4 && c->t4lut) {
        const uint32_t *lut = c->t4lut;
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = c->src + y * c->row_bytes;
            uint32_t yb = (((uint32_t)y >> 3) * c->tiles_w * 64 + s_my[y & 7]) >> 1;
            for (int x = x0; x < x1; x += 8) {
                uint32_t b = yb + ((uint32_t)x >> 3) * 32;
                const uint8_t *r = row + (x >> 1);
                dst32[b]      = lut[r[0]];
                dst32[b + 2]  = lut[r[1]];
                dst32[b + 8]  = lut[r[2]];
                dst32[b + 10] = lut[r[3]];
            }
        }
    } else if (c->format == GU_PSM_T8 && c->pal5551) {
        const uint16_t *pal = c->pal5551;
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = c->src + y * c->row_bytes;
            uint32_t yb = (((uint32_t)y >> 3) * c->tiles_w * 64 + s_my[y & 7]) >> 1;
            for (int x = x0; x < x1; x += 8) {
                uint32_t b = yb + ((uint32_t)x >> 3) * 32;
                const uint8_t *r = row + x;
                dst32[b]      = (uint32_t)pal[r[0]] | ((uint32_t)pal[r[1]] << 16);
                dst32[b + 2]  = (uint32_t)pal[r[2]] | ((uint32_t)pal[r[3]] << 16);
                dst32[b + 8]  = (uint32_t)pal[r[4]] | ((uint32_t)pal[r[5]] << 16);
                dst32[b + 10] = (uint32_t)pal[r[6]] | ((uint32_t)pal[r[7]] << 16);
            }
        }
    } else if (c->dlut) {
        const uint16_t *dlut = c->dlut;
        for (int y = y0; y < y1; y++) {
            const uint16_t *row16 = (const uint16_t *)(c->src + y * c->row_bytes);
            uint32_t yb = (((uint32_t)y >> 3) * c->tiles_w * 64 + s_my[y & 7]) >> 1;
            for (int x = x0; x < x1; x += 8) {
                uint32_t b = yb + ((uint32_t)x >> 3) * 32;
                const uint16_t *r = row16 + x;
                dst32[b]      = (uint32_t)dlut[r[0]] | ((uint32_t)dlut[r[1]] << 16);
                dst32[b + 2]  = (uint32_t)dlut[r[2]] | ((uint32_t)dlut[r[3]] << 16);
                dst32[b + 8]  = (uint32_t)dlut[r[4]] | ((uint32_t)dlut[r[5]] << 16);
                dst32[b + 10] = (uint32_t)dlut[r[6]] | ((uint32_t)dlut[r[7]] << 16);
            }
        }
    }
}

/* Reconvert e: full sheet, or just its dirty tile list. The palette LUT is
 * built ONCE here, then every tile reuses it — converting per-tile without
 * this would rebuild the 256-entry LUT hundreds of times per frame. */
static int reconvert_entry(TexCacheEntry *e, int full) {
    int w = e->w, h = e->h;
    static uint8_t *swz; /* scratch for the (rare) swizzled path */
    static size_t swz_sz;

#if GU_PROFILE
    prof_fmt[e->format & 7]++;
    prof_dim_w = w; prof_dim_h = h;
#endif

    ConvCtx c;
    c.src = (const uint8_t *)e->tex_ptr;
    c.format = e->format;
    c.dst = (uint16_t *)e->tex.data;
    c.row_bytes = (e->format == GU_PSM_T4) ? (uint32_t)w / 2
                  : (e->format == GU_PSM_T8) ? (uint32_t)w
                                             : (uint32_t)w * 2;
    c.tiles_w = (uint32_t)w >> 3;
    c.pal5551 = NULL;
    c.t4lut = NULL;
    c.dlut = NULL;

    if (e->swizzled) { /* always 0 in this port, but keep correct */
        size_t need = c.row_bytes * h;
        if (need > swz_sz) { free(swz); swz = malloc(need); swz_sz = swz ? need : 0; }
        if (swz) { psp_unswizzle(swz, c.src, c.row_bytes, h); c.src = swz; }
    }

    static uint32_t t4lut[256];
    const uint16_t *pal = (const uint16_t *)e->clut_ptr;
    if ((e->format == GU_PSM_T4 || e->format == GU_PSM_T8) && pal) {
        int pal_count = (e->format == GU_PSM_T4) ? 16 : 256;
        /* Rebuild the index->RGBA LUT only when the palette changed since we last
         * built it for this entry — animated sheets reconvert every frame with an
         * unchanged palette, so this skips ~256 conversions per reconvert. CPS3
         * colorkey: index 0 transparent (keep palette alpha), all others forced
         * opaque so the alpha test doesn't tear glyphs/sprites. (Tried forcing
         * index 0 to hard transparent regardless of source alpha as a fix for
         * the opening-montage tile corruption — didn't fix it and caused a
         * broader color regression, reverted.) */
        if (e->lut_clut != pal || e->lut_sum != e->clut_sum) {
            e->lut[0] = conv5551(pal[0]);
            for (int i = 1; i < pal_count; i++)
                e->lut[i] = conv5551(pal[i]) | 1u;
            e->lut_clut = pal;
            e->lut_sum = e->clut_sum;
        }
        c.pal5551 = e->lut;
        if (e->format == GU_PSM_T4) {
            for (int b = 0; b < 256; b++)
                t4lut[b] = (uint32_t)e->lut[b & 0xF] | ((uint32_t)e->lut[b >> 4] << 16);
            c.t4lut = t4lut;
        }
    } else if (e->format != GU_PSM_T4 && e->format != GU_PSM_T8) {
        if (!s_lut5551 || !s_lut4444) {
            s_lut5551 = (uint16_t *)malloc(65536 * sizeof(uint16_t));
            s_lut4444 = (uint16_t *)malloc(65536 * sizeof(uint16_t));
            if (s_lut5551 && s_lut4444)
                for (int v = 0; v < 65536; v++) {
                    s_lut5551[v] = conv5551_direct((uint16_t)v);
                    s_lut4444[v] = conv4444((uint16_t)v);
                }
        }
        c.dlut = (e->format == GU_PSM_4444) ? s_lut4444 : s_lut5551;
    }

    PROF_TICK_START();
    PROF_ADD(conv, 1);
    if (full || e->dirty_n < 0) {
        PROF_ADD(cfull, 1);
        convert_rect(&c, w, h, 0, 0, w, h);
    } else {
        PROF_ADD(ctiles, 1);
        PROF_ADD(ctilecnt, e->dirty_n);
        for (int t = 0; t < e->dirty_n; t++)
            convert_rect(&c, w, h, e->dirty[t].x, e->dirty[t].y,
                         e->dirty[t].x + e->dirty[t].w, e->dirty[t].y + e->dirty[t].h);
    }
    PROF_TICK_END(conv_ticks);

    C3D_TexFlush(&e->tex);
    return 1;
}

/* Free the C3D_Tex of the least-recently-used cached entry (other than `keep`)
 * that isn't needed by this frame's deferred draws, to reclaim texture memory
 * when C3D_TexInit runs out. Returns 1 if one was freed. */
static int free_lru_texture(TexCacheEntry *keep) {
    TexCacheEntry *victim = NULL;
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        TexCacheEntry *e = &s_cache[i];
        if (e == keep || !e->tex_alive || e->drawn_frame == s_frame_id)
            continue;
        if (!victim || e->last_use < victim->last_use)
            victim = e;
    }
    if (!victim)
        return 0;
    C3D_TexDelete(&victim->tex);
    victim->tex_alive = 0;
    victim->valid = 0; /* slot reusable; re-convert from source if needed again */
    victim->dirty_n = 0;
    return 1;
}

/* allocate (or reuse) + fill a native texture for a cache entry */
static int convert_texture(TexCacheEntry *e) {
    if (!e->tex_alive) {
        /* The 3DS has far less texture memory than this 256-entry cache can ask
         * for when many large (256x256) sheets coexist — the opening montage
         * exhausts it, and failed allocations render whole screens blank. On
         * failure, free LRU textures to reclaim memory and retry instead of
         * giving up (which is what produced the blank/missing opening frames). */
        while (!C3D_TexInit(&e->tex, (u16)e->w, (u16)e->h, GPU_RGBA5551)) {
            if (!free_lru_texture(e)) {
                debug_print("gu_draw: C3D_TexInit %dx%d FAILED (no texture memory)", e->w, e->h);
                return 0;
            }
        }
        C3D_TexSetFilter(&e->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&e->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        e->tex_alive = 1;
    }

    e->data_sum = data_checksum(e->tex_ptr, src_bytes(e->format, e->w, e->h));
    e->check_frame = s_frame_id;
    e->dirty_n = 0; /* clean */

    if (!reconvert_entry(e, 1)) {
        C3D_TexDelete(&e->tex);
        e->tex_alive = 0;
        return 0;
    }
    return 1;
}

/* DIAG toggle: when 1, rebuild the entire sheet from srcRam on every bind,
 * bypassing the per-tile dirty/notify path — bisects per-tile mapping bugs
 * (artifacts gone) vs. melt/source/conversion bugs (artifacts remain). */
#define GU_DIAG_FULL_RECONVERT 0

/* a cache entry matched the bind: reconvert dirty tiles if needed, mark used */
static TexCacheEntry *use_entry(TexCacheEntry *e) {
#if GU_DIAG_FULL_RECONVERT
    reconvert_entry(e, 1);
    e->dirty_n = 0;
    e->check_frame = s_frame_id;
#else
    if (e->dirty_n != 0) {
        reconvert_entry(e, 0); /* full if dirty_n<0, else the tile list */
        e->dirty_n = 0;
        e->check_frame = s_frame_id; /* trust the hook; refresh baseline lazily */
    } else if (s_frame_id - e->check_frame >= 240u + (uint32_t)((e - s_cache) & 0x3F)) {
        /* Rare safety net for writers we haven't hooked. The per-slot phase
         * offset (0..63) staggers these checks so textures created on the same
         * load frame don't all checksum together — that clustering was the
         * periodic ~50ms frame spike. */
        e->check_frame = s_frame_id;
        uint32_t ds = data_checksum(e->tex_ptr, src_bytes(e->format, e->w, e->h));
        if (ds != e->data_sum) {
            e->data_sum = ds;
            reconvert_entry(e, 1);
        }
    }
#endif
    e->last_use = ++s_use_counter;
    e->drawn_frame = s_frame_id;
    return e;
}

static TexCacheEntry *resolve_texture(void) {
    if (!s_gu.tex_ptr)
        return NULL;

    /* Palette checksum is the per-quad hot cost (256 entries for T8). This used
     * to be memoized per (clut_ptr, frame) on the assumption that a palette is
     * stable within a frame. It isn't: the game reuses a single CLUT buffer and
     * reloads it MID-FRAME, so the memo handed back a stale checksum and the next
     * sprite matched an old (often not-yet-loaded, all-black) palette variant
     * already in the cache -> "black boxes throughout the game". Always hash the
     * live palette so a reloaded CLUT is detected immediately and gets its own
     * cache entry. (Memoize only across consecutive identical pointers+contents.) */
    static const void *memo_clut;
    static uint32_t memo_sum;
    static uint16_t memo_fp0, memo_fp1, memo_fp2;
    uint32_t sum;
    if (s_gu.format == GU_PSM_T4 || s_gu.format == GU_PSM_T8) {
        int pal_count = (s_gu.format == GU_PSM_T4) ? 16 : 256;
        const uint16_t *p = (const uint16_t *)s_gu.clut_ptr;
        if (!p) {
            sum = 0;
        } else if (p == memo_clut && p[0] == memo_fp0 && p[pal_count / 2] == memo_fp1 &&
                   p[pal_count - 1] == memo_fp2) {
            /* same pointer AND a spread-out fingerprint still matches -> the
             * buffer hasn't been reloaded since we last hashed it. */
            sum = memo_sum;
        } else {
            sum = clut_checksum(p, pal_count);
            memo_clut = p;
            memo_sum = sum;
            memo_fp0 = p[0];
            memo_fp1 = p[pal_count / 2];
            memo_fp2 = p[pal_count - 1];
        }
    } else {
        sum = 0;
    }

    /* fast path: identical bind to the previous quad (batched sprites) —
     * skip the linear cache scan entirely */
    TexCacheEntry *L = s_last_resolved;
    if (L && L->valid && L->tex_ptr == s_gu.tex_ptr && L->clut_ptr == s_gu.clut_ptr &&
        L->clut_sum == sum && L->w == s_gu.tex_w && L->h == s_gu.tex_h &&
        L->format == s_gu.format && L->swizzled == s_gu.swizzled)
        return use_entry(L);

    TexCacheEntry *lru = &s_cache[0];
    TexCacheEntry *pal_reuse = NULL; /* same pixels, different palette (cycling) */
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        TexCacheEntry *e = &s_cache[i];
        if (e->valid && e->tex_ptr == s_gu.tex_ptr && e->w == s_gu.tex_w && e->h == s_gu.tex_h &&
            e->format == s_gu.format && e->swizzled == s_gu.swizzled &&
            (e->clut_ptr != s_gu.clut_ptr || e->clut_sum != sum) && e->drawn_frame != s_frame_id)
            pal_reuse = e;
        if (e->valid && e->tex_ptr == s_gu.tex_ptr && e->clut_ptr == s_gu.clut_ptr && e->clut_sum == sum &&
            e->w == s_gu.tex_w && e->h == s_gu.tex_h && e->format == s_gu.format && e->swizzled == s_gu.swizzled) {
            s_last_resolved = e;
            return use_entry(e);
        }
        if (!e->valid)
            lru = e;
        else if (lru->valid && e->drawn_frame != s_frame_id &&
                 (lru->drawn_frame == s_frame_id || e->last_use < lru->last_use))
            lru = e;
    }

    /* If the best eviction candidate is still needed by this frame's
     * deferred draws, we can't make a new entry. Only THEN fall back to
     * reconverting a same-sheet palette variant in place. Doing this as a
     * last resort (not preferentially) lets P1/P2 palette variants of the
     * same sheet COEXIST as separate cached entries — otherwise they
     * ping-pong, reconverting hundreds of times per frame. */
    if (lru->valid && lru->drawn_frame == s_frame_id) {
        if (pal_reuse) {
            pal_reuse->clut_ptr = s_gu.clut_ptr;
            pal_reuse->clut_sum = sum;
            pal_reuse->dirty_n = 0;
            PROF_ADD(cpal, 1);
            /* palette changed → whole sheet must re-expand */
            if (!reconvert_entry(pal_reuse, 1)) {
                pal_reuse->valid = 0;
                return NULL;
            }
            pal_reuse->last_use = ++s_use_counter;
            pal_reuse->drawn_frame = s_frame_id;
            s_last_resolved = pal_reuse;
            return pal_reuse;
        }
        return NULL; /* genuinely out of slots this frame */
    }

    /* miss — evict lru, keeping its GPU allocation when dims match */
    PROF_ADD(misses, 1);
    if (lru->valid) {
        lru->valid = 0;
        if (lru->tex_alive && (lru->w != s_gu.tex_w || lru->h != s_gu.tex_h)) {
            C3D_TexDelete(&lru->tex);
            lru->tex_alive = 0;
        }
    }

    lru->tex_ptr = s_gu.tex_ptr;
    lru->clut_ptr = s_gu.clut_ptr;
    lru->clut_sum = sum;
    lru->w = s_gu.tex_w;
    lru->h = s_gu.tex_h;
    lru->format = s_gu.format;
    lru->swizzled = s_gu.swizzled;

    if (!convert_texture(lru))
        return NULL;

    lru->valid = 1;
    lru->last_use = ++s_use_counter;
    lru->drawn_frame = s_frame_id;
    s_last_resolved = lru;
    return lru;
}

/* ------------------------------------------------------------- draws -- */

void ctrGuDrawTexQuad(float x1, float y1, float u1, float v1, float x2, float y2, float u2, float v2,
                      uint32_t color) {
    /* Cull fully off-screen quads before the costly resolve + C2D_DrawImage.
     * The game keeps submitting sprites that have scrolled out of the 400x240
     * top screen; drawing them is wasted work. Coords are screen-space. */
    {
        float xlo = x1 < x2 ? x1 : x2, xhi = x1 < x2 ? x2 : x1;
        float ylo = y1 < y2 ? y1 : y2, yhi = y1 < y2 ? y2 : y1;
        if (xhi <= 0.0f || xlo >= 400.0f || yhi <= 0.0f || ylo >= 240.0f) {
            s_quad_z_set = 0; /* consume the caller's per-quad z like the normal path */
            return;
        }
    }

    TexCacheEntry *e = resolve_texture();
    if (!e)
        return;
    PROF_ADD(quads, 1);
    apply_blend();

    /* GU accepts corner pairs in any order (mirrored sprites swap them);
     * C2D needs a positive-size dest rect, so normalize and mirror UVs. */
    if (x2 < x1) {
        float t = x1; x1 = x2; x2 = t;
        t = u1; u1 = u2; u2 = t;
    }
    if (y2 < y1) {
        float t = y1; y1 = y2; y2 = t;
        t = v1; v1 = v2; v2 = t;
    }

    float w = (float)e->w, h = (float)e->h;

    Tex3DS_SubTexture sub;
    sub.width = (u16)(u2 - u1);
    sub.height = (u16)(v2 - v1);
    sub.left = u1 / w;
    sub.right = u2 / w;
    /* rows stored bottom-up (tex3ds convention) */
    sub.top = 1.0f - v1 / h;
    sub.bottom = 1.0f - v2 / h;

    C2D_Image img = { &e->tex, &sub };

    C2D_DrawParams params;
    memset(&params, 0, sizeof(params));
    params.pos.x = x1;
    params.pos.y = y1;
    params.pos.w = x2 - x1;
    params.pos.h = y2 - y1;
    /* game sprites use their mapped z; direct overlay callers (sprites.c)
     * don't set one and draw on top */
    params.depth = s_quad_z_set ? map_depth(s_quad_z) : 0.95f;
    s_quad_z_set = 0;

    /* Skip tint setup for the common untinted (white) case — pass NULL so C2D
     * draws the image without per-vertex colour modulation. */
    C2D_ImageTint *tintp = NULL;
    if (color != 0xFFFFFFFFu) {
        C2D_PlainImageTint(&s_tint, color, 1.0f);
        tintp = &s_tint;
    }
    /* No per-quad svcGetSystemTick timing here: at ~300 quads/frame the two
     * syscalls per call dwarfed the work being measured. Frame avg/max + the
     * per-batch conv/csum timers give an accurate picture without the skew. */
    C2D_DrawImage(img, &params, tintp);
}

void ctrGuDrawRectSolid(float x, float y, float w, float h, uint32_t color) {
    /* overlay helper (menu/debug) — draw on top */
    apply_blend();
    C2D_DrawRectSolid(x, y, 0.95f, w, h, color);
}

void ctrGuDrawCropBars(float off_x, float off_y) {
    /* The game renders the 384x224 CPS3 field centered in the 400x240 top
     * screen (NATIVE -> 8px margins all round). On real hardware the field was
     * clipped to that window; here there is no scissor, so sprites/background
     * at the field edge bleed into the margins ("artifacts on the sides").
     * Paint the margins opaque black at frontmost depth to reproduce the crop.
     * Only the margin strips are touched — never the play area. */
    const float SW = 400.0f, SH = 240.0f;
    if (off_x <= 0.0f && off_y <= 0.0f)
        return; /* stretch mode: full-bleed, nothing to crop */

    /* force standard alpha so black actually covers (the last fx draw may have
     * left additive blend, under which black would be a no-op) */
    s_blend_mode = 0x32;
    apply_blend();

    const uint32_t black = C2D_Color32(0, 0, 0, 0xFF);
    const float z = 1.0f; /* >= every game layer's mapped depth (GEQUAL) */
    if (off_x > 0.0f) {
        C2D_DrawRectSolid(0.0f, 0.0f, z, off_x, SH, black);       /* left  */
        C2D_DrawRectSolid(SW - off_x, 0.0f, z, off_x, SH, black); /* right */
    }
    if (off_y > 0.0f) {
        C2D_DrawRectSolid(0.0f, 0.0f, z, SW, off_y, black);       /* top    */
        C2D_DrawRectSolid(0.0f, SH - off_y, z, SW, off_y, black); /* bottom */
    }
}

/* Draw a simple lettered button glyph (light disc + dark label) filling the
 * given screen-space rect — replaces the PlayStation button sprites so prompts
 * read as Nintendo controls. label is a short string ("A","B","X","Y","L"...). */
void ctrDrawButtonGlyph(float x0, float y0, float x1, float y1, const char *label) {
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
    float w = x1 - x0, h = y1 - y0;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float r = (w < h ? w : h) * 0.5f * 0.80f; /* margin so the disc sits a bit smaller than its cell */
    if (r < 3.0f) return;
    const float z = 0.95f;

    s_blend_mode = 0x32; /* standard alpha so the disc is opaque */
    apply_blend();

    /* thin white outline so the black disc stays visible on dark backgrounds,
     * flat black face (no gradient), white letter — Nintendo button look */
    C2D_DrawCircleSolid(cx, cy, z, r, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));         /* outline */
    C2D_DrawCircleSolid(cx, cy, z, r * 0.86f, C2D_Color32(0x00, 0x00, 0x00, 0xFF)); /* black face */

    static C2D_TextBuf gbuf;
    if (!gbuf) gbuf = C2D_TextBufNew(32);
    C2D_TextBufClear(gbuf);
    C2D_Text txt;
    C2D_TextParse(&txt, gbuf, label);
    C2D_TextOptimize(&txt);
    float scale = (2.0f * r) / 40.0f;
    float tw = 0.0f, th = 0.0f;
    C2D_TextGetDimensions(&txt, scale, scale, &tw, &th);
    C2D_DrawText(&txt, C2D_WithColor, cx - tw * 0.5f, cy - th * 0.5f, z, scale, scale,
                 C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
}

/* --------------------------------------------- GU entry points (fl.c) -- */

void sceGuTexImage(int mipmap, int width, int height, int tbw, const void *tbp) {
    (void)mipmap;
    s_gu.tex_ptr = tbp;
    s_gu.tex_w = width;
    s_gu.tex_h = height;
    s_gu.tex_stride = tbw;
}

void sceGuTexMode(int tpsm, int maxmips, int a2, int swizzle) {
    (void)maxmips;
    (void)a2;
    s_gu.format = tpsm;
    s_gu.swizzled = swizzle;
}

void sceGuClutMode(unsigned int cpsm, unsigned int shift, unsigned int mask, unsigned int a3) {
    (void)cpsm; (void)shift; (void)mask; (void)a3; /* always 5551 here */
}

void sceGuClutLoad(int num_blocks, const void *cbp) {
    (void)num_blocks;
    s_gu.clut_ptr = cbp;
}

void sceGuEnable(int state) {
    if (state == GU_TEXTURE_2D)
        s_gu.texture_2d_enabled = 1;
}

void sceGuDisable(int state) {
    if (state == GU_TEXTURE_2D)
        s_gu.texture_2d_enabled = 0;
}

void sceGuTexWrap(int u, int v) { (void)u; (void)v; }
void sceGuTexFilter(int min, int mag) { (void)min; (void)mag; }
void sceGuTexFunc(int tfx, int tcc) { (void)tfx; (void)tcc; }

/* Vertex layouts the game actually uses:
 *  - PPGFile.c sprites: GU_SPRITES, {s16 u,v; u32 colour; float x,y,z}
 *    (corner pairs, screen-space coords, texel UVs)
 *  - DC_Ghost.c njdp2d: GU_TRIANGLES, {u32 colour; float x,y,z} */
typedef struct {
    uint32_t colour;
    float x, y, z;
} GuColorVertex;

typedef struct {
    int16_t u, v;
    uint32_t colour;
    float x, y, z;
} GuTexVertex;

#define VTYPE_TEX_SPRITE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define VTYPE_COLOR_TRI (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

void sceGuDrawArray(int prim, int vtype, int count, const void *indices, const void *vertices) {
    (void)indices;
    apply_blend();

    if (prim == GU_SPRITES && vtype == VTYPE_TEX_SPRITE) {
        const GuTexVertex *v = (const GuTexVertex *)vertices;
        if (s_gu.texture_2d_enabled) {
            for (int i = 0; i + 1 < count; i += 2) {
                s_quad_z = v[i + 1].z;
                s_quad_z_set = 1;
                ctrGuDrawTexQuad(v[i].x, v[i].y, (float)v[i].u, (float)v[i].v,
                                 v[i + 1].x, v[i + 1].y, (float)v[i + 1].u, (float)v[i + 1].v,
                                 v[i + 1].colour);
            }
        } else {
            for (int i = 0; i + 1 < count; i += 2) {
                PROF_ADD(texsolid, 1);
                C2D_DrawRectSolid(v[i].x, v[i].y, map_depth(v[i + 1].z), v[i + 1].x - v[i].x,
                                  v[i + 1].y - v[i].y, v[i + 1].colour);
            }
        }
        return;
    }

    if (prim == GU_TRIANGLE_STRIP && vtype == VTYPE_TEX_SPRITE && count == 4) {
        /* ppgWriteQuad: axis-aligned quad as a 4-vertex strip; corners 0 and
         * 3 are the diagonal */
        const GuTexVertex *v = (const GuTexVertex *)vertices;
        s_quad_z = v[3].z;
        s_quad_z_set = 1;
        ctrGuDrawTexQuad(v[0].x, v[0].y, (float)v[0].u, (float)v[0].v,
                         v[3].x, v[3].y, (float)v[3].u, (float)v[3].v, v[3].colour);
        return;
    }

    if (prim == GU_SPRITES && vtype == VTYPE_COLOR_TRI) {
        const GuColorVertex *v = (const GuColorVertex *)vertices;
        for (int i = 0; i + 1 < count; i += 2) {
            PROF_ADD(colorspr, 1);
            C2D_DrawRectSolid(v[i].x, v[i].y, map_depth(v[i + 1].z), v[i + 1].x - v[i].x,
                              v[i + 1].y - v[i].y, v[i + 1].colour);
        }
        return;
    }

    if (prim == GU_TRIANGLES && vtype == VTYPE_COLOR_TRI) {
        const GuColorVertex *v = (const GuColorVertex *)vertices;
        for (int i = 0; i + 2 < count; i += 3) {
            PROF_ADD(colortri, 1);
            C2D_DrawTriangle(v[i].x, v[i].y, v[i].colour,
                             v[i + 1].x, v[i + 1].y, v[i + 1].colour,
                             v[i + 2].x, v[i + 2].y, v[i + 2].colour, map_depth(v[i].z));
        }
        return;
    }

    /* log each unknown combination only once — these calls are hot */
    static int logged_vtype[8];
    static int logged_count;
    for (int i = 0; i < logged_count; i++)
        if (logged_vtype[i] == vtype)
            return;
    if (logged_count < 8)
        logged_vtype[logged_count++] = vtype;
    debug_print("gu_draw: unhandled DrawArray prim=%d vtype=0x%x count=%d", prim, vtype, count);
}

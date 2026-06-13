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

#define TEXCACHE_SIZE 48

typedef struct {
    const void *tex_ptr;
    const void *clut_ptr;
    uint32_t clut_sum;
    uint32_t data_sum; /* full content checksum — detects CPU-side melt writes */
    uint32_t check_frame;
    uint32_t dirty_min, dirty_max; /* byte range written by melt hooks; min>max = clean */
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
#define GU_PROFILE 1
#if GU_PROFILE
static uint32_t prof_frames;
static uint32_t prof_conv, prof_rows, prof_quads, prof_csum, prof_misses;
static uint64_t prof_conv_ticks, prof_csum_ticks;
static uint32_t prof_fmt[8];      /* conversions per GU_PSM_* */
static uint32_t prof_dim_w, prof_dim_h; /* last converted dims */
#define PROF_ADD(var, n) (prof_##var += (n))
#define PROF_TICK_START() uint64_t _t0 = svcGetSystemTick()
#define PROF_TICK_END(acc) (prof_##acc += svcGetSystemTick() - _t0)
#else
#define PROF_ADD(var, n) ((void)0)
#define PROF_TICK_START() ((void)0)
#define PROF_TICK_END(acc) ((void)0)
#endif

void ctrGuFrameBegin(void) {
    /* Dynamic sheets (texcash melts) are detected by content checksum at
     * first bind each frame — see resolve_texture.
     * TODO(old-3ds): replace checksums with dirty hooks in the melt path. */
    s_frame_id++;
#if GU_PROFILE
    if (++prof_frames >= 60) {
        uint64_t cps = (uint64_t)CPU_TICKS_PER_USEC;
        debug_print("prof/60f: conv=%lu rows=%lu (%lu us)  csum=%lu (%lu us)  quads=%lu  miss=%lu",
                    (unsigned long)prof_conv, (unsigned long)prof_rows,
                    (unsigned long)(prof_conv_ticks / cps), (unsigned long)prof_csum,
                    (unsigned long)(prof_csum_ticks / cps), (unsigned long)prof_quads,
                    (unsigned long)prof_misses);
        if (prof_conv)
            debug_print("   fmt T4=%lu T8=%lu 5551=%lu 4444=%lu 8888=%lu other=%lu  last=%lux%lu",
                        (unsigned long)prof_fmt[GU_PSM_T4], (unsigned long)prof_fmt[GU_PSM_T8],
                        (unsigned long)prof_fmt[GU_PSM_5551], (unsigned long)prof_fmt[GU_PSM_4444],
                        (unsigned long)prof_fmt[GU_PSM_8888],
                        (unsigned long)(prof_fmt[0] + prof_fmt[GU_PSM_T16]),
                        (unsigned long)prof_dim_w, (unsigned long)prof_dim_h);
        prof_frames = prof_conv = prof_rows = prof_quads = prof_csum = prof_misses = 0;
        prof_conv_ticks = prof_csum_ticks = 0;
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
        if (src == NULL || s_cache[i].tex_ptr == src) {
            if (s_cache[i].tex_alive) {
                C3D_TexDelete(&s_cache[i].tex);
                s_cache[i].tex_alive = 0;
            }
            s_cache[i].valid = 0;
        }
    }
}

/* called from the melt/tile-streaming writers (ppgRenewDotDataSeqs etc.):
 * record the written byte range so the next bind reconverts only that span */
void ctrGuTexcacheNotifyWrite(const void *sheet_base, uint32_t start, uint32_t end) {
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        TexCacheEntry *e = &s_cache[i];
        if (e->valid && e->tex_ptr == sheet_base) {
            if (e->dirty_min > e->dirty_max) {
                e->dirty_min = start;
                e->dirty_max = end;
            } else {
                if (start < e->dirty_min) e->dirty_min = start;
                if (end > e->dirty_max) e->dirty_max = end;
            }
        }
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

/* fill rows [y0, y1) of an already-allocated C3D_Tex from the source */
static int convert_texture_into_impl(TexCacheEntry *e, int y0, int y1) {
    int w = e->w, h = e->h;

    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;

    PROF_ADD(conv, 1);
    PROF_ADD(rows, y1 - y0);
#if GU_PROFILE
    prof_fmt[e->format & 7]++;
    prof_dim_w = w; prof_dim_h = h;
#endif

    const uint8_t *src = (const uint8_t *)e->tex_ptr;
    uint8_t *linear = NULL;

    /* source row size in bytes */
    uint32_t row_bytes;
    switch (e->format) {
    case GU_PSM_T4: row_bytes = w / 2; break;
    case GU_PSM_T8: row_bytes = w; break;
    default: row_bytes = w * 2; break; /* 5551/4444/5650 direct */
    }

    if (e->swizzled) {
        linear = malloc(row_bytes * h);
        if (!linear) {
            C3D_TexDelete(&e->tex);
            return 0;
        }
        psp_unswizzle(linear, src, row_bytes, h);
        src = linear;
    }

    const uint16_t *pal = (const uint16_t *)e->clut_ptr;
    uint16_t *dst = (uint16_t *)e->tex.data;
    uint16_t pal5551[256];

    int pal_count = (e->format == GU_PSM_T4) ? 16 : 256;
    if (pal) {
        for (int i = 0; i < pal_count; i++)
            pal5551[i] = conv5551(pal[i]);
    }

    /* Precomputed Morton offsets within an 8x8 tile: off(x,y) inside a tile
     * = mx[x&7] + my[y&7]. Hoisting these out of the per-pixel loop replaces
     * two morton7() calls per texel with two array reads. Within a tile,
     * even x and x+1 map to adjacent offsets, enabling 2-texel u32 stores. */
    static const uint16_t mx[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    static const uint16_t my[8] = {0, 2, 8, 10, 32, 34, 40, 42};
    uint32_t tiles_w = (uint32_t)w >> 3;

    if (e->format == GU_PSM_T4 && pal) {
        /* two texels per source byte → one u32 store via pair-LUT */
        uint32_t lut[256];
        for (int b = 0; b < 256; b++)
            lut[b] = (uint32_t)pal5551[b & 0xF] | ((uint32_t)pal5551[b >> 4] << 16);
        uint32_t *dst32 = (uint32_t *)dst;
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = src + y * row_bytes;
            uint32_t ybase = ((uint32_t)y >> 3) * tiles_w * 64 + my[y & 7];
            for (int x = 0; x < w; x += 2) {
                uint32_t off = ybase + ((uint32_t)x >> 3) * 64 + mx[x & 7];
                dst32[off >> 1] = lut[row[x >> 1]];
            }
        }
    } else if (e->format == GU_PSM_T8 && pal) {
        /* two independent bytes → combine into one u32 store */
        uint32_t *dst32 = (uint32_t *)dst;
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = src + y * row_bytes;
            uint32_t ybase = ((uint32_t)y >> 3) * tiles_w * 64 + my[y & 7];
            for (int x = 0; x < w; x += 2) {
                uint32_t off = ybase + ((uint32_t)x >> 3) * 64 + mx[x & 7];
                dst32[off >> 1] = (uint32_t)pal5551[row[x]] | ((uint32_t)pal5551[row[x + 1]] << 16);
            }
        }
    } else {
        /* direct 16-bit formats (5551/4444) — value-LUT + pair store */
        int is4444 = (e->format == GU_PSM_4444);
        if (!s_lut5551 || !s_lut4444) {
            s_lut5551 = (uint16_t *)malloc(65536 * sizeof(uint16_t));
            s_lut4444 = (uint16_t *)malloc(65536 * sizeof(uint16_t));
            if (s_lut5551 && s_lut4444)
                for (int v = 0; v < 65536; v++) {
                    s_lut5551[v] = conv5551((uint16_t)v);
                    s_lut4444[v] = conv4444((uint16_t)v);
                }
        }
        const uint16_t *lut = is4444 ? s_lut4444 : s_lut5551;
        uint32_t *dst32 = (uint32_t *)dst;
        for (int y = y0; y < y1; y++) {
            const uint16_t *row16 = (const uint16_t *)(src + y * row_bytes);
            uint32_t ybase = ((uint32_t)y >> 3) * tiles_w * 64 + my[y & 7];
            if (lut) {
                for (int x = 0; x < w; x += 2) {
                    uint32_t off = ybase + ((uint32_t)x >> 3) * 64 + mx[x & 7];
                    dst32[off >> 1] = (uint32_t)lut[row16[x]] | ((uint32_t)lut[row16[x + 1]] << 16);
                }
            } else { /* alloc failed — slow but correct */
                for (int x = 0; x < w; x++) {
                    uint32_t off = ybase + ((uint32_t)x >> 3) * 64 + mx[x & 7];
                    dst[off] = is4444 ? conv4444(row16[x]) : conv5551(row16[x]);
                }
            }
        }
    }

    free(linear);
    C3D_TexFlush(&e->tex);
    return 1;
}

/* timing wrapper around the (multi-exit) converter */
static int convert_texture_into(TexCacheEntry *e, int y0, int y1) {
    PROF_TICK_START();
    int r = convert_texture_into_impl(e, y0, y1);
    PROF_TICK_END(conv_ticks);
    return r;
}

/* allocate (or reuse) + fill a native texture for a cache entry */
static int convert_texture(TexCacheEntry *e) {
    if (!e->tex_alive) {
        if (!C3D_TexInit(&e->tex, (u16)e->w, (u16)e->h, GPU_RGBA5551)) {
            debug_print("gu_draw: C3D_TexInit %dx%d FAILED", e->w, e->h);
            return 0;
        }
        C3D_TexSetFilter(&e->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&e->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        e->tex_alive = 1;
    }

    e->data_sum = data_checksum(e->tex_ptr, src_bytes(e->format, e->w, e->h));
    e->check_frame = s_frame_id;
    e->dirty_min = 1;
    e->dirty_max = 0;

    if (!convert_texture_into(e, 0, e->h)) {
        C3D_TexDelete(&e->tex);
        e->tex_alive = 0;
        return 0;
    }
    return 1;
}

static TexCacheEntry *resolve_texture(void) {
    if (!s_gu.tex_ptr)
        return NULL;

    int pal_count = (s_gu.format == GU_PSM_T4) ? 16 : 256;
    uint32_t sum = (s_gu.format == GU_PSM_T4 || s_gu.format == GU_PSM_T8)
                       ? (s_gu.clut_ptr ? clut_checksum(s_gu.clut_ptr, pal_count) : 0)
                       : 0;

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
            /* melt hooks report exact byte ranges — reconvert only those
             * rows; the periodic checksum is a safety net for unhooked
             * writers */
            if (e->dirty_min <= e->dirty_max) {
                uint32_t rb = (e->format == GU_PSM_T4) ? (uint32_t)e->w / 2
                              : (e->format == GU_PSM_T8) ? (uint32_t)e->w
                                                         : (uint32_t)e->w * 2;
                int y0 = (int)(e->dirty_min / rb);
                int y1 = (int)(e->dirty_max / rb) + 1;
                e->dirty_min = 1;
                e->dirty_max = 0;
                convert_texture_into(e, y0, y1);
                e->data_sum = data_checksum(e->tex_ptr, src_bytes(e->format, e->w, e->h));
                e->check_frame = s_frame_id;
            } else if (s_frame_id - e->check_frame >= 30) {
                e->check_frame = s_frame_id;
                uint32_t ds = data_checksum(e->tex_ptr, src_bytes(e->format, e->w, e->h));
                if (ds != e->data_sum) {
                    e->data_sum = ds;
                    convert_texture_into(e, 0, e->h);
                }
            }
            e->last_use = ++s_use_counter;
            e->drawn_frame = s_frame_id;
            return e;
        }
        if (!e->valid)
            lru = e;
        else if (lru->valid && e->drawn_frame != s_frame_id &&
                 (lru->drawn_frame == s_frame_id || e->last_use < lru->last_use))
            lru = e;
    }

    /* never evict a texture already referenced by this frame's deferred
     * draws — the GPU reads it at frame end */
    if (lru->valid && lru->drawn_frame == s_frame_id)
        return NULL;

    /* palette-cycling fast path: same pixels under a new palette — reuse
     * the entry and reconvert in place instead of evicting other textures */
    if (pal_reuse) {
        pal_reuse->clut_ptr = s_gu.clut_ptr;
        pal_reuse->clut_sum = sum;
        pal_reuse->dirty_min = 1;
        pal_reuse->dirty_max = 0;
        if (!convert_texture_into(pal_reuse, 0, pal_reuse->h)) {
            pal_reuse->valid = 0;
            return NULL;
        }
        pal_reuse->last_use = ++s_use_counter;
        pal_reuse->drawn_frame = s_frame_id;
        return pal_reuse;
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
    return lru;
}

/* ------------------------------------------------------------- draws -- */

void ctrGuDrawTexQuad(float x1, float y1, float u1, float v1, float x2, float y2, float u2, float v2,
                      uint32_t color) {
    TexCacheEntry *e = resolve_texture();
    if (!e)
        return;
    PROF_ADD(quads, 1);

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
    params.depth = 0.0f;

    C2D_PlainImageTint(&s_tint, color, 1.0f);
    C2D_DrawImage(img, &params, &s_tint);
}

void ctrGuDrawRectSolid(float x, float y, float w, float h, uint32_t color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
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

    if (prim == GU_SPRITES && vtype == VTYPE_TEX_SPRITE) {
        const GuTexVertex *v = (const GuTexVertex *)vertices;
        if (s_gu.texture_2d_enabled) {
            for (int i = 0; i + 1 < count; i += 2) {
                ctrGuDrawTexQuad(v[i].x, v[i].y, (float)v[i].u, (float)v[i].v,
                                 v[i + 1].x, v[i + 1].y, (float)v[i + 1].u, (float)v[i + 1].v,
                                 v[i + 1].colour);
            }
        } else {
            for (int i = 0; i + 1 < count; i += 2) {
                C2D_DrawRectSolid(v[i].x, v[i].y, 0.0f, v[i + 1].x - v[i].x, v[i + 1].y - v[i].y,
                                  v[i + 1].colour);
            }
        }
        return;
    }

    if (prim == GU_TRIANGLE_STRIP && vtype == VTYPE_TEX_SPRITE && count == 4) {
        /* ppgWriteQuad: axis-aligned quad as a 4-vertex strip; corners 0 and
         * 3 are the diagonal */
        const GuTexVertex *v = (const GuTexVertex *)vertices;
        ctrGuDrawTexQuad(v[0].x, v[0].y, (float)v[0].u, (float)v[0].v,
                         v[3].x, v[3].y, (float)v[3].u, (float)v[3].v, v[3].colour);
        return;
    }

    if (prim == GU_SPRITES && vtype == VTYPE_COLOR_TRI) {
        const GuColorVertex *v = (const GuColorVertex *)vertices;
        for (int i = 0; i + 1 < count; i += 2) {
            C2D_DrawRectSolid(v[i].x, v[i].y, 0.0f, v[i + 1].x - v[i].x, v[i + 1].y - v[i].y, v[i + 1].colour);
        }
        return;
    }

    if (prim == GU_TRIANGLES && vtype == VTYPE_COLOR_TRI) {
        const GuColorVertex *v = (const GuColorVertex *)vertices;
        for (int i = 0; i + 2 < count; i += 3) {
            C2D_DrawTriangle(v[i].x, v[i].y, v[i].colour,
                             v[i + 1].x, v[i + 1].y, v[i + 1].colour,
                             v[i + 2].x, v[i + 2].y, v[i + 2].colour, 0.0f);
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

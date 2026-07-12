// GPU-accelerated renderer for 3DS using citro2d/citro3d
// Replaces the CPU software framebuffer with per-sprite GPU rendering.
// Textures are palette-resolved to RGBA, morton-tiled, and cached as C3D_Tex.

#include "ctr/ctr_game_renderer.h"
#include "common.h"
#include "ctr/port_compat.h"
#include "fl.h"


#include "Game/texcash.h"
#include "Game/WORK_SYS.h"
#include "ctr/libgraph.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define RENDER_TASK_MAX 1024
#define CACHE_MAX       512

#define CPS3_WIDTH  384
#define CPS3_HEIGHT 224

// ---------------------------------------------------------------------------
// Utility: next power of two
// ---------------------------------------------------------------------------

static u32 next_pot(u32 v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

// ---------------------------------------------------------------------------
// Color helpers — GPU_RGBA5551 (2 bytes/texel, halves VRAM usage)
// CPS3 natively uses RGB555+1-bit alpha, so no visual quality loss.
// ---------------------------------------------------------------------------

// Texel type — u16 for GPU_RGBA4 (4 bits per channel, 2 bytes/texel)
typedef u16 texel_t;
#define GPU_TEX_FMT GPU_RGBA4

/* Per-frame perf-diagnostic prints + periodic stats sweeps. OFF for release
 * builds (they cost formatting + syscalls on the 268MHz target); build with
 * -DSF3_PERF_LOG=1 to re-enable. Cheap always-on counters stay unconditional. */
#ifndef SF3_PERF_LOG
#define SF3_PERF_LOG 0
#endif
#define TEXEL_BYTES 2

// Tile base address: texel_t stride (128 bytes/tile for 16-bit, 256 for 32-bit)
#define TILE_AT(base, tile_idx) ((texel_t*)((texel_t*)(base) + (tile_idx) * 64))

// 3DS GPU_RGBA4: R4(15:12) G4(11:8) B4(7:4) A4(3:0)
// Any non-zero alpha must stay non-zero (minimum 1) to pass alpha test.
static inline texel_t pack_rgba(u8 r, u8 g, u8 b, u8 a) {
    u8 a4 = a >> 4;
    if (a && !a4) a4 = 1;  // Preserve non-zero alpha
    return ((texel_t)(r >> 4) << 12) | ((texel_t)(g >> 4) << 8) |
           ((texel_t)(b >> 4) << 4)  | (texel_t)a4;
}

// Convert PS2 16-bit color (ABGR1555) to GPU_RGBA4
static texel_t color16_to_rgba(u16 p) {
    // PS2 PSMCT16: R(4:0) G(9:5) B(14:10) A(15)
    u8 r = (u8)(( p        & 0x1F) * 255 / 31);
    u8 g = (u8)(((p >>  5) & 0x1F) * 255 / 31);
    u8 b = (u8)(((p >> 10) & 0x1F) * 255 / 31);
    // CPS3: all colours are opaque. Transparency is per palette INDEX (index 0),
    // not per colour value. Black (0x0000) is a valid opaque colour.
    u8 a = 255;
    return pack_rgba(r, g, b, a);
}

// Convert a "direct" (non-palette) 16-bit color to GPU_RGBA4. Direct textures
// (e.g. the title/attract-mode logo art, GU_PSM_5551) store ARGB1555 —
// A bit15, R 14:10, G 9:5, B 4:0 — which is bit-swapped relative to PSMCT16
// CLUT entries (color16_to_rgba, R at bit0). Using the CLUT layout here
// scrambles the channels (washed-out/monochrome look). Preserves the real
// per-pixel alpha bit rather than forcing opaque, matching direct-texture
// semantics (no palette index to carry transparency instead).
static texel_t color16_direct_to_rgba(u16 p) {
    u8 a = (u8)((p >> 15) & 1);
    u8 r = (u8)(((p >> 10) & 0x1F) * 255 / 31);
    u8 g = (u8)(((p >>  5) & 0x1F) * 255 / 31);
    u8 b = (u8)(( p        & 0x1F) * 255 / 31);
    return pack_rgba(r, g, b, a ? 255 : 0);
}

// Convert PS2 32-bit color (BGRA8888 in memory) to GPU_RGBA4
static texel_t color32_to_rgba(u32 p) {
    u8 r = (u8)((p >> 16) & 0xFF);
    u8 g = (u8)((p >>  8) & 0xFF);
    u8 b = (u8)( p        & 0xFF);
    u8 a = (u8)((p >> 24) & 0xFF);
    return pack_rgba(r, g, b, a);
}

// PS2 CLUT index shuffle: swap bits 3 and 4 for 256-color palettes
#define clut_shuf(x) (((x) & ~0x18) | ((((x) & 0x08) << 1) | (((x) & 0x10) >> 1)))

// ---------------------------------------------------------------------------
// Morton tiling — pre-computed LUT for zero per-pixel computation
// ---------------------------------------------------------------------------

// Pre-computed morton offsets for all 64 positions within an 8x8 tile
static u8 morton_lut[8][8]; // morton_lut[y][x] = offset within tile (0-63)
static bool morton_lut_ready = false;

static void init_morton_lut(void) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            u32 i = (x & 7) | ((y & 7) << 8);
            i = (i ^ (i << 2)) & 0x1313;
            i = (i ^ (i << 1)) & 0x1515;
            i = (i | (i >> 7)) & 0x3F;
            morton_lut[y][x] = (u8)i;
        }
    }
    morton_lut_ready = true;
}

// Pre-computed row-major morton offsets: morton_row[y] has 8 offsets for x=0..7
// Indexed as morton_row[flipped_y][x] where flipped_y = 7 - source_y_within_tile
static u8 morton_row[8][8];

static void init_morton_rows(void) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            morton_row[y][x] = morton_lut[y][x];
        }
    }
}

// Fast morton-tile one 8x8 block: PSMT8 palette-indexed source → GPU RGBA.
// Caller guarantees tile is fully within source bounds (no per-pixel bounds check).
static inline void morton_tile_block_psmt8_full(
    texel_t* __restrict__ dst,
    const u8* __restrict__ src_row,  // pointer to first row of this tile
    int src_stride,
    const texel_t* __restrict__ palette)
{
    for (int fy = 0; fy < 8; fy++) {
        const u8* row = src_row + fy * src_stride;
        const u8* m = morton_row[7 - fy]; // flipped y
        dst[m[0]] = palette[row[0]];
        dst[m[1]] = palette[row[1]];
        dst[m[2]] = palette[row[2]];
        dst[m[3]] = palette[row[3]];
        dst[m[4]] = palette[row[4]];
        dst[m[5]] = palette[row[5]];
        dst[m[6]] = palette[row[6]];
        dst[m[7]] = palette[row[7]];
    }
}

// Slow path: handles edge tiles that may be partially outside source bounds
static inline void morton_tile_block_psmt8_edge(
    texel_t* dst, const u8* src, int src_stride, int src_x, int src_y,
    int src_w, int src_h, const texel_t* palette)
{
    for (int fy = 0; fy < 8; fy++) {
        int sy = src_y + fy;
        const u8* m = morton_row[7 - fy];
        if (sy < 0 || sy >= src_h) {
            dst[m[0]] = 0; dst[m[1]] = 0; dst[m[2]] = 0; dst[m[3]] = 0;
            dst[m[4]] = 0; dst[m[5]] = 0; dst[m[6]] = 0; dst[m[7]] = 0;
            continue;
        }
        const u8* row = src + sy * src_stride + src_x;
        int avail = src_w - src_x; // pixels available in this row
        for (int fx = 0; fx < 8; fx++) {
            dst[m[fx]] = (fx < avail) ? palette[row[fx]] : 0;
        }
    }
}

// Legacy wrapper for cache_update_entry_region which still uses old signature
static inline void morton_tile_block_psmt8(
    texel_t* dst, const u8* src, int src_stride, int src_x, int src_y,
    int src_w, int src_h, const texel_t* palette, int pal_n,
    int flip_dy_base)
{
    // Check if tile is fully within bounds — use fast path
    if (src_x >= 0 && src_y >= 0 &&
        src_x + 8 <= src_w && src_y + 8 <= src_h) {
        morton_tile_block_psmt8_full(dst, src + src_y * src_stride + src_x,
                                      src_stride, palette);
    } else {
        morton_tile_block_psmt8_edge(dst, src, src_stride, src_x, src_y,
                                      src_w, src_h, palette);
    }
}

static u32 hash_bytes_fnv1a(const void* data, size_t size) {
    const u8* p = (const u8*)data;
    u32 hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

/* This tree's fl layer stores PSP GU_PSM_* texture formats; the renderer
 * logic compares PS2 GS constants. Translate at the capture boundary.
 * (Palette formats 0/1/2 happen to coincide with PSMCT32/24/16 already.) */
static u32 gu_to_gs_format(u32 f) {
    switch (f) {
    case 5: return SCE_GS_PSMT8;   /* GU_PSM_T8 */
    case 4: return SCE_GS_PSMT4;   /* GU_PSM_T4 */
    case 1: return SCE_GS_PSMCT16; /* GU_PSM_5551 */
    case 2: return SCE_GS_PSMCT16; /* GU_PSM_4444 (16-bit direct; bit layout differs — rare) */
    case 3: return SCE_GS_PSMCT32; /* GU_PSM_8888 */
    default: return f;
    }
}

static size_t texture_byte_size_for_format(int w, int h, int fmt) {
    switch (fmt) {
    case SCE_GS_PSMT8:
        return (size_t)w * (size_t)h;
    case SCE_GS_PSMT4:
        return ((size_t)w * (size_t)h) >> 1;
    case SCE_GS_PSMCT16:
        return (size_t)w * (size_t)h * sizeof(u16);
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Source texture / palette descriptors (raw game data)
// ---------------------------------------------------------------------------

/* Bitmap tracking which 8×8 tiles were recently written by region updates.
   Max 32×32 = 1024 tiles per 256×256 texture = 128 bytes per bitmap. */
#define TILE_DIRTY_WORDS 32  /* 32 × 32 bits = 1024 tiles */

typedef struct {
    int w, h, fmt;
    const void* pixels;
    size_t byte_size;
    u32 checksum;
    bool valid;
    u32 tile_dirty[TILE_DIRTY_WORDS]; /* bitmask of recently-written tiles */
    bool has_region_updates;          /* true if any tile_dirty bit set */
    bool is_placeholder;              /* true if pixels currently points at the
                                          mode=0 all-zero wkVram buffer rather
                                          than real mem_handle/lock_ptr content */
} SrcTexture;

typedef struct {
    int count;
    texel_t colors[256];  // Pre-resolved to GPU_RGBA5551
    u32 checksum;
    bool valid;
} SrcPalette;

static SrcTexture src_textures[FL_TEXTURE_MAX];
static SrcPalette src_palettes[FL_PALETTE_MAX];

// Version counters — incremented on every Create/Unlock
u32 texture_versions[FL_TEXTURE_MAX];
static u32 palette_versions[FL_PALETTE_MAX];

// Currently bound texture/palette for subsequent draw calls
static int current_texture_index = -1;
static int current_palette_index = -1;
// current_cache_entry declared after CacheEntry typedef below

// ---------------------------------------------------------------------------
// GPU texture cache
// ---------------------------------------------------------------------------

typedef struct {
    int texture_index;
    int palette_index;
    u32 tex_version;
    u32 pal_version;
    int pool_slot;
    u32 pot_w;
    u32 pot_h;
    int src_w;
    int src_h;
    bool allocated;
    bool dirty;
    bool pinned;
    bool pending_delete;
    bool src_empty_at_build; /* source was (sampled-)all-zero when this entry
                                was built. Some game paths create a texture
                                handle and only decode its pixels a few frames
                                LATER, silently (no unlock/region notify) —
                                e.g. the char/stage-select background sheets.
                                cache_find_clean rechecks flagged entries and
                                forces one rebuild when content appears. */
    u32 last_used_frame;
    int atlas_cell;      // >=0 if stored in tile atlas, -1 if using pool
} CacheEntry;

/* Pool slots no longer own a C3D_Tex each: slots of the same bucket live as
 * horizontal CELLS inside shared single-row STRIP textures (the same trick
 * the atlas uses). Quads whose entries share a strip batch into one
 * C3D_TexBind + C3D_DrawElements instead of one draw call per slot — the
 * attract/opening montage was measured at 70-210 draw calls/frame almost
 * entirely from per-slot pool textures. Strip height == slot pot_h, so pool
 * Y-flip math is unchanged; only X gains a cell offset. */
#define POOL_STRIP_MAX 64
typedef struct {
    C3D_Tex tex;   /* strip_w x pot_h, strip_w = smallest POT covering cells */
    u32 strip_w;
    u32 pot_h;
    bool initialized;
    bool dirty;    /* texels written, C3D_TexFlush pending (deferred) */
} PoolStrip;
static PoolStrip pool_strips[POOL_STRIP_MAX];
static int pool_strip_count = 0;

typedef struct {
    int strip;     /* index into pool_strips[] */
    int cell;      /* horizontal cell index within the strip */
    u32 pot_w;
    u32 pot_h;
    bool initialized;
    bool in_use;
} TexturePoolSlot;

typedef struct {
    u32 pot_w;
    u32 pot_h;
    int count;
} TexturePoolBucketDesc;

static const TexturePoolBucketDesc texture_pool_buckets[] = {
    { 8,   8,   4 },
    { 16,  16,  8 },
    { 32,  32,  24 },
    { 64,  64,  40 },
    { 128, 64,  12 },
    { 64,  128, 12 },
    { 128, 128, 24 },
    { 256, 128, 16 },
    { 128, 256, 12 },
    /* 256x256 is the atlas cell size, so it's also the size every background
     * chip that overflows the atlas (bg.c forces gpu_cache_prefer_pool=1)
     * lands in. A detailed/parallaxed stage can have more distinct 256x256
     * chips alive in a frame than the pool had slots for; once exhausted,
     * cache_create() has nothing evictable (every slot already touched this
     * frame) and returns NULL, which drops that chip's quad entirely —
     * small holes, biased toward whichever chips are requested last in the
     * per-frame draw order. Bumped 72->104 to give stages more headroom. */
    { 256, 256, 104 },
    { 512, 256, 6 },
    { 256, 512, 8 },
    { 512, 512, 4 },
};

/* Sum of per-bucket strip capacities (strips round cell counts up to the
 * next power-of-two strip width, so a few buckets gained free headroom
 * slots): 4+8+32+40+12+16+24+16+12+104+6+8+4 = 286. Must match what the
 * init loop actually creates — it fills slots up to this hard bound. */
#define TEXTURE_POOL_SLOT_COUNT 286

static CacheEntry gpu_cache[CACHE_MAX];
static TexturePoolSlot texture_pool[TEXTURE_POOL_SLOT_COUNT];
static u32 frame_number = 0;
static CacheEntry* current_cache_entry = NULL;
static int cache_fail_invalid = 0;
static int cache_fail_too_big = 0;
static int cache_fail_texinit = 0;
static int cache_fail_noslot = 0;

/* PORT DIAG accessors. TEMP. */
int cache_fail_invalid_get(void) { return cache_fail_invalid; }
int cache_fail_too_big_get(void) { return cache_fail_too_big; }
int cache_fail_texinit_get(void) { return cache_fail_texinit; }
int cache_fail_noslot_get(void) { return cache_fail_noslot; }
static u32 settex_calls = 0, settex_miss = 0, settex_create = 0;
static u32 settex_fail = 0, settex_nopal = 0;
static u32 cache_pool_evictions = 0;
static u32 cache_pool_evictions_16 = 0;
static u32 cache_pool_evictions_32 = 0;
static u32 cache_pool_evictions_64 = 0;
static u32 cache_pool_evictions_128x64 = 0;
static u32 cache_pool_evictions_64x128 = 0;
static u32 cache_pool_evictions_128x128 = 0;
static u32 cache_pool_evictions_256x128 = 0;
static u32 cache_pool_evictions_128x256 = 0;
static u32 cache_pool_evictions_256x256 = 0;
static u32 cache_pool_evictions_512x256 = 0;
static u32 cache_pool_evictions_256x512 = 0;
static u32 cache_pool_evictions_512x512 = 0;
static u32 cache_pool_evictions_other = 0;
// Forward declarations for atlas code
static void cache_hash_remove(int tex_idx, int pal_idx);
static texel_t resolve_texel(const SrcTexture* src, const SrcPalette* pal, int x, int y);
static void queue_pending_texture(int ti, int pi);

// ---------------------------------------------------------------------------
// Tile Atlas: single 1024x1024 GPU texture for draw call batching
// All entries with pot_w/pot_h <= 256 go here. 4x4 grid of 256x256 cells.
// Result: all atlas-backed quads share one C3D_Tex → 1 draw call.
// ---------------------------------------------------------------------------

// Shared atlas: single-row C3D_Tex strips (1024×256 each).
// Multi-row textures break on Citra, so we use multiple single-row strips.
// Each strip holds 4 cells. All quads on the same strip = 1 draw call.
#define ATLAS_CELL_SIZE 256
#define ATLAS_CELLS_PER_STRIP 4
#define ATLAS_MAX_STRIPS 6  // 24 cells. Fights hold more live (tex,pal)
                            // combos than 16 cells: aev (evictions) ticked up
                            // every second mid-fight, and every eviction is a
                            // future SYNCHRONOUS full rebuild right when an
                            // animation binds its sheet — i.e. a stutter.
                            // +1MB linear for the two extra strips.
#define ATLAS_MAX_CELL_COUNT (ATLAS_CELLS_PER_STRIP * ATLAS_MAX_STRIPS)

typedef struct {
    C3D_Tex strip_tex[ATLAS_MAX_STRIPS];  // each strip is one 1024×256 texture
    int strip_count;                       // how many strips allocated
    bool cell_init[ATLAS_MAX_CELL_COUNT];
    int cell_owner[ATLAS_MAX_CELL_COUNT];
    u32 cell_last_used[ATLAS_MAX_CELL_COUNT];
    bool initialized;
} TileAtlas;

static TileAtlas atlas;
static int atlas_cell_count = 0;  // total cells across all strips
static bool atlas_dirty = false;
/* Which strips have CPU-written texels awaiting a cache flush. Every atlas
 * write path (full build, palette re-resolve, region update, direct tile
 * upload, zero) marks its strip here instead of calling C3D_TexFlush eagerly;
 * the flush consumers (RenderFrame / ProcessPending*) flush each pending
 * strip exactly ONCE. Before this, a fight frame with several palette
 * re-resolves paid a full 512KB strip flush PER resolve, and the consumers
 * flushed per-CELL (4x redundant per strip). */
static u8 atlas_strip_flush_pending = 0;
static inline void atlas_mark_strip_dirty(int strip) {
    atlas_strip_flush_pending |= (u8)(1u << strip);
    atlas_dirty = true;
}

/* Pool-strip addressing helpers (see PoolStrip). All pool texel writes go
 * through these so the strip stride + cell X-offset stay in one place. */
static inline C3D_Tex* pool_slot_tex(const TexturePoolSlot* s) {
    return &pool_strips[s->strip].tex;
}
static inline texel_t* pool_slot_data(const TexturePoolSlot* s) {
    return (texel_t*)pool_strips[s->strip].tex.data;
}
static inline int pool_slot_strip_tiles_x(const TexturePoolSlot* s) {
    return (int)(pool_strips[s->strip].strip_w >> 3);
}
static inline int pool_slot_base_tx(const TexturePoolSlot* s) {
    return s->cell * (int)(s->pot_w >> 3);
}
static inline void pool_mark_slot_dirty(const TexturePoolSlot* s) {
    pool_strips[s->strip].dirty = true;
    atlas_dirty = true; /* shared "some strip needs flushing" gate */
}
/* Zero the slot's own cell within its strip (replaces whole-texture memset). */
static void pool_clear_slot(const TexturePoolSlot* s) {
    texel_t* base = pool_slot_data(s);
    int stx = pool_slot_strip_tiles_x(s);
    int btx = pool_slot_base_tx(s);
    int tw = (int)(s->pot_w >> 3), th = (int)(s->pot_h >> 3);
    for (int ty = 0; ty < th; ty++)
        memset(&base[(ty * stx + btx) * 64], 0, (size_t)tw * 64 * sizeof(texel_t));
}

static void atlas_flush_pending_strips(void) {
    if (!atlas_dirty) return;
    for (int s = 0; s < atlas.strip_count; s++) {
        if (atlas_strip_flush_pending & (1u << s))
            C3D_TexFlush(&atlas.strip_tex[s]);
    }
    atlas_strip_flush_pending = 0;
    for (int i = 0; i < pool_strip_count; i++) {
        if (pool_strips[i].dirty) {
            C3D_TexFlush(&pool_strips[i].tex);
            pool_strips[i].dirty = false;
        }
    }
    atlas_dirty = false;
}
/* L8 index cache for atlas: cached morton-tiled 8-bit indices per cell.
   On palette change, re-resolve palette from indices (skip full rebuild). */
static u8* atlas_l8[ATLAS_MAX_CELL_COUNT]; /* 64KB each, heap-allocated */
static bool atlas_l8_valid[ATLAS_MAX_CELL_COUNT];
static u32 atlas_builds_total = 0;
static u32 atlas_builds_period = 0;
static u32 atlas_evictions_total = 0;
static u32 atlas_try_count = 0;
static u32 atlas_hit_count = 0;
static u32 atlas_skip_big = 0;
static u32 atlas_skip_noinit = 0;

static void atlas_init(void) {
    memset(&atlas, 0, sizeof(atlas));
    atlas_cell_count = 0;
    atlas.strip_count = 0;

    /* Allocate single-row strips (1024×256 each, 4 cells per strip).
       Multi-row textures break on Citra so each strip is strictly 1 row. */
    int strip_w = ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE; // 1024
    int strip_h = ATLAS_CELL_SIZE; // 256

    for (int s = 0; s < ATLAS_MAX_STRIPS; s++) {
        if (!C3D_TexInit(&atlas.strip_tex[s], (u16)strip_w, (u16)strip_h, GPU_TEX_FMT)) {
            break; // out of VRAM
        }
        C3D_TexSetFilter(&atlas.strip_tex[s], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&atlas.strip_tex[s], GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
        memset(atlas.strip_tex[s].data, 0, (size_t)strip_w * strip_h * TEXEL_BYTES);
        C3D_TexFlush(&atlas.strip_tex[s]);
        atlas.strip_count++;

        for (int c = 0; c < ATLAS_CELLS_PER_STRIP; c++) {
            int cell = s * ATLAS_CELLS_PER_STRIP + c;
            atlas.cell_init[cell] = true;
            atlas.cell_owner[cell] = -1;
            atlas.cell_last_used[cell] = 0;
        }
        atlas_cell_count += ATLAS_CELLS_PER_STRIP;
    }

    atlas.initialized = (atlas.strip_count > 0);
    /* Allocate L8 index cache for palette re-resolve */
    for (int c = 0; c < atlas_cell_count; c++) {
        atlas_l8[c] = (u8*)malloc(ATLAS_CELL_SIZE * ATLAS_CELL_SIZE);
        atlas_l8_valid[c] = false;
    }
    printf("atlas: %d strips × %d cells = %d total (%dx%d each)\n",
           atlas.strip_count, ATLAS_CELLS_PER_STRIP, atlas_cell_count,
           strip_w, strip_h);
}

/* Get the strip index and cell-within-strip for a given cell */
static inline int atlas_cell_strip(int cell) { return cell / ATLAS_CELLS_PER_STRIP; }
static inline int atlas_cell_in_strip(int cell) { return cell % ATLAS_CELLS_PER_STRIP; }

/* Get the C3D_Tex for a cell */
static inline C3D_Tex* atlas_cell_tex(int cell) {
    return &atlas.strip_tex[atlas_cell_strip(cell)];
}

/* Get pointer to an 8×8 tile within a cell's strip texture.
   All strips are single-row (1024×256), so cell_in_strip gives the X offset. */
static inline texel_t* atlas_tile_ptr(int cell, int local_tx, int local_ty) {
    int strip = atlas_cell_strip(cell);
    int cell_in = atlas_cell_in_strip(cell);
    int strip_w = ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE; // 1024
    int strip_tiles_x = strip_w / 8; // 128
    int global_tx = cell_in * (ATLAS_CELL_SIZE / 8) + local_tx;
    int global_ty = local_ty; // single row — no Y offset
    return &((texel_t*)atlas.strip_tex[strip].data)[(global_ty * strip_tiles_x + global_tx) * 64];
}

/* Get the UV offset for a cell within its strip texture */
static inline void atlas_cell_uv_offset(int cell, float* u_off, float* v_off) {
    int strip_w = ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE;
    *u_off = (float)atlas_cell_in_strip(cell) * ATLAS_CELL_SIZE / (float)strip_w;
    *v_off = 0.0f; // single row — no V offset
}

// Free an atlas cell (called when cache entry is evicted/invalidated)
static void atlas_free_cell(int cell) {
    if (cell >= 0 && cell < atlas_cell_count) {
        atlas.cell_owner[cell] = -1;
        /* The L8 index cache belongs to the departing tenant's content —
         * without this, a new tenant whose build path doesn't refresh L8
         * (non-PSMT8 content) would have its first palette change repaint
         * the cell from the OLD tenant's indices. */
        atlas_l8_valid[cell] = false;
    }
}

// Allocate an atlas cell for cache_idx. LRU evicts if full. Returns cell index or -1.
static int atlas_alloc_cell(int cache_idx) {
    if (!atlas.initialized) return -1;

    // Find free cell
    for (int i = 0; i < atlas_cell_count; i++) {
        if (atlas.cell_owner[i] < 0) {
            atlas.cell_owner[i] = cache_idx;
            atlas.cell_last_used[i] = frame_number;
            atlas_l8_valid[i] = false; /* new tenant — old indices are garbage */
            return i;
        }
    }

    // No free cell — LRU evict ONLY if oldest cell is stale (60+ frames unused).
    // Prevents thrashing while allowing scene transitions to reclaim cells.
    u32 oldest = UINT32_MAX;
    int oldest_cell = -1;
    for (int i = 0; i < atlas_cell_count; i++) {
        int owner = atlas.cell_owner[i];
        if (owner >= 0 && owner < CACHE_MAX && !gpu_cache[owner].pinned) {
            if (atlas.cell_last_used[i] < oldest) {
                oldest = atlas.cell_last_used[i];
                oldest_cell = i;
            }
        }
    }

    if (oldest_cell >= 0 && frame_number - oldest >= 60) { /* Reverted to original —
        the real fix was routing background chips to the pool path (see
        bgDrawOneChip), not atlas eviction timing. */
        int old_owner = atlas.cell_owner[oldest_cell];
        if (old_owner >= 0 && old_owner < CACHE_MAX) {
            CacheEntry* old_entry = &gpu_cache[old_owner];
            cache_hash_remove(old_entry->texture_index, old_entry->palette_index);
            old_entry->allocated = false;
            old_entry->atlas_cell = -1;
            old_entry->pool_slot = -1;
        }
        atlas_evictions_total++;
        atlas.cell_owner[oldest_cell] = cache_idx;
        atlas.cell_last_used[oldest_cell] = frame_number;
        atlas_l8_valid[oldest_cell] = false; /* new tenant — old indices are garbage */
        return oldest_cell;
    }

    return -1; // All cells recently used — fall through to pool
}

// Forward declarations for GX build
static void atlas_build_cell(int cell, const SrcTexture* src, const SrcPalette* pal,
                              u32 pot_w, u32 pot_h);
/* Staging buffer — defined and allocated later, declared here for GX build */
#define STAGING_MAX (512 * 512)
static texel_t* staging_buf = NULL;

// GX-accelerated build: CPU writes linear palette-resolved data,
// GPU hardware does morton tiling via GX_DisplayTransfer.
static C3D_Tex gx_temp_tex;
static bool gx_temp_init = false;

static void atlas_build_cell_gx(int cell, const SrcTexture* src, const SrcPalette* pal,
                                  u32 pot_w, u32 pot_h) {
    if (cell < 0 || cell >= atlas_cell_count || !atlas.cell_init[cell]) return;
    if (!staging_buf || !src->pixels) return;

    const u8* px = (const u8*)src->pixels;
    const texel_t* palette = pal ? pal->colors : NULL;
    if (!palette) return;
    if (pot_w > 512 || pot_h > 512) return; /* staging_buf is 512×512 */

    /* Step 1: palette resolve into linear staging buffer (sequential writes) */
    int w = (src->w < (int)pot_w) ? src->w : (int)pot_w;
    int h = (src->h < (int)pot_h) ? src->h : (int)pot_h;

    if (src->fmt == SCE_GS_PSMT8) {
        for (int y = 0; y < (int)pot_h; y++) {
            int src_y = (int)pot_h - 1 - y; /* Y-flip for GPU */
            texel_t* row_out = &staging_buf[y * (int)pot_w];
            if (src_y >= 0 && src_y < h) {
                const u8* src_row = px + src_y * src->w;
                for (int x = 0; x < w; x++)
                    row_out[x] = palette[src_row[x]];
                if (w < (int)pot_w)
                    memset(&row_out[w], 0, ((int)pot_w - w) * sizeof(texel_t));
            } else {
                memset(row_out, 0, (int)pot_w * sizeof(texel_t));
            }
        }
    } else {
        /* Non-PSMT8: fall back to CPU build */
        atlas_build_cell(cell, src, pal, pot_w, pot_h);
        return;
    }

    /* Step 2: GX_DisplayTransfer directly into the atlas strip cell region.
       Write to the strip at the cell's tile offset. GX tiles for pot_w×pot_h. */
    C3D_Tex* strip_tex = atlas_cell_tex(cell);
    int cell_in = atlas_cell_in_strip(cell);
    /* Byte offset of this cell within the strip texture data */
    size_t cell_byte_offset = (size_t)cell_in * ATLAS_CELL_SIZE / 8 * 64 * sizeof(texel_t);
    /* For cell 0 in a strip: offset=0. For cell 1: offset = 32*64*2 = 4096 */

    GSPGPU_FlushDataCache(staging_buf, pot_w * pot_h * sizeof(texel_t));

    /* GX transfer directly into the strip at cell offset.
       Use pot dimensions so tiling matches a 256×256 standalone texture. */
    GX_DisplayTransfer(
        (u32*)staging_buf,
        GX_BUFFER_DIM(pot_w, pot_h),
        (u32*)((u8*)strip_tex->data + cell_byte_offset),
        GX_BUFFER_DIM(pot_w, pot_h),
        GX_TRANSFER_FLIP_VERT(0) |
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA4) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA4) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO)
    );
    svcSleepThread(500000); /* 0.5ms — wait for GPU transfer */

    C3D_TexFlush(strip_tex);
    atlas_builds_total++;
    atlas_builds_period++;
}

// CPU fallback build: morton tiling done on CPU.
static void atlas_build_cell(int cell, const SrcTexture* src, const SrcPalette* pal,
                              u32 pot_w, u32 pot_h) {
    if (cell < 0 || cell >= atlas_cell_count || !atlas.cell_init[cell]) return;

    int local_tiles = ATLAS_CELL_SIZE >> 3; // 32 tiles per cell dimension

    const u8* px = (const u8*)src->pixels;
    const texel_t* pal_colors = pal ? pal->colors : NULL;

    // Clear this cell's region in the shared texture
    for (int ty = 0; ty < local_tiles; ty++)
        for (int tx = 0; tx < local_tiles; tx++)
            memset(atlas_tile_ptr(cell, tx, ty), 0, 64 * TEXEL_BYTES);

    /* NOTE: no special-casing for still-all-zero placeholder textures —
     * palette index 0 is forced transparent at capture (CPS3 rule), so
     * unfilled tiles render invisible by design, same as the reference. */

    int pot_tiles_y = (int)(pot_h >> 3);

    if (src->fmt == SCE_GS_PSMT8 && pal_colors) {
        /* Row-based single-pass: sequential source reads, minimal overhead.
           Process source image row by row instead of tile by tile.
           Palette is 512 bytes (hot in L1), morton_row loaded once per src row. */
        int w = (src->w < (int)pot_w) ? src->w : (int)pot_w;
        int h = (src->h < (int)pot_h) ? src->h : (int)pot_h;
        int tiles_x_cell = w >> 3;
        int strip = atlas_cell_strip(cell);
        int cell_in = atlas_cell_in_strip(cell);
        int strip_tiles_x = (ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE) >> 3;
        int cell_base_tx = cell_in * (ATLAS_CELL_SIZE >> 3);
        texel_t* strip_data = (texel_t*)atlas.strip_tex[strip].data;

        /* Also capture L8 indices for fast palette re-resolve */
        u8* l8 = atlas_l8[cell];
        int cell_tiles_x = ATLAS_CELL_SIZE >> 3;

        for (int src_y = 0; src_y < h; src_y++) {
            const u8* src_row = px + src_y * src->w;
            /* Y-flip within the CELL height, not pot_h: the GPU's v axis
             * samples v=0 at the LAST texel row, and the sampler maps task
             * v∈[0,1] to the strip range [0, src_h/CELL] — i.e. rows
             * CELL-1 .. CELL-src_h. Flipping within pot_h placed sub-cell-
             * height content (this tree's 128x128 background sheets — a PSP
             * re-chunking the reference tree never has) in rows 0..127, the
             * half the sampler never reads: chips drew fully transparent.
             * Full-size 256x256 content is unaffected (same formula). */
            int gpu_y = ATLAS_CELL_SIZE - 1 - src_y;
            int tile_y = gpu_y >> 3;
            int fy = gpu_y & 7;
            const u8* m = morton_row[fy];
            int row_base = tile_y * strip_tiles_x + cell_base_tx;
            int l8_row_base = tile_y * cell_tiles_x;

            for (int tx = 0; tx < tiles_x_cell; tx++) {
                texel_t* tile_dst = &strip_data[(row_base + tx) * 64];
                const u8* s = src_row + tx * 8;
                tile_dst[m[0]] = pal_colors[s[0]];
                tile_dst[m[1]] = pal_colors[s[1]];
                tile_dst[m[2]] = pal_colors[s[2]];
                tile_dst[m[3]] = pal_colors[s[3]];
                tile_dst[m[4]] = pal_colors[s[4]];
                tile_dst[m[5]] = pal_colors[s[5]];
                tile_dst[m[6]] = pal_colors[s[6]];
                tile_dst[m[7]] = pal_colors[s[7]];
                if (l8) {
                    u8* idx = &l8[(l8_row_base + tx) * 64];
                    idx[m[0]] = s[0]; idx[m[1]] = s[1];
                    idx[m[2]] = s[2]; idx[m[3]] = s[3];
                    idx[m[4]] = s[4]; idx[m[5]] = s[5];
                    idx[m[6]] = s[6]; idx[m[7]] = s[7];
                }
            }
        }
        atlas_l8_valid[cell] = (l8 != NULL);
    } else {
        int pot_tiles_x = (int)(pot_w >> 3);
        /* Same cell-height flip as the fast path: content occupies the TOP
         * tile rows of the cell (rows CELL-pot_h .. CELL-1). */
        int ty_shift = (ATLAS_CELL_SIZE - (int)pot_h) >> 3;
        /* Capture L8 indices for PSMT4 too, so palette animation on 16-color
         * content (tons of fight fx) takes the fast atlas_palette_resolve
         * path instead of a full rebuild — this was PSMT8-only, which is why
         * roughly half of all palette changes missed the L8 fast path. The
         * 4-bit indices store fine in the u8 cache (0-15), and the resolve's
         * pal_colors[idx] lookup works unchanged. */
        u8* l8 = atlas_l8[cell];
        int fill_l8 = (src->fmt == SCE_GS_PSMT4 && pal_colors && l8 != NULL);
        int src_half_w = src->w >> 1; /* PSMT4: two pixels per byte */
        for (int ty = 0; ty < pot_tiles_y && ty < local_tiles; ty++) {
            for (int tx = 0; tx < pot_tiles_x && tx < local_tiles; tx++) {
                int src_tile_x = tx * 8;
                int src_tile_y = (int)pot_h - 8 - ty * 8;
                texel_t* tile_dst = atlas_tile_ptr(cell, tx, ty + ty_shift);
                u8* l8_tile = fill_l8
                    ? &l8[((ty + ty_shift) * local_tiles + tx) * 64] : NULL;

                for (int fy = 0; fy < 8; fy++) {
                    int sy = src_tile_y + fy;
                    int dy_fine = 7 - fy;
                    for (int fx = 0; fx < 8; fx++) {
                        int sx = src_tile_x + fx;
                        texel_t color = 0;
                        if (sx >= 0 && sx < src->w && sy >= 0 && sy < src->h) {
                            color = resolve_texel(src, pal, sx, sy);
                            if (l8_tile) {
                                /* same nibble decode as resolve_texel's PSMT4 case */
                                u8 b = px[sy * src_half_w + (sx >> 1)];
                                l8_tile[morton_lut[dy_fine][fx]] =
                                    (sx & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
                            }
                        } else if (l8_tile) {
                            l8_tile[morton_lut[dy_fine][fx]] = 0;
                        }
                        tile_dst[morton_lut[dy_fine][fx]] = color;
                    }
                }
            }
        }
        atlas_l8_valid[cell] = fill_l8 != 0;
    }

    atlas_mark_strip_dirty(atlas_cell_strip(cell));
    atlas_builds_total++;
    atlas_builds_period++;
}

/* Fast palette re-resolve: apply new palette to cached morton-tiled indices.
   ~2× faster than full build — no morton LUT, no source stride, no Y-flip. */
static void atlas_palette_resolve(int cell, const texel_t* pal_colors, u32 pot_w, u32 pot_h) {
    if (cell < 0 || cell >= atlas_cell_count || !atlas_l8_valid[cell] || !atlas_l8[cell]) return;

    int strip = atlas_cell_strip(cell);
    int cell_in = atlas_cell_in_strip(cell);
    int strip_tiles_x = (ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE) >> 3;
    int cell_tiles = ATLAS_CELL_SIZE >> 3;
    texel_t* strip_data = (texel_t*)atlas.strip_tex[strip].data;
    const u8* l8 = atlas_l8[cell];

    /* Content sits in the TOP tile rows of the cell (cell-height Y-flip in
     * atlas_build_cell) — re-resolve exactly those rows. */
    int ty_shift = cell_tiles - (int)(pot_h >> 3);
    if (ty_shift < 0) ty_shift = 0;
    for (int ty = ty_shift; ty < cell_tiles; ty++) {
        int strip_row = ty * strip_tiles_x + cell_in * cell_tiles;
        int l8_row = ty * cell_tiles;
        for (int tx = 0; tx < (int)(pot_w >> 3) && tx < cell_tiles; tx++) {
            /* Hot loop — this runs for every atlas cell whose palette
             * animates (SF3 fights animate palettes near-constantly). Pack
             * two 16-bit texels per 32-bit store: tile data is 4-byte
             * aligned (tiles are 128B-aligned within the linearAlloc'd
             * strip), and on the 268MHz ARM11 halving the store count is a
             * real win. Output is bit-identical to the scalar loop. */
            u32* dst32 = (u32*)&strip_data[(strip_row + tx) * 64];
            const u8* src = &l8[(l8_row + tx) * 64];
            for (int p = 0; p < 64; p += 8) {
                u32 v0 = (u32)pal_colors[src[p + 0]] | ((u32)pal_colors[src[p + 1]] << 16);
                u32 v1 = (u32)pal_colors[src[p + 2]] | ((u32)pal_colors[src[p + 3]] << 16);
                u32 v2 = (u32)pal_colors[src[p + 4]] | ((u32)pal_colors[src[p + 5]] << 16);
                u32 v3 = (u32)pal_colors[src[p + 6]] | ((u32)pal_colors[src[p + 7]] << 16);
                dst32[(p >> 1) + 0] = v0;
                dst32[(p >> 1) + 1] = v1;
                dst32[(p >> 1) + 2] = v2;
                dst32[(p >> 1) + 3] = v3;
            }
        }
    }
    atlas_mark_strip_dirty(strip);
}

typedef struct {
    u32 pot_w;
    u32 pot_h;
    u32 count;
} EvictionOtherStat;
static EvictionOtherStat cache_pool_evictions_other_stats[8];
static u32 cache_invalidate_texture_calls = 0;
static u32 cache_invalidate_palette_calls = 0;
static u32 cache_invalidate_texture_entries = 0;
static u32 cache_invalidate_palette_entries = 0;
static u32 unlock_texture_calls = 0;
static u32 unlock_texture_changed = 0;
static u32 unlock_palette_calls = 0;
static u32 unlock_palette_changed = 0;
static u32 destroy_texture_counts[FL_TEXTURE_MAX] = { 0 };
static u32 destroy_palette_counts[FL_PALETTE_MAX] = { 0 };
static u64 cache_create_ticks_total = 0;
static u64 cache_create_ticks_build = 0;
/* Frame build tracking (no deferral — builds are immediate) */
static u64 frame_build_used = 0;
/* When true, cache_create skips full texture build — expects region updates
   to fill needed tiles. Set during melt2 batch decode. */
int gpu_cache_skip_full_build = 0;
/* When true, skip atlas path — all textures go to pool.
   Useful for screens with many unique textures where atlas thrashing hurts. */
int gpu_cache_prefer_pool = 0;
static u64 cache_create_ticks_flush = 0;
static u32 cache_create_count_total = 0;
/* Fine-grained timing for hot path analysis */
u64 region_update_ticks = 0;
u32 region_update_count = 0;
static u64 region_morton_ticks = 0;
static u32 region_tile_count = 0;
static u32 cache_create_count_psmt8 = 0;
static u32 cache_create_count_psmt4 = 0;
static u32 cache_create_count_psmct16 = 0;
static u32 cache_create_count_other = 0;
static u64 render_ticks_sort = 0;
static u64 render_ticks_fill = 0;
static u64 render_ticks_submit = 0;
static u32 render_draw_call_count = 0;
static char last_renderer_profile[128] = { 0 };
u32 dbg_settex_miss(void); u32 dbg_settex_create(void); u32 dbg_settex_fail(void); u32 dbg_atlas_evict(void);
// (atlas_uv_debug removed — was debug-only)

// (hash table removed — simple scan is more reliable)

// staging_buf and STAGING_MAX declared earlier (before atlas_build_cell_gx)

// ---------------------------------------------------------------------------
// L8 Index Cache: caches morton-tiled 8-bit palette indices per texture page.
// When ONLY the palette changes, re-resolve from cached indices (skip LZ decompress).
// ---------------------------------------------------------------------------

#define L8_CACHE_MAX 48 /* was 24, then 32 — char select cycles 10+ sheets per
                         * hovered character through this cache, and every
                         * miss turns a ~2ms fast re-resolve into a 6.7ms full
                         * rebuild during the super-art preview (measured:
                         * sequential ti=8..14 rebuild bursts at select) */
typedef struct {
    int texture_index;   // which source texture this caches
    u32 tex_version;     // version when indices were captured
    int pot_w, pot_h;    // texture dimensions
    u8* indices;         // morton-tiled index buffer (pot_w * pot_h bytes)
    u32 last_used;       // frame number for LRU eviction
    bool valid;
} L8CacheEntry;

static L8CacheEntry l8_cache[L8_CACHE_MAX];
static u32 l8_hits = 0, l8_misses = 0;

static L8CacheEntry* l8_cache_find(int tex_idx, u32 tex_ver) {
    for (int i = 0; i < L8_CACHE_MAX; i++) {
        if (l8_cache[i].valid && l8_cache[i].texture_index == tex_idx && l8_cache[i].tex_version == tex_ver) {
            l8_cache[i].last_used = frame_number;
            return &l8_cache[i];
        }
    }
    return NULL;
}

static L8CacheEntry* l8_cache_alloc(int tex_idx, u32 tex_ver, int pot_w, int pot_h) {
    // Find free slot or LRU evict
    L8CacheEntry* best = NULL;
    u32 oldest = UINT32_MAX;
    for (int i = 0; i < L8_CACHE_MAX; i++) {
        if (!l8_cache[i].valid) { best = &l8_cache[i]; break; }
        if (l8_cache[i].last_used < oldest) { oldest = l8_cache[i].last_used; best = &l8_cache[i]; }
    }
    if (!best) return NULL;

    // Realloc if size changed
    size_t need = (size_t)pot_w * pot_h;
    if (best->indices && (best->pot_w != pot_w || best->pot_h != pot_h)) {
        free(best->indices);
        best->indices = NULL;
    }
    if (!best->indices) {
        best->indices = (u8*)malloc(need);
        if (!best->indices) return NULL;
    }
    /* index 0 == transparent (pal_colors[0] forced 0): padding tiles the
     * build never writes must not hold malloc garbage, or a later palette
     * re-resolve paints noise into them */
    memset(best->indices, 0, need);
    best->texture_index = tex_idx;
    best->tex_version = tex_ver;
    best->pot_w = pot_w;
    best->pot_h = pot_h;
    best->last_used = frame_number;
    best->valid = true;
    return best;
}

// Fast palette re-resolve: read cached morton-tiled indices, apply new palette, write to GPU texture.
// Skips: LZ decompress, source data read, morton coordinate computation.
/* Re-resolve cached indices with a new palette into a pool slot. The slot is
 * a cell of a shared strip: indices are stored slot-linear (tiles_x wide),
 * the destination walks the STRIP stride (dst_tiles_x) at the cell's X
 * offset (dst_base_tx). Two RGBA4 texels packed per u32 store — tiles are
 * 128B-aligned within the strip, and halving stores is a real win on the
 * 268MHz ARM11. Output bit-identical to the scalar loop. */
static void l8_resolve_palette(const L8CacheEntry* l8, const texel_t* palette,
                                texel_t* tex_data, int tiles_x, int tiles_y,
                                int dst_tiles_x, int dst_base_tx) {
    for (int ty = 0; ty < tiles_y; ty++) {
        for (int tx = 0; tx < tiles_x; tx++) {
            const u8* src_idx = &l8->indices[(ty * tiles_x + tx) * 64];
            u32* dst32 = (u32*)&tex_data[(ty * dst_tiles_x + dst_base_tx + tx) * 64];
            for (int p = 0; p < 64; p += 8) {
                u32 v0 = (u32)palette[src_idx[p + 0]] | ((u32)palette[src_idx[p + 1]] << 16);
                u32 v1 = (u32)palette[src_idx[p + 2]] | ((u32)palette[src_idx[p + 3]] << 16);
                u32 v2 = (u32)palette[src_idx[p + 4]] | ((u32)palette[src_idx[p + 5]] << 16);
                u32 v3 = (u32)palette[src_idx[p + 6]] | ((u32)palette[src_idx[p + 7]] << 16);
                dst32[(p >> 1) + 0] = v0;
                dst32[(p >> 1) + 1] = v1;
                dst32[(p >> 1) + 2] = v2;
                dst32[(p >> 1) + 3] = v3;
            }
        }
    }
}

// Forward declarations
static void imm_bind(void);

// ---------- Hash index for O(1) cache lookup ----------
// Maps (texture_index, palette_index) → cache slot index.
// Open addressing with linear probe. MUCH faster than O(512) scan.
#define CACHE_HASH_SIZE 1024  /* must be power of 2 */
#define CACHE_HASH_MASK (CACHE_HASH_SIZE - 1)
typedef struct { int tex_idx; int pal_idx; int cache_slot; bool occupied; } CacheHashEntry;
static CacheHashEntry cache_hash[CACHE_HASH_SIZE];

static inline u32 cache_hash_key(int tex_idx, int pal_idx) {
    u32 h = (u32)tex_idx * 2654435761u + (u32)(pal_idx + 1) * 2246822519u;
    return h & CACHE_HASH_MASK;
}

static void cache_hash_insert(int tex_idx, int pal_idx, int slot) {
    u32 h = cache_hash_key(tex_idx, pal_idx);
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        u32 idx = (h + i) & CACHE_HASH_MASK;
        if (!cache_hash[idx].occupied ||
            (cache_hash[idx].tex_idx == tex_idx && cache_hash[idx].pal_idx == pal_idx)) {
            cache_hash[idx].tex_idx = tex_idx;
            cache_hash[idx].pal_idx = pal_idx;
            cache_hash[idx].cache_slot = slot;
            cache_hash[idx].occupied = true;
            return;
        }
    }
}

static void cache_hash_remove(int tex_idx, int pal_idx) {
    u32 h = cache_hash_key(tex_idx, pal_idx);
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        u32 idx = (h + i) & CACHE_HASH_MASK;
        if (!cache_hash[idx].occupied) return; /* not found */
        if (cache_hash[idx].tex_idx == tex_idx && cache_hash[idx].pal_idx == pal_idx) {
            cache_hash[idx].occupied = false;
            /* Rehash subsequent entries in cluster */
            u32 next = (idx + 1) & CACHE_HASH_MASK;
            while (cache_hash[next].occupied) {
                CacheHashEntry tmp = cache_hash[next];
                cache_hash[next].occupied = false;
                cache_hash_insert(tmp.tex_idx, tmp.pal_idx, tmp.cache_slot);
                next = (next + 1) & CACHE_HASH_MASK;
            }
            return;
        }
    }
}

// Find a live cached GPU texture for this texture/palette pair.
static CacheEntry* cache_find(int tex_idx, int pal_idx) {
    u32 h = cache_hash_key(tex_idx, pal_idx);
    /* Walk the whole cluster: insert probes the full table, so capping the
     * lookup (was 16 probes) made entries beyond the cap unfindable under
     * churn — the pair exists but find returns NULL every frame, forcing a
     * full rebuild each bind (and desyncing find vs create's linear scan). */
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        u32 idx = (h + i) & CACHE_HASH_MASK;
        if (!cache_hash[idx].occupied) return NULL;
        if (cache_hash[idx].tex_idx == tex_idx && cache_hash[idx].pal_idx == pal_idx) {
            int slot = cache_hash[idx].cache_slot;
            CacheEntry* e = &gpu_cache[slot];
            if (!e->allocated || e->pending_delete) return NULL;
            if (e->texture_index != tex_idx || e->palette_index != pal_idx) {
                /* Stale hash entry */
                cache_hash[idx].occupied = false;
                return NULL;
            }
            e->last_used_frame = frame_number;
            if (e->atlas_cell >= 0 && e->atlas_cell < atlas_cell_count)
                atlas.cell_last_used[e->atlas_cell] = frame_number;
            return e;
        }
    }
    return NULL;
}

/* Sampled all-zero check (~32 spread-out bytes). For PSMT8 sources a zero
 * byte is palette index 0 = transparent, so "sampled all zero" ≈ nothing
 * would render from this content anyway. */
static bool src_effectively_empty(const SrcTexture* src) {
    if (!src->valid || !src->pixels || src->byte_size == 0) return true;
    const u8* bp = (const u8*)src->pixels;
    size_t n = src->byte_size;
    size_t stride = (n > 512) ? (n / 32) : 16;
    for (size_t off = 0; off < n; off += stride)
        if (bp[off]) return false;
    return bp[n - 1] == 0;
}

static CacheEntry* cache_find_clean(int tex_idx, int pal_idx) {
    CacheEntry* e = cache_find(tex_idx, pal_idx);
    if (!e) return NULL;
    /* Built from a not-yet-decoded (all-zero) source? Recheck each bind and
     * force ONE rebuild via the miss path when the real pixels have arrived
     * (some decode paths deliver content a few frames after handle creation
     * with no unlock/region-update signal — see CacheEntry). */
    if (e->src_empty_at_build &&
        tex_idx >= 0 && tex_idx < FL_TEXTURE_MAX &&
        !src_effectively_empty(&src_textures[tex_idx])) {
        return NULL;
    }
    /* Atlas entries: skip texture-dirty (region updates handle tile changes).
       But still rebuild on PALETTE changes (colors baked into GPU data). */
    if (e->atlas_cell >= 0) {
        u32 cur_tex_ver = (tex_idx >= 0 && tex_idx < FL_TEXTURE_MAX) ? texture_versions[tex_idx] : 0;
        u32 cur_pal_ver = (pal_idx >= 0 && pal_idx < FL_PALETTE_MAX) ? palette_versions[pal_idx] : 0;

        if (e->tex_version != cur_tex_ver) {
            /* Texture data changed (new character, etc.) — must full rebuild.
               Can't use L8 cache — source indices are different now. */
            return NULL;
        }

        if (e->pal_version != cur_pal_ver) {
            /* TRIAL FIX REVERTED: forcing a full rebuild here (skipping the
             * fast L8 palette-only re-resolve) did NOT fix the transient
             * attract-mode miss, and cost real performance (large slowdown
             * spikes, user-confirmed) — not the mechanism. Back to the fast
             * path. */
            if (atlas_l8_valid[e->atlas_cell] && pal_idx >= 0 && pal_idx < FL_PALETTE_MAX
                && src_palettes[pal_idx].valid) {
                const u64 t0 = svcGetSystemTick();
                atlas_palette_resolve(e->atlas_cell, src_palettes[pal_idx].colors,
                                      e->pot_w, e->pot_h);
                cache_create_ticks_build += svcGetSystemTick() - t0;
                e->pal_version = cur_pal_ver;
                e->dirty = false;
                return e;
            }
            return NULL; /* no L8 cache — fall back to full rebuild */
        }

        /* Neither changed — clear spurious dirty flag from UnlockTexture */
        e->dirty = false;
        return e;
    }
    if (!e->dirty) return e;
    return NULL;
}

static void pool_release_slot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= TEXTURE_POOL_SLOT_COUNT) return;
    texture_pool[slot_idx].in_use = false;
}

static int pool_acquire_slot(u32 pot_w, u32 pot_h) {
    int best_idx = -1;
    u32 best_area = UINT32_MAX;

    for (int i = 0; i < TEXTURE_POOL_SLOT_COUNT; i++) {
        TexturePoolSlot* slot = &texture_pool[i];
        if (!slot->initialized || slot->in_use) continue;
        if (slot->pot_w >= pot_w && slot->pot_h >= pot_h) {
            u32 area = slot->pot_w * slot->pot_h;
            if (area < best_area) {
                best_area = area;
                best_idx = i;
            }
        }
    }
    if (best_idx >= 0) {
        texture_pool[best_idx].in_use = true;
        return best_idx;
    }
    return -1;
}

static void cache_record_pool_eviction(u32 pot_w, u32 pot_h) {
    cache_pool_evictions++;
    if (pot_w == 16 && pot_h == 16) {
        cache_pool_evictions_16++;
    } else if (pot_w == 32 && pot_h == 32) {
        cache_pool_evictions_32++;
    } else if (pot_w == 64 && pot_h == 64) {
        cache_pool_evictions_64++;
    } else if (pot_w == 128 && pot_h == 64) {
        cache_pool_evictions_128x64++;
    } else if (pot_w == 64 && pot_h == 128) {
        cache_pool_evictions_64x128++;
    } else if (pot_w == 128 && pot_h == 128) {
        cache_pool_evictions_128x128++;
    } else if (pot_w == 256 && pot_h == 128) {
        cache_pool_evictions_256x128++;
    } else if (pot_w == 128 && pot_h == 256) {
        cache_pool_evictions_128x256++;
    } else if (pot_w == 256 && pot_h == 256) {
        cache_pool_evictions_256x256++;
    } else if (pot_w == 512 && pot_h == 256) {
        cache_pool_evictions_512x256++;
    } else if (pot_w == 256 && pot_h == 512) {
        cache_pool_evictions_256x512++;
    } else if (pot_w == 512 && pot_h == 512) {
        cache_pool_evictions_512x512++;
    } else {
        cache_pool_evictions_other++;
        for (int i = 0; i < (int)SDL_arraysize(cache_pool_evictions_other_stats); i++) {
            EvictionOtherStat* stat = &cache_pool_evictions_other_stats[i];
            if (stat->count > 0 && stat->pot_w == pot_w && stat->pot_h == pot_h) {
                stat->count++;
                return;
            }
        }
        for (int i = 0; i < (int)SDL_arraysize(cache_pool_evictions_other_stats); i++) {
            EvictionOtherStat* stat = &cache_pool_evictions_other_stats[i];
            if (stat->count == 0) {
                stat->pot_w = pot_w;
                stat->pot_h = pot_h;
                stat->count = 1;
                return;
            }
        }
        int min_i = 0;
        for (int i = 1; i < (int)SDL_arraysize(cache_pool_evictions_other_stats); i++) {
            if (cache_pool_evictions_other_stats[i].count < cache_pool_evictions_other_stats[min_i].count) {
                min_i = i;
            }
        }
        cache_pool_evictions_other_stats[min_i].pot_w = pot_w;
        cache_pool_evictions_other_stats[min_i].pot_h = pot_h;
        cache_pool_evictions_other_stats[min_i].count++;
    }
}

// Resolve one texel from the source data + palette into GPU_TEX_FMT.
static texel_t resolve_texel(const SrcTexture* src, const SrcPalette* pal, int x, int y) {
    const u8* px = (const u8*)src->pixels;

    switch (src->fmt) {
    case SCE_GS_PSMT8: {
        u8 idx = px[y * src->w + x];
        if (pal && idx < (unsigned)pal->count) return pal->colors[idx];
        return 0;
    }
    case SCE_GS_PSMT4: {
        int byte_offset = y * (src->w / 2) + (x >> 1);
        u8 byte = px[byte_offset];
        u8 idx = (x & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
        if (pal && idx < (unsigned)pal->count) return pal->colors[idx];
        return 0;
    }
    case SCE_GS_PSMCT16: {
        // No palette is passed for this format (see the switch below) —
        // it's always a direct/non-indexed texture, so use the ARGB1555
        // direct layout, not the PSMCT16 CLUT layout.
        u16 raw = ((const u16*)px)[y * src->w + x];
        return color16_direct_to_rgba(raw);
    }
    default:
        return 0;
    }
}

static void cache_update_entry_region(CacheEntry* entry, int x, int y, int w, int h) {
    static u32 region_sample_ctr = 0;
    int region_sampling = (++region_sample_ctr % 100 == 0);
    u64 region_t0 = region_sampling ? svcGetSystemTick() : 0;
    if (!entry || !entry->allocated || entry->pending_delete) return;

    /* L8 index cache: PATCH the affected tiles below (PSMT8 pool path)
     * instead of the old blanket invalidation. Melting sheets get their
     * palette ticked constantly; with the cache invalidated, every tick was
     * a measured 6.8ms full rebuild — patched, it's a ~2ms fast re-resolve.
     * Formats the patch loop doesn't handle still invalidate here. */
    const SrcTexture* src = &src_textures[entry->texture_index];
    if (src->fmt != SCE_GS_PSMT8) {
        for (int li = 0; li < L8_CACHE_MAX; li++) {
            if (l8_cache[li].valid && l8_cache[li].texture_index == entry->texture_index) {
                l8_cache[li].valid = false;
            }
        }
    }

    if (!src->valid) return;

    const SrcPalette* pal = NULL;
    if (entry->palette_index >= 0 && entry->palette_index < FL_PALETTE_MAX && src_palettes[entry->palette_index].valid) {
        pal = &src_palettes[entry->palette_index];
    }

    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= src->w || y >= src->h) return;
    if (x + w > src->w) w = src->w - x;
    if (y + h > src->h) h = src->h - y;
    if (w <= 0 || h <= 0) return;

    // Determine target texture data and tile layout
    texel_t* tex_data;
    int data_tiles_x;  // tiles per row in the target texture
    int base_tx = 0;   // tile offset for atlas regions
    int base_ty = 0;
    int region_pot_h;
    bool is_atlas = (entry->atlas_cell >= 0);

    if (is_atlas) {
        if (!atlas.initialized) return;
        int cell = entry->atlas_cell;
        if (cell < 0 || cell >= atlas_cell_count || !atlas.cell_init[cell]) return;
        tex_data = (texel_t*)atlas_cell_tex(cell)->data;
        int strip_w = ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE;
        data_tiles_x = strip_w >> 3;
        base_tx = atlas_cell_in_strip(cell) * (ATLAS_CELL_SIZE >> 3);
        base_ty = 0; /* single-row strips */
        /* Y-flip against the CELL height, matching atlas_build_cell — content
         * occupies the cell's top tile rows for sub-cell-height sources. */
        region_pot_h = ATLAS_CELL_SIZE;
    } else {
        if (entry->pool_slot < 0 || entry->pool_slot >= TEXTURE_POOL_SLOT_COUNT) return;
        TexturePoolSlot* slot = &texture_pool[entry->pool_slot];
        /* slot = a cell of a shared strip: strip stride + cell X offset */
        tex_data = pool_slot_data(slot);
        data_tiles_x = pool_slot_strip_tiles_x(slot);
        base_tx = pool_slot_base_tx(slot);
        region_pot_h = (int)slot->pot_h;
    }

    const u8* px = (const u8*)src->pixels;
    int pal_n = pal ? pal->count : 0;
    const texel_t* pal_colors = pal ? pal->colors : NULL;

    int tile_x0 = x >> 3;
    int src_tile_y0 = y >> 3;
    int tile_x1 = (x + w - 1) >> 3;
    int src_tile_y1 = (y + h - 1) >> 3;
    int tiles_y = region_pot_h >> 3;

    for (int src_ty = src_tile_y0; src_ty <= src_tile_y1; src_ty++) {
        int ty = (tiles_y - 1) - src_ty;
        for (int tx = tile_x0; tx <= tile_x1; tx++) {
            int src_tile_x = tx * 8;
            int src_tile_y = src_ty * 8;
            int tile_idx = (base_ty + ty) * data_tiles_x + (base_tx + tx);
            texel_t* tile_dst = TILE_AT(tex_data, tile_idx);

            if (src->fmt == SCE_GS_PSMT8 && pal_colors) {
                morton_tile_block_psmt8(tile_dst, px, src->w, src_tile_x, src_tile_y,
                                         src->w, src->h, pal_colors, pal_n, 0);
                /* Keep the cell's L8 index copy in sync: atlas_palette_resolve
                 * re-resolves texels from atlas_l8 on palette-only changes, so
                 * stale indices here would revert this region to its pre-update
                 * content on the next palette bump. Same morton/Y-flip
                 * convention as the texel write above. */
                if (is_atlas && atlas_l8_valid[entry->atlas_cell] && atlas_l8[entry->atlas_cell]) {
                    u8* l8tile = &atlas_l8[entry->atlas_cell]
                                          [((base_ty + ty) * (ATLAS_CELL_SIZE >> 3) + tx) * 64];
                    if (src_tile_x >= 0 && src_tile_y >= 0 &&
                        src_tile_x + 8 <= src->w && src_tile_y + 8 <= src->h) {
                        /* fully in-bounds tile (the always-case for melt
                         * writes) — unrolled, no per-texel bounds checks.
                         * This sync runs inside every melt region update, so
                         * its cost lands directly in the animation-stutter
                         * budget (MELTSPIKE). */
                        for (int fy = 0; fy < 8; fy++) {
                            const u8* srow = px + (src_tile_y + fy) * src->w + src_tile_x;
                            const u8* m = morton_row[7 - fy];
                            l8tile[m[0]] = srow[0]; l8tile[m[1]] = srow[1];
                            l8tile[m[2]] = srow[2]; l8tile[m[3]] = srow[3];
                            l8tile[m[4]] = srow[4]; l8tile[m[5]] = srow[5];
                            l8tile[m[6]] = srow[6]; l8tile[m[7]] = srow[7];
                        }
                    } else {
                        for (int fy = 0; fy < 8; fy++) {
                            int sy = src_tile_y + fy;
                            const u8* m = morton_row[7 - fy];
                            for (int fx = 0; fx < 8; fx++) {
                                int sx = src_tile_x + fx;
                                u8 v = 0;
                                if (sx >= 0 && sx < src->w && sy >= 0 && sy < src->h)
                                    v = px[sy * src->w + sx];
                                l8tile[m[fx]] = v;
                            }
                        }
                    }
                }
            } else {
                /* Non-PSMT8 region write. If this cell has a live L8 index
                 * cache, it must be kept in sync (PSMT4) or invalidated
                 * (anything else) — otherwise the next palette-only resolve
                 * would repaint this region from PRE-update indices,
                 * silently reverting the content. */
                u8* l8tile = NULL;
                if (is_atlas && atlas_l8_valid[entry->atlas_cell] && atlas_l8[entry->atlas_cell]) {
                    if (src->fmt == SCE_GS_PSMT4 && pal_colors) {
                        l8tile = &atlas_l8[entry->atlas_cell]
                                          [((base_ty + ty) * (ATLAS_CELL_SIZE >> 3) + tx) * 64];
                    } else {
                        atlas_l8_valid[entry->atlas_cell] = false;
                    }
                }
                int src_half_w = src->w >> 1;
                for (int fy = 0; fy < 8; fy++) {
                    int sy = src_tile_y + fy;
                    int dy_fine = 7 - fy;
                    for (int fx = 0; fx < 8; fx++) {
                        int sx = src_tile_x + fx;
                        texel_t color = 0;
                        u8 idxv = 0;
                        if (sx >= 0 && sx < src->w && sy >= 0 && sy < src->h) {
                            color = resolve_texel(src, pal, sx, sy);
                            if (l8tile) {
                                u8 b = px[sy * src_half_w + (sx >> 1)];
                                idxv = (sx & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
                            }
                        }
                        if (l8tile) l8tile[morton_lut[dy_fine][fx]] = idxv;
                        tile_dst[morton_lut[dy_fine][fx]] = color;
                    }
                }
            }
        }
    }

    if (is_atlas) {
        int cell = entry->atlas_cell;
        if (cell >= 0 && cell < atlas_cell_count)
            atlas_mark_strip_dirty(atlas_cell_strip(cell));
    } else {
        pool_mark_slot_dirty(&texture_pool[entry->pool_slot]);
    }
    entry->dirty = false;
    entry->last_used_frame = frame_number;
    if (region_sampling) {
        region_update_ticks += svcGetSystemTick() - region_t0;
        region_update_count++;
    }
}

int dbg_cache_full_returns = 0; /* PORT DIAG: cache_create's uncounted "all N in use" NULL. TEMP */

// Build a GPU texture from source data + palette, with morton tiling.
// The texture is palette-resolved to linear RGBA in a staging buffer,
// then written morton-tiled into the C3D_Tex.
static CacheEntry* cache_create(int tex_idx, int pal_idx) {
    const u64 create_start_tick = svcGetSystemTick();
    const SrcTexture* src = &src_textures[tex_idx];
    if (!src->valid || !staging_buf || !src->pixels) {
        cache_fail_invalid++;
        { /* PORT DIAG: identify which texture indices are permanently
             invalid, to trace back to their creation call site. TEMP. */
            extern void debug_print(const char *fmt, ...);
            static int seen[FL_TEXTURE_MAX];
            if (tex_idx >= 0 && tex_idx < FL_TEXTURE_MAX && !seen[tex_idx]) {
                seen[tex_idx] = 1;
                debug_print("INVTEX idx=%d w=%d h=%d valid=%d px=%p",
                            tex_idx, src->w, src->h, (int)src->valid, src->pixels);
            }
        }
        return NULL;
    }

    const SrcPalette* pal = NULL;
    if (pal_idx >= 0 && pal_idx < FL_PALETTE_MAX && src_palettes[pal_idx].valid) {
        pal = &src_palettes[pal_idx];
    }

    u32 pot_w = next_pot((u32)src->w);
    u32 pot_h = next_pot((u32)src->h);
    u32 req_pot_w = pot_w;
    u32 req_pot_h = pot_h;
    // Minimum 8x8 for C3D_TexInit
    if (pot_w < 8) pot_w = 8;
    if (pot_h < 8) pot_h = 8;
    if (pot_w * pot_h > STAGING_MAX) {
        cache_fail_too_big++;
        return NULL;
    }

    // 1. Prefer reusing an older version of the same texture/palette pair —
    // INCLUDING entries marked pending_delete (invalidated earlier this same
    // frame): resurrect them in place instead of creating a duplicate.
    // Skipping pending entries here created a second live entry for the same
    // pair; at EndFrame, cache_flush_pending freed the old one and its
    // cache_hash_remove(tex,pal) deleted the hash mapping that now pointed at
    // the NEW entry — leaving it allocated but permanently unfindable, so the
    // next frame created yet another duplicate (and leaked another atlas cell
    // until eviction). Resurrection keeps entry identity, its atlas cell, and
    // its hash mapping stable across the destroy→recreate churn.
    CacheEntry* entry = NULL;
    for (int i = 0; i < CACHE_MAX; i++) {
        CacheEntry* e = &gpu_cache[i];
        if (!e->allocated) continue;
        if (e->texture_index == tex_idx && e->palette_index == pal_idx) {
            entry = e;
            entry->pending_delete = false;
            break;
        }
    }

    // 2. Find a free cache entry metadata slot.
    if (!entry) {
        for (int i = 0; i < CACHE_MAX; i++) {
            if (!gpu_cache[i].allocated) {
                entry = &gpu_cache[i];
                break;
            }
        }
    }

    // 3. No free metadata slot - LRU evict the oldest non-pending entry.
    if (!entry) {
        u32 oldest_frame = UINT32_MAX;
        int oldest_idx = -1;
        for (int i = 0; i < CACHE_MAX; i++) {
            CacheEntry* e = &gpu_cache[i];
            if (e->allocated && !e->pending_delete && !e->pinned && e->last_used_frame < oldest_frame) {
                oldest_frame = e->last_used_frame;
                oldest_idx = i;
            }
        }
        if (oldest_idx >= 0) {
            entry = &gpu_cache[oldest_idx];
            cache_hash_remove(entry->texture_index, entry->palette_index);
            if (entry->atlas_cell >= 0) {
                atlas_free_cell(entry->atlas_cell);
                entry->atlas_cell = -1;
            }
            pool_release_slot(entry->pool_slot);
            entry->allocated = false;
            entry->pool_slot = -1;
        } else {
            dbg_cache_full_returns++; /* all metadata slots in use this frame (profile) */
            return NULL;
        }
    }

    // Clean up if this slot had a previous allocation pending delete.
    if (entry->pending_delete && entry->allocated) {
        cache_hash_remove(entry->texture_index, entry->palette_index);
        if (entry->atlas_cell >= 0) {
            atlas_free_cell(entry->atlas_cell);
            entry->atlas_cell = -1;
        }
        pool_release_slot(entry->pool_slot);
        entry->allocated = false;
        entry->pending_delete = false;
        entry->pool_slot = -1;
    }

    // --- Atlas path: try atlas for entries that fit in a 256x256 cell ---
    if (!atlas.initialized) {
        atlas_skip_noinit++;
    } else if (pot_w > ATLAS_CELL_SIZE || pot_h > ATLAS_CELL_SIZE) {
        atlas_skip_big++;
    }
    if (atlas.initialized && !gpu_cache_prefer_pool &&
        pot_w <= ATLAS_CELL_SIZE && pot_h <= ATLAS_CELL_SIZE) {
        atlas_try_count++;
        int cache_idx = (int)(entry - gpu_cache);
        int cell = -1;

        // Reuse existing atlas cell if entry already has one
        if (entry->allocated && entry->atlas_cell >= 0) {
            cell = entry->atlas_cell;
            atlas.cell_owner[cell] = cache_idx;
            atlas.cell_last_used[cell] = frame_number;
        } else {
            // Release old pool slot if switching from pool to atlas
            if (entry->allocated && entry->pool_slot >= 0) {
                cache_hash_remove(entry->texture_index, entry->palette_index);
                pool_release_slot(entry->pool_slot);
                entry->allocated = false;
                entry->pool_slot = -1;
            }
            cell = atlas_alloc_cell(cache_idx);
        }

        if (cell >= 0) {
            atlas_hit_count++;

            entry->texture_index = tex_idx;
            entry->palette_index = pal_idx;
            entry->pool_slot = -1;
            entry->atlas_cell = cell;
            entry->tex_version = (tex_idx >= 0 && tex_idx < FL_TEXTURE_MAX) ? texture_versions[tex_idx] : 0;
            entry->pal_version = (pal_idx >= 0 && pal_idx < FL_PALETTE_MAX) ? palette_versions[pal_idx] : 0;
            entry->pot_w = pot_w;
            entry->pot_h = pot_h;
            entry->src_w = src->w;
            entry->src_h = src->h;
            entry->allocated = true;
            entry->pinned = false;
            entry->pending_delete = false;
            entry->last_used_frame = frame_number;
            entry->src_empty_at_build = src_effectively_empty(src);

            /* Full build on creation. cache_find_clean skips PALETTE-ONLY
               dirty (uses L8 re-resolve). TEXTURE changes still trigger
               full rebuild via cache_create. */
            {   const u64 build_start_tick = svcGetSystemTick();
                atlas_build_cell(cell, src, pal, pot_w, pot_h);
                const u64 build_end_tick = svcGetSystemTick();
                frame_build_used += build_end_tick - build_start_tick;
                cache_create_ticks_build += build_end_tick - build_start_tick;
            }
            entry->dirty = false;

            { int slot_i = cache_idx; cache_hash_insert(tex_idx, pal_idx, slot_i); }

            cache_create_count_total++;
            switch (src->fmt) {
            case SCE_GS_PSMT8: cache_create_count_psmt8++; break;
            case SCE_GS_PSMT4: cache_create_count_psmt4++; break;
            case SCE_GS_PSMCT16: cache_create_count_psmct16++; break;
            default: cache_create_count_other++; break;
            }
            return entry;
        }
        // Atlas full and all pinned — fall through to pool path
    }

    // --- Pool path (for entries too large for atlas, or atlas overflow) ---
    // Release atlas cell if entry had one (switching to pool)
    if (entry->allocated && entry->atlas_cell >= 0) {
        atlas_free_cell(entry->atlas_cell);
        entry->atlas_cell = -1;
    }

    int slot_idx = -1;
    if (entry->allocated &&
        entry->pool_slot >= 0 &&
        entry->pool_slot < TEXTURE_POOL_SLOT_COUNT &&
        texture_pool[entry->pool_slot].initialized &&
        texture_pool[entry->pool_slot].pot_w >= req_pot_w &&
        texture_pool[entry->pool_slot].pot_h >= req_pot_h) {
        slot_idx = entry->pool_slot;
        texture_pool[slot_idx].in_use = true;
    } else {
        if (entry->allocated) {
            cache_hash_remove(entry->texture_index, entry->palette_index);
            pool_release_slot(entry->pool_slot);
            entry->allocated = false;
            entry->pool_slot = -1;
        }
        slot_idx = pool_acquire_slot(pot_w, pot_h);
        if (slot_idx < 0) {
            u32 oldest_frame = UINT32_MAX;
            int oldest_idx = -1;
            for (int i = 0; i < CACHE_MAX; i++) {
                CacheEntry* e = &gpu_cache[i];
                if (!e->allocated || e->pending_delete) continue;
                if (e->pinned) continue;
                if (e->atlas_cell >= 0) continue; // atlas entries don't have pool slots
                if (e->pot_w < req_pot_w || e->pot_h < req_pot_h) continue;
                if (e->last_used_frame < oldest_frame) {
                    oldest_frame = e->last_used_frame;
                    oldest_idx = i;
                }
            }
            if (oldest_idx < 0) {
                cache_fail_noslot++;
                return NULL;
            }

            CacheEntry* evicted = &gpu_cache[oldest_idx];
            cache_hash_remove(evicted->texture_index, evicted->palette_index);
            if (evicted->atlas_cell >= 0) {
                atlas_free_cell(evicted->atlas_cell);
                evicted->atlas_cell = -1;
            }
            slot_idx = evicted->pool_slot;
            u32 evicted_pot_w = evicted->pot_w;
            u32 evicted_pot_h = evicted->pot_h;
            evicted->allocated = false;
            evicted->pending_delete = false;
            evicted->pool_slot = -1;
            cache_record_pool_eviction(evicted_pot_w, evicted_pot_h);
            if (slot_idx < 0 || slot_idx >= TEXTURE_POOL_SLOT_COUNT) {
                cache_fail_noslot++;
                return NULL;
            }
            texture_pool[slot_idx].in_use = true;
        }
    }

    pot_w = texture_pool[slot_idx].pot_w;
    pot_h = texture_pool[slot_idx].pot_h;

    if (gpu_cache_skip_full_build) {
        /* Skip full build during melt2 only. */
        pool_clear_slot(&texture_pool[slot_idx]);
        pool_mark_slot_dirty(&texture_pool[slot_idx]);
        goto pool_skip_build;
    }

    // Build all tiles with morton tiling + vertical flip.
    // The slot is a cell inside a shared strip: destination tile rows use the
    // STRIP stride plus the cell's X offset; source-space math is unchanged.
    const u64 build_start_tick = svcGetSystemTick();
    texel_t* tex_data = pool_slot_data(&texture_pool[slot_idx]);
    const int dst_tiles_x = pool_slot_strip_tiles_x(&texture_pool[slot_idx]);
    const int dst_base_tx = pool_slot_base_tx(&texture_pool[slot_idx]);

    int tiles_x = pot_w >> 3;
    int tiles_y = pot_h >> 3;

    const u8* px = (const u8*)src->pixels;
    int pal_n = pal ? pal->count : 0;
    const texel_t* pal_colors = pal ? pal->colors : NULL;

    int src_full_tx_max = src->w >> 3;
    int gy_full_min = (pot_h > src->h) ? ((int)pot_h - src->h) / 8 : 0;
    int gy_full_max = tiles_y;

    if ((src->fmt == SCE_GS_PSMT8 || src->fmt == SCE_GS_PSMT4) && pal_colors) {
        // L8 index cache: if texture indices haven't changed, fast palette
        // re-resolve. PSMT4 included: its 4-bit indices store fine in the u8
        // cache. Without this, a palette-animated 16-color effect sheet in
        // the pool (fireballs, super fx) full-rebuilt through the slow
        // resolve_texel path on EVERY palette tick — measured at 20ms per
        // rebuild (BUILDSPIKE ti=0 fmt=PSMT4), i.e. a guaranteed stutter
        // every time such a move played.
        u32 tv = (tex_idx >= 0 && tex_idx < FL_TEXTURE_MAX) ? texture_versions[tex_idx] : 0;
        L8CacheEntry* l8 = l8_cache_find(tex_idx, tv);
        if (l8 && l8->pot_w == (int)pot_w && l8->pot_h == (int)pot_h) {
            // HIT: indices cached, just re-resolve with new palette
            l8_hits++;
            l8_resolve_palette(l8, pal_colors, tex_data, tiles_x, tiles_y,
                               dst_tiles_x, dst_base_tx);
            goto l8_done;
        }

        // MISS: build normally and capture indices into L8 cache
        l8_misses++;
        L8CacheEntry* l8_new = l8_cache_alloc(tex_idx, tv, pot_w, pot_h);

        if (src->fmt == SCE_GS_PSMT4) {
            /* PSMT4 row-based build: two pixels per byte, low nibble = even x
             * (same decode as resolve_texel). Captures indices for the fast
             * path above. */
            pool_clear_slot(&texture_pool[slot_idx]);
            int w = (src->w < (int)pot_w) ? src->w : (int)pot_w;
            int h = (src->h < (int)pot_h) ? src->h : (int)pot_h;
            int full_tx = w >> 3;
            int src_half_w = src->w >> 1;
            for (int src_y = 0; src_y < h; src_y++) {
                const u8* src_row = px + src_y * src_half_w;
                int gpu_y = (int)pot_h - 1 - src_y;
                int tile_y = gpu_y >> 3;
                int fy = gpu_y & 7;
                const u8* m = morton_row[fy];
                int row_base = tile_y * tiles_x;
                int dst_row_base = tile_y * dst_tiles_x + dst_base_tx;
                for (int tx = 0; tx < full_tx; tx++) {
                    texel_t* tile_dst = &tex_data[(dst_row_base + tx) * 64];
                    const u8* s = src_row + tx * 4; /* 8 px = 4 bytes */
                    u8 i0 = s[0] & 0xF, i1 = (s[0] >> 4) & 0xF;
                    u8 i2 = s[1] & 0xF, i3 = (s[1] >> 4) & 0xF;
                    u8 i4 = s[2] & 0xF, i5 = (s[2] >> 4) & 0xF;
                    u8 i6 = s[3] & 0xF, i7 = (s[3] >> 4) & 0xF;
                    tile_dst[m[0]] = pal_colors[i0];
                    tile_dst[m[1]] = pal_colors[i1];
                    tile_dst[m[2]] = pal_colors[i2];
                    tile_dst[m[3]] = pal_colors[i3];
                    tile_dst[m[4]] = pal_colors[i4];
                    tile_dst[m[5]] = pal_colors[i5];
                    tile_dst[m[6]] = pal_colors[i6];
                    tile_dst[m[7]] = pal_colors[i7];
                    if (l8_new) {
                        u8* idx_dst = &l8_new->indices[(row_base + tx) * 64];
                        idx_dst[m[0]] = i0; idx_dst[m[1]] = i1;
                        idx_dst[m[2]] = i2; idx_dst[m[3]] = i3;
                        idx_dst[m[4]] = i4; idx_dst[m[5]] = i5;
                        idx_dst[m[6]] = i6; idx_dst[m[7]] = i7;
                    }
                }
            }
            goto l8_done;
        }

        /* Row-based single-pass: sequential source reads, no per-tile call overhead.
           Also captures L8 indices in the same pass. */
        pool_clear_slot(&texture_pool[slot_idx]);
        {
            int w = (src->w < (int)pot_w) ? src->w : (int)pot_w;
            int h = (src->h < (int)pot_h) ? src->h : (int)pot_h;
            int full_tx = w >> 3;

            for (int src_y = 0; src_y < h; src_y++) {
                const u8* src_row = px + src_y * src->w;
                int gpu_y = (int)pot_h - 1 - src_y;
                int tile_y = gpu_y >> 3;
                int fy = gpu_y & 7;
                const u8* m = morton_row[fy];
                int row_base = tile_y * tiles_x; /* SOURCE-space tile row (tile_dirty + l8 layout) */
                int dst_row_base = tile_y * dst_tiles_x + dst_base_tx;

                for (int tx = 0; tx < full_tx; tx++) {
                    texel_t* tile_dst = &tex_data[(dst_row_base + tx) * 64];
                    int tidx = row_base + tx;
#ifdef __3DS__
                    /* Only build tiles marked active by region updates.
                       Seqs textures (has_region_updates=true): stale tiles → zero.
                       Non-seqs textures: full build all tiles normally. */
                    if (gpu_cache_prefer_pool && src->has_region_updates &&
                        tidx < TILE_DIRTY_WORDS * 32 &&
                        !(src->tile_dirty[tidx >> 5] & (1u << (tidx & 31)))) {
                        memset(tile_dst, 0, 64 * sizeof(texel_t));
                        continue;
                    }
#endif
                    const u8* s = src_row + tx * 8;
                    tile_dst[m[0]] = pal_colors[s[0]];
                    tile_dst[m[1]] = pal_colors[s[1]];
                    tile_dst[m[2]] = pal_colors[s[2]];
                    tile_dst[m[3]] = pal_colors[s[3]];
                    tile_dst[m[4]] = pal_colors[s[4]];
                    tile_dst[m[5]] = pal_colors[s[5]];
                    tile_dst[m[6]] = pal_colors[s[6]];
                    tile_dst[m[7]] = pal_colors[s[7]];
                    if (l8_new) {
                        u8* idx_dst = &l8_new->indices[(row_base + tx) * 64];
                        idx_dst[m[0]] = s[0]; idx_dst[m[1]] = s[1];
                        idx_dst[m[2]] = s[2]; idx_dst[m[3]] = s[3];
                        idx_dst[m[4]] = s[4]; idx_dst[m[5]] = s[5];
                        idx_dst[m[6]] = s[6]; idx_dst[m[7]] = s[7];
                    }
                }
            }
        }
        /* tex_data was memset to 0 above — edge/padding tiles already clear */
        (void)gy_full_min; (void)gy_full_max; (void)src_full_tx_max;
l8_done:
        (void)0; // label needs statement
    } else {
        // General path for PSMT4/PSMCT16/other
        pool_clear_slot(&texture_pool[slot_idx]);
        for (int ty = 0; ty < tiles_y; ty++) {
            for (int tx = 0; tx < tiles_x; tx++) {
                int src_tile_x = tx * 8;
                int src_tile_y = (int)pot_h - 8 - ty * 8;
                int tile_idx = ty * dst_tiles_x + dst_base_tx + tx;
                texel_t* tile_dst = TILE_AT(tex_data, tile_idx);
                for (int fy = 0; fy < 8; fy++) {
                    int sy = src_tile_y + fy;
                    int dy_fine = 7 - fy;
                    for (int fx = 0; fx < 8; fx++) {
                        int sx = src_tile_x + fx;
                        texel_t color = 0;
                        if (sx >= 0 && sx < src->w && sy >= 0 && sy < src->h) {
                            color = resolve_texel(src, pal, sx, sy);
                        }
                        tile_dst[morton_lut[dy_fine][fx]] = color;
                    }
                }
            }
        }
    }

    const u64 flush_start_tick = svcGetSystemTick();
    pool_mark_slot_dirty(&texture_pool[slot_idx]); /* flush deferred to frame consumer */
    const u64 create_end_tick = svcGetSystemTick();

pool_skip_build:
    entry->texture_index = tex_idx;
    entry->palette_index = pal_idx;
    entry->pool_slot = slot_idx;
    entry->atlas_cell = -1;
    entry->tex_version = (tex_idx >= 0 && tex_idx < FL_TEXTURE_MAX) ? texture_versions[tex_idx] : 0;
    entry->pal_version = (pal_idx >= 0 && pal_idx < FL_PALETTE_MAX) ? palette_versions[pal_idx] : 0;
    entry->pot_w = pot_w;
    entry->pot_h = pot_h;
    entry->src_w = src->w;
    entry->src_h = src->h;
    entry->allocated = true;
    entry->dirty = false;
    entry->pending_delete = false;
    entry->last_used_frame = frame_number;
    entry->src_empty_at_build = src_effectively_empty(src);

    entry->pinned = false;

    /* Update hash index */
    { int slot_i = (int)(entry - gpu_cache); cache_hash_insert(tex_idx, pal_idx, slot_i); }

    cache_create_count_total++;
    if (!gpu_cache_skip_full_build) {
        cache_create_ticks_total += create_end_tick - create_start_tick;
        cache_create_ticks_build += flush_start_tick - build_start_tick;
        cache_create_ticks_flush += create_end_tick - flush_start_tick;
        { /* Animation-stutter triage probe: one synchronous build taking
           * >=4ms inside a frame is a stutter by itself. Silent otherwise. */
            double ms = (double)(create_end_tick - create_start_tick) * 1000.0 / SYSCLOCK_ARM11;
            if (ms >= 4.0) {
                extern void debug_print(const char *fmt, ...);
                debug_print("BUILDSPIKE %.1fms ti=%d %dx%d fmt=%d", ms, tex_idx,
                            (int)pot_w, (int)pot_h, (int)src->fmt);
            }
        }
    }
    switch (src->fmt) {
    case SCE_GS_PSMT8:
        cache_create_count_psmt8++;
        break;
    case SCE_GS_PSMT4:
        cache_create_count_psmt4++;
        break;
    case SCE_GS_PSMCT16:
        cache_create_count_psmct16++;
        break;
    default:
        cache_create_count_other++;
        break;
    }

    return entry;
}

// Mark cache entries for deferred deletion (actual delete in EndFrame)
// This prevents render tasks from referencing deleted GPU textures.
static void cache_invalidate_texture(int tex_idx) {
    cache_invalidate_texture_calls++;
    for (int i = 0; i < CACHE_MAX; i++) {
        CacheEntry* e = &gpu_cache[i];
        if (e->allocated && e->texture_index == tex_idx) {
            /* pinned entries still get invalidated normally */
            e->pending_delete = true;
            cache_invalidate_texture_entries++;
        }
    }
}

static void cache_invalidate_palette(int pal_idx) {
    /* Palette destroy is LAZY — entries survive it untouched. The game's
     * palette-cycling content (char-select background, stage backdrops,
     * score screen) destroys and recreates the same palette handle every
     * frame. Eagerly pending_delete-ing here meant every such entry died at
     * EndFrame and was recreated from scratch the next frame — a fresh
     * metadata slot + a fresh atlas cell + a full 256x256 build per pair per
     * frame, exhausting the 16 atlas cells mid-frame and spilling every
     * background chip into the (unbatched, one-draw-call-per-chip) pool.
     * Worse, the EndFrame flush of the dead entry removed the (tex,pal)
     * hash mapping that cache_create had just re-pointed at the NEW entry,
     * making the replacement unfindable — the next frame repeated the whole
     * cycle. (That churn was the real mechanism behind the pt24q "missing/
     * fragmented backgrounds": pool slot exhaustion silently dropped chips.)
     *
     * Correctness without the eager kill:
     *  - src_palettes[idx] keeps last-known-good colors (lazy release in
     *    SDLGameRenderer_DestroyPalette), so a bind in the destroy→recreate
     *    window still renders correctly.
     *  - The recreate (SDLGameRenderer_CreatePalette) content-compares and
     *    bumps palette_versions only on REAL change; cache_find_clean then
     *    takes the L8 fast re-resolve for atlas entries, and
     *    cache_mark_palette_dirty triggers the pool-entry rebuild.
     *  - A destroyed-and-never-recreated palette leaves entries that simply
     *    stop being bound; normal LRU (metadata) and staleness (atlas cell)
     *    eviction reclaims them. */
    cache_invalidate_palette_calls++;
}

// Actually free pending-delete entries (called from EndFrame after rendering)
static void cache_flush_pending(void) {
    for (int i = 0; i < CACHE_MAX; i++) {
        CacheEntry* e = &gpu_cache[i];
        if (e->allocated && e->pending_delete) {
            cache_hash_remove(e->texture_index, e->palette_index);
            if (e->atlas_cell >= 0) {
                atlas_free_cell(e->atlas_cell);
                e->atlas_cell = -1;
            }
            pool_release_slot(e->pool_slot);
            e->allocated = false;
            e->dirty = false;
            e->pinned = false;
            e->pending_delete = false;
            e->pool_slot = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// Render task queue
// ---------------------------------------------------------------------------

typedef struct {
    // Texture cache entry (NULL for solid quads)
    CacheEntry* cache_entry;

    // 4-vertex quad geometry (positions)
    float px[4];
    float py[4];

    // 4-vertex UVs (normalized 0..1 relative to source texture)
    float us[4];
    float vs[4];

    // Per-vertex color (same for all 4 in current usage)
    u32 vertex_colors[4];

    // Sorting keys
    float z;
    int submission_index;

    // Is this a textured or solid quad?
    bool textured;

    /* Identity stamp at push time: cache entries can be recycled for a
       different (tex,pal) between task queueing and the frame flush (entry
       eviction under churn). resolve_task_texture compares and drops the
       quad rather than sampling whatever content the entry now holds. */
    s16 dbg_tex;
    s16 dbg_pal;
} RenderTask;

int dbg_task_repurposed = 0; /* mid-frame entry recycling counter (profile) */

static RenderTask render_tasks[RENDER_TASK_MAX];
static int render_task_count = 0;

// Sort indices — sort lightweight u16 indices instead of 100-byte RenderTask structs
static u16 sort_indices[RENDER_TASK_MAX];
static u16 sort_indices_tmp[RENDER_TASK_MAX];

// Pending texture creation queue (processed outside GPU frame via ProcessPending)
#define PENDING_MAX 128
typedef struct { int ti, pi; } PendingEntry;
static PendingEntry pending[PENDING_MAX];
static int pending_n = 0;
int SDLGameRenderer_GetPendingCount(void) { return pending_n; }

/* Combined CPS3→Morton one-pass tile upload.
   Reads CPS3-swizzled 8bpp pixels, resolves palette, writes morton-tiled
   RGBA directly to the GPU cache entry. Skips the srcAdrs intermediate.

   tile_w/tile_h: 8, 16, or 32 (the tile dimensions)
   cps3_data: LZ-decompressed tile in CPS3 swizzle order
   tex_handle: the texture handle (1-based)
   pixel_x, pixel_y: position within the 256x256 texture page
*/
extern s16* dctex_linear; /* CPS3 unswizzle LUT from texcash.c */

/* Zero GPU cache texture data for all entries matching tex_idx.
   Used when character switches on char select — clears stale tile data
   so unused slots render as transparent instead of previous character. */
void SDLGameRenderer_ClearTileDirty(unsigned int th) {
    int idx = (int)th - 1;
    if (idx >= 0 && idx < FL_TEXTURE_MAX && src_textures[idx].valid) {
        memset(src_textures[idx].tile_dirty, 0, sizeof(src_textures[idx].tile_dirty));
    }
}

void SDLGameRenderer_ZeroCacheTexture(unsigned int th) {
    int tex_idx = (int)th - 1;
    if (tex_idx < 0 || tex_idx >= FL_TEXTURE_MAX) return;
    for (int ci = 0; ci < CACHE_MAX; ci++) {
        CacheEntry* e = &gpu_cache[ci];
        if (!e->allocated || e->pending_delete) continue;
        if (e->texture_index != tex_idx) continue;
        if (e->pool_slot >= 0 && e->pool_slot < TEXTURE_POOL_SLOT_COUNT) {
            TexturePoolSlot* slot = &texture_pool[e->pool_slot];
            if (slot->initialized) {
                pool_clear_slot(slot);
                pool_mark_slot_dirty(slot);
            }
        } else if (e->atlas_cell >= 0) {
            int ct = ATLAS_CELL_SIZE >> 3;
            int strip = atlas_cell_strip(e->atlas_cell);
            int cell_in = atlas_cell_in_strip(e->atlas_cell);
            int stx = (ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE) >> 3;
            texel_t* sd = (texel_t*)atlas.strip_tex[strip].data;
            for (int ty = 0; ty < ct; ty++) {
                int rb = ty * stx + cell_in * ct;
                memset(&sd[rb * 64], 0, ct * 64 * sizeof(texel_t));
            }
            /* the L8 index cache still holds the pre-zero content — a
             * palette-only resolve would resurrect it */
            atlas_l8_valid[e->atlas_cell] = false;
            atlas_mark_strip_dirty(strip);
        }
    }
}

void SDLGameRenderer_DirectTileUpload(unsigned int tex_handle, int pal_handle,
                                       const u8* cps3_data, int tile_w, int tile_h,
                                       int pixel_x, int pixel_y) {
    int tex_idx = (int)tex_handle - 1;
    if (tex_idx < 0 || tex_idx >= FL_TEXTURE_MAX) return;

    int pal_idx = pal_handle > 0 ? pal_handle - 1 : -1;

    /* Find or create the GPU cache entry */
    CacheEntry* entry = cache_find(tex_idx, pal_idx);
    if (!entry) {
        entry = cache_create(tex_idx, pal_idx);
        if (!entry) return;
    }

    /* Get the destination GPU texture data */
    texel_t* tex_data;
    int data_tiles_x;
    int pot_h;

    int base_tx2 = 0, base_ty2 = 0;
    if (entry->atlas_cell >= 0) {
        int cell = entry->atlas_cell;
        if (cell < 0 || cell >= atlas_cell_count || !atlas.cell_init[cell]) return;
        tex_data = (texel_t*)atlas_cell_tex(cell)->data;
        int strip_w = ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE;
        data_tiles_x = strip_w >> 3;
        base_tx2 = atlas_cell_in_strip(cell) * (ATLAS_CELL_SIZE >> 3);
        base_ty2 = 0; /* single-row strips */
        pot_h = (int)entry->pot_h;
    } else if (entry->pool_slot >= 0) {
        TexturePoolSlot* slot = &texture_pool[entry->pool_slot];
        /* slot = a cell of a shared strip: strip stride + cell X offset */
        tex_data = pool_slot_data(slot);
        data_tiles_x = pool_slot_strip_tiles_x(slot);
        base_tx2 = pool_slot_base_tx(slot);
        base_ty2 = 0;
        pot_h = (int)slot->pot_h;
    } else {
        return;
    }

    /* Get palette */
    const texel_t* pal_colors = NULL;
    if (pal_idx >= 0 && pal_idx < FL_PALETTE_MAX && src_palettes[pal_idx].valid) {
        pal_colors = src_palettes[pal_idx].colors;
    }
    if (!pal_colors) return;

    int tiles_y = pot_h >> 3;

    /* For each 8x8 GPU tile covered by this CPS3 tile */
    int gpu_tiles_w = tile_w >> 3;  /* 1, 2, or 4 */
    int gpu_tiles_h = tile_h >> 3;

    for (int ty = 0; ty < gpu_tiles_h; ty++) {
        for (int tx = 0; tx < gpu_tiles_w; tx++) {
            int src_x = pixel_x + tx * 8;
            int src_y = pixel_y + ty * 8;

            /* GPU tile index (Y flipped) */
            int gpu_ty = (tiles_y - 1) - (src_y >> 3);
            int gpu_tx = src_x >> 3;
            int tile_idx = (base_ty2 + gpu_ty) * data_tiles_x + (base_tx2 + gpu_tx);
            texel_t* tile_dst = &tex_data[tile_idx * 64];

            /* Read 8x8 pixels from CPS3 data using dctex_linear LUT,
               apply palette, write to morton order in one pass */
            for (int fy = 0; fy < 8; fy++) {
                int page_y = ty * 8 + fy;
                const u8* m = morton_row[7 - fy];
                for (int fx = 0; fx < 8; fx++) {
                    int page_x = tx * 8 + fx;
                    /* dctex_linear maps (page_x, page_y) → CPS3 source index */
                    int cps3_idx = dctex_linear[page_x + (page_y << 5)];
                    u8 pixel = cps3_data[cps3_idx];
                    tile_dst[m[fx]] = pal_colors[pixel];
                }
            }
        }
    }

    /* Flush the GPU texture */
    if (entry->atlas_cell >= 0) {
        atlas_mark_strip_dirty(atlas_cell_strip(entry->atlas_cell)); /* deferred flush in RenderFrame */
        /* CPS3 direct writes bypass the L8 index cache — indices for the
         * touched tiles are now stale; next palette resolve would revert
         * them. (This path targets PSMT8 sheets, but its own morton write
         * above doesn't refresh l8, so invalidate.) */
        if (atlas_l8_valid[entry->atlas_cell]) {
            u8* l8 = atlas_l8[entry->atlas_cell];
            if (l8) {
                /* keep it valid by patching the same tiles into the index
                 * cache — cheap (one u8 store per texel), preserves the
                 * fast palette path for melt-heavy sheets. Mirrors the texel
                 * write above EXACTLY (same tiles_y flip base, same morton
                 * rows) — the l8 array is cell-relative and base_ty2==0, so
                 * gpu_ty indexes the same tile row in both. */
                for (int ty = 0; ty < gpu_tiles_h; ty++) {
                    for (int tx = 0; tx < gpu_tiles_w; tx++) {
                        int src_x = pixel_x + tx * 8;
                        int src_y = pixel_y + ty * 8;
                        int gpu_ty = (tiles_y - 1) - (src_y >> 3);
                        int gpu_tx = src_x >> 3;
                        u8* l8_tile = &l8[(gpu_ty * (ATLAS_CELL_SIZE >> 3) + gpu_tx) * 64];
                        for (int fy = 0; fy < 8; fy++) {
                            int page_y = ty * 8 + fy;
                            const u8* m = morton_row[7 - fy];
                            for (int fx = 0; fx < 8; fx++) {
                                int page_x = tx * 8 + fx;
                                int cps3_idx = dctex_linear[page_x + (page_y << 5)];
                                l8_tile[m[fx]] = cps3_data[cps3_idx];
                            }
                        }
                    }
                }
            } else {
                atlas_l8_valid[entry->atlas_cell] = false;
            }
        }
    } else if (entry->pool_slot >= 0) {
        pool_mark_slot_dirty(&texture_pool[entry->pool_slot]);
    }

    entry->dirty = false;
    entry->last_used_frame = frame_number;
}

void SDLGameRenderer_RedirectTexturePixels(unsigned int th, const void* pixels) {
    int idx = (int)th - 1;
    if (idx >= 0 && idx < FL_TEXTURE_MAX && src_textures[idx].valid) {
        src_textures[idx].pixels = pixels;
    }
}
static int pending_last_created = 0;
static double pending_last_ms = 0.0;

static bool pending_contains(int ti, int pi) {
    for (int i = 0; i < pending_n; i++) {
        if (pending[i].ti == ti && pending[i].pi == pi) {
            return true;
        }
    }
    return false;
}

static void queue_pending_texture(int ti, int pi) {
    if (ti < 0 || ti >= FL_TEXTURE_MAX) return;
    if (pending_n >= PENDING_MAX) return;
    if (pending_contains(ti, pi)) return;
    pending[pending_n].ti = ti;
    pending[pending_n].pi = pi;
    pending_n++;
}

static void pending_remove_texture(int ti) {
    int out = 0;
    for (int i = 0; i < pending_n; i++) {
        if (pending[i].ti == ti) continue;
        pending[out++] = pending[i];
    }
    pending_n = out;
}

static void pending_remove_palette(int pi) {
    int out = 0;
    for (int i = 0; i < pending_n; i++) {
        if (pending[i].pi == pi) continue;
        pending[out++] = pending[i];
    }
    pending_n = out;
}

static void cache_mark_texture_dirty(int tex_idx) {
    for (int i = 0; i < CACHE_MAX; i++) {
        CacheEntry* e = &gpu_cache[i];
        if (!e->allocated || e->pending_delete) continue;
        if (e->texture_index != tex_idx) continue;
        /* pinned entries still get dirty — pinning only prevents eviction */
        e->dirty = true;
        queue_pending_texture(e->texture_index, e->palette_index);
    }
}

static void cache_mark_palette_dirty(int pal_idx) {
    for (int i = 0; i < CACHE_MAX; i++) {
        CacheEntry* e = &gpu_cache[i];
        if (!e->allocated || e->pending_delete) continue;
        if (e->palette_index != pal_idx) continue;
        e->dirty = true;
        queue_pending_texture(e->texture_index, e->palette_index);
    }
}

static void cache_set_pinned(int tex_idx, int pal_idx, bool pinned) {
    CacheEntry* e = cache_find(tex_idx, pal_idx);
    if (!e) return;
    e->pinned = pinned;
}

// ---------------------------------------------------------------------------
// Sorting: z ascending, then submission_index descending for equal z
// ---------------------------------------------------------------------------

// Return a sortable key that groups quads by GPU texture.
// All atlas entries on a strip share one texture → same key → one batch.
// Pool entries now share per-bucket STRIP textures too, so same-bucket pool
// quads (montage tiles, overflow bg chips) also collapse into one draw call.
static inline uintptr_t task_texture_key(const RenderTask* t) {
    CacheEntry* ce = t->cache_entry;
    if (!ce || !ce->allocated) return 0;
    if (ce->atlas_cell >= 0 && ce->atlas_cell < atlas_cell_count)
        return (uintptr_t)atlas_cell_tex(ce->atlas_cell);
    if (ce->pool_slot >= 0 && ce->pool_slot < TEXTURE_POOL_SLOT_COUNT)
        return (uintptr_t)pool_slot_tex(&texture_pool[ce->pool_slot]);
    return 0;
}

static inline bool render_task_precedes(const RenderTask* ta, const RenderTask* tb) {
    if (ta->z < tb->z) return true;
    if (ta->z > tb->z) return false;

    // Equal z: group by GPU texture to minimize draw calls (texture switches).
    // Same-z quads are at the same depth layer — reordering is safe.
    uintptr_t ka = task_texture_key(ta);
    uintptr_t kb = task_texture_key(tb);
    if (ka != kb) return ka < kb;

    // Same z, same texture: preserve painter's order.
    return ta->submission_index > tb->submission_index;
}

static void sort_render_tasks(int count) {
    if (count <= 1) {
        if (count == 1) sort_indices[0] = 0;
        return;
    }

    // Initialize index array
    for (int i = 0; i < count; i++) sort_indices[i] = (u16)i;

    // Merge sort on lightweight u16 indices (~2 bytes each vs ~100 byte RenderTask)
    u16* src_idx = sort_indices;
    u16* dst_idx = sort_indices_tmp;

    for (int width = 1; width < count; width <<= 1) {
        for (int left = 0; left < count; left += width << 1) {
            int mid = left + width;
            int right = left + (width << 1);
            if (mid > count) mid = count;
            if (right > count) right = count;

            int i = left;
            int j = mid;
            int k = left;

            while (i < mid && j < right) {
                if (render_task_precedes(&render_tasks[src_idx[i]], &render_tasks[src_idx[j]])) {
                    dst_idx[k++] = src_idx[i++];
                } else {
                    dst_idx[k++] = src_idx[j++];
                }
            }
            while (i < mid) dst_idx[k++] = src_idx[i++];
            while (j < right) dst_idx[k++] = src_idx[j++];
        }

        u16* tmp = src_idx;
        src_idx = dst_idx;
        dst_idx = tmp;
    }

    // Ensure result is in sort_indices
    if (src_idx != sort_indices) {
        memcpy(sort_indices, src_idx, (size_t)count * sizeof(u16));
    }
}

// ---------------------------------------------------------------------------
// Internal: push a textured render task
// ---------------------------------------------------------------------------

static bool quad_is_culled(const float px[4], const float py[4]) {
    float min_x = px[0];
    float max_x = px[0];
    float min_y = py[0];
    float max_y = py[0];

    for (int i = 1; i < 4; i++) {
        if (px[i] < min_x) min_x = px[i];
        if (px[i] > max_x) max_x = px[i];
        if (py[i] < min_y) min_y = py[i];
        if (py[i] > max_y) max_y = py[i];
    }

    if (max_x <= 0.0f || max_y <= 0.0f) return true;
    if (min_x >= (float)CPS3_WIDTH || min_y >= (float)CPS3_HEIGHT) return true;
    if (max_x - min_x <= 0.0f || max_y - min_y <= 0.0f) return true;

    return false;
}

static void push_textured_task(
    const float px[4], const float py[4],
    const float us[4], const float vs[4],
    float z_raw, u32 color)
{
    if (No_Trans) return;
    if (render_task_count >= RENDER_TASK_MAX) return;
    if (current_texture_index < 0) return;
    if (quad_is_culled(px, py)) return;

    // Use pre-resolved cache entry from SetTexture (O(1) — no scan)
    CacheEntry* ce = current_cache_entry;
    if (!ce) return; // Texture not cached yet — queued in SetTexture, skip this frame

    RenderTask* task = &render_tasks[render_task_count];
    task->cache_entry = ce;
    task->textured = true;
    task->z = flPS2ConvScreenFZ(z_raw);
    task->submission_index = render_task_count;
    task->dbg_tex = (s16)current_texture_index;
    task->dbg_pal = (s16)current_palette_index;

    for (int i = 0; i < 4; i++) {
        task->px[i] = px[i];
        task->py[i] = py[i];
        task->us[i] = us[i];
        task->vs[i] = vs[i];
        task->vertex_colors[i] = color;
    }

    render_task_count++;
}

// ---------------------------------------------------------------------------
// Internal: push a solid (untextured) render task
// ---------------------------------------------------------------------------

static void push_solid_task(
    const float px[4], const float py[4],
    float z_raw, u32 color)
{
    if (No_Trans) return;
    if (render_task_count >= RENDER_TASK_MAX) return;
    if (quad_is_culled(px, py)) return;

    RenderTask* task = &render_tasks[render_task_count];
    task->cache_entry = NULL;
    task->textured = false;
    task->z = flPS2ConvScreenFZ(z_raw);
    task->submission_index = render_task_count;
    task->dbg_tex = -2; /* solid quad marker (diagnostics) */
    task->dbg_pal = -2;

    for (int i = 0; i < 4; i++) {
        task->px[i] = px[i];
        task->py[i] = py[i];
        task->us[i] = 0.0f;
        task->vs[i] = 0.0f;
        task->vertex_colors[i] = color;
    }

    render_task_count++;
}

// ---------------------------------------------------------------------------
// citro3d immediate mode renderer (replaces citro2d for textured quads)
// ---------------------------------------------------------------------------

#include "render2d_shbin.h"

static DVLB_s* imm_dvlb = NULL;
static shaderProgram_s imm_program;
static int imm_uLoc_projection;
static C3D_Mtx imm_projection;
static C3D_AttrInfo imm_attrInfo;
static bool imm_ready = false;

// 1x1 white texture for solid quads
static C3D_Tex white_tex;
static bool white_tex_ready = false;

// ---------------------------------------------------------------------------
// Batched vertex buffer renderer
// ---------------------------------------------------------------------------

#define BATCH_MAX_QUADS 1024

typedef struct {
    float x, y, z;      // position
    float u, v;          // texcoord
    float r, g, b, a;    // color (0.0-1.0)
} BatchVertex;

static BatchVertex* batch_vtx_buf = NULL;  // linearAlloc'd, GPU-visible
static u16* batch_idx_buf = NULL;           // linearAlloc'd, GPU-visible

/* Dedicated small buffer for SDLGameRenderer_DrawRawQuadToTarget — MUST NOT
 * share batch_vtx_buf/batch_idx_buf. GPU command processing is asynchronous:
 * by the time this function runs (called from endFrame, after this same
 * frame's main SDLGameRenderer_RenderFrame() already submitted the top
 * screen's whole quad batch via C3D_DrawElements referencing batch_vtx_buf),
 * the GPU may not have actually READ that data yet — command submission
 * just queues a pointer + count, it doesn't copy. Overwriting the shared
 * buffer here raced that read and corrupted the top screen's geometry. */
static BatchVertex* raw_quad_vtx_buf = NULL;
static u16* raw_quad_idx_buf = NULL;

/* Dedicated ring of quad slots for SDLGameRenderer_DrawGlyphQuad, which
 * (unlike the single-draw-per-frame raw_quad_*_buf above) is called many
 * times per frame — up to 8 button rows per player. Sharing one 4-vertex
 * buffer across those calls hit the exact same async-GPU-read race described
 * above, just within a single frame: each memcpy could clobber a still-unread
 * earlier glyph's vertices before the GPU got to them. Each call gets its own
 * slot instead, sized generously above the ~16 glyphs a frame could plausibly
 * draw (2 players x 8 rows). */
#define GLYPH_QUAD_SLOTS 32
static BatchVertex* glyph_quad_vtx_buf = NULL; /* GLYPH_QUAD_SLOTS * 4 verts */
static u16* glyph_quad_idx_buf = NULL;         /* GLYPH_QUAD_SLOTS * 6 indices */
static int glyph_quad_slot = 0;

static void imm_init(void) {
    imm_dvlb = DVLB_ParseFile((u32*)render2d_shbin, render2d_shbin_len);
    shaderProgramInit(&imm_program);
    shaderProgramSetVsh(&imm_program, &imm_dvlb->DVLE[0]);
    imm_uLoc_projection = shaderInstanceGetUniformLocation(
        imm_program.vertexShader, "projection");

    AttrInfo_Init(&imm_attrInfo);
    AttrInfo_AddLoader(&imm_attrInfo, 0, GPU_FLOAT, 3); // position
    AttrInfo_AddLoader(&imm_attrInfo, 1, GPU_FLOAT, 2); // texcoord
    AttrInfo_AddLoader(&imm_attrInfo, 2, GPU_FLOAT, 4); // color

    // Orthographic projection: center 384x224 game area on 400x240 screen
    // Game visible area is 336x200, centered with offsets:
    //   X: (400 - 384) / 2 = 8  (or (400-336)/2 = 32 for visible area)
    //   Y: (240 - 224) / 2 = 8  (or (240-200)/2 = 20 for visible area)
    // Map game coord (0,0) → screen (8,8), no scaling (1:1 pixels)
    float offset_x = (400.0f - 384.0f) / 2.0f;  // 8px
    float offset_y = (240.0f - 224.0f) / 2.0f;  // 8px
    Mtx_OrthoTilt(&imm_projection,
        -offset_x, 400.0f - offset_x,    // left, right: game X range
        224.0f + offset_y, -offset_y,     // bottom, top: game Y range (inverted for screen-down)
        0.0f, 1.0f, true);

    // Create 1x1 white texture for solid quads
    if (C3D_TexInit(&white_tex, 8, 8, GPU_TEX_FMT)) {
        texel_t* data = (texel_t*)white_tex.data;
        for (int i = 0; i < 64; i++) data[i] = (texel_t)0xFFFF; // White opaque
        C3D_TexFlush(&white_tex);
        C3D_TexSetFilter(&white_tex, GPU_NEAREST, GPU_NEAREST);
        white_tex_ready = true;
    }

    imm_ready = true;
}

static void imm_bind(void) {
    if (!imm_ready) return;

    C3D_BindProgram(&imm_program);
    C3D_SetAttrInfo(&imm_attrInfo);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, imm_uLoc_projection, &imm_projection);

    // TexEnv: output = texture_color * vertex_color (GPU_MODULATE)
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    // Disable extra texenv stages
    C3D_TexEnv* env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, 0, 0);
    C3D_TexEnvFunc(env1, C3D_Both, GPU_REPLACE);

    // Alpha blending
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(true, GPU_GREATER, 0);

    // Scissor: clip to game area (centered on 400x240 screen)
    // Physical framebuffer is 240x400 (rotated 90° CCW)
    // Logical rect (8, 8) to (392, 232) maps to physical:
    //   phy_left = 240 - logical_bottom = 240 - 232 = 8
    //   phy_top = logical_left = 8
    //   phy_right = 240 - logical_top = 240 - 8 = 232
    //   phy_bottom = logical_right = 392
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 8, 8, 232, 392);
}

// ---------------------------------------------------------------------------
// Internal: resolve a task's texture pointer (NULL = skip)
// ---------------------------------------------------------------------------

static C3D_Tex* resolve_task_texture(const RenderTask* task,
                                      float u_out[4], float v_out[4]) {
    if (task->textured) {
        CacheEntry* ce = task->cache_entry;
        if (!ce || !ce->allocated) return NULL;

        /* Entry recycled for a different (tex,pal) since the task was
           queued — drop the quad rather than sampling the wrong content. */
        if (ce->texture_index != task->dbg_tex || ce->palette_index != task->dbg_pal) {
            dbg_task_repurposed++;
            return NULL;
        }

        if (ce->atlas_cell >= 0) {
            // Shared atlas: remap UVs to the correct cell quadrant.
            int cell = ce->atlas_cell;
            if (cell >= atlas_cell_count || !atlas.cell_init[cell]) return NULL;
            float u_off, v_off;
            atlas_cell_uv_offset(cell, &u_off, &v_off);
            int strip_w = ATLAS_CELLS_PER_STRIP * ATLAS_CELL_SIZE;
            float uscale = (float)ce->src_w / (float)strip_w;
            float vscale = (float)ce->src_h / (float)ATLAS_CELL_SIZE;
            for (int i = 0; i < 4; i++) {
                u_out[i] = u_off + task->us[i] * uscale;
                v_out[i] = v_off + task->vs[i] * vscale;
            }
            return atlas_cell_tex(cell);
        }

        // Pool path — slot is a cell of a shared strip: remap U into the
        // cell's horizontal sub-range (V unchanged, strip height == pot_h).
        if (ce->pool_slot < 0 || ce->pool_slot >= TEXTURE_POOL_SLOT_COUNT) return NULL;
        TexturePoolSlot* slot = &texture_pool[ce->pool_slot];
        if (!slot->initialized) return NULL;
        PoolStrip* st = &pool_strips[slot->strip];
        if (!st->initialized) return NULL;

        float u_off = (float)((u32)slot->cell * slot->pot_w) / (float)st->strip_w;
        float uscale = (float)ce->src_w / (float)st->strip_w;
        float vscale = (float)ce->src_h / (float)ce->pot_h;
        for (int i = 0; i < 4; i++) {
            u_out[i] = u_off + task->us[i] * uscale;
            v_out[i] = task->vs[i] * vscale;
        }
        return &st->tex;
    } else {
        if (!white_tex_ready) return NULL;
        for (int i = 0; i < 4; i++) {
            u_out[i] = 0.0f;
            v_out[i] = 0.0f;
        }
        return &white_tex;
    }
}

// ---------------------------------------------------------------------------
// Internal: write one quad (4 verts + 6 indices) into the batch buffers
// ---------------------------------------------------------------------------

// Pre-computed byte-to-float table: avoids per-quad division by 255.0
static float byte_to_float[256];
static bool byte_to_float_ready = false;

static void init_byte_to_float(void) {
    for (int i = 0; i < 256; i++) byte_to_float[i] = (float)i / 255.0f;
    byte_to_float_ready = true;
}

static void batch_write_quad(int quad_idx,
                              const float px[4], const float py[4],
                              const float u[4], const float v[4],
                              u32 color) {
    float cr = byte_to_float[(color >> 16) & 0xFF];
    float cg = byte_to_float[(color >>  8) & 0xFF];
    float cb = byte_to_float[ color        & 0xFF];
    float ca = byte_to_float[(color >> 24) & 0xFF];

    int vbase = quad_idx * 4;
    BatchVertex* vp = &batch_vtx_buf[vbase];

    // Unrolled — avoid loop overhead on ARM
    vp[0].x = px[0]; vp[0].y = py[0]; vp[0].z = 0.5f;
    vp[0].u = u[0];  vp[0].v = v[0];
    vp[0].r = cr; vp[0].g = cg; vp[0].b = cb; vp[0].a = ca;

    vp[1].x = px[1]; vp[1].y = py[1]; vp[1].z = 0.5f;
    vp[1].u = u[1];  vp[1].v = v[1];
    vp[1].r = cr; vp[1].g = cg; vp[1].b = cb; vp[1].a = ca;

    vp[2].x = px[2]; vp[2].y = py[2]; vp[2].z = 0.5f;
    vp[2].u = u[2];  vp[2].v = v[2];
    vp[2].r = cr; vp[2].g = cg; vp[2].b = cb; vp[2].a = ca;

    vp[3].x = px[3]; vp[3].y = py[3]; vp[3].z = 0.5f;
    vp[3].u = u[3];  vp[3].v = v[3];
    vp[3].r = cr; vp[3].g = cg; vp[3].b = cb; vp[3].a = ca;

    // 6 indices: two triangles (0,1,2) (1,3,2)
    int ibase = quad_idx * 6;
    u16 vb = (u16)vbase;
    batch_idx_buf[ibase + 0] = vb;
    batch_idx_buf[ibase + 1] = vb + 1;
    batch_idx_buf[ibase + 2] = vb + 2;
    batch_idx_buf[ibase + 3] = vb + 1;
    batch_idx_buf[ibase + 4] = vb + 3;
    batch_idx_buf[ibase + 5] = vb + 2;
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

void SDLGameRenderer_Init(void) {
    memset(src_textures, 0, sizeof(src_textures));
    memset(src_palettes, 0, sizeof(src_palettes));
    memset(gpu_cache, 0, sizeof(gpu_cache));
    memset(cache_hash, 0, sizeof(cache_hash));
    memset(texture_pool, 0, sizeof(texture_pool));
    memset(texture_versions, 0, sizeof(texture_versions));
    memset(palette_versions, 0, sizeof(palette_versions));
    frame_number = 0;
    render_task_count = 0;
    for (int i = 0; i < CACHE_MAX; i++) {
        gpu_cache[i].pool_slot = -1;
        gpu_cache[i].atlas_cell = -1;
    }
    staging_buf = (texel_t*)linearAlloc(STAGING_MAX * sizeof(texel_t));
    if (!staging_buf) printf("FATAL: staging_buf alloc failed!\n");
    if (!morton_lut_ready) init_morton_lut();
    init_morton_rows();
    if (!byte_to_float_ready) init_byte_to_float();
    imm_init();

    // Reserve the GPU-visible command buffers before texture pool init.
    // They are mandatory for rendering; the pool can degrade gracefully via cfail.
    batch_vtx_buf = (BatchVertex*)linearAlloc(BATCH_MAX_QUADS * 4 * sizeof(BatchVertex));
    batch_idx_buf = (u16*)linearAlloc(BATCH_MAX_QUADS * 6 * sizeof(u16));
    if (!batch_vtx_buf || !batch_idx_buf)
        printf("FATAL: batch buffer alloc failed!\n");

    raw_quad_vtx_buf = (BatchVertex*)linearAlloc(4 * sizeof(BatchVertex));
    raw_quad_idx_buf = (u16*)linearAlloc(6 * sizeof(u16));
    if (!raw_quad_vtx_buf || !raw_quad_idx_buf)
        printf("FATAL: raw quad buffer alloc failed!\n");

    glyph_quad_vtx_buf = (BatchVertex*)linearAlloc(GLYPH_QUAD_SLOTS * 4 * sizeof(BatchVertex));
    glyph_quad_idx_buf = (u16*)linearAlloc(GLYPH_QUAD_SLOTS * 6 * sizeof(u16));
    if (!glyph_quad_vtx_buf || !glyph_quad_idx_buf)
        printf("FATAL: glyph quad buffer alloc failed!\n");

    // Atlas FIRST — guaranteed VRAM for shared draw call batching
    atlas_init();

    /* Pool strips: each bucket's slots become horizontal cells of shared
     * single-row strip textures (max 1024 px wide) so same-bucket quads can
     * batch into one draw call. Strip width = smallest POT covering the
     * cells this strip holds; the POT rounding can leave spare cells, which
     * become free extra slots (same allocation either way). */
    int pool_slot = 0;
    pool_strip_count = 0;
    for (unsigned int b = 0; b < SDL_arraysize(texture_pool_buckets); b++) {
        const TexturePoolBucketDesc* bucket = &texture_pool_buckets[b];
        u32 pot_w = bucket->pot_w, pot_h = bucket->pot_h;
        int cells_max = (int)(1024 / pot_w);
        if (cells_max < 1) cells_max = 1;
        int remaining = bucket->count;
        while (remaining > 0 && pool_slot < TEXTURE_POOL_SLOT_COUNT &&
               pool_strip_count < POOL_STRIP_MAX) {
            int want = (remaining < cells_max) ? remaining : cells_max;
            u32 strip_w = pot_w;
            while ((int)(strip_w / pot_w) < want) strip_w <<= 1;
            int cap = (int)(strip_w / pot_w);

            PoolStrip* st = &pool_strips[pool_strip_count];
            if (!C3D_TexInit(&st->tex, (u16)strip_w, (u16)pot_h, GPU_TEX_FMT)) {
                cache_fail_texinit++;
                break; /* out of linear memory — stop growing this bucket */
            }
            C3D_TexSetFilter(&st->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&st->tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
            memset(st->tex.data, 0, (size_t)strip_w * pot_h * TEXEL_BYTES);
            C3D_TexFlush(&st->tex);
            st->strip_w = strip_w;
            st->pot_h = pot_h;
            st->initialized = true;
            st->dirty = false;

            for (int c = 0; c < cap && pool_slot < TEXTURE_POOL_SLOT_COUNT; c++, pool_slot++) {
                TexturePoolSlot* slot = &texture_pool[pool_slot];
                slot->strip = pool_strip_count;
                slot->cell = c;
                slot->pot_w = pot_w;
                slot->pot_h = pot_h;
                slot->initialized = true;
                slot->in_use = false;
            }
            remaining -= cap;
            pool_strip_count++;
        }
    }
    printf("pool: %d strips, %d slots\n", pool_strip_count, pool_slot);

    current_texture_index = -1;
    current_palette_index = -1;
}

void SDLGameRenderer_BeginFrame(void) {
    render_task_count = 0;
    current_texture_index = -1;
    current_palette_index = -1;
    current_cache_entry = NULL;
    frame_build_used = 0;
    if (imm_ready) imm_bind();
}

void SDLGameRenderer_DebugDumpProfile(void) {
    if (last_renderer_profile[0]) {
        printf("%s\n", last_renderer_profile);
    }
}

const char* SDLGameRenderer_GetProfile(void) {
    return last_renderer_profile;
}

u32 dbg_settex_miss(void)   { return settex_miss; }
u32 dbg_settex_create(void) { return settex_create; }
u32 dbg_settex_fail(void)   { return settex_fail; }
u32 dbg_atlas_evict(void)   { return atlas_evictions_total; }

void SDLGameRenderer_RenderFrame(void) {
    if (!imm_ready || !batch_vtx_buf || !batch_idx_buf) return;

    /* This tree interleaves citro2d draws (bottom screen, HUD/overlay text,
     * button glyphs) with the native renderer in the SAME C3D frame — unlike
     * the reference build, which never touches citro2d. citro2d's own flush
     * can rebind its shader/attribute state at points we don't control, so
     * re-establish our program/attrinfo/texenv/blend/depth/scissor
     * immediately before submitting, rather than trusting the one bind done
     * at BeginFrame. */
    imm_bind();

    if (render_task_count == 0) return;

    // Flush atlas strips before GPU reads — builds/resolves/region updates
    // may have written mid-frame. One flush per DIRTY strip (tracked by
    // atlas_mark_strip_dirty), not one per cell (which flushed each strip
    // 4x) and not per write (which flushed a 512KB strip per palette tick).
    atlas_flush_pending_strips();

    u64 tick_sort_start = svcGetSystemTick();
    // Sort: z ascending, then submission order for equal-z.
    sort_render_tasks(render_task_count);
    render_ticks_sort += svcGetSystemTick() - tick_sort_start;

    // --- Pass 1: Fill vertex + index buffers, record resolved texture per quad ---
    // We use a parallel array to store the resolved C3D_Tex* for each quad,
    // since we need it during the draw-batch pass.
    C3D_Tex* quad_textures[RENDER_TASK_MAX];
    int quad_count = 0;

    u64 tick_fill_start = svcGetSystemTick();
    for (int i = 0; i < render_task_count; i++) {
        if (quad_count >= BATCH_MAX_QUADS) break;

        const RenderTask* task = &render_tasks[sort_indices[i]];
        u32 color = task->vertex_colors[0];
        if (((color >> 24) & 0xFF) == 0) continue;  // fully transparent

        float u[4], v[4];
        C3D_Tex* tex = resolve_task_texture(task, u, v);
        if (!tex) continue;

        batch_write_quad(quad_count, task->px, task->py, u, v, color);
        quad_textures[quad_count] = tex;
        quad_count++;
    }
    render_ticks_fill += svcGetSystemTick() - tick_fill_start;

    if (quad_count == 0) goto frame_timing;

    // Flush vertex + index data from CPU cache so GPU can see it
    GSPGPU_FlushDataCache(batch_vtx_buf, quad_count * 4 * sizeof(BatchVertex));
    GSPGPU_FlushDataCache(batch_idx_buf, quad_count * 6 * sizeof(u16));

    // --- Configure vertex buffer (replaces C3D_ImmDrawBegin attribute setup) ---
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, batch_vtx_buf, sizeof(BatchVertex), 3, 0x210);

    // --- Pass 2: Draw in batches, one C3D_DrawElements per texture change ---
    C3D_Tex* cur_tex = NULL;
    int batch_start_idx = 0;  // index into the index buffer (in units of indices)
    u64 tick_submit_start = svcGetSystemTick();

    for (int q = 0; q <= quad_count; q++) {
        bool flush = false;

        if (q == quad_count) {
            // End of all quads — flush remaining batch
            flush = true;
        } else if (cur_tex != NULL && quad_textures[q] != cur_tex) {
            // Texture changed — flush previous batch
            flush = true;
        }

        if (flush && cur_tex != NULL) {
            int batch_end_idx = q * 6;
            int idx_count = batch_end_idx - batch_start_idx;
            if (idx_count > 0) {
                C3D_TexBind(0, cur_tex);
                C3D_DrawElements(GPU_TRIANGLES, idx_count,
                                 C3D_UNSIGNED_SHORT,
                                 &batch_idx_buf[batch_start_idx]);
                render_draw_call_count++;
            }
            batch_start_idx = batch_end_idx;
            cur_tex = NULL;
        }

        if (q < quad_count) {
            cur_tex = quad_textures[q];
        }
    }
    render_ticks_submit += svcGetSystemTick() - tick_submit_start;

    /* Per-frame quick stats (every 120 frames). The old per-quad atlas-vs-
     * pool membership scan (quads x cells x strips pointer compares EVERY
     * frame) is gone — it was profiling overhead the real 268MHz target
     * would pay for nothing. Strip membership is a pointer range check. */
#if SF3_PERF_LOG
    {
        static u32 pf_frame = 0;
        static u32 pf_dc_accum = 0;
        static u32 pf_quad_accum = 0;
        static u32 pf_task_accum = 0;
        static u32 pf_pool_accum = 0;
        int p_this = 0;
        for (int q = 0; q < quad_count; q++) {
            const C3D_Tex* t = quad_textures[q];
            if (t < &atlas.strip_tex[0] || t >= &atlas.strip_tex[ATLAS_MAX_STRIPS])
                p_this++;
        }
        pf_quad_accum += quad_count;
        pf_task_accum += render_task_count;
        pf_pool_accum += p_this;
        pf_frame++;
        if (pf_frame % 8 == 0) { /* PORT DIAG: shortened from 120 for fast iteration. TEMP. */
            static u64 pf_last_tick = 0;
            static u64 pf_sort_prev = 0, pf_fill_prev = 0, pf_sub_prev = 0;
            static u64 pf_build_prev = 0;
            u64 pf_now = svcGetSystemTick();
            double pf_fps = 0;
            if (pf_last_tick > 0) {
                double pf_ms = (double)(pf_now - pf_last_tick) * 1000.0 / SYSCLOCK_ARM11;
                pf_fps = 120000.0 / pf_ms;
            }
            double sort_ms = (double)(render_ticks_sort - pf_sort_prev) * 1000.0 / SYSCLOCK_ARM11;
            double fill_ms = (double)(render_ticks_fill - pf_fill_prev) * 1000.0 / SYSCLOCK_ARM11;
            double sub_ms = (double)(render_ticks_submit - pf_sub_prev) * 1000.0 / SYSCLOCK_ARM11;
            double bld_ms = (double)(cache_create_ticks_build - pf_build_prev) * 1000.0 / SYSCLOCK_ARM11;
            pf_last_tick = pf_now;
            pf_sort_prev = render_ticks_sort;
            pf_fill_prev = render_ticks_fill;
            pf_sub_prev = render_ticks_submit;
            pf_build_prev = cache_create_ticks_build;
            { static int prev_noslot = 0, prev_texinit = 0, prev_evict = 0;
              static u32 prev_stcalls = 0, prev_stmiss = 0, prev_stcreate = 0;
              static u32 prev_stfail = 0, prev_stnopal = 0;
              int d_noslot = cache_fail_noslot - prev_noslot;
              int d_texinit = cache_fail_texinit - prev_texinit;
              int d_evict = (int)(atlas_evictions_total - prev_evict);
              u32 d_miss = settex_miss - prev_stmiss;
              u32 d_create = settex_create - prev_stcreate;
              u32 d_fail = settex_fail - prev_stfail;
              u32 d_nopal = settex_nopal - prev_stnopal;
              printf("rf: %.0ffps q=%u dc=%.1f p=%u | bld=%.0f",
                     pf_fps,
                     pf_quad_accum / 120,
                     (double)(render_draw_call_count - pf_dc_accum) / 120.0,
                     pf_pool_accum / 120,
                     bld_ms);
              if (d_miss || d_fail)
                  printf(" ST m=%u c=%u f=%u np=%u", d_miss, d_create, d_fail, d_nopal);
              if (d_noslot || d_texinit || d_evict)
                  printf(" FAIL ns=%d ti=%d ev=%d", d_noslot, d_texinit, d_evict);
              printf("\n");
              prev_noslot = cache_fail_noslot;
              prev_texinit = cache_fail_texinit;
              prev_evict = (int)atlas_evictions_total;
              prev_stcalls = settex_calls;
              prev_stmiss = settex_miss;
              prev_stcreate = settex_create;
              prev_stfail = settex_fail;
              prev_stnopal = settex_nopal;
            }
            pf_dc_accum = render_draw_call_count;
            pf_quad_accum = 0; pf_task_accum = 0;
            pf_pool_accum = 0;
        }
    }
#endif /* SF3_PERF_LOG */

frame_timing:;
#if SF3_PERF_LOG
    // Frame timing (print every 60 frames)
    {
        static u32 frame_count = 0;
        static u64 last_report = 0;
        static u64 prev_cache_create_ticks_total = 0;
        static u64 prev_cache_create_ticks_build = 0;
        static u64 prev_cache_create_ticks_flush = 0;
        static u64 prev_render_ticks_sort = 0;
        static u64 prev_render_ticks_fill = 0;
        static u64 prev_render_ticks_submit = 0;
        static u32 prev_cache_create_count_total = 0;
        static u32 prev_cache_create_count_psmt8 = 0;
        static u32 prev_cache_create_count_psmt4 = 0;
        static u32 prev_cache_create_count_psmct16 = 0;
        static u32 prev_cache_create_count_other = 0;
        static u32 prev_render_draw_call_count = 0;
        static u32 prev_cache_pool_evictions = 0;
        static u32 prev_cache_pool_evictions_16 = 0;
        static u32 prev_cache_pool_evictions_32 = 0;
        static u32 prev_cache_pool_evictions_64 = 0;
        static u32 prev_cache_pool_evictions_128x64 = 0;
        static u32 prev_cache_pool_evictions_64x128 = 0;
        static u32 prev_cache_pool_evictions_128x128 = 0;
        static u32 prev_cache_pool_evictions_256x128 = 0;
        static u32 prev_cache_pool_evictions_128x256 = 0;
        static u32 prev_cache_pool_evictions_256x256 = 0;
        static u32 prev_cache_pool_evictions_512x256 = 0;
        static u32 prev_cache_pool_evictions_256x512 = 0;
        static u32 prev_cache_pool_evictions_512x512 = 0;
        static u32 prev_cache_pool_evictions_other = 0;
        static u32 prev_cache_pool_evictions_other_stats[8] = {0};
        static u32 prev_cache_invalidate_texture_calls = 0;
        static u32 prev_cache_invalidate_palette_calls = 0;
        static u32 prev_cache_invalidate_texture_entries = 0;
        static u32 prev_cache_invalidate_palette_entries = 0;
        static u32 prev_unlock_texture_calls = 0;
        static u32 prev_unlock_texture_changed = 0;
        static u32 prev_unlock_palette_calls = 0;
        static u32 prev_unlock_palette_changed = 0;
        static u32 prev_texcash_purge_counts[24] = {0};
        static u32 prev_destroy_texture_counts[FL_TEXTURE_MAX] = {0};
        static u32 prev_destroy_palette_counts[FL_PALETTE_MAX] = {0};
        frame_count++;
        if (frame_count % 60 == 0) {
            u64 now = svcGetSystemTick();
            if (last_report > 0) {
                double ms = (double)(now - last_report) * 1000.0 / SYSCLOCK_ARM11;
                double fps = 60000.0 / ms;
                u32 delta_create_count = cache_create_count_total - prev_cache_create_count_total;
                u64 delta_create_ticks_total = cache_create_ticks_total - prev_cache_create_ticks_total;
                u64 delta_create_ticks_build = cache_create_ticks_build - prev_cache_create_ticks_build;
                u64 delta_create_ticks_flush = cache_create_ticks_flush - prev_cache_create_ticks_flush;
                u64 delta_render_ticks_sort = render_ticks_sort - prev_render_ticks_sort;
                u64 delta_render_ticks_fill = render_ticks_fill - prev_render_ticks_fill;
                u64 delta_render_ticks_submit = render_ticks_submit - prev_render_ticks_submit;
                u32 delta_psmt8 = cache_create_count_psmt8 - prev_cache_create_count_psmt8;
                u32 delta_psmt4 = cache_create_count_psmt4 - prev_cache_create_count_psmt4;
                u32 delta_psmct16 = cache_create_count_psmct16 - prev_cache_create_count_psmct16;
                u32 delta_other = cache_create_count_other - prev_cache_create_count_other;
                u32 delta_draw_calls = render_draw_call_count - prev_render_draw_call_count;
                u32 delta_evictions = cache_pool_evictions - prev_cache_pool_evictions;
                u32 delta_evictions_16 = cache_pool_evictions_16 - prev_cache_pool_evictions_16;
                u32 delta_evictions_32 = cache_pool_evictions_32 - prev_cache_pool_evictions_32;
                u32 delta_evictions_64 = cache_pool_evictions_64 - prev_cache_pool_evictions_64;
                u32 delta_evictions_128x64 = cache_pool_evictions_128x64 - prev_cache_pool_evictions_128x64;
                u32 delta_evictions_64x128 = cache_pool_evictions_64x128 - prev_cache_pool_evictions_64x128;
                u32 delta_evictions_128x128 = cache_pool_evictions_128x128 - prev_cache_pool_evictions_128x128;
                u32 delta_evictions_256x128 = cache_pool_evictions_256x128 - prev_cache_pool_evictions_256x128;
                u32 delta_evictions_128x256 = cache_pool_evictions_128x256 - prev_cache_pool_evictions_128x256;
                u32 delta_evictions_256x256 = cache_pool_evictions_256x256 - prev_cache_pool_evictions_256x256;
                u32 delta_evictions_512x256 = cache_pool_evictions_512x256 - prev_cache_pool_evictions_512x256;
                u32 delta_evictions_256x512 = cache_pool_evictions_256x512 - prev_cache_pool_evictions_256x512;
                u32 delta_evictions_512x512 = cache_pool_evictions_512x512 - prev_cache_pool_evictions_512x512;
                u32 delta_evictions_other = cache_pool_evictions_other - prev_cache_pool_evictions_other;
                char other_sizes[96];
                int other_sizes_len = 0;
                other_sizes[0] = '\0';
                for (int i = 0; i < (int)SDL_arraysize(cache_pool_evictions_other_stats); i++) {
                    u32 delta_other_stat = cache_pool_evictions_other_stats[i].count - prev_cache_pool_evictions_other_stats[i];
                    if (delta_other_stat == 0) continue;
                    int wrote = snprintf(other_sizes + other_sizes_len,
                                         sizeof(other_sizes) - (size_t)other_sizes_len,
                                         "%s%lux%lu=%lu",
                                         other_sizes_len ? " " : "",
                                         (unsigned long)cache_pool_evictions_other_stats[i].pot_w,
                                         (unsigned long)cache_pool_evictions_other_stats[i].pot_h,
                                         (unsigned long)delta_other_stat);
                    if (wrote <= 0 || wrote >= (int)(sizeof(other_sizes) - (size_t)other_sizes_len)) break;
                    other_sizes_len += wrote;
                }
                u32 delta_inv_tex_calls = cache_invalidate_texture_calls - prev_cache_invalidate_texture_calls;
                u32 delta_inv_pal_calls = cache_invalidate_palette_calls - prev_cache_invalidate_palette_calls;
                u32 delta_inv_tex_entries = cache_invalidate_texture_entries - prev_cache_invalidate_texture_entries;
                u32 delta_inv_pal_entries = cache_invalidate_palette_entries - prev_cache_invalidate_palette_entries;
                u32 delta_unlock_tex_calls = unlock_texture_calls - prev_unlock_texture_calls;
                u32 delta_unlock_tex_changed = unlock_texture_changed - prev_unlock_texture_changed;
                u32 delta_unlock_pal_calls = unlock_palette_calls - prev_unlock_palette_calls;
                u32 delta_unlock_pal_changed = unlock_palette_changed - prev_unlock_palette_changed;
                double create_total_ms = (double)delta_create_ticks_total * 1000.0 / SYSCLOCK_ARM11;
                double create_build_ms = (double)delta_create_ticks_build * 1000.0 / SYSCLOCK_ARM11;
                double create_flush_ms = (double)delta_create_ticks_flush * 1000.0 / SYSCLOCK_ARM11;
                double create_avg_ms = (delta_create_count > 0) ? (create_total_ms / (double)delta_create_count) : 0.0;
                double render_sort_ms = (double)delta_render_ticks_sort * 1000.0 / SYSCLOCK_ARM11;
                double render_fill_ms = (double)delta_render_ticks_fill * 1000.0 / SYSCLOCK_ARM11;
                double render_submit_ms = (double)delta_render_ticks_submit * 1000.0 / SYSCLOCK_ARM11;
                u32 top_purge_ix[3] = {0, 0, 0};
                u32 top_purge_count[3] = {0, 0, 0};
                u32 top_destroy_tex_ix[3] = {0, 0, 0};
                u32 top_destroy_tex_count[3] = {0, 0, 0};
                u32 top_destroy_pal_ix[2] = {0, 0};
                u32 top_destroy_pal_count[2] = {0, 0};
                for (u32 i = 0; i < 24; i++) {
                    u32 delta_purge = texcash_purge_counts[i] - prev_texcash_purge_counts[i];
                    if (delta_purge > top_purge_count[0]) {
                        top_purge_count[2] = top_purge_count[1];
                        top_purge_ix[2] = top_purge_ix[1];
                        top_purge_count[1] = top_purge_count[0];
                        top_purge_ix[1] = top_purge_ix[0];
                        top_purge_count[0] = delta_purge;
                        top_purge_ix[0] = i;
                    } else if (delta_purge > top_purge_count[1]) {
                        top_purge_count[2] = top_purge_count[1];
                        top_purge_ix[2] = top_purge_ix[1];
                        top_purge_count[1] = delta_purge;
                        top_purge_ix[1] = i;
                    } else if (delta_purge > top_purge_count[2]) {
                        top_purge_count[2] = delta_purge;
                        top_purge_ix[2] = i;
                    }
                }
                for (u32 i = 0; i < FL_TEXTURE_MAX; i++) {
                    u32 delta_destroy = destroy_texture_counts[i] - prev_destroy_texture_counts[i];
                    if (delta_destroy > top_destroy_tex_count[0]) {
                        top_destroy_tex_count[2] = top_destroy_tex_count[1];
                        top_destroy_tex_ix[2] = top_destroy_tex_ix[1];
                        top_destroy_tex_count[1] = top_destroy_tex_count[0];
                        top_destroy_tex_ix[1] = top_destroy_tex_ix[0];
                        top_destroy_tex_count[0] = delta_destroy;
                        top_destroy_tex_ix[0] = i;
                    } else if (delta_destroy > top_destroy_tex_count[1]) {
                        top_destroy_tex_count[2] = top_destroy_tex_count[1];
                        top_destroy_tex_ix[2] = top_destroy_tex_ix[1];
                        top_destroy_tex_count[1] = delta_destroy;
                        top_destroy_tex_ix[1] = i;
                    } else if (delta_destroy > top_destroy_tex_count[2]) {
                        top_destroy_tex_count[2] = delta_destroy;
                        top_destroy_tex_ix[2] = i;
                    }
                }
                for (u32 i = 0; i < FL_PALETTE_MAX; i++) {
                    u32 delta_destroy = destroy_palette_counts[i] - prev_destroy_palette_counts[i];
                    if (delta_destroy > top_destroy_pal_count[0]) {
                        top_destroy_pal_count[1] = top_destroy_pal_count[0];
                        top_destroy_pal_ix[1] = top_destroy_pal_ix[0];
                        top_destroy_pal_count[0] = delta_destroy;
                        top_destroy_pal_ix[0] = i;
                    } else if (delta_destroy > top_destroy_pal_count[1]) {
                        top_destroy_pal_count[1] = delta_destroy;
                        top_destroy_pal_ix[1] = i;
                    }
                }

                u32 delta_atlas = atlas_builds_period;
                atlas_builds_period = 0;
                // Count how many active entries are in atlas vs pool
                int atlas_active = 0, pool_active = 0;
                for (int ci = 0; ci < CACHE_MAX; ci++) {
                    CacheEntry* ce = &gpu_cache[ci];
                    if (!ce->allocated || ce->pending_delete) continue;
                    if (ce->atlas_cell >= 0) atlas_active++;
                    else if (ce->pool_slot >= 0) pool_active++;
                }
                snprintf(last_renderer_profile, sizeof(last_renderer_profile),
                         "ren b=%.0f s=%.0f sub=%.0f dc=%lu a=%d/%d l8=%lu/%lu",
                         create_build_ms, render_sort_ms, render_submit_ms,
                         (unsigned long)(delta_draw_calls),
                         atlas_active, pool_active,
                         (unsigned long)l8_hits, (unsigned long)l8_misses);
            }
            last_report = now;
            prev_cache_create_ticks_total = cache_create_ticks_total;
            prev_cache_create_ticks_build = cache_create_ticks_build;
            prev_cache_create_ticks_flush = cache_create_ticks_flush;
            prev_render_ticks_sort = render_ticks_sort;
            prev_render_ticks_fill = render_ticks_fill;
            prev_render_ticks_submit = render_ticks_submit;
            prev_cache_create_count_total = cache_create_count_total;
            prev_cache_create_count_psmt8 = cache_create_count_psmt8;
            prev_cache_create_count_psmt4 = cache_create_count_psmt4;
            prev_cache_create_count_psmct16 = cache_create_count_psmct16;
            prev_cache_create_count_other = cache_create_count_other;
            prev_render_draw_call_count = render_draw_call_count;
            prev_cache_pool_evictions = cache_pool_evictions;
            prev_cache_pool_evictions_16 = cache_pool_evictions_16;
            prev_cache_pool_evictions_32 = cache_pool_evictions_32;
            prev_cache_pool_evictions_64 = cache_pool_evictions_64;
            prev_cache_pool_evictions_128x64 = cache_pool_evictions_128x64;
            prev_cache_pool_evictions_64x128 = cache_pool_evictions_64x128;
            prev_cache_pool_evictions_128x128 = cache_pool_evictions_128x128;
            prev_cache_pool_evictions_256x128 = cache_pool_evictions_256x128;
            prev_cache_pool_evictions_128x256 = cache_pool_evictions_128x256;
            prev_cache_pool_evictions_256x256 = cache_pool_evictions_256x256;
            prev_cache_pool_evictions_512x256 = cache_pool_evictions_512x256;
            prev_cache_pool_evictions_256x512 = cache_pool_evictions_256x512;
            prev_cache_pool_evictions_512x512 = cache_pool_evictions_512x512;
            prev_cache_pool_evictions_other = cache_pool_evictions_other;
            for (int i = 0; i < (int)SDL_arraysize(cache_pool_evictions_other_stats); i++) {
                prev_cache_pool_evictions_other_stats[i] = cache_pool_evictions_other_stats[i].count;
            }
            prev_cache_invalidate_texture_calls = cache_invalidate_texture_calls;
            prev_cache_invalidate_palette_calls = cache_invalidate_palette_calls;
            prev_cache_invalidate_texture_entries = cache_invalidate_texture_entries;
            prev_cache_invalidate_palette_entries = cache_invalidate_palette_entries;
            prev_unlock_texture_calls = unlock_texture_calls;
            prev_unlock_texture_changed = unlock_texture_changed;
            prev_unlock_palette_calls = unlock_palette_calls;
            prev_unlock_palette_changed = unlock_palette_changed;
            for (int i = 0; i < 24; i++) {
                prev_texcash_purge_counts[i] = texcash_purge_counts[i];
            }
            for (int i = 0; i < FL_TEXTURE_MAX; i++) {
                prev_destroy_texture_counts[i] = destroy_texture_counts[i];
            }
            for (int i = 0; i < FL_PALETTE_MAX; i++) {
                prev_destroy_palette_counts[i] = destroy_palette_counts[i];
            }
        }
    }
#endif /* SF3_PERF_LOG */
}

void SDLGameRenderer_EndFrame(void) {
    render_task_count = 0;
    cache_flush_pending();  // Actually free textures marked for deletion during this frame
    frame_number++;
}

/* Draw one full-target textured quad using this renderer's own raw C3D
 * pipeline (shader, attribute layout, immediate submission) instead of
 * citro2d — for content on a target OTHER than the main game's top screen
 * (currently: the bottom-screen character art).
 *
 * Why not citro2d: imm_bind()'s projection matrix (imm_projection) is fixed
 * at init for the TOP screen's 400x240 game-area mapping. citro2d draws
 * issued right after this renderer's per-frame submission (crop bars, then
 * the bottom-screen image) were found to render nothing on the bottom
 * target — even a plain solid-color C2D_DrawRectSolid — while a bare
 * C3D target clear on the same target displayed correctly. That symptom
 * (draws silently absent, clears fine) matches geometry landing outside the
 * active clip space from a stale/mismatched projection more closely than a
 * state-visibility issue, and citro2d's own shader/projection binding
 * behavior isn't inspectable from here. Rather than continue guessing at
 * citro2d's internals, this sidesteps citro2d for this one draw entirely,
 * reusing only machinery already proven reliable (it's how the whole top
 * screen renders): its own explicit target bind, its own projection sized
 * for the actual target, and the same shader/attribute setup as every other
 * quad this renderer draws. */
void SDLGameRenderer_DrawRawQuadToTarget(C3D_RenderTarget* target, void* tex_v,
                                          u16 target_w, u16 target_h,
                                          float u0, float v0, float u1, float v1) {
    C3D_Tex* tex = (C3D_Tex*)tex_v;
    if (!imm_ready || !tex || !target) return;

    C3D_FrameDrawOn(target);

    C3D_BindProgram(&imm_program);
    C3D_SetAttrInfo(&imm_attrInfo);

    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, (float)target_w, (float)target_h, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, imm_uLoc_projection, &proj);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    C3D_TexEnv* env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, 0, 0);
    C3D_TexEnvFunc(env1, C3D_Both, GPU_REPLACE);

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_GREATER, 0);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

    C3D_TexBind(0, tex);

    BatchVertex verts[4] = {
        { 0.0f,            0.0f,            0.5f, u0, v0, 1.0f, 1.0f, 1.0f, 1.0f },
        { (float)target_w, 0.0f,            0.5f, u1, v0, 1.0f, 1.0f, 1.0f, 1.0f },
        { 0.0f,            (float)target_h, 0.5f, u0, v1, 1.0f, 1.0f, 1.0f, 1.0f },
        { (float)target_w, (float)target_h, 0.5f, u1, v1, 1.0f, 1.0f, 1.0f, 1.0f },
    };
    u16 idx[6] = { 0, 1, 2, 1, 3, 2 };
    if (!raw_quad_vtx_buf || !raw_quad_idx_buf) return;
    memcpy(raw_quad_vtx_buf, verts, sizeof(verts));
    memcpy(raw_quad_idx_buf, idx, sizeof(idx));

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, raw_quad_vtx_buf, sizeof(BatchVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, raw_quad_idx_buf);
}

/* Draw an arbitrary-rect textured quad on the TOP screen using this renderer's
 * own raw C3D pipeline instead of citro2d — for UI content interleaved
 * mid-frame with the game's own sprite drawing (currently: Nintendo button-
 * glyph icons drawn from gu_draw.c's ctrDrawButtonGlyph).
 *
 * Why not citro2d: citro2d draws issued between this renderer's per-frame
 * binds were found to silently render nothing (see
 * SDLGameRenderer_DrawRawQuadToTarget's comment for the same symptom on the
 * bottom screen) regardless of scissor/depth/alpha/cull state, forced
 * flushing, or re-calling C2D_Prepare() beforehand — while a raw C3D draw
 * reusing this renderer's own already-bound shader/projection is exactly how
 * the whole top screen already renders every frame. imm_projection is fixed
 * for the top screen's 400x240 game-area space, which is exactly the
 * coordinate space the game's own SCALE_X/SCALE_Y already produce, so x0..y1
 * need no adjustment here. */
void SDLGameRenderer_DrawGlyphQuad(void* tex_v, float x0, float y0, float x1, float y1,
                                    float u0, float v0, float u1, float v1) {
    extern C3D_RenderTarget *ctrGetTopTarget(void);
    C3D_Tex* tex = (C3D_Tex*)tex_v;
    if (!imm_ready || !tex) return;

    C3D_RenderTarget *top = ctrGetTopTarget();
    if (top) C3D_FrameDrawOn(top);
    imm_bind();

    /* imm_bind()'s imm_projection expects RAW CPS3 game-native coordinates
     * (0..384 x 0..224) and bakes in its own +8px screen-centering offset —
     * but x0..y1 here are already-scaled, already-offset SCREEN pixels (from
     * the game's own SCALE_X/SCALE_Y macros, 0..400 x 0..240). Feeding
     * already-offset coordinates through a projection that applies its own
     * offset again put every glyph in the wrong place. Upload a fresh
     * projection calibrated for the screen-pixel space these coordinates are
     * actually in, same as SDLGameRenderer_DrawRawQuadToTarget does for the
     * bottom screen. */
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, imm_uLoc_projection, &proj);

    C3D_TexBind(0, tex);

    BatchVertex verts[4] = {
        { x0, y0, 0.5f, u0, v0, 1.0f, 1.0f, 1.0f, 1.0f },
        { x1, y0, 0.5f, u1, v0, 1.0f, 1.0f, 1.0f, 1.0f },
        { x0, y1, 0.5f, u0, v1, 1.0f, 1.0f, 1.0f, 1.0f },
        { x1, y1, 0.5f, u1, v1, 1.0f, 1.0f, 1.0f, 1.0f },
    };
    u16 idx[6] = { 0, 1, 2, 1, 3, 2 };
    if (!glyph_quad_vtx_buf || !glyph_quad_idx_buf) return;

    int slot = glyph_quad_slot;
    glyph_quad_slot = (glyph_quad_slot + 1) % GLYPH_QUAD_SLOTS;
    BatchVertex* vtx = glyph_quad_vtx_buf + (size_t)slot * 4;
    u16* ix = glyph_quad_idx_buf + (size_t)slot * 6;
    memcpy(vtx, verts, sizeof(verts));
    memcpy(ix, idx, sizeof(idx));

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, vtx, sizeof(BatchVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ix);
}

// Called BEFORE C3D_FrameBegin - safe for DisplayTransfer.
// Adaptive budget: if pending queue is large (transition/screen load),
// spend more time building so screens appear quickly. During fights
// (small queue), use a conservative budget to preserve frame time.
#define TEXTURE_PENDING_MIN_PER_FRAME 8
#define TEXTURE_PENDING_MAX_NORMAL 24
#define TEXTURE_PENDING_MAX_BURST 128
#define TEXTURE_PENDING_BUDGET_NORMAL_US 6000
#define TEXTURE_PENDING_BUDGET_BURST_US 12000
#define TEXTURE_PENDING_BURST_THRESHOLD 16
void SDLGameRenderer_ProcessPending(void) {
    int created = 0;
    int remaining = 0;
    const u64 start_tick = svcGetSystemTick();
    /* Adaptive: large queue → spend more time, small queue → conservative */
    int max_per_frame = (pending_n > TEXTURE_PENDING_BURST_THRESHOLD)
                        ? TEXTURE_PENDING_MAX_BURST
                        : TEXTURE_PENDING_MAX_NORMAL;
    u32 budget_us = (pending_n > TEXTURE_PENDING_BURST_THRESHOLD)
                    ? TEXTURE_PENDING_BUDGET_BURST_US
                    : TEXTURE_PENDING_BUDGET_NORMAL_US;
    const u64 budget_ticks = ((u64)SYSCLOCK_ARM11 * budget_us) / 1000000ULL;

    for (int i = 0; i < pending_n; i++) {
        int ti = pending[i].ti, pi = pending[i].pi;
        if (ti < 0 || ti >= FL_TEXTURE_MAX || !src_textures[ti].valid) {
            continue; // Source texture is gone; drop the request.
        }
        if (pi >= 0 && (pi >= FL_PALETTE_MAX || !src_palettes[pi].valid)) {
            continue; // Palette is gone; drop the request.
        }
        if (cache_find_clean(ti, pi)) continue; // Already cached and current

        if (created >= max_per_frame) {
            pending[remaining] = pending[i];
            remaining++;
            continue;
        }

        if (created >= TEXTURE_PENDING_MIN_PER_FRAME) {
            u64 elapsed_ticks = svcGetSystemTick() - start_tick;
            if (elapsed_ticks >= budget_ticks) {
                pending[remaining] = pending[i];
                remaining++;
                continue;
            }
        }

        if (cache_create(ti, pi)) {
            created++;
            continue;
        }

        // Carry over to next frame if creation failed.
        pending[remaining] = pending[i];
        remaining++;
    }

    // Flush dirty atlas strips once after all builds
    atlas_flush_pending_strips();

    pending_last_created = created;
    pending_last_ms = (double)(svcGetSystemTick() - start_tick) * 1000.0 / SYSCLOCK_ARM11;
    pending_n = remaining;
}

void SDLGameRenderer_ProcessPendingBlocking(void) {
    int total_created = 0;
    const u64 start_tick = svcGetSystemTick();

    for (;;) {
        int created_this_pass = 0;
        int remaining = 0;

        for (int i = 0; i < pending_n; i++) {
            int ti = pending[i].ti, pi = pending[i].pi;
            if (ti < 0 || ti >= FL_TEXTURE_MAX || !src_textures[ti].valid) {
                continue;
            }
            if (pi >= 0 && (pi >= FL_PALETTE_MAX || !src_palettes[pi].valid)) {
                continue;
            }
            if (cache_find_clean(ti, pi)) {
                continue;
            }

            if (cache_create(ti, pi)) {
                created_this_pass++;
                total_created++;
                continue;
            }

            pending[remaining] = pending[i];
            remaining++;
        }

        pending_n = remaining;
        if (pending_n == 0 || created_this_pass == 0) {
            break;
        }
    }

    atlas_flush_pending_strips();

    pending_last_created = total_created;
    pending_last_ms = (double)(svcGetSystemTick() - start_tick) * 1000.0 / SYSCLOCK_ARM11;
}

// ---------------------------------------------------------------------------
// Public API: texture management
// ---------------------------------------------------------------------------

void SDLGameRenderer_CreateTexture(unsigned int th) {
    int idx = LO_16_BITS(th) - 1;
    if (idx < 0 || idx >= FL_TEXTURE_MAX) return;

    const FLTexture* ft = &flTexture[idx];
    SrcTexture* s = &src_textures[idx];
    /* flPS2ConvertTextureFromContext(mode=0) stores the pixel pointer
     * directly in wkVram and deliberately zeroes mem_handle (no system-
     * memory-pool allocation for that path) — fall back to it so those
     * textures aren't mistaken for a failed allocation. */
    const void* pixels = ft->lock_ptr ? (const void*)(uintptr_t)ft->lock_ptr
                                      : flPS2GetSystemBuffAdrs(ft->mem_handle);
    bool used_wkvram_fallback = (!pixels && ft->wkVram);
    if (!pixels) pixels = ft->wkVram;
    size_t byte_size = texture_byte_size_for_format(ft->width, ft->height, gu_to_gs_format(ft->format));
    bool was_valid = s->valid;

    s->w      = ft->width;
    s->h      = ft->height;
    s->fmt    = gu_to_gs_format(ft->format);
    s->pixels = pixels;
    s->byte_size = byte_size;
    s->checksum = 0; /* no hash on create — version bump is sufficient */
    /* This tree creates "empty placeholder" texture handles up front (real
     * pixel memory allocated but not yet written — content arrives later via
     * the melt/region-update path) for a class of texture the reference
     * build's game flow apparently never captures at this point. Without
     * this guard, a NULL/unready pixels pointer still gets marked valid, and
     * cache_create/atlas_build_cell read through it later — an unbounded
     * out-of-bounds read that floods the log with memory errors and stalls
     * the frame. Leave the entry invalid until it has a real pointer. */
    s->valid  = (pixels != NULL);
    s->is_placeholder = used_wkvram_fallback;
    /* Deliberately no tile_dirty/has_region_updates manipulation here —
     * matches the reference renderer. (A reset-on-recreate was tried and
     * caused progressive sprite loss across rounds: seqs pages are released
     * and recreated per round while their GPU-side state must keep
     * accumulating melt content.) */
    texture_versions[idx]++;
    if (was_valid) {
        cache_mark_texture_dirty(idx); /* only scan cache for re-creation */
    }
}

void SDLGameRenderer_DestroyTexture(unsigned int texture_handle) {
    int idx = texture_handle - 1;
    if (idx < 0 || idx >= FL_TEXTURE_MAX) return;

    destroy_texture_counts[idx]++;
    // Invalidate L8 index cache for this texture
    for (int li = 0; li < L8_CACHE_MAX; li++) {
        if (l8_cache[li].valid && l8_cache[li].texture_index == idx) l8_cache[li].valid = false;
    }
    cache_invalidate_texture(idx);
    pending_remove_texture(idx);
    if (current_texture_index == idx) {
        current_cache_entry = NULL;
    }
    /* Deliberately do NOT wipe src_textures[idx] here (no memset to
     * invalid/NULL). This port's game logic (faithful to the original,
     * unmodified decomp — see OPENING.c oh_reload_tex) releases a texture
     * handle as soon as ONE scrolling-background grid cell reassigns away
     * from it, even though ANOTHER cell may still reference the same
     * texture index for one more frame before its own reassignment catches
     * up. On the original PS2/PSP hardware this is harmless: "releasing" a
     * handle only marks memory as available for reuse, it doesn't
     * instantly clear it, so a stale reference for a frame or two still
     * reads correct pixels. Our previous immediate memset made that same
     * one-frame race fatal (NULL/garbage read -> permanently invisible
     * tile). Leaving the last-known-good src_textures entry in place lets
     * cache_create() rebuild a fresh GPU-side entry from it if something
     * draws this index again before the underlying memory is genuinely
     * reused for different content — at which point SDLGameRenderer_
     * CreateTexture/UnlockTexture naturally overwrite it with the new
     * capture anyway. */
}

void SDLGameRenderer_UnlockTexture(unsigned int th) {
    int idx = th;
    if (idx > 0 && idx <= FL_TEXTURE_MAX) {
        unlock_texture_calls++;
        int ti = idx - 1;
        if (ti >= 0 && ti < FL_TEXTURE_MAX) {
            const FLTexture* ft = &flTexture[ti];
            SrcTexture* s = &src_textures[ti];
            const void* pixels = ft->lock_ptr ? (const void*)(uintptr_t)ft->lock_ptr
                                              : flPS2GetSystemBuffAdrs(ft->mem_handle);
            bool used_wkvram_fallback = (!pixels && ft->wkVram);
            if (!pixels) pixels = ft->wkVram; /* see SDLGameRenderer_CreateTexture */
            s->is_placeholder = used_wkvram_fallback;
            u32 gsfmt = gu_to_gs_format(ft->format);
            size_t byte_size = texture_byte_size_for_format(ft->width, ft->height, gsfmt);
            /* Sampled content checksum (~256 spread-out bytes, not a full
             * 65KB hash): some game paths re-decode a sheet's pixels IN
             * PLACE — same pointer, same dims — with no region-update
             * notify (e.g. the char/stage-select background sheets on
             * screen transitions). With no signal at all, cache entries
             * keep serving the old capture: after the palette-churn
             * invalidation was made lazy, this surfaced as stale/partial
             * background art. A byte-identical sampled sum can in theory
             * miss a change confined to unsampled bytes, but these
             * re-decodes replace whole sheets; region-update paths remain
             * the precise mechanism for partial writes. */
            u32 sum = 0;
            if (pixels && byte_size) {
                const u8* bp = (const u8*)pixels;
                size_t stride = (byte_size > 4096) ? (byte_size / 256) : 16;
                for (size_t off = 0; off < byte_size; off += stride)
                    sum = sum * 33 + bp[off];
                sum = sum * 33 + bp[byte_size - 1];
                if (!sum) sum = 1; /* keep 0 as "no checksum" sentinel */
            }
            /* checksum==0 means "unknown": fresh capture, or content since
             * modified through the region-update path (which patches GPU
             * copies directly and resets the checksum) — don't double-
             * rebuild those; only a mismatch against a KNOWN previous sum
             * marks a silent in-place re-decode. */
            bool changed = !s->valid ||
                           s->w != ft->width ||
                           s->h != ft->height ||
                           s->fmt != gsfmt ||
                           s->pixels != pixels ||
                           (s->checksum != 0 && s->checksum != sum);

            s->w = ft->width;
            s->h = ft->height;
            s->fmt = gsfmt;
            s->pixels = pixels;
            s->byte_size = byte_size;
            s->checksum = sum;
            s->valid = (pixels != NULL); /* see SDLGameRenderer_CreateTexture */
            if (changed) {
                texture_versions[ti]++;
                unlock_texture_changed++;
                cache_mark_texture_dirty(ti);
#ifdef __3DS__
                { static u32 _utc = 0;
                  /* Print first few + every 500th for MTS 13 seqs pages (gix 1030+) */
                  if (++_utc <= 5 || _utc % 500 == 1)
                      printf("UNLOCK dirty ti=%d w=%d/%d h=%d/%d fmt=%d/%d px=%p/%p\n",
                             ti, s->w, ft->width, s->h, ft->height,
                             s->fmt, ft->format,
                             (void*)s->pixels, (void*)pixels);
                }
#endif
            }
        }
    }
}

int dbg_regionupd_calls = 0;   /* PORT DIAG. TEMP */
int dbg_regionupd_patched = 0; /* PORT DIAG. TEMP */

void SDLGameRenderer_UpdateTextureRegion(unsigned int th, int x, int y, int w, int h) {
    int idx = (int)th - 1;
    if (idx < 0 || idx >= FL_TEXTURE_MAX) return;
    dbg_regionupd_calls++;

    /* Mark the affected 8×8 tiles as "active" in the dirty bitmap. */
    { SrcTexture* s = &src_textures[idx];
      if (s->valid && s->w > 0) {
          s->has_region_updates = true;
          s->checksum = 0; /* content changed via region path — invalidate the
                              sampled sum so the next Unlock doesn't misread
                              the delta as a silent re-decode (see
                              SDLGameRenderer_UnlockTexture) */
          int tx0 = x / 8, ty0 = y / 8;
          int tx1 = (x + w + 7) / 8, ty1 = (y + h + 7) / 8;
          int tiles_per_row = s->w / 8;
          for (int ty = ty0; ty < ty1; ty++) {
              for (int tx = tx0; tx < tx1; tx++) {
                  int tidx = ty * tiles_per_row + tx;
                  if (tidx >= 0 && tidx < TILE_DIRTY_WORDS * 32)
                      s->tile_dirty[tidx >> 5] |= (1u << (tidx & 31));
              }
          }
      }
    }

    /* Patch the texture-keyed pool L8 index cache ONCE per notify (PSMT8):
     * melting sheets keep their palette ticking, and with stale/invalidated
     * indices every tick was a measured 6.8ms full rebuild — patched, it's
     * a ~2ms fast re-resolve. Same morton/Y-flip convention as the pool
     * build (gpu tile row = tiles_y-1-src_ty, fine row = morton_row[7-fy]). */
    {
        SrcTexture* s = &src_textures[idx];
        if (s->valid && s->fmt == SCE_GS_PSMT8 && s->pixels) {
            L8CacheEntry* l8p = l8_cache_find(idx, texture_versions[idx]);
            if (l8p && l8p->indices) {
                const u8* px8 = (const u8*)s->pixels;
                int ltx = l8p->pot_w >> 3;
                int lty = l8p->pot_h >> 3;
                int px0 = x >> 3, px1 = (x + w - 1) >> 3;
                int py0 = y >> 3, py1 = (y + h - 1) >> 3;
                for (int sty = py0; sty <= py1; sty++) {
                    int ty = (lty - 1) - sty;
                    if (ty < 0 || ty >= lty) continue;
                    for (int tx = px0; tx <= px1 && tx < ltx; tx++) {
                        if (tx < 0) continue;
                        int stx = tx * 8, sy0 = sty * 8;
                        u8* dst = &l8p->indices[(ty * ltx + tx) * 64];
                        if (stx + 8 <= s->w && sy0 + 8 <= s->h) {
                            for (int fy = 0; fy < 8; fy++) {
                                const u8* srow = px8 + (sy0 + fy) * s->w + stx;
                                const u8* m = morton_row[7 - fy];
                                dst[m[0]] = srow[0]; dst[m[1]] = srow[1];
                                dst[m[2]] = srow[2]; dst[m[3]] = srow[3];
                                dst[m[4]] = srow[4]; dst[m[5]] = srow[5];
                                dst[m[6]] = srow[6]; dst[m[7]] = srow[7];
                            }
                        } else {
                            for (int fy = 0; fy < 8; fy++) {
                                int sy = sy0 + fy;
                                const u8* m = morton_row[7 - fy];
                                for (int fx = 0; fx < 8; fx++) {
                                    int sx = stx + fx;
                                    dst[m[fx]] = (sx < s->w && sy < s->h)
                                                     ? px8[sy * s->w + sx] : 0;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* This function runs once per melted tile — hundreds of times in a
     * heavy animation frame — so the work here lands directly in the
     * MELTSPIKE stutter budget. Two hot-path cuts, both measured off the
     * user's stutter session:
     *
     * 1. MEMO the entry list: melts stream many tiles of ONE sheet
     *    back-to-back, but each notify was re-scanning all CACHE_MAX(512)
     *    entries. Memo the per-texture entry list for (idx, frame); every
     *    memo'd pointer is re-verified before use, so a stale memo is
     *    harmless (an entry recycled mid-frame just fails the check; a
     *    BRAND-NEW entry created mid-frame was built from the
     *    already-melted source, so it doesn't need the patch).
     *
     * 2. Patch only variants BOUND RECENTLY (last 2 frames) or pinned. A
     *    melting sheet averaged ~5 live cache entries (palette variants),
     *    and every variant got the full morton rewrite for every tile even
     *    though only the bound one is drawn. Stale variants are lazily
     *    invalidated instead: they full-rebuild IF ever bound again
     *    (atlas: version flip since its find path ignores `dirty`;
     *    pool: dirty flag). Actively-flashing palettes alternate binds
     *    within 1-2 frames, so both live variants still get patched —
     *    no visual change. */
    static int memo_idx = -1;
    static u32 memo_frame = 0xFFFFFFFFu;
    static CacheEntry* memo_e[12];
    static int memo_n = 0;
    static bool memo_overflow = false;

    if (idx != memo_idx || frame_number != memo_frame) {
        memo_idx = idx;
        memo_frame = frame_number;
        memo_n = 0;
        memo_overflow = false;
        for (int i = 0; i < CACHE_MAX; i++) {
            CacheEntry* e = &gpu_cache[i];
            if (!e->allocated || e->pending_delete) continue;
            if (e->texture_index != idx) continue;
            if (memo_n >= 12) { memo_overflow = true; break; }
            memo_e[memo_n++] = e;
        }
    }

    if (memo_overflow) {
        /* >12 live variants (never observed; ~5 typical) — correctness
         * first: patch everything via the full scan. */
        for (int i = 0; i < CACHE_MAX; i++) {
            CacheEntry* e = &gpu_cache[i];
            if (!e->allocated || e->pending_delete) continue;
            if (e->texture_index != idx) continue;
            cache_update_entry_region(e, x, y, w, h);
            dbg_regionupd_patched++;
        }
        return;
    }

    for (int i = 0; i < memo_n; i++) {
        CacheEntry* e = memo_e[i];
        if (!e->allocated || e->pending_delete || e->texture_index != idx)
            continue; /* memo gone stale — skip safely */
        if (e->pinned || e->last_used_frame + 10 >= frame_number) {
            /* 10-frame window: cyclically-bound variants (palette flashes,
             * alternating fx) stay hot-patched; a 2-frame window pushed them
             * into repeated 6.8ms full rebuilds (measured). */
            cache_update_entry_region(e, x, y, w, h);
            dbg_regionupd_patched++;
        } else if (e->atlas_cell >= 0) {
            /* force rebuild on next bind — idempotent across repeated
             * notifies (XOR of the CURRENT version, not of e's) */
            e->tex_version = texture_versions[idx] ^ 0x80000000u;
            atlas_l8_valid[e->atlas_cell] = false;
        } else {
            e->dirty = true; /* pool find path honors dirty */
        }
    }
}

void SDLGameRenderer_PinTexture(unsigned int th) {
    int tex_idx = LO_16_BITS(th) - 1;
    int pal_idx = HI_16_BITS(th);

    if (tex_idx < 0 || tex_idx >= FL_TEXTURE_MAX) return;
    if (pal_idx > 0) pal_idx -= 1;
    else pal_idx = -1;

    if (!cache_find(tex_idx, pal_idx)) {
        SDLGameRenderer_PrewarmTexture(th);
    }
    cache_set_pinned(tex_idx, pal_idx, true);
}

void SDLGameRenderer_UnpinTexture(unsigned int th) {
    int tex_idx = LO_16_BITS(th) - 1;
    int pal_idx = HI_16_BITS(th);

    if (tex_idx < 0 || tex_idx >= FL_TEXTURE_MAX) return;
    if (pal_idx > 0) pal_idx -= 1;
    else pal_idx = -1;

    cache_set_pinned(tex_idx, pal_idx, false);
}

// ---------------------------------------------------------------------------
// Public API: palette management
// ---------------------------------------------------------------------------

void SDLGameRenderer_CreatePalette(unsigned int ph) {
    int idx = HI_16_BITS(ph) - 1;
    if (idx < 0 || idx >= FL_PALETTE_MAX) return;

    const FLTexture* fp = &flPalette[idx];
    const void* px = flPS2GetSystemBuffAdrs(fp->mem_handle);
    int color_count = fp->width * fp->height;

    size_t color_size = 0;
    switch (fp->format) {
    case SCE_GS_PSMCT32: color_size = 4; break;
    case SCE_GS_PSMCT16: color_size = 2; break;
    default: return;
    }

    SrcPalette* p = &src_palettes[idx];
    texel_t new_colors[256];

    if (color_count == 16) {
        for (int i = 0; i < 16; i++) {
            if (color_size == 2) {
                new_colors[i] = color16_to_rgba(((const u16*)px)[i]);
            } else {
                new_colors[i] = color32_to_rgba(((const u32*)px)[i]);
            }
        }
        new_colors[0] = 0; /* Index 0 = transparent on CPS3 */
    } else if (color_count == 256) {
        for (int i = 0; i < 256; i++) {
            int ci = clut_shuf(i);
            if (color_size == 2) {
                new_colors[i] = color16_to_rgba(((const u16*)px)[ci]);
            } else {
                new_colors[i] = color32_to_rgba(((const u32*)px)[ci]);
            }
        }
        new_colors[0] = 0; /* Index 0 = transparent on CPS3 */
    } else {
        return;
    }

    u32 checksum = hash_bytes_fnv1a(new_colors, (size_t)color_count * sizeof(texel_t));
    bool changed = !p->valid ||
                   p->count != color_count ||
                   p->checksum != checksum ||
                   memcmp(p->colors, new_colors, (size_t)color_count * sizeof(texel_t)) != 0;

    p->count = color_count;
    memcpy(p->colors, new_colors, (size_t)color_count * sizeof(texel_t));
    p->checksum = checksum;
    p->valid = true;
    if (changed) {
        palette_versions[idx]++;
        cache_mark_palette_dirty(idx);
    }
}

void SDLGameRenderer_DestroyPalette(unsigned int palette_handle) {
    int idx = palette_handle - 1;
    if (idx < 0 || idx >= FL_PALETTE_MAX) return;

    destroy_palette_counts[idx]++;
    cache_invalidate_palette(idx);
    pending_remove_palette(idx);
    if (current_palette_index == idx) {
        current_cache_entry = NULL;
    }
    /* Deliberately do NOT wipe src_palettes[idx] (mirrors the lazy-release
     * rationale in SDLGameRenderer_DestroyTexture): palette-cycling
     * animations (char-select background, stage backdrops, score screen)
     * release and recreate the same palette handle every frame, and content
     * can be BOUND in the window between release and recreate. Wiping here
     * made those binds build cache entries with pal=NULL, whose texels all
     * resolve transparent — invisible backgrounds with no failure counters.
     * Keeping the last-known-good colors lets the in-between bind render;
     * the recreate's capture overwrites them immediately after. */
}

void SDLGameRenderer_UnlockPalette(unsigned int ph) {
    int idx = ph;
    if (idx > 0 && idx <= FL_PALETTE_MAX) {
        unlock_palette_calls++;
        int pi = idx - 1;
        u32 old_version = palette_versions[pi];
        SDLGameRenderer_CreatePalette(ph << 16);
        if (pi >= 0 && pi < FL_PALETTE_MAX && palette_versions[pi] != old_version) {
            unlock_palette_changed++;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API: explicit prewarm
// ---------------------------------------------------------------------------

void SDLGameRenderer_PrewarmTexture(unsigned int th) {
    int tex_idx = LO_16_BITS(th) - 1;
    int pal_idx = HI_16_BITS(th);

    if (tex_idx < 0 || tex_idx >= FL_TEXTURE_MAX) return;

    if (pal_idx > 0) {
        pal_idx -= 1;
    } else {
        pal_idx = -1;
    }

    if (cache_find_clean(tex_idx, pal_idx)) {
        return;
    }

    if (!cache_create(tex_idx, pal_idx)) {
        queue_pending_texture(tex_idx, pal_idx);
    }
}

// ---------------------------------------------------------------------------
// Public API: set current texture + palette for subsequent draw calls
// ---------------------------------------------------------------------------

void SDLGameRenderer_SetTexture(unsigned int th) {
    current_texture_index = LO_16_BITS(th) - 1;
    current_palette_index = HI_16_BITS(th);

    if (current_palette_index > 0) {
        current_palette_index -= 1;
    } else {
        current_palette_index = -1;
    }

    // Do cache lookup ONCE here, not per-draw
    current_cache_entry = NULL;
    if (current_texture_index < 0) return;
    settex_calls++;

    if (current_palette_index < 0) settex_nopal++;

    current_cache_entry = cache_find_clean(current_texture_index, current_palette_index);
    if (!current_cache_entry) {
        settex_miss++;
        /* Miss or dirty — try immediate rebuild */
        current_cache_entry = cache_create(current_texture_index, current_palette_index);
        if (current_cache_entry) {
            settex_create++;
        } else {
            settex_fail++;
            queue_pending_texture(current_texture_index, current_palette_index);
            /* Fall back to dirty entry rather than nothing */
            current_cache_entry = cache_find(current_texture_index, current_palette_index);
        }
    }

    /* TRIAL FIX REVERTED: pinning ALL bg-chip entries regressed stages —
     * scrolling/parallax stage backdrops cycle through far more distinct
     * (tex,pal) combinations than a static menu screen, so permanently
     * pinning every one starved character-sprite/effect cache entries of
     * capacity. Left unpinned; see pt24p/q memory for the still-open bug. */
}

// ---------------------------------------------------------------------------
// Public API: draw calls
// ---------------------------------------------------------------------------

// DrawSprite: axis-aligned rect from v[0] (top-left) and v[3] (bottom-right),
// with UVs from t[0] and t[3]. Matches the PC port exactly.
void SDLGameRenderer_DrawSprite(const Sprite* sprite, unsigned int color) {
    if (No_Trans) return;

    // Positions: v[0] = top-left, v[3] = bottom-right
    // Expand to full 4-vertex quad (axis-aligned)
    float px[4], py[4], us[4], vs[4];

    px[0] = sprite->v[0].x;  py[0] = sprite->v[0].y;  // top-left
    px[1] = sprite->v[3].x;  py[1] = sprite->v[0].y;  // top-right
    px[2] = sprite->v[0].x;  py[2] = sprite->v[3].y;  // bottom-left
    px[3] = sprite->v[3].x;  py[3] = sprite->v[3].y;  // bottom-right

    us[0] = sprite->t[0].s;  vs[0] = sprite->t[0].t;  // top-left
    us[1] = sprite->t[3].s;  vs[1] = sprite->t[0].t;  // top-right
    us[2] = sprite->t[0].s;  vs[2] = sprite->t[3].t;  // bottom-left
    us[3] = sprite->t[3].s;  vs[3] = sprite->t[3].t;  // bottom-right

    push_textured_task(px, py, us, vs, sprite->v[0].z, color);
}

// DrawSprite2: 2-vertex sprite, expanded to 4-vertex axis-aligned quad.
// Matches the PC port: builds a Sprite from Sprite2 then calls DrawSprite.
void SDLGameRenderer_DrawSprite2(const Sprite2* sprite2) {
    if (No_Trans) return;

    Sprite sprite;
    memset(&sprite, 0, sizeof(sprite));

    sprite.v[0] = sprite2->v[0];
    sprite.v[1].x = sprite2->v[1].x;
    sprite.v[1].y = sprite2->v[0].y;
    sprite.v[1].z = sprite2->v[0].z;
    sprite.v[2].x = sprite2->v[0].x;
    sprite.v[2].y = sprite2->v[1].y;
    sprite.v[2].z = sprite2->v[0].z;
    sprite.v[3] = sprite2->v[1];
    sprite.v[3].z = sprite2->v[0].z;

    sprite.t[0] = sprite2->t[0];
    sprite.t[1].s = sprite2->t[1].s;
    sprite.t[1].t = sprite2->t[0].t;
    sprite.t[2].s = sprite2->t[0].s;
    sprite.t[2].t = sprite2->t[1].t;
    sprite.t[3] = sprite2->t[1];

    SDLGameRenderer_DrawSprite(&sprite, sprite2->vertex_color);
}

// DrawTexturedQuad: arbitrary 4-vertex quad. Preserves all 4 positions and UVs.
// citro2d only supports axis-aligned rects, so we compute the bounding box of
// both positions and UVs. This handles the common axis-aligned case correctly
// and provides a reasonable approximation for rotated quads.
void SDLGameRenderer_DrawTexturedQuad(const Sprite* sprite, unsigned int color) {
    if (No_Trans) return;

    float px[4], py[4], us[4], vs[4];

    for (int i = 0; i < 4; i++) {
        px[i] = sprite->v[i].x;
        py[i] = sprite->v[i].y;
        us[i] = sprite->t[i].s;
        vs[i] = sprite->t[i].t;
    }

    push_textured_task(px, py, us, vs, sprite->v[0].z, color);
}

// DrawSolidQuad: 4-vertex solid colored quad (no texture).
void SDLGameRenderer_DrawSolidQuad(const Quad* quad, unsigned int color) {
    if (No_Trans) return;

    float px[4], py[4];

    for (int i = 0; i < 4; i++) {
        px[i] = quad->v[i].x;
        py[i] = quad->v[i].y;
    }

    push_solid_task(px, py, quad->v[0].z, color);
}

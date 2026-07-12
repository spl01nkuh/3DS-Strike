#include "Game/MTRANS.h"
#include "common.h"
#include "common/graphics.h"
#include "ctr/ctr_game_renderer.h"
//#include "sf33rd/AcrSDK/ps2/flps2render.h"
//#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "fl.h"

#include "psp/PPGFile.h"
#include "Game/DC_Ghost.h"
#include "Game/EFFECT.h"
#include "Game/WORK_SYS.h"
#include "Game/aboutspr.h"
#include "Game/chren3rd.h"
#include "Game/color3rd.h"
#include "Game/debug/Debug.h"
#include "Game/texcash.h"
#include "Game/texgroup.h"
//#include "PS2/ps2Quad.h"
#include "structs.h"

typedef struct {
    Sprite2* chip;
    u16 sprTotal;
    u16 sprMax;
    s8 up[24];
} SpriteChipSet;

// sbss
s32 curr_bright;
SpriteChipSet seqs_w;

// bss
u32 PrioBase[PRIO_BASE_SIZE];
u32 PrioBaseOriginal[PRIO_BASE_SIZE];

// rodata
static const u16 flptbl[4] = { 0x0000, 0x8000, 0x4000, 0xC000 };

static const u32 bright_type[4][16] = { { 0x00FFFFFF,
                                          0x00EEEEEE,
                                          0x00DDDDDD,
                                          0x00CCCCCC,
                                          0x00BBBBBB,
                                          0x00AAAAAA,
                                          0x00999999,
                                          0x00888888,
                                          0x00777777,
                                          0x00666666,
                                          0x00555555,
                                          0x00444444,
                                          0x00333333,
                                          0x00222222,
                                          0x00111111,
                                          0x00000000 },
                                        { 0x00FFFFFF,
                                          0x00FFEEEE,
                                          0x00FFDDDD,
                                          0x00FFCCCC,
                                          0x00FFBBBB,
                                          0x00FFAAAA,
                                          0x00FF9999,
                                          0x00FF8888,
                                          0x00FF7777,
                                          0x00FF6666,
                                          0x00FF5555,
                                          0x00FF4444,
                                          0x00FF3333,
                                          0x00FF2222,
                                          0x00FF1111,
                                          0x00FF0000 },
                                        { 0x00FFFFFF,
                                          0x00EEFFEE,
                                          0x00DDFFDD,
                                          0x00CCFFCC,
                                          0x00BBFFBB,
                                          0x00AAFFAA,
                                          0x0099FF99,
                                          0x0088FF88,
                                          0x0077FF77,
                                          0x0066FF66,
                                          0x0055FF55,
                                          0x0044FF44,
                                          0x0033FF33,
                                          0x0022FF22,
                                          0x0011FF11,
                                          0x0000FF00 },
                                        { 0x00FFFFFF,
                                          0x00EEEEFF,
                                          0x00DDDDFF,
                                          0x00CCCCFF,
                                          0x00BBBBFF,
                                          0x00AAAAFF,
                                          0x009999FF,
                                          0x008888FF,
                                          0x007777FF,
                                          0x006666FF,
                                          0x005555FF,
                                          0x004444FF,
                                          0x003333FF,
                                          0x002222FF,
                                          0x001111FF,
                                          0x000000FF } };

// forward decls
static void DebugLine(f32 x, f32 y, f32 w, f32 h);
s32 seqsStoreChip(f32 x, f32 y, s32 w, s32 h, s32 gix, s32 code, s32 attr, s32 alpha, s32 id);
void appRenewTempPriority(s32 z);
static s16 check_patcash_ex_trans(PatternCollection* padr, u32 cg);
static s32 get_free_patcash_index(PatternCollection* padr);
static s32 get_mltbuf16(MultiTexture* mt, u32 code, u32 palt, s32* ret);
static s32 get_mltbuf16_ext(MultiTexture* mt, u32 code, u32 palt);
static s32 get_mltbuf16_ext_2(MultiTexture* mt, u32 code, u32 palt, s32* ret, PatternInstance* cp);
static s32 get_mltbuf32(MultiTexture* mt, u32 code, u32 palt, s32* ret);
static s32 get_mltbuf32_ext(MultiTexture* mt, u32 code, u32 palt);
static s32 get_mltbuf32_ext_2(MultiTexture* mt, u32 code, u32 palt, s32* ret, PatternInstance* cp);
static inline void lz_ext_p6_fx(u8* srcptr, u8* dstptr, u32 len);
static void lz_ext_p6_cx(u8* srcptr, u16* dstptr, u32 len, u16* palptr);
static inline u16 x16_mapping_set(PatternMap* map, s32 code);
static inline u16 x32_mapping_set(PatternMap* map, s32 code);

/* ---- 3DS BACKGROUND PREWARM -------------------------------------------
 * First-use of a move used to hitch: its tiles LZ-decompress + GPU-upload
 * synchronously mid-fight. With lazy slot release (see texcash.c), decoded
 * tiles now persist — so we can decode a character's WHOLE tile set ahead
 * of time and it stays warm. A (mt, group) pair self-registers on its first
 * pattern build (this happens during the round-intro poses), and
 * mlt_prewarm_tick() — called once per frame from texture_cash_update() —
 * walks the group's full CG table decoding missing tiles under a strict
 * per-frame time budget. By "FIGHT!" both characters are fully decoded. */
/* P1-vs-P2 char-select diagnosis: per-group pattern-instance cache hit/build
 * counters. A parked preview that keeps BUILDING (vs hitting) means its CG
 * loop cycles the 64-entry PatternCollection pool → constant rebuild work.
 * Read as deltas by the GRPSTAT probe (texcash.c). */
u32 g_pat_build[24];
u32 g_pat_hit[24];

#define PREWARM_JOBS_MAX 12 /* fights can now register fx + cx jobs per
                             * player plus stage/menu groups */
typedef struct {
    MultiTexture* mt;
    s32 group;
    s32 palo;      /* cx mode: the player's color code (wk->colcd) */
    u32 next_cg;
    u32 decoded;   /* wrap guard: stop if we decode more than capacity */
    u8 state;      /* 0=free 1=running 2=done */
    u8 cx;         /* 0 = indexed tiles (lz_ext_p6_fx, palt 0)
                      1 = palette-BAKED 16-bit tiles (lz_ext_p6_cx with
                          ColorRAM[(attr&0x1FF)+palo], upload size<<1) —
                          the path every NON-DEFAULT player color uses; the
                          fx-only prewarm covered none of it (user-confirmed
                          first-use stutter when playing an alt color). */
} PrewarmJob;
static PrewarmJob prewarm_jobs[PREWARM_JOBS_MAX];
extern const u8 obj_group_table[37664];
extern u16 ColorRAM[512][64];

void mlt_prewarm_reset(void) {
    for (int i = 0; i < PREWARM_JOBS_MAX; i++)
        prewarm_jobs[i].state = 0;
}

/* Loading-window support (user plan: preload around scene transitions).
 * While boosted, the per-frame tick budget grows ~3x so whole charsets
 * finish decoding inside the VS card / round intro / fades instead of
 * trickling at 1.5ms into gameplay. Armed on every G_No scene change
 * (texcash.c), which covers all four planned transition points. */
static u32 prewarm_boost_frames = 0;

void mlt_prewarm_boost(u32 frames) {
    if (frames > prewarm_boost_frames)
        prewarm_boost_frames = frames;
}

/* Re-arm completed jobs so a new scene re-verifies its groups (tiles may
 * have been evicted since; already-cached tiles skip instantly). */
void mlt_prewarm_restart(void) {
    for (int i = 0; i < PREWARM_JOBS_MAX; i++) {
        if (prewarm_jobs[i].state == 2) {
            prewarm_jobs[i].state = 1;
            prewarm_jobs[i].next_cg = 0;
            prewarm_jobs[i].decoded = 0;
        }
    }
}

static void prewarm_register_mode(MultiTexture* mt, s32 group, u8 cx, s32 palo) {
    int free_i = -1;
    if (!mt) return;
    for (int i = 0; i < PREWARM_JOBS_MAX; i++) {
        if (prewarm_jobs[i].state != 0 &&
            prewarm_jobs[i].mt == mt && prewarm_jobs[i].group == group &&
            prewarm_jobs[i].cx == cx && prewarm_jobs[i].palo == palo)
            return; /* already running or done */
        if (prewarm_jobs[i].state == 0 && free_i < 0)
            free_i = i;
    }
    if (free_i < 0) return;
    prewarm_jobs[free_i].mt = mt;
    prewarm_jobs[free_i].group = group;
    prewarm_jobs[free_i].palo = palo;
    prewarm_jobs[free_i].cx = cx;
    prewarm_jobs[free_i].next_cg = 0;
    prewarm_jobs[free_i].decoded = 0;
    prewarm_jobs[free_i].state = 1;
}

static void prewarm_register(MultiTexture* mt, s32 group) {
    prewarm_register_mode(mt, group, 0, 0);
}

/* allocate a slot for prewarmed content: cached-but-unreferenced (time=0).
 * Returns slot index if the tile needs decoding, -1 if already cached or no
 * slot is available. Mirrors get_mltbuf16_ext_2 minus PatternInstance maps. */
/* Prewarm slot claim: FREE slots only — NEVER evict. An earlier version
 * clock-evicted time<=0 slots; walking a full charset through a smaller
 * cache then perpetually evicted the game's own on-demand-cached tiles
 * (including ones about to be drawn), so first-use missed everything again
 * AND the churn flooded the renderer with rebuilds (measured: BUILDSPIKE
 * 97→522). Returns: slot index to decode, -1 = already cached (skip),
 * -2 = cache full (job should stop — prewarm is opportunistic only). */
static s32 prewarm_slot16(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh16;
    u16* used = mt->tpu->x16_used;
    for (s32 i = 0; i < mt->tpu->x16; i++) {
        if (mc[used[i]].cs.code == code && mc[used[i]].state == palt)
            return -1; /* already cached */
    }
    if ((mt->tpu->x16 != mt->mltnum16) && mt->tpf->x16) {
        mt->tpf->x16 -= 1;
        u16 slot = mt->tpf->x16_free[mt->tpf->x16];
        mt->tpu->x16_used[mt->tpu->x16] = slot;
        mt->tpu->x16 += 1;
        mc[slot].cs.code = code;
        mc[slot].state = palt;
        mc[slot].time = 0;
        return slot;
    }
    return -2; /* no free slot — leave the live cache alone */
}

/* Non-ext (lifetime-based) groups — menus, HUD, objects — have NO tpf/tpu
 * lists; their slots live or die by a per-slot countdown that
 * mlt_obj_trans_update decrements every frame. Prewarm mirrors
 * get_mltbuf16/32: find the code, else claim a code==-1 slot with the
 * group's normal lifetime, so prewarmed tiles decay exactly like real use
 * (never starving the cache). */
static s32 prewarm_slot16_life(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh16;
    s32 b = -1;
    for (s32 i = 0; i < mt->mltnum16; i++) {
        if (mc[i].cs.code == code && mc[i].state == palt)
            return -1; /* already cached */
        if (mc[i].cs.code == -1 && b < 0)
            b = i;
    }
    if (b < 0) return -2; /* full — real users take priority */
    mc[b].cs.code = code;
    mc[b].state = palt;
    mc[b].time = mt->mltcshtime16;
    return b;
}

static s32 prewarm_slot32_life(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh32;
    s32 b = -1;
    for (s32 i = 0; i < mt->mltnum32; i++) {
        if (mc[i].cs.code == code && mc[i].state == palt)
            return -1;
        if (mc[i].cs.code == -1 && b < 0)
            b = i;
    }
    if (b < 0) return -2;
    mc[b].cs.code = code;
    mc[b].state = palt;
    mc[b].time = mt->mltcshtime32;
    return b;
}

static s32 prewarm_slot32(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh32;
    u16* used = mt->tpu->x32_used;
    for (s32 i = 0; i < mt->tpu->x32; i++) {
        if (mc[used[i]].cs.code == code && mc[used[i]].state == palt)
            return -1;
    }
    if ((mt->tpu->x32 != mt->mltnum32) && mt->tpf->x32) {
        mt->tpf->x32 -= 1;
        u16 slot = mt->tpf->x32_free[mt->tpf->x32];
        mt->tpu->x32_used[mt->tpu->x32] = slot;
        mt->tpu->x32 += 1;
        mc[slot].cs.code = code;
        mc[slot].state = palt;
        mc[slot].time = 0;
        return slot;
    }
    return -2; /* no free slot — see prewarm_slot16 */
}

void mlt_prewarm_tick(void) {
    /* NOT during character select: the hovered character's super-art preview
     * animates from the very sheets the prewarm would be writing — the
     * region-update churn made P1's preview visibly slow (P2, which triggers
     * no hover loads, stayed full speed; user-reproduced). The round intro
     * still gives the prewarm ~3s before the first playable frame. */
    extern u8 G_No[4];
    if (G_No[0] == 2 && G_No[1] == 1) return;

    /* ~1.5ms budget per frame normally; ~5ms inside a post-transition
     * loading window (VS card, round intro, fades — scenes with slack),
     * so whole charsets finish before gameplay. */
    u64 budget = (u64)(0.0015 * 268111856.0);
    if (prewarm_boost_frames) {
        prewarm_boost_frames--;
        budget = (u64)(0.005 * 268111856.0);
    }
    u64 t0 = svcGetSystemTick();

    /* njReLoadTexturePartNumG uploads through the GLOBAL current-data-list
     * (ppg_w.cur) — the real decode sites run with the group's list already
     * current, but this tick runs at an arbitrary point in the frame. Set
     * the target group's list per job and restore the game's afterward;
     * without this, prewarm uploads wrote tiles into UNRELATED groups
     * (user-visible sprite flicker + palette corruption). */
    extern void* ppgGetCurrentDataListPtr(void);
    extern void ppgSetCurrentDataListPtr(void* p);
    void* saved_dlist = ppgGetCurrentDataListPtr();

    /* Two passes: CX jobs (real player colors) FIRST. The fx (palt=0) job
     * for a group a colored player uses would otherwise fill the whole free
     * pool with palette-0 tiles that player never draws, and the cx job
     * would then hit cache-full immediately — measured: a colored player's
     * special produced a 24-frame melt wall because its REAL tiles never
     * got prewarmed. An fx job is skipped outright when a cx twin exists. */
    for (int pass = 0; pass < 2; pass++) {
    for (int j = 0; j < PREWARM_JOBS_MAX; j++) {
        PrewarmJob* job = &prewarm_jobs[j];
        if (job->state != 1) continue;
        if (pass == 0 && !job->cx) continue; /* cx first */
        if (pass == 1 && job->cx) continue;
        if (!job->cx) {
            int has_cx_twin = 0;
            for (int k = 0; k < PREWARM_JOBS_MAX; k++) {
                if (prewarm_jobs[k].state != 0 && prewarm_jobs[k].cx &&
                    prewarm_jobs[k].mt == job->mt &&
                    prewarm_jobs[k].group == job->group) {
                    has_cx_twin = 1;
                    break;
                }
            }
            if (has_cx_twin) { job->state = 2; continue; }
        }
        MultiTexture* mt = job->mt;
        s32 grp = job->group;
        if (!texgrplds[grp].ok) { job->state = 2; continue; }
        ppgSetCurrentDataListPtr((void*)&mt->texList);

        u32* textbl = (u32*)texgrplds[grp].texture_table;
        u32* trstbl = (u32*)texgrplds[grp].trans_table;
        u32 cap = (u32)(mt->mltnum16 + mt->mltnum32);
        /* Both tables are SELF-DESCRIBING: entry 0's byte offset delimits the
         * offset-index array itself, giving exact entry counts. Walking past
         * these bounds fed garbage to the LZ decompressor (scratch-buffer
         * overrun → the "holes in sprites" corruption). */
        u32 n_max = trstbl[0] >> 2;
        u32 code_max = textbl[0] >> 2;

        while (job->next_cg < sizeof(obj_group_table)) {
            u32 cg = job->next_cg;
            if (obj_group_table[cg] != (u8)grp) { job->next_cg++; continue; }
            s32 n = (s32)cg - (s32)texgrpdat[grp].num_of_1st;
            if (n < 0 || (u32)n >= n_max) { job->next_cg++; continue; }
            if (trstbl[n] == 0 || (trstbl[n] & 1)) { job->next_cg++; continue; }

            u16* trsbas = (u16*)((uintptr_t)trstbl + trstbl[n]);
            s32 count = *trsbas;
            trsbas++;
            TileMapEntry* trsptr = (TileMapEntry*)trsbas;
            s32 cache_full = 0;
            if (count > 0 && count <= 64) { /* sanity guard on table holes */
                PatternCode cc;
                cc.parts.group = grp;
                for (s32 c = 0; c < count; c++, trsptr++) {
                    if ((u32)trsptr->code >= code_max) continue; /* hole */
                    if (textbl[trsptr->code] == 0) continue;
                    TEX* texptr = (TEX*)((uintptr_t)textbl + textbl[trsptr->code]);
                    s32 wh = (texptr->wh & 3) + 1;
                    s32 size = (wh * wh) << 6;
                    cc.parts.offset = trsptr->code;
                    /* cx jobs: palette baked per tile, same formula as the
                     * rgb builder (palt = (attr & 0x1FF) + player colcd) */
                    u32 palt = 0;
                    if (job->cx) {
                        palt = (u32)((trsptr->attr & 0x1FF) + job->palo);
                        if (palt >= 512) continue; /* ColorRAM bound */
                    }
                    s32 slot = -1;
                    if (wh == 1 || wh == 2) {
                        slot = mt->ext ? prewarm_slot16(mt, cc.code, palt)
                                       : prewarm_slot16_life(mt, cc.code, palt);
                        if (slot >= 0) {
                            if (job->cx) {
                                lz_ext_p6_cx(&((u8*)texptr)[1], (u16*)mt->mltbuf,
                                             size, (u16*)(ColorRAM[palt]));
                                njReLoadTexturePartNumG(mt->mltgidx16 + (slot >> 8),
                                                        mt->mltbuf, slot & 0xFF, size << 1);
                            } else {
                                lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                                njReLoadTexturePartNumG(mt->mltgidx16 + (slot >> 8),
                                                        mt->mltbuf, slot & 0xFF, size);
                            }
                            job->decoded++;
                        }
                    } else if (wh == 4) {
                        slot = mt->ext ? prewarm_slot32(mt, cc.code, palt)
                                       : prewarm_slot32_life(mt, cc.code, palt);
                        if (slot >= 0) {
                            if (job->cx) {
                                lz_ext_p6_cx(&((u8*)texptr)[1], (u16*)mt->mltbuf,
                                             size, (u16*)(ColorRAM[palt]));
                                njReLoadTexturePartNumG(mt->mltgidx32 + (slot >> 6),
                                                        mt->mltbuf, slot & 0x3F, size << 1);
                            } else {
                                lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                                njReLoadTexturePartNumG(mt->mltgidx32 + (slot >> 6),
                                                        mt->mltbuf, slot & 0x3F, size);
                            }
                            job->decoded++;
                        }
                    }
                    /* wh==3: not a valid size class — table hole, skip */
                    if (slot == -2) { cache_full = 1; break; }
                    /* honest budget: a 40-tile CG can blow well past the
                     * limit if only checked between CGs. Mid-list resume
                     * is safe: cached tiles skip instantly next tick. */
                    if ((c & 7) == 7 && svcGetSystemTick() - t0 > budget) {
                        ppgSetCurrentDataListPtr(saved_dlist);
                        return;
                    }
                }
            }
            if (cache_full) { job->state = 2; break; } /* opportunistic only */
            job->next_cg++;

            if (job->decoded > cap) { job->state = 2; break; } /* wrapped capacity */
            if (svcGetSystemTick() - t0 > budget) {
                ppgSetCurrentDataListPtr(saved_dlist);
                return; /* resume next frame */
            }
        }
        if (job->next_cg >= sizeof(obj_group_table)) job->state = 2;
    }
    } /* pass loop */
    ppgSetCurrentDataListPtr(saved_dlist);
}
/* ---- end 3DS BACKGROUND PREWARM ---------------------------------------- */

static void search_trsptr(uintptr_t trstbl, s32 i, s32 n, s32 cods, s32 atrs, s32 codd, s32 atrd) {
    s32 j;
    u16* tmpbas;
    s32 ctemp;
    TileMapEntry* tmpptr;
    TileMapEntry* unused_s4;

    atrd &= 0x3FFF;

    for (j = i; j < n; j++) {
        tmpbas = (u16*)(trstbl + ((u32*)trstbl)[j]);
        ctemp = *tmpbas;
        tmpbas++;
        tmpptr = (TileMapEntry*)tmpbas;

        while (ctemp != 0) {
            if (!(tmpptr->attr & 0x1000) && (tmpptr->code == cods) && ((tmpptr->attr & 0xF) == atrs)) {
                tmpptr->code = codd;
                tmpptr->attr = (tmpptr->attr & 0xC000) | atrd;
            }

            ctemp--;
            unused_s4 = tmpptr;
            tmpptr = unused_s4 + 1;
        }
    }
}

void mlt_obj_disp(MultiTexture* mt, WORK* wk, s32 base_y) {
    u16* trsbas;
    TileMapEntry* trsptr;
    s32 rnum;
    s32 attr;
    s32 palo;
    s32 count;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    s32 dw, dw2;
    s32 dh, dh2;

    ppgSetupCurrentDataList(&mt->texList);
    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("disp The trans data is not valid. Group number: %d\n", i);
        return;
    }

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    attr = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd & 0xF;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);

    u16 x_flip = attr & 0x8000;
    u16 y_flip = attr & 0x4000;
    u8 debug_10 = Debug_w[0x10];

    while (count--) {
        dw = ((trsptr->attr & 0xC00) >> 7) + 8;
        dh = ((trsptr->attr & 0x300) >> 5) + 8;
        if (x_flip) {
            x += trsptr->x;
            dw2 = dw;
        } else {
            x -= trsptr->x;
            dw2 = 0;
        }

        if (y_flip) {
            y -= trsptr->y;
            dh2 = dh;
        } else {
            y += trsptr->y;
            dh2 = 0;
        }


        if (!(trsptr->attr & 0x2000)) {
            if (debug_10) {
                DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - dw2,
                                 y + dh2,
                                 dw,
                                 dh,
                                 mt->mltgidx16,
                                 trsptr->code,
                                 palo + ((trsptr->attr ^ attr) & 0xE00F),
                                 wk->my_clear_level,
                                 mt->id);
        } else {
            if (debug_10) {
                DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - dw2,
                                 y + dh2,
                                 dw,
                                 dh,
                                 mt->mltgidx32,
                                 trsptr->code,
                                 palo + ((trsptr->attr ^ attr) & 0xE00F),
                                 wk->my_clear_level,
                                 mt->id);
        }

        if (!rnum) {
            break;
        }

        trsptr++;
    }

    seqs_w.up[mt->id] = 1;
    appRenewTempPriority(wk->position_z);
}

void mlt_obj_disp_rgb(MultiTexture* mt, WORK* wk, s32 base_y) {
    u16* trsbas;
    TileMapEntry* trsptr;
    s32 rnum;
    s32 attr;
    s32 count;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    s32 dw;
    s32 dh;

    ppgSetupCurrentDataList(&mt->texList);
    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("disp_rgb The trans data is not valid. Group number: %d\n", i);
        return;
    }

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    attr = flptbl[wk->cg_flip ^ wk->rl_flag];

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);

    while (count--) {
        if (attr & 0x8000) {
            x += trsptr->x;
        } else {
            x -= trsptr->x;
        }

        if (attr & 0x4000) {
            y -= trsptr->y;
        } else {
            y += trsptr->y;
        }

        dw = ((trsptr->attr & 0xC00) >> 7) + 8;
        dh = ((trsptr->attr & 0x300) >> 5) + 8;

        if (!(trsptr->attr & 0x2000)) {
            if (Debug_w[0x10]) {
                DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
            }
            
            rnum = seqsStoreChip(x - (dw * BOOL(attr & 0x8000)),
                                 y + (dh * BOOL(attr & 0x4000)),
                                 dw,
                                 dh,
                                 mt->mltgidx16,
                                 trsptr->code,
                                 (trsptr->attr ^ attr) & 0xE000,
                                 wk->my_clear_level,
                                 mt->id);
        } else {
            if (Debug_w[0x10]) {
                DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - (dw * BOOL(attr & 0x8000)),
                                 y + (dh * BOOL(attr & 0x4000)),
                                 dw,
                                 dh,
                                 mt->mltgidx32,
                                 trsptr->code,
                                 (trsptr->attr ^ attr) & 0xE000,
                                 wk->my_clear_level,
                                 mt->id);
        }

        if (rnum == 0) {
            break;
        }

        trsptr++;
    }

    seqs_w.up[mt->id] = 1;
    appRenewTempPriority(wk->position_z);
}

s16 getObjectHeight(u16 cgnum) {
    s32 count;
    TileMapEntry* trsptr;
    s16 maxHeight;
    u16* trsbas;
    s32 i = obj_group_table[cgnum];
    s16 height;

    if (i == 0) {
        return 0;
    }

    if (texgrplds[i].ok == 0) {
        return 0;
    }

    cgnum -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)((s8*)texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[cgnum]);
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;

    for (maxHeight = height = 0; count--; trsptr++) {
        height = height + trsptr->y;

        if (height > maxHeight) {
            maxHeight = height;
        }
    }

    if (height) {
        // do nothing
    }

    return maxHeight;
}


void mlt_obj_trans_ext(MultiTexture* mt, WORK* wk, s32 base_y) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    s32 rnum;
    s32 attr;
    s32 palo;
    s32 count;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    s16 ix;
    PatternCode cc;
    PatternInstance* cp;

    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("_ext The trans data is not valid. Group number: %d\n", i);
        return;
    }

    prewarm_register(mt, i); /* 3DS: queue background decode of this whole
                                group's tiles (see mlt_prewarm_tick) */

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    textbl = (u32*)texgrplds[i].texture_table;
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    attr = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);
    cc.parts.group = 0;
    cc.parts.offset = wk->cg_number;
    ix = check_patcash_ex_trans(mt->cpat, cc.code);
    if (mt->id < 24) { if (ix < 0) g_pat_build[mt->id]++; else g_pat_hit[mt->id]++; }

    u16 x_flip = attr & 0x8000;
    u16 y_flip = attr & 0x4000;
    u8 debug_10 = Debug_w[0x10];

    if (ix < 0) {
        {
            s32 size;
            s32 code;
            s32 wh;
            s32 dw, dw2;
            s32 dh, dh2;

            (void)dw;
            (void)dh;

            ix = get_free_patcash_index(mt->cpat);
            cp = &mt->cpat->patt[ix];
            mt->cpat->adr[mt->cpat->kazu] = cp;
            mt->cpat->kazu += 1;
            cp->curr_disp = 1;
            cp->time = mt->mltcshtime16;
            cp->cg.code = cc.code;
            cp->x16 = 0;
            cp->x32 = 0;
            memset(&cp->map, 0, sizeof(PatternMap));
            cc.parts.group = i;

            while (count--) {
                texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
                dw = (texptr->wh & 0xE0) >> 2;
                dh = (texptr->wh & 0x1C) << 1;

                if (x_flip) {
                    x += trsptr->x;
                    dw2 = dw;
                } else {
                    x -= trsptr->x;
                    dw2 = 0;
                }

                if (y_flip) {
                    y -= trsptr->y;
                    dh2 = dh;
                } else {
                    y += trsptr->y;
                    dh2 = 0;
                }

                wh = (texptr->wh & 3) + 1;
                size = (wh * wh) << 6;
                cc.parts.offset = trsptr->code;

                switch (wh) {
                case 1:
                case 2:
                    if (get_mltbuf16_ext_2(mt, cc.code, 0, &code, cp)) {
                        lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                        njReLoadTexturePartNumG(mt->mltgidx16 + (code >> 8), mt->mltbuf, code & 0xFF, size);
                    }

                    if (debug_10) {
                        DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr >> 15))), dw, dh);
                    }

                    rnum = seqsStoreChip(x - dw2,
                                         y + dh2,
                                         dw,
                                         dh,
                                         mt->mltgidx16,
                                         code,
                                         palo | ((trsptr->attr ^ attr) & 0xC000),
                                         wk->my_clear_level,
                                         mt->id);
                    break;

                case 4:
                    if (get_mltbuf32_ext_2(mt, cc.code, 0, &code, cp)) {
                        lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                        njReLoadTexturePartNumG(mt->mltgidx32 + (code >> 6), mt->mltbuf, code & 0x3F, size);
                    }

                    if (debug_10) {
                        DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
                    }

                    rnum = seqsStoreChip(x - dw2,
                                         y + dh2,
                                         dw,
                                         dh,
                                         mt->mltgidx32,
                                         code,
                                         palo | (((trsptr->attr ^ attr) & 0xC000) | 0x2000),
                                         wk->my_clear_level,
                                         mt->id);
                    break;
                }

                if (!rnum) {
                    break;
                }

                trsptr++;
            }

            seqs_w.up[mt->id] = 1;
            appRenewTempPriority(wk->position_z);
            return;
        }
    }

    {
        s32 code;
        s32 wh;
        s32 dw, dw2;
        s32 dh, dh2;

        (void)dw;
        (void)dh;

        cp = mt->cpat->adr[ix];
        cp->curr_disp = 1;
        cp->time = mt->mltcshtime16;

        makeup_tpu_free(mt->mltnum16 >> 8, mt->mltnum32 >> 6, &cp->map);
        cc.parts.group = i;

        u16 x_flip = attr & 0x8000;
        u16 y_flip = attr & 0x4000;
        u8 debug_10 = Debug_w[0x10];

        while (count--) {
            texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
            dw = (texptr->wh & 0xE0) >> 2;
            dh = (texptr->wh & 0x1C)  << 1;

            if (x_flip) {
                x += trsptr->x;
                dw2 = dw;
            } else {
                x -= trsptr->x;
                dw2 = 0;
            }

            if (y_flip) {
                y -= trsptr->y;
                dh2 = dh;
            } else {
                y += trsptr->y;
                dh2 = 0;
            }

            wh = (texptr->wh & 3) + 1;
            cc.parts.offset = trsptr->code;

            switch (wh) {
            case 1:
            case 2:
                code = get_mltbuf16_ext(mt, cc.code, 0);

                if (debug_10) {
                    DebugLine(x - (dw & ((s16)attr >> 16)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
                }

                rnum = seqsStoreChip(x - dw2,
                                     y + dh2,
                                     dw,
                                     dh,
                                     mt->mltgidx16,
                                     code,
                                     palo | ((trsptr->attr ^ attr) & 0xC000),
                                     wk->my_clear_level,
                                     mt->id);
                break;

            case 4:
                code = get_mltbuf32_ext(mt, cc.code, 0);

                if (debug_10) {
                    DebugLine(x - (dw & ((s16)attr >> 16)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
                }

                rnum = seqsStoreChip(x - dw2,
                                     y + dh2,
                                     dw,
                                     dh,
                                     mt->mltgidx32,
                                     code,
                                     palo | (((trsptr->attr ^ attr) & 0xC000) | 0x2000),
                                     wk->my_clear_level,
                                     mt->id);
                break;
            }

            if (!rnum) {
                break;
            }

            trsptr++;
        }

        seqs_w.up[mt->id] = 1;
        appRenewTempPriority(wk->position_z);
    }
}

void mlt_obj_trans(MultiTexture* mt, WORK* wk, s32 base_y) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    s32 rnum;
    s32 attr;
    s32 count;
    s32 palo;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    PatternCode cc;
    s32 size;
    s32 code;
    s32 wh;
    s32 dw;
    s32 dh;

    ppgSetupCurrentDataList(&mt->texList);

    if (mt->ext) {
        mlt_obj_trans_ext(mt, wk, base_y);
        return;
    }

    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("_ The trans data is not valid. Group number: %d\n", i);
        return;
    }

    prewarm_register(mt, i); /* 3DS: queue background decode of this whole
                                group's tiles (see mlt_prewarm_tick) */

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    textbl = (u32*)texgrplds[i].texture_table;
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    attr = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);
    cc.parts.group = i;

    while (count--) {
        if (attr & 0x8000) {
            x += trsptr->x;
        } else {
            x -= trsptr->x;
        }

        if (attr & 0x4000) {
            y -= trsptr->y;
        } else {
            y += trsptr->y;
        }

        texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
        dw = (texptr->wh & 0xE0) >> 2;
        dh = (texptr->wh & 0x1C) << 1;
        wh = (texptr->wh & 3) + 1;
        size = (wh * wh) << 6;
        cc.parts.offset = trsptr->code;

        switch (wh) {
        case 1:
        case 2:
            if (get_mltbuf16(mt, cc.code, 0, &code)) {
                lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                njReLoadTexturePartNumG(mt->mltgidx16 + (code >> 8), mt->mltbuf, code & 0xFF, size);
            }

            if (Debug_w[0x10]) {
                DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - (dw * BOOL(attr & 0x8000)),
                                 y + (dh * BOOL(attr & 0x4000)),
                                 dw,
                                 dh,
                                 mt->mltgidx16,
                                 code,
                                 palo | ((trsptr->attr ^ attr) & 0xC000),
                                 wk->my_clear_level,
                                 mt->id);
            break;

        case 4:
            if (get_mltbuf32(mt, cc.code, 0, &code)) {
                lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                njReLoadTexturePartNumG(mt->mltgidx32 + (code >> 6), mt->mltbuf, code & 0x3F, size);
            }

            if (Debug_w[0x10]) {
                DebugLine(x - (dw & ((s16)attr >> 0x10)), y + (dh & ((s16)(attr << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - (dw * BOOL(attr & 0x8000)),
                                 y + (dh * BOOL(attr & 0x4000)),
                                 dw,
                                 dh,
                                 mt->mltgidx32,
                                 code,
                                 palo | (((trsptr->attr ^ attr) & 0xC000) | 0x2000),
                                 wk->my_clear_level,
                                 mt->id);
            break;
        }

        if (rnum == 0) {
            break;
        }

        trsptr++;
    }

    seqs_w.up[mt->id] = 1;
    appRenewTempPriority(wk->position_z);
}

// DEMMA THIS IS THE ONE THE CAR USES

void mlt_obj_trans_cp3_ext(MultiTexture* mt, WORK* wk, s32 base_y) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    s32 rnum;
    s32 flip;
    s32 palo;
    s32 count;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    s16 ix;
    PatternCode cc;
    PatternInstance* cp;

    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("cp3_ext The trans data is not valid. Group number: %d\n", i);
        return;
    }

    prewarm_register(mt, i); /* 3DS background prewarm */

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    textbl = (u32*)texgrplds[i].texture_table;
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    flip = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);
    cc.parts.group = 0;
    cc.parts.offset = wk->cg_number;
    ix = check_patcash_ex_trans(mt->cpat, cc.code);
    if (mt->id < 24) { if (ix < 0) g_pat_build[mt->id]++; else g_pat_hit[mt->id]++; }

    u16 x_flip = flip & 0x8000;
    u16 y_flip = flip & 0x4000;
    u8 debug_10 = Debug_w[0x10];

    if (ix < 0) {
        {
            s32 size;
            s32 code;
            s32 wh;
            s32 dw, dw2;
            s32 dh, dh2;
            s32 attr;
            s32 palt;

            (void)dw;
            (void)dh;

            ix = get_free_patcash_index(mt->cpat);
            cp = &mt->cpat->patt[ix];
            mt->cpat->adr[mt->cpat->kazu] = cp;
            mt->cpat->kazu += 1;
            cp->curr_disp = 1;
            cp->time = mt->mltcshtime16;
            cp->cg.code = cc.code;
            cp->x16 = 0;
            cp->x32 = 0;
            memset(&cp->map, 0, sizeof(PatternMap));
            cc.parts.group = i;

            while (count--) {
                texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
                dw = (texptr->wh & 0xE0) >> 2;
                dh = (texptr->wh & 0x1C) << 1;

                if(x_flip){
                    x += trsptr->x;
                    dw2 = dw;
                }
                else{
                    x -= trsptr->x;
                    dw2 = 0;
                }

                if(y_flip){
                    y -= trsptr->y;
                    dh2 = dh;
                }
                else{
                    y += trsptr->y;
                    dh2 = 0;
                }

                wh = (texptr->wh & 3) + 1;
                size = (wh * wh) << 6;
                attr = trsptr->attr;
                palt = (attr & 0x1FF) + palo;
                attr = (attr ^ flip) & 0xC000;
                cc.parts.offset = trsptr->code;

                switch (wh) {
                case 1:
                case 2:
                    if (get_mltbuf16_ext_2(mt, cc.code, 0, &code, cp)) {
                        lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                        njReLoadTexturePartNumG(mt->mltgidx16 + (code >> 8), mt->mltbuf, code & 0xFF, size);
                    }

                    if (debug_10) {
                        DebugLine(x - (dw & ((s16)flip >> 0x10)), y + (dh & ((s16)(flip << 1) >> 16)), dw, dh);
                    }

                    rnum = seqsStoreChip(x - dw2,
                                         y + dh2,
                                         dw,
                                         dh,
                                         mt->mltgidx16,
                                         code,
                                         attr | palt,
                                         wk->my_clear_level,
                                         mt->id);
                    break;

                case 4:
                    if (get_mltbuf32_ext_2(mt, cc.code, 0, &code, cp)) {
                        lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                        njReLoadTexturePartNumG(mt->mltgidx32 + (code >> 6), mt->mltbuf, code & 0x3F, size);
                    }

                    if (debug_10) {
                        DebugLine(x - (dw & ((s16)flip >> 0x10)), y + (dh & ((s16)(flip << 1) >> 16)), dw, dh);
                    }

                    rnum = seqsStoreChip(x - dw2,
                                         y + dh2,
                                         dw,
                                         dh,
                                         mt->mltgidx32,
                                         code,
                                         (attr | 0x2000) | palt,
                                         wk->my_clear_level,
                                         mt->id);
                    break;
                }

                if (!rnum) {
                    break;
                }

                trsptr++;
            }

            seqs_w.up[mt->id] = 1;
            appRenewTempPriority(wk->position_z);
        }

        return;
    }

    {
        s32 code;
        s32 wh;
        s32 dw, dw2;
        s32 dh, dh2;
        s32 attr;
        s32 palt;

        (void)dw;
        (void)dh;

        cp = mt->cpat->adr[ix];
        cp->curr_disp = 1;
        cp->time = mt->mltcshtime16;
        makeup_tpu_free(mt->mltnum16 >> 8, mt->mltnum32 >> 6, &cp->map);
        cc.parts.group = i;

        u16 x_flip = flip & 0x8000;
        u16 y_flip = flip & 0x4000;

        while (count--) {
            texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
            dw = (texptr->wh & 0xE0) >> 2;
            dh = (texptr->wh & 0x1C) << 1;

            if(x_flip){
                x += trsptr->x;
                dw2 = dw;
            }
            else{
                x -= trsptr->x;
                dw2 = 0;
            }

            if(y_flip){
                y -= trsptr->y;
                dh2 = dh;
            }
            else{
                y += trsptr->y;
                dh2 = 0;
            }

            wh = (texptr->wh & 3) + 1;
            attr = trsptr->attr;
            palt = (attr & 0x1FF) + palo;
            attr = (attr ^ flip) & 0xC000;
            cc.parts.offset = trsptr->code;

            switch (wh) {
            case 1:
            case 2:
                code = get_mltbuf16_ext(mt, cc.code, 0);

                if (debug_10) {
                    DebugLine(x - (dw & ((s16)flip >> 0x10)), y + (dh & ((s16)(flip << 1) >> 16)), dw, dh);
                }

                rnum = seqsStoreChip(x - dw2,
                                     y + dh2,
                                     dw,
                                     dh,
                                     mt->mltgidx16,
                                     code,
                                     attr | palt,
                                     wk->my_clear_level,
                                     mt->id);
                break;

            case 4:
                code = get_mltbuf32_ext(mt, cc.code, 0);

                if (debug_10) {
                    DebugLine(x - (dw & ((s16)flip >> 0x10)), y + (dh & ((s16)(flip << 1) >> 16)), dw, dh);
                }

                rnum = seqsStoreChip(x - dw2,
                                     y + dh2,
                                     dw,
                                     dh,
                                     mt->mltgidx32,
                                     code,
                                     (attr | 0x2000) | palt,
                                     wk->my_clear_level,
                                     mt->id);
                break;
            }

            if (!rnum) {
                break;
            }

            trsptr++;
        }

        seqs_w.up[mt->id] = 1;
        appRenewTempPriority(wk->position_z);
    }
}

void mlt_obj_trans_cp3(MultiTexture* mt, WORK* wk, s32 base_y) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    s32 rnum;
    s32 flip;
    s32 count;
    s32 palo;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    PatternCode cc;
    s32 size;
    s32 code;
    s32 wh;
    s32 dw, dw2;
    s32 dh, dh2;
    s32 attr;
    s32 palt;

    ppgSetupCurrentDataList(&mt->texList);

    if (mt->ext) {
        mlt_obj_trans_cp3_ext(mt, wk, base_y);
        return;
    }

    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("cp3 The trans data is not valid. Group number: %d\n", i);
        return;
    }

    prewarm_register(mt, i); /* 3DS background prewarm */

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    textbl = (u32*)texgrplds[i].texture_table;
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    flip = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);
    cc.parts.group = i;

    u16 x_flip = flip & 0x8000;
    u16 y_flip = flip & 0x4000;
    u8 debug_10 = Debug_w[0x10];

    while (count--) {
        texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
        dw = (s32)(texptr->wh & 0xE0) >> 2;
        dh = (texptr->wh & 0x1C) << 1;

        if(x_flip){
            x += trsptr->x;
            dw2 = dw;
        }
        else{
            x -= trsptr->x;
            dw2 = 0;
        }

        if(y_flip){
            y -= trsptr->y;
            dh2 = dh;
        }
        else{
            y += trsptr->y;
            dh2 = 0;
        }

        
        wh = (texptr->wh & 3) + 1;
        size = (wh * wh) << 6;
        attr = trsptr->attr;
        palt = (attr & 0x1FF) + palo;
        attr = (attr ^ flip) & 0xC000;
        cc.parts.offset = trsptr->code;

        switch (wh) {
        case 1:
        case 2:
            if (get_mltbuf16(mt, cc.code, 0, &code)) {
                lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                njReLoadTexturePartNumG(mt->mltgidx16 + (code >> 8), mt->mltbuf, code & 0xFF, size);
            }

            if (debug_10) {
                DebugLine(x - (dw & ((s16)flip >> 0x10)), y + (dh & ((s16)(flip << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - dw2,
                                 y + dh2,
                                 dw,
                                 dh,
                                 mt->mltgidx16,
                                 code,
                                 attr | palt,
                                 wk->my_clear_level,
                                 mt->id);
            break;

        case 4:
            if (get_mltbuf32(mt, cc.code, 0, &code)) {
                lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                njReLoadTexturePartNumG(mt->mltgidx32 + (code >> 6), mt->mltbuf, code & 0x3F, size);
            }

            if (debug_10) {
                DebugLine(x - (dw & ((s16)flip >> 0x10)), y + (dh & ((s16)(flip << 1) >> 16)), dw, dh);
            }

            rnum = seqsStoreChip(x - dw2,
                                 y + dh2,
                                 dw,
                                 dh,
                                 mt->mltgidx32,
                                 code,
                                 attr | 0x2000 | palt,
                                 wk->my_clear_level,
                                 mt->id);
            break;
        }

        if (!rnum) {
            break;
        }

        trsptr++;
    }

    seqs_w.up[mt->id] = 1;
    appRenewTempPriority(wk->position_z);
}

void mlt_obj_trans_rgb_ext(MultiTexture* mt, WORK* wk, s32 base_y) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    s32 rnum;
    s32 flip;
    s32 palo;
    s32 count;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    s16 ix;
    PatternCode cc;
    PatternInstance* cp;

    (void)textbl;

    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("rgb_ext The trans data is not valid. Group number: %d\n", i);
        return;
    }

    /* 3DS: cx-mode prewarm — this path bakes the player's palette into
     * 16-bit tiles (lz_ext_p6_cx with ColorRAM[(attr&0x1FF)+colcd]); it's
     * what every NON-DEFAULT color uses, so the fx-only prewarm covered
     * none of a colored player's charset (user-confirmed stutter). */
    prewarm_register_mode(mt, i, 1, wk->colcd);

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    textbl = (u32*)texgrplds[i].texture_table;
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    flip = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);
    cc.parts.group = wk->colcd;
    cc.parts.offset = wk->cg_number;
    ix = check_patcash_ex_trans(mt->cpat, cc.code);
    if (mt->id < 24) { if (ix < 0) g_pat_build[mt->id]++; else g_pat_hit[mt->id]++; }


    u16 x_flip = flip & 0x8000;
    u16 y_flip = flip & 0x4000;
    
    if (ix < 0) {
        {
            s32 size;
            s32 code;
            s32 attr;
            s32 palt;
            s32 wh;
            s32 dw, dw2;
            s32 dh, dh2;

            ix = get_free_patcash_index(mt->cpat);
            cp = &mt->cpat->patt[ix];
            mt->cpat->adr[mt->cpat->kazu] = cp;
            mt->cpat->kazu += 1;
            cp->curr_disp = 1;
            cp->time = mt->mltcshtime16;
            cp->cg.code = cc.code;
            cp->x16 = 0;
            cp->x32 = 0;
            memset(&cp->map, 0, sizeof(PatternMap));
            cc.parts.group = i;
    
            while (count--) {
                texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
                dw = (texptr->wh & 0xE0) >> 2;
                dh = (texptr->wh & 0x1C) << 1;

                if(x_flip){
                    x += trsptr->x;
                    dw2 = dw;
                }
                else{
                    x -= trsptr->x;
                    dw2 = 0;
                }

                if(y_flip){
                    y -= trsptr->y;
                    dh2 = dh;
                }
                else{
                    y -= trsptr->y;
                    dh2 = 0;
                }

                wh = (texptr->wh & 3) + 1;
                size = (wh * wh) << 6;
                attr = trsptr->attr;
                palt = (attr & 0x1FF) + palo;
                attr = (attr ^ flip) & 0xC000;
                cc.parts.offset = trsptr->code;

                switch (wh) {
                case 1:
                case 2:
                    if (get_mltbuf16_ext_2(mt, cc.code, palt, &code, cp) != 0) {
                        lz_ext_p6_cx(&((u8*)texptr)[1], (u16*)mt->mltbuf, size, (u16*)(ColorRAM[palt]));
                        njReLoadTexturePartNumG(mt->mltgidx16 + (code >> 8), mt->mltbuf, code & 0xFF, size << 1);
                    }

                    rnum = seqsStoreChip(x - dw2,
                                         y + dh2,
                                         dw,
                                         dh,
                                         mt->mltgidx16,
                                         code,
                                         attr,
                                         wk->my_clear_level,
                                         mt->id);
                    break;

                case 4:
                    if (get_mltbuf32_ext_2(mt, cc.code, palt, &code, cp) != 0) {
                        lz_ext_p6_cx(&((u8*)texptr)[1], (u16*)mt->mltbuf, size, (u16*)(ColorRAM[palt]));
                        njReLoadTexturePartNumG(mt->mltgidx32 + (code >> 6), mt->mltbuf, code & 0x3F, size << 1);
                    }

                    rnum = seqsStoreChip(x - dw2,
                                         y + dh2,
                                         dw,
                                         dh,
                                         mt->mltgidx32,
                                         code,
                                         attr | 0x2000,
                                         wk->my_clear_level,
                                         mt->id);
                    break;
                }

                if (!rnum) {
                    break;
                }

                trsptr++;
            }

            seqs_w.up[mt->id] = 1;
            appRenewTempPriority(wk->position_z);
        }

        return;
    }

    {
        s32 code;
        s32 attr;
        s32 palt;
        s32 wh;
        s32 dw, dw2;
        s32 dh, dh2;

        cp = mt->cpat->adr[ix];
        cp->curr_disp = 1;
        cp->time = mt->mltcshtime16;
        makeup_tpu_free(mt->mltnum16 >> 8, mt->mltnum32 >> 6, &cp->map);
        cc.parts.group = i;

        u16 x_flip = flip & 0x8000;
        u16 y_flip = flip & 0x4000;

        while (count--) {
            texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
            dw = (texptr->wh & 0xE0) >> 2;
            dh = (texptr->wh & 0x1C) << 1;

            if(x_flip){
                x += trsptr->x;
                dw2 = dw;
            }
            else{
                x -= trsptr->x;
                dw2 = 0;
            }

            if(y_flip){
                y -= trsptr->y;
                dh2 = dh;
            }
            else{
                y += trsptr->y;
                dh2 = 0;
            }

            wh = (texptr->wh & 3) + 1;
            attr = trsptr->attr;
            palt = (attr & 0x1FF) + palo;
            attr = (attr ^ flip) & 0xC000;
            cc.parts.offset = trsptr->code;

            switch (wh) {
            case 1:
            case 2:
                code = get_mltbuf16_ext(mt, cc.code, palt);

                rnum = seqsStoreChip(x - dw2,
                                     y + dh2,
                                     dw,
                                     dh,
                                     mt->mltgidx16,
                                     code,
                                     attr,
                                     wk->my_clear_level,
                                     mt->id);
                break;

            case 4:
                code = get_mltbuf32_ext(mt, cc.code, palt);

                rnum = seqsStoreChip(x - dw2,
                                     y + dh2,
                                     dw,
                                     dh,
                                     mt->mltgidx32,
                                     code,
                                     attr | 0x2000,
                                     wk->my_clear_level,
                                     mt->id);
                break;
            }

            if (rnum == 0) {
                break;
            }

            trsptr++;
        }

        seqs_w.up[mt->id] = 1;
        appRenewTempPriority(wk->position_z);
    }
}

void mlt_obj_trans_rgb(MultiTexture* mt, WORK* wk, s32 base_y) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    s32 rnum;
    s32 flip;
    s32 palo;
    s32 count;
    s32 n;
    s32 i;
    f32 x;
    f32 y;
    PatternCode cc;
    s32 size;
    s32 code;
    s32 attr;
    s32 palt;
    s32 wh;
    s32 dw, dw2;
    s32 dh, dh2;

    ppgSetupCurrentDataList(&mt->texList);

    if (mt->ext) {
        mlt_obj_trans_rgb_ext(mt, wk, base_y);
        return;
    }

    n = wk->cg_number;
    i = obj_group_table[n];

    if (i == 0) {
        return;
    }

    if (texgrplds[i].ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("rgb The trans data is not valid. Group number: %d\n", i);
        return;
    }

    /* 3DS: cx-mode prewarm — see rgb_ext note. */
    prewarm_register_mode(mt, i, 1, wk->colcd);

    n -= texgrpdat[i].num_of_1st;
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    textbl = (u32*)texgrplds[i].texture_table;
    count = *trsbas;
    trsbas++;
    trsptr = (TileMapEntry*)trsbas;
    x = y = 0.0f;
    flip = flptbl[wk->cg_flip ^ wk->rl_flag];
    palo = wk->colcd;

    if (wk->my_bright_type) {
        curr_bright = bright_type[wk->my_bright_type - 1][wk->my_bright_level];
    } else {
        curr_bright = 0xFFFFFF;
    }

    mlt_obj_matrix(wk, base_y);
    cc.parts.group = i;
    u16 x_flip = flip & 0x8000;
    u16 y_flip = flip & 0x4000;
    
    while (count--) {
        texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
        dw = (texptr->wh & 0xE0) >> 2;
        dh = (texptr->wh & 0x1C) << 1;

        if(x_flip){
            x += trsptr->x;
            dw2 = dw;
        }
        else{
            x -= trsptr->x;
            dw2 = 0;
        }

        if(y_flip){
            y -= trsptr->y;
            dh2 = dh;
        }
        else{
            y += trsptr->y;
            dh2 = 0;
        }

        wh = (texptr->wh & 3) + 1;
        size = (wh * wh) << 6;
        attr = trsptr->attr;
        palt = (attr & 0x1FF) + palo;
        attr = (attr ^ flip) & 0xC000;
        cc.parts.offset = trsptr->code;

        switch (wh) {
        case 1:
        case 2:
            if (get_mltbuf16(mt, cc.code, palt, &code) != 0) {
                lz_ext_p6_cx(&((u8*)texptr)[1], (u16*)mt->mltbuf, size, (u16*)(ColorRAM[palt]));
                njReLoadTexturePartNumG(mt->mltgidx16 + (code >> 8), mt->mltbuf, code & 0xFF, size << 1);
            }

            rnum = seqsStoreChip(x - dw2,
                                 y + dh2,
                                 dw,
                                 dh,
                                 mt->mltgidx16,
                                 code,
                                 attr,
                                 wk->my_clear_level,
                                 mt->id);
            break;

        case 4:
            if (get_mltbuf32(mt, cc.code, palt, &code)) {
                lz_ext_p6_cx(&((u8*)texptr)[1], (u16*)mt->mltbuf, size, (u16*)(ColorRAM[palt]));
                njReLoadTexturePartNumG(mt->mltgidx32 + (code >> 6), mt->mltbuf, code & 0x3F, size << 1);
            }

            rnum = seqsStoreChip(x - dw2,
                                 y + dh2,
                                 dw,
                                 dh,
                                 mt->mltgidx32,
                                 code,
                                 attr | 0x2000,
                                 wk->my_clear_level,
                                 mt->id);
            break;
        }

        if (!rnum) {
            break;
        }

        trsptr++;
    }

    seqs_w.up[mt->id] = 1;
    appRenewTempPriority(wk->position_z);
}

#define one_by_64 1.0f/64.0f

void mlt_obj_matrix(WORK* wk, s32 base_y) {
    njSetMatrix(NULL, &BgMATRIX[wk->my_family]);
    njTranslate(NULL, wk->position_x, wk->position_y + base_y, PrioBase[wk->position_z]);

    if (wk->my_mr_flag) {
        njScale(NULL, (one_by_64) * (wk->my_mr.size.x + 1), (one_by_64) * (wk->my_mr.size.y + 1), 1.0f);
    }
}

void appSetupBasePriority() {
    s32 i;

    for (i = 0; i < PRIO_BASE_SIZE; i++) {
        PrioBaseOriginal[i] = ((i << 9) + 1) >> 1;
    }
}

void appSetupTempPriority() {
    s32 i;

    for (i = 0; i < PRIO_BASE_SIZE; i++) {
        PrioBase[i] = PrioBaseOriginal[i];
    }
}

void appRenewTempPriority_1_Chip() {
    njTranslate(NULL, 0, 0, 1.0f);
}

void appRenewTempPriority(s32 z) {
    MTX mtx;
    njGetMatrix(&mtx);
    PrioBase[z] = mtx.a[3][2];
}

void seqsInitialize(void* adrs) {
    if (adrs == NULL) {
        while (1) {
            // Do nothing
        }
    }

    seqs_w.chip = (Sprite2*)adrs;
    seqs_w.sprMax = 0;
}

u16 seqsGetSprMax() {
    return seqs_w.sprMax;
}

u32 seqsGetUseMemorySize() {
    return 0xD000;
}

void seqsBeforeProcess() {
    s32 i;

    seqs_w.sprTotal = 0;

    // FIXME: Extract 24 into a define
    for (i = 0; i < 24; i++) {
        seqs_w.up[i] = 0;
    }
}

void seqsAfterProcess() {
    s32 i;
    u32 keep = 0;
    u32 val = 0;

    if ((Debug_w[0x27] != 3) && (seqs_w.sprTotal != 0)) {
        for (i = 0; i < 24; i++) {
            if (seqs_w.up[i]) {
                if (Debug_w[0x22]) {
                    if (ppgCheckTextureDataBe(mts[i].texList.tex) == 0) {
                        seqs_w.up[i] = 0;
                    }
                } else if (ppgRenewTexChunkSeqs(mts[i].texList.tex) == 0) {
                    seqs_w.up[i] = 0;
                }
            }
        }

        if (seqs_w.sprMax < seqs_w.sprTotal) {
            seqs_w.sprMax = seqs_w.sprTotal;
        }

        for (i = 0; i < seqs_w.sprTotal; i++) {
            if (seqs_w.up[seqs_w.chip[i].id]) {
                val = seqs_w.chip[i].tex_code;

                if (keep != val) {
                    keep = val;
                    flSetRenderState(FLRENDER_TEXSTAGE0, val);
                }

                SDLGameRenderer_DrawSprite2(&seqs_w.chip[i]);
            }
        }
    }
}

s32 seqsStoreChip(f32 x, f32 y, s32 w, s32 h, s32 gix, s32 code, s32 attr, s32 alpha, s32 id) {
    Sprite2* chip;
    s32 u;
    s32 v;

    chip = &seqs_w.chip[seqs_w.sprTotal];
    chip->v[0].x = x;
    chip->v[0].y = y;
    chip->v[1].x = x + w;
    chip->v[1].y = y - h;
    chip->v[0].z = chip->v[1].z = 0.0f;

    njCalcPoint(NULL, &chip->v[0], &chip->v[0]);
    njCalcPoint(NULL, &chip->v[1], &chip->v[1]);

    //if ((chip->v[0].x >= 384.0f) || (chip->v[1].x < 0.0f) || (chip->v[0].y >= 224.0f) || (chip->v[1].y < 0.0f)) {
    if ((chip->v[0].x >= Max_X) || (chip->v[1].x < Min_X) || (chip->v[0].y >= Max_Y) || (chip->v[1].y < Min_Y)) {
        return 1;
    }

    if (!(attr & 0x2000)) {
        u = (code & 0xF) << 4;
        v = code & 0xF0;
        chip->tex_code = ppgGetUsingTextureHandle(NULL, gix + (code >> 8));
    } else {
        u = (code & 7) << 5;
        v = (code & 0x38) << 2;
        chip->tex_code = ppgGetUsingTextureHandle(NULL, gix + (code >> 6));
    }

    appRenewTempPriority_1_Chip();

    /* Native renderer expects NORMALIZED [0,1] texture coordinates (its
     * atlas/pool UV remap multiplies by src_w/strip_w) — the old PSP
     * pipeline consumed raw texel coords here. Divide by the 256px sheet
     * size, matching the reference tree's seqsStoreChip. */
    if (attr & 0x8000) {
        chip->t[1].s = u / 256.0f;
        chip->t[0].s = (u + w) / 256.0f;
    } else {
        chip->t[0].s = u / 256.0f;
        chip->t[1].s = (u + w) / 256.0f;
    }

    if (attr & 0x4000) {
        chip->t[1].t = v / 256.0f;
        chip->t[0].t = (v + h) / 256.0f;
    } else {
        chip->t[0].t = v / 256.0f;
        chip->t[1].t = (v + h) / 256.0f;
    }

    chip->tex_code |= ppgGetUsingPaletteHandle(NULL, attr & 0x1FF) << 16;
    chip->vertex_color = curr_bright | ((0xFF - alpha) << 24);
    chip->id = id;
    seqs_w.sprTotal += 1;

    if (seqs_w.sprTotal > 0x400) {
        // The number of OBJ fragments has exceeded the planned number
        flLogOut("The number of OBJ fragments has exceeded the planned number\n");
        return -1;
    }

    return 1;
}

static s32 get_mltbuf16(MultiTexture* mt, u32 code, u32 palt, s32* ret) {
    s32 i;
    s32 b = -1;
    PatternState* mc = mt->mltcsh16;

    i = mt->mltnum16;

    while (1) {
        if ((mc->cs.code == code) && (mc->state == palt)) {
            mc->time = mt->mltcshtime16;
            *ret = mt->mltnum16 - i;
            return 0;
        }

        if ((mc->cs.code == -1) && (b < 0)) {
            b = i;
        }

        mc++;
        i -= 1;

        if (i <= 0) {
            if (b >= 0) {
                b = mt->mltnum16 - b;
                mt->mltcsh16[b].time = mt->mltcshtime16;
                mt->mltcsh16[b].state = palt;
                mt->mltcsh16[b].cs.code = code;
                *ret = b;
                return 1;
            }

            // CG cache is full. 16x16: %d\n
            flLogOut("CG cache is full. 16x16: %d\n", mt->id);
            return -1;
        }
    }
}

static s32 get_mltbuf32(MultiTexture* mt, u32 code, u32 palt, s32* ret) {
    s32 i;
    s32 b = -1;
    PatternState* mc = mt->mltcsh32;

    i = mt->mltnum32;

    while (1) {
        if ((mc->cs.code == code) && (mc->state == palt)) {
            mc->time = mt->mltcshtime32;
            *ret = mt->mltnum32 - i;
            return 0;
        }

        if ((mc->cs.code == -1) && (b < 0)) {
            b = i;
        }

        mc++;
        i -= 1;

        if (i <= 0) {
            if (b >= 0) {
                b = mt->mltnum32 - b;
                mt->mltcsh32[b].time = mt->mltcshtime32;
                mt->mltcsh32[b].state = palt;
                mt->mltcsh32[b].cs.code = code;
                *ret = b;
                return 1;
            }

            // CG cache is full. 32x32 : %d\n
            flLogOut("CG cache is full. 32x32 : %d\n", mt->id);
            return -1;
        }
    }
}

static s32 get_mltbuf16_ext_2(MultiTexture* mt, u32 code, u32 palt, s32* ret, PatternInstance* cp) {
    PatternState* mc = mt->mltcsh16;
    s32 i;

    u16 *tpu_x16 = mt->tpu->x16_used;

    for (i = 0; i < mt->tpu->x16; i++) {
        if ((code == mc[*tpu_x16].cs.code) && (palt == mc[*tpu_x16].state)) {
            *ret = *tpu_x16;

            if (x16_mapping_set(&cp->map, *ret)) {
                cp->x16 += 1;
                mc[*tpu_x16].time += 1;
            }

            /* 3DS: promote toward the front (halving). Under lazy release
             * the used list stays permanently FULL, and screens that rebuild
             * pattern instances every frame (char-select preview: 2-frame
             * instance life, ~31 chips) were paying full-list linear scans
             * per chip per frame (~1.2M iterations/s, measured via GRPACT —
             * and the P1-slower asymmetry was just P1's fuller list). The
             * hot loop's chips migrate to the head; scans collapse to
             * ~loop size. The list is an unordered set — order is free. */
            if (i > 0) {
                u16* head = mt->tpu->x16_used;
                s32 j = i >> 1;
                u16 tmp = head[i];
                head[i] = head[j];
                head[j] = tmp;
            }

            return 0;
        }
        tpu_x16++;
    }

    if ((i != mt->mltnum16) && (mt->tpf->x16)) {
        mt->tpf->x16 -= 1;
        *tpu_x16 = mt->tpf->x16_free[mt->tpf->x16];
        mt->tpu->x16 += 1;
        mc[*tpu_x16].cs.code = code;
        mc[*tpu_x16].state = palt;
        *ret = *tpu_x16;
        mc[*tpu_x16].time = 1;

        if (x16_mapping_set(&cp->map, *ret)) {
            cp->x16 += 1;
        }

        return 1;
    }

    /* 3DS LAZY RELEASE: no fresh slot — evict an unreferenced CACHED slot
     * (time<=0: every instance that mapped it has expired; see texcash.c
     * update_with_tpu_free, which now keeps identities instead of wiping).
     * RANDOMIZED start (LCG): a looping animation whose tile set exceeds
     * capacity (char-select super-art preview, measured) hits 100% misses
     * under clock/LRU eviction (always evicts exactly what the loop needs
     * next); random eviction retains ~capacity/loop of the set instead. */
    /* clock-hand eviction (FIFO-ish). NOTE: randomized eviction was tried
     * for the loop-exceeds-capacity case and made general play WORSE (it
     * evicts uniformly, including tiles the current move decoded frames ago
     * and still cycles) — reverted. */
    {
        static u32 clock16 = 0;
        s32 n = mt->tpu->x16;
        s32 k;
        for (k = 0; k < n; k++) {
            u16 slot = mt->tpu->x16_used[(clock16 + k) % n];
            if (mc[slot].time <= 0) {
                clock16 = (clock16 + k + 1) % (n > 0 ? (u32)n : 1u);
                mc[slot].cs.code = code;
                mc[slot].state = palt;
                mc[slot].time = 1;
                *ret = slot;
                if (x16_mapping_set(&cp->map, *ret)) {
                    cp->x16 += 1;
                }
                return 1;
            }
        }
    }

    // CG cache is full. x16 EXT2\n
    flLogOut("CG cache is full. x16 EXT2\n");
    return -1;
}

static s32 get_mltbuf32_ext_2(MultiTexture* mt, u32 code, u32 palt, s32* ret, PatternInstance* cp) {
    PatternState* mc = mt->mltcsh32;
    s32 i;

    u16 *tpu_x32 = mt->tpu->x32_used;

    for (i = 0; i < mt->tpu->x32; i++) {
        if ((code == mc[*tpu_x32].cs.code) && (palt == mc[*tpu_x32].state)) {
            *ret = *tpu_x32;

            if (x32_mapping_set(&cp->map, *ret)) {
                cp->x32 += 1;
                mc[*tpu_x32].time += 1;
            }

            /* 3DS: halving promotion — see get_mltbuf16_ext_2 */
            if (i > 0) {
                u16* head = mt->tpu->x32_used;
                s32 j = i >> 1;
                u16 tmp = head[i];
                head[i] = head[j];
                head[j] = tmp;
            }

            return 0;
        }
        tpu_x32++;
    }

    if ((i != mt->mltnum32) && (mt->tpf->x32)) {
        mt->tpf->x32 -= 1;
        *tpu_x32 = mt->tpf->x32_free[mt->tpf->x32];
        mt->tpu->x32 += 1;
        mc[*tpu_x32].cs.code = code;
        mc[*tpu_x32].state = palt;
        *ret = *tpu_x32;
        mc[*tpu_x32].time += 1;

        if (x32_mapping_set(&cp->map, *ret)) {
            cp->x32 += 1;
        }

        return 1;
    }

    /* 3DS LAZY RELEASE eviction (clock) — see get_mltbuf16_ext_2. */
    {
        static u32 clock32 = 0;
        s32 n = mt->tpu->x32;
        s32 k;
        for (k = 0; k < n; k++) {
            u16 slot = mt->tpu->x32_used[(clock32 + k) % n];
            if (mc[slot].time <= 0) {
                clock32 = (clock32 + k + 1) % (n > 0 ? (u32)n : 1u);
                mc[slot].cs.code = code;
                mc[slot].state = palt;
                mc[slot].time = 1;
                *ret = slot;
                if (x32_mapping_set(&cp->map, *ret)) {
                    cp->x32 += 1;
                }
                return 1;
            }
        }
    }

    flLogOut("CG cache is full. x32 EXT2\n");
    return -1;
}

static s32 get_mltbuf16_ext(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh16;
    s32 i;

    u16 *tpu_x16 = tpu_free->x16_used;

    for (i = 0; i < tpu_free->x16; i++) {
        if ((code == mc[*tpu_x16].cs.code) && (palt == mc[*tpu_x16].state)) {
            return *tpu_x16;
        }
        tpu_x16++;
    }

    flLogOut("CG decompress error 16x16\n");
    return -1;
}


static s32 get_mltbuf32_ext(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh32;
    s32 i;

    for (i = 0; i < tpu_free->x32; i++) {
        if ((code == mc[tpu_free->x32_used[i]].cs.code) && (palt == mc[tpu_free->x32_used[i]].state)) {
            return tpu_free->x32_used[i];
        }
    }

    flLogOut("CG decompress error 32x32\n");
    return -1;
}

static inline u16 x16_mapping_set(PatternMap* map, s32 code) {
    u16 flg = 0;

    u32 high = code >> 8;            // instead of / 256
    u32 mid  = (code >> 4) & 0xF;    // instead of %256 /16
    u32 num  = code & 0xF;

    u16 bit = (1 << num);
    u16 *cell = &map->x16_map[high][mid];

    if (!(*cell & bit)) {
        *cell |= bit;
        flg = 1;
    }

    return flg;
}

static inline u16 x32_mapping_set(PatternMap* map, s32 code) {
    u16 flg = 0;

    u32 high = code >> 6;            // instead of /64
    u32 mid  = (code >> 3) & 0x7;    // instead of %64 /8
    u32 num  = code & 7;

    u8 bit = (1 << num);
    u8 *cell = &map->x32_map[high][mid];

    if (!(*cell & bit)) {
        *cell |= bit;
        flg = 1;
    }

    return flg;
}

void makeup_tpu_free(s32 x16, s32 x32, PatternMap* map) {
    s16 i;
    s16 j;
    s16 k;

    u16 *map_x16 = &map->x16_map[0][0];
    u8 *map_x32 = &map->x32_map[0][0];

    tpu_free->x16 = 0;
    tpu_free->x32 = 0;

    for (i = 0; i < x16; i++) {
        for (j = 0; j < 16; j++) {
            if (*map_x16) {
                for (k = 0; k < 16; k++) {
                    if ((1 << k) & map->x16_map[i][j]) {
                        tpu_free->x16_used[tpu_free->x16] = (i << 8) + (j << 4) + k;
                        tpu_free->x16 += 1;
                    }
                }
            }
            map_x16++;
        }
    }

    for (i = 0; i < x32; i++) {
        for (j = 0; j < 8; j++) {
            if (*map_x32) {
                for (k = 0; k < 8; k++) {
                    if (*map_x32 & (1 << k)) {
                        tpu_free->x32_used[tpu_free->x32] = (i << 6) + (j << 3) + k;
                        tpu_free->x32 += 1;
                    }
                }
            }
            map_x32++;
        }
    }
}

static s16 check_patcash_ex_trans(PatternCollection* padr, u32 cg) {
    s16 i;

    for (i = 0; i < padr->kazu; i++) {
        if (padr->adr[i]->cg.code == cg) {
            return i;
        }
    }

    return -1;
}

static s32 get_free_patcash_index(PatternCollection* padr) {
    s16 i;

    for (i = 0; i < 0x40; i++) {
        if (padr->patt[i].time == 0) {
            return i;
        }
    }

    flLogOut("CG cache buffer is full\n");
    return -1;
}

static inline void lz_ext_p6_fx(u8* srcptr, u8* dstptr, u32 len) {
    u8* endptr = dstptr + len;
    u8* tmpptr;
    u32 tmp;
    u32 flg;
    u8 type;

    while (dstptr < endptr) {
        tmp = *srcptr++;
        type = tmp & 0xC0;

        if(type == 0)
            *dstptr++ = tmp;
        else if(type == 0x40){
            tmp &= 0x3F;
            tmpptr = (dstptr - (tmp >> 2)) - 1;
            tmp = (tmp & 3) + 2;

            while (tmp--) {
                *dstptr++ = *tmpptr++;
            }   
        }
        else if(type == 0x80){
            tmp = ((tmp & 0x3F) << 8) | *srcptr++;
            tmpptr = (dstptr - (tmp >> 6)) - 1;
            tmp = (tmp & 0x3F) + 2;

            while (tmp--) {
                *dstptr++ = *tmpptr++;
            }
        }
        else {
            flg = tmp & 0x30;
            tmp = (tmp & 0xF) + 2;

            while (tmp--) {
                *dstptr++ = flg | (*srcptr >> 4);
                *dstptr++ = flg | (*srcptr++ & 0xF);
            }
        }
    }
}

static void lz_ext_p6_cx(u8* srcptr, u16* dstptr, u32 len, u16* palptr) {
    u16* endptr = dstptr + len;
    u16* tmpptr;
    u32 tmp;
    u32 flg;

    while (dstptr < endptr) {
        tmp = *srcptr++;

        switch (tmp & 0xC0) {
        case 0x0:
            *dstptr++ = palptr[tmp];
            break;

        case 0x40:
            tmp &= 0x3F;
            tmpptr = (dstptr - (tmp >> 2)) - 1;
            tmp = (tmp & 3) + 2;

            while (tmp--) {
                *dstptr++ = *tmpptr++;
            }

            break;

        case 0x80:
            tmp = ((tmp & 0x3F) << 8) | *srcptr++;
            tmpptr = (dstptr - (tmp >> 6)) - 1;
            tmp = (tmp & 0x3F) + 2;

            while (tmp--) {
                *dstptr++ = *tmpptr++;
            }

            break;

        case 0xC0:
            flg = tmp & 0x30;
            tmp = (tmp & 0xF) + 2;

            while (tmp--) {
                *dstptr++ = palptr[flg | (*srcptr >> 4)];
                *dstptr++ = palptr[flg | (*srcptr++ & 0xF)];
            }

            break;
        }
    }
}

void mlt_obj_trans_init(MultiTexture* mt, s32 mode, u8* adrs) {
    PatternState* mc;
    PPGFileHeader ppg;
    s32 i;

    ppg.width = ppg.height = 16;
    ppg.compress = 0;
    ppg.formARGB = 0x1555;
    ppg.transNums = 0;
    mt->texList.tex = &mt->tex;

    switch (mode & 7) {
    case 4:
        ppg.pixel = 0x82;
        mt->texList.pal = NULL;
        break;

    case 2:
        ppg.pixel = 0x81;
        mt->texList.pal = palGetChunkGhostCP3();
        break;

    default:
        ppg.pixel = 0x81;
        mt->texList.pal = palGetChunkGhostDC();
        break;
    }

    mt->texList.tex->be = 0;
    ppgSetupTexChunkSeqs(&mt->tex, &ppg, adrs, mt->mltgidx16, mt->mltnum, mt->attribute);

    if (!(mode & 0x20)) {
        mc = mt->mltcsh16;

        memset(mt->mltcsh16, 0, mt->mltnum16 * sizeof(PatternState));
        for (i = 0; i < mt->mltnum16; i++) {
            mc->cs.code = -1;
            mc++;
        }

        mc = mt->mltcsh32;

        for (i = 0; i < mt->mltnum32; i++) {
            mc->time = 0;
            mc->cs.code = -1;
            mc++;
        }
    }
}

void mlt_obj_trans_update(MultiTexture* mt) {
    s32 i;
    PatternState* mc;

    PatternState* assign1;
    PatternState* assign2;

    for (mc = mt->mltcsh16, i = 0; i < mt->mltnum16; i++, mc += 1, assign1 = mc) {
        if (mc->time) {
            if (--mc->time == 0) {
                mc->cs.code = -1;
            }
        }
    }

    for (mc = mt->mltcsh32, i = 0; i < mt->mltnum32; i++, mc += 1, assign2 = mc) {
        if (mc->time) {
            if (--mc->time == 0) {
                mc->cs.code = -1U;
            }
        }
    }
}

void draw_box(f64 arg0, f64 arg1, f64 arg2, f64 arg3, u32 col, u32 attr, s16 prio) {
    f32 px;
    f32 py;
    f32 sx;
    f32 sy;
    Vec3 point[2];
    PAL_CURSOR line;
    PAL_CURSOR_P xy[4];
    PAL_CURSOR_COL cc[4];

    px = arg0;
    py = arg1;
    sx = arg2;
    sy = arg3;
    point[0].x = px;
    point[0].y = py;
    point[0].z = 0.0f;
    point[1].x = px + sx;
    point[1].y = py + sy;
    point[1].z = 0.0f;
    njCalcPoints(NULL, point, point, 2);
    line.p = xy;
    line.col = cc;
    line.tex = NULL;
    line.num = 4;
    line.p[0].x = line.p[2].x = point[0].x;
    line.p[1].x = line.p[3].x = point[1].x;
    line.p[0].y = line.p[1].y = point[0].y;
    line.p[2].y = line.p[3].y = point[1].y;
    line.col[0].color = line.col[1].color = line.col[2].color = line.col[3].color = col;
    njDrawPolygon2D(&line, 4, PrioBase[prio], attr);
    appRenewTempPriority(prio);
}

static void DebugLine(f32 x, f32 y, f32 w, f32 h) {
    Vec3 point[2];
    PAL_CURSOR line;
    PAL_CURSOR_P xy[4];
    PAL_CURSOR_COL cc[4];

    line.p = &xy[0];
    line.col = &cc[0];
    line.tex = NULL;
    line.num = 4;
    point[0].x = x;
    point[0].y = y;
    point[0].z = 1.0f;
    point[1].x = x + w;
    point[1].y = y - h;
    point[1].z = 1.0f;
    njCalcPoints(NULL, point, point, 2);
    line.p[0].x = line.p[2].x = point[0].x;
    line.p[1].x = line.p[3].x = point[1].x;
    line.p[0].y = line.p[1].y = point[0].y;
    line.p[2].y = line.p[3].y = point[1].y;
    line.col[0].color = line.col[1].color = line.col[2].color = line.col[3].color = 0x80FFFFFF;
    njDrawPolygon2D(&line, 4, PrioBase[1], 0x20);
}

void mlt_obj_melt2(MultiTexture* mt, u16 cg_number) {
    u32* textbl;
    u16* trsbas;
    TileMapEntry* trsptr;
    TEX* texptr;
    TEX_GRP_LD* grplds;
    s32 count;
    s32 n;
    s32 i;
    s32 cd16;
    s32 cd32;
    s32 size;
    s32 attr;
    s32 palt;
    s32 wh;
    s32 dd;

    ppgSetupCurrentDataList(&mt->texList);
    grplds = &texgrplds[obj_group_table[cg_number]];

    if (grplds->ok == 0) {
        // The trans data is not valid. Group number: %d\n
        flLogOut("melt2 The trans data is not valid. Group number: %d\n", obj_group_table[cg_number]);
        return;
    }

    n = *(u32*)grplds->trans_table / 4;
    textbl = (u32*)grplds->texture_table;
    cd16 = 0;
    cd32 = 0;

    for (i = 0; i < n; i++) {
        trsbas = (u16*)(grplds->trans_table + ((u32*)grplds->trans_table)[i]);
        count = *trsbas;
        trsbas++;
        trsptr = (TileMapEntry*)trsbas;

        while (count) {
            attr = trsptr->attr;

            if (!(attr & 0x1000)) {
                texptr = (TEX*)((uintptr_t)textbl + ((u32*)textbl)[trsptr->code]);
                dd = (((texptr->wh & 0xE0) << 5) - 0x400) | (((texptr->wh & 0x1C) << 6) - 0x100);
                wh = (texptr->wh & 3) + 1;
                size = (wh * wh) << 6;
                palt = attr & 3;

                switch (wh) {
                case 1:
                case 2:
                    lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                    njReLoadTexturePartNumG(mt->mltgidx16 + (cd16 >> 8), mt->mltbuf, cd16 & 0xFF, size);
                    attr = (attr & 0xC000) | 0x1000 | dd;
                    trsptr->attr |= 0x1000;
                    attr |= palt;
                    search_trsptr(grplds->trans_table, i, n, trsptr->code, palt, cd16, attr);
                    trsptr->code = cd16;
                    trsptr->attr = attr;
                    cd16 += 1;
                    break;

                case 4:
                    lz_ext_p6_fx(&((u8*)texptr)[1], mt->mltbuf, size);
                    njReLoadTexturePartNumG(mt->mltgidx32 + (cd32 >> 6), mt->mltbuf, cd32 & 0x3F, size);
                    attr = (attr & 0xC000) | 0x3000 | dd;
                    trsptr->attr |= 0x1000;
                    attr |= palt;
                    search_trsptr(grplds->trans_table, i, n, trsptr->code, palt, cd32, attr);
                    trsptr->code = cd32;
                    trsptr->attr = attr;
                    cd32 += 1;
                    break;
                }
            }

            count -= 1;
            trsptr++;
        }
    }

    ppgRenewTexChunkSeqs(NULL);
}

/* graphics.c — citro2d/citro3d frame lifecycle for the 3DS port.
 *
 * Contract preserved from the PSP version: startFrame applies the
 * render_mode scaling globals (Scale_Factor/Off, Min/Max clip bounds,
 * Fade_Pos_tbl) that the decompiled game reads directly, then opens the
 * frame; endFrame presents. Draws flow through src/ctr/gu_draw.c.
 */
#include "graphics.h"
#include "sprites.h"
#include "ctr/ctr_game_renderer.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdlib.h>

#include "ctr/gu_draw.h"

// just for this project
#include "Game/WORK_SYS.h"
#include "Game/sc_data.h"

extern void ppgResetTextureCache(void);
extern void pspshim_gu_frame_reset(void);

/* Per-frame perf-diagnostic logging (FRAMETOTAL/PENDMS/FRAMEBEGIN/FRAMEEND/
 * RENDPROF). Each line costs printf-style formatting plus a syscall, several
 * times per frame — real waste on the 268MHz target. OFF for release builds;
 * build with -DSF3_PERF_LOG=1 to re-enable for profiling sessions. */
#ifndef SF3_PERF_LOG
#define SF3_PERF_LOG 0
#endif

s16 render_mode = SCREEN_DEFAULT_MODE;

static uint32_t bg_color = 0xFF000000;

/* CPS3 clipping bounds — used by decompiled game code (bg.c, DC_Ghost.c, etc.) */
float Min_X = 0.0f;
float Max_X = 384.0f;
float Min_Y = 0.0f;
float Max_Y = 224.0f;

int my_gu_init = 0;
int RTT_Enabled = SCREEN_DEFAULT_RTT;

/* CPS3 native resolution */
#define CPS3_WIDTH 384
#define CPS3_HEIGHT 224

s32 blit_filter = SCREEN_DEFAULT_FILTER; /* 0=bilinear, 1=nearest */

float Scale_Factor_X = 1.0f;
float Scale_Factor_Y = 1.0f;
float Scale_Off_X = 0.0f;
float Scale_Off_Y = 0.0f;

static int RTT_Enabled_temp = -1;
static int Scaling_mode_temp = -1;

/* --- citro2d state --- */
static C3D_RenderTarget *s_top_target;
static int s_in_frame = 0;
C3D_RenderTarget *ctrGetTopTarget(void) { return s_top_target; }

void setupScaling(int mode) {
    if (RTT_Enabled_temp == RTT_Enabled && Scaling_mode_temp == mode)
        return;
    RTT_Enabled_temp = RTT_Enabled;
    Scaling_mode_temp = mode;

    switch (mode) {
    case SCREEN_MODE_STRETCH: /* fill entire top screen */
        Scale_Factor_X = (float)SCREEN_WIDTH / CPS3_WIDTH;   /* 400/384 */
        Scale_Factor_Y = (float)SCREEN_HEIGHT / CPS3_HEIGHT; /* 240/224 */
        Scale_Off_X = 0.0f;
        Scale_Off_Y = 0.0f;

        Min_X = 0.0f;
        Max_X = 384.0f;
        Min_Y = 0.0f;
        Max_Y = 224.0f;
        break;
    case SCREEN_MODE_SQUARE: /* 4:3-ish: 320 wide, full height */
        Scale_Factor_X = 320.0f / 384.0f;
        Scale_Factor_Y = (float)SCREEN_HEIGHT / CPS3_HEIGHT;
        Scale_Off_X = (SCREEN_WIDTH - 320.0f) / 2.0f;
        Scale_Off_Y = 0.0f;

        Min_X = 0.0f;
        Max_X = 384.0f;
        Min_Y = 0.0f;
        Max_Y = 224.0f;
        break;
    case SCREEN_MODE_NATIVE: /* 1:1 pixel perfect, centered */
    case SCREEN_MODE_VERTICAL:
    case SCREEN_MODE_EXTENDED:
    default:
        Scale_Factor_X = 1.0f;
        Scale_Factor_Y = 1.0f;
        Scale_Off_X = (SCREEN_WIDTH - CPS3_WIDTH) / 2.0f;   /* 8 */
        Scale_Off_Y = (SCREEN_HEIGHT - CPS3_HEIGHT) / 2.0f; /* 8 */

        Min_X = 0.0f;
        Max_X = 384.0f;
        Min_Y = 0.0f;
        Max_Y = 224.0f;
        break;
    }

    /* Fade plane bounds (CPS3 640x480 fade-coordinate space) */
    Fade_Pos_tbl[0] = Min_X * 640 / 384;
    Fade_Pos_tbl[1] = Min_Y * 480 / 224;
    Fade_Pos_tbl[2] = Max_X * 640 / 384;
    Fade_Pos_tbl[3] = Min_Y * 480 / 224;
    Fade_Pos_tbl[4] = Min_X * 640 / 384;
    Fade_Pos_tbl[5] = Max_Y * 480 / 224;
    Fade_Pos_tbl[6] = Max_X * 640 / 384;
    Fade_Pos_tbl[7] = Max_Y * 480 / 224;
}

void enableOffscreenMode() {}

static C3D_RenderTarget *s_bot_target;
static C2D_SpriteSheet s_bot_sheet;
static C2D_Image s_bot_img;
static int s_have_bot;

/* Per-character command-list artwork on the bottom screen, indexed by character
 * id (Alex=1, Akuma=14, Makoto=16, ...). Only ids with a romfs:/cmdN.t3x load.
 *
 * Loaded ON DEMAND, one at a time, instead of all 20 up front: each is a
 * 320x240 image padded to a 512x256 RGBA8 GPU texture (512KB resident,
 * regardless of the ~90-100KB compressed size on disk). Preloading all 20
 * measured ~5.4MB free in the linear heap at this point in init, enough for
 * exactly 10 before every later C2D_SpriteSheetLoad silently returned NULL —
 * which is why roughly half the roster (anything id>=9 in load order) always
 * fell back to the default artwork no matter how clean the source PNG was.
 * Only one character's art is ever on screen at a time, so keeping a single
 * resident slot and swapping it when the selected id changes fits comfortably
 * within budget with zero quality loss. */
#define CMD_CHAR_MAX 20

/* The load itself (romfs read + LZ11 decompress + linear alloc) takes long
 * enough to visibly hitch a 60fps frame, and the FIRST version of this
 * lazy loader did it synchronously inside endFrame() the moment the select
 * cursor highlighted a new character — a stutter on every cursor move (user-
 * confirmed). Loads now happen on a LOW-PRIORITY WORKER THREAD which runs in
 * the main thread's vsync idle time; until the requested sheet is ready we
 * keep showing the previously-shown one (or the default cover), so the swap
 * is seamless. A small LRU keeps the last CMD_KEEP_MAX sheets resident so
 * cursor wiggling between neighbors doesn't reload at all. Evicted sheets
 * are freed a few frames LATE (s_cmd_frees) because the GPU may still be
 * asynchronously reading a texture drawn in the previous frame. */
#define CMD_KEEP_MAX 3   /* ~512KB per resident sheet — linear heap is TIGHT
                          * (~4MB free); 6 resident + in-flight + delayed
                          * frees could OOM the loads themselves, which then
                          * latched characters as permanently failed and the
                          * bottom screen "stopped switching" (user report). */
#define CMD_FREE_DELAY 2 /* frames before an evicted sheet's memory is freed */
#define CMD_RETRY_FRAMES 90 /* transient-failure retry cooldown (~1.5s) */

typedef struct {
    int id; /* character id, -1 = empty slot */
    C2D_SpriteSheet sheet;
    C2D_Image img;
    u32 last_use;
} CmdSlot;
static CmdSlot s_cmd_slots[CMD_KEEP_MAX]; /* ids set to -1 in initGu */
static u32 s_cmd_use_counter = 0;
static int s_cmd_shown_id = -1; /* what's on screen — never evicted */
static u32 s_cmd_frame = 0;     /* frame counter for retry cooldowns */
static u32 s_cmd_retry_at[CMD_CHAR_MAX]; /* earliest frame to retry a failed id */

typedef struct {
    C2D_SpriteSheet sheet;
    int frames_left;
} CmdFree;
static CmdFree s_cmd_frees[CMD_KEEP_MAX + 2];

/* worker handshake: single producer (loader) / single consumer (main).
 * main sets req_id then signals; loader publishes done_sheet BEFORE done_id
 * (with barriers); main consumes when done_id >= 0. */
static Thread s_cmd_thread;
static LightEvent s_cmd_req_ev;
static volatile int s_cmd_req_id = -1;
static volatile int s_cmd_done_id = -1;
static C2D_SpriteSheet s_cmd_done_sheet;
static volatile int s_cmd_thread_run = 0;

/* Compressed t3x file bytes, read into heap ONCE at boot (~95KB each, ~2MB
 * total). The loader thread decompresses from RAM — ZERO mid-game filesystem
 * traffic, so it can never contend with the main thread's AFS streaming on
 * the shared fs session (a suspected char-select slowdown; also just the
 * right architecture). */
static void* s_cmd_filebuf[CMD_CHAR_MAX];
static u32 s_cmd_filesize[CMD_CHAR_MAX];

static void cmd_preload_files(void) {
    for (int id = 0; id < CMD_CHAR_MAX; id++) {
        char path[24];
        snprintf(path, sizeof(path), "romfs:/cmd%d.t3x", id);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            void* buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                s_cmd_filebuf[id] = buf;
                s_cmd_filesize[id] = (u32)sz;
            } else if (buf) {
                free(buf);
            }
        }
        fclose(f);
    }
}

static void cmd_loader_thread(void* arg) {
    (void)arg;
    while (s_cmd_thread_run) {
        LightEvent_Wait(&s_cmd_req_ev);
        if (!s_cmd_thread_run)
            break;
        int id = s_cmd_req_id;
        if (id < 0 || s_cmd_done_id >= 0)
            continue; /* spurious wake / previous result not consumed yet */
        /* CPU-only work: LZ11 decompress from the boot-time RAM copy +
         * linearAlloc + data-cache flush. No filesystem, no C3D commands. */
        C2D_SpriteSheet sh = NULL;
        if (id >= 0 && id < CMD_CHAR_MAX && s_cmd_filebuf[id]) {
            sh = C2D_SpriteSheetLoadFromMem(s_cmd_filebuf[id], s_cmd_filesize[id]);
            if (sh && C2D_SpriteSheetCount(sh) == 0) {
                C2D_SpriteSheetFree(sh);
                sh = NULL;
            }
        }
        s_cmd_done_sheet = sh; /* publish result BEFORE the id that flags it */
        __sync_synchronize();
        s_cmd_done_id = id;
    }
}

static void cmd_defer_free(C2D_SpriteSheet sh) {
    if (!sh) return;
    for (unsigned i = 0; i < sizeof(s_cmd_frees) / sizeof(s_cmd_frees[0]); i++) {
        if (!s_cmd_frees[i].sheet) {
            s_cmd_frees[i].sheet = sh;
            s_cmd_frees[i].frames_left = CMD_FREE_DELAY;
            return;
        }
    }
    C2D_SpriteSheetFree(sh); /* queue full (shouldn't happen) — free now */
}

/* called once per frame from endFrame() */
static void cmd_service_frees(void) {
    s_cmd_frame++;
    for (unsigned i = 0; i < sizeof(s_cmd_frees) / sizeof(s_cmd_frees[0]); i++) {
        if (s_cmd_frees[i].sheet && --s_cmd_frees[i].frames_left <= 0) {
            C2D_SpriteSheetFree(s_cmd_frees[i].sheet);
            s_cmd_frees[i].sheet = NULL;
        }
    }
}

static C2D_Image *get_cmd_image(int id) {
    if (id < 0 || id >= CMD_CHAR_MAX)
        return NULL;

    /* adopt a finished background load, if any */
    if (s_cmd_done_id >= 0) {
        int did = s_cmd_done_id;
        C2D_SpriteSheet sh = s_cmd_done_sheet;
        s_cmd_done_sheet = NULL;
        s_cmd_req_id = -1;
        __sync_synchronize();
        s_cmd_done_id = -1; /* loader may take the next request now */
        if (sh) {
            int victim = -1;
            u32 oldest = UINT32_MAX;
            for (int i = 0; i < CMD_KEEP_MAX; i++) {
                if (s_cmd_slots[i].id < 0) { victim = i; break; }
                if (s_cmd_slots[i].id != s_cmd_shown_id &&
                    s_cmd_slots[i].last_use < oldest) {
                    oldest = s_cmd_slots[i].last_use;
                    victim = i;
                }
            }
            if (victim >= 0) {
                cmd_defer_free(s_cmd_slots[victim].sheet);
                s_cmd_slots[victim].id = did;
                s_cmd_slots[victim].sheet = sh;
                s_cmd_slots[victim].img = C2D_SpriteSheetGetImage(sh, 0);
                s_cmd_slots[victim].last_use = ++s_cmd_use_counter;
            } else {
                cmd_defer_free(sh);
            }
        } else if (did >= 0 && did < CMD_CHAR_MAX) {
            /* TRANSIENT failure (usually out-of-linear-memory under load, not
             * a missing file) — do NOT latch it permanently, or the bottom
             * screen stops updating for that character for the whole session.
             * Free the LRU slot right now to make room, then retry after a
             * short cooldown. */
            s_cmd_retry_at[did] = s_cmd_frame + CMD_RETRY_FRAMES;
            int victim = -1;
            u32 oldest = UINT32_MAX;
            for (int i = 0; i < CMD_KEEP_MAX; i++) {
                if (s_cmd_slots[i].id >= 0 && s_cmd_slots[i].id != s_cmd_shown_id &&
                    s_cmd_slots[i].last_use < oldest) {
                    oldest = s_cmd_slots[i].last_use;
                    victim = i;
                }
            }
            if (victim >= 0) {
                cmd_defer_free(s_cmd_slots[victim].sheet);
                s_cmd_slots[victim].id = -1;
                s_cmd_slots[victim].sheet = NULL;
            }
        }
    }

    /* resident already? */
    for (int i = 0; i < CMD_KEEP_MAX; i++) {
        if (s_cmd_slots[i].id == id) {
            s_cmd_slots[i].last_use = ++s_cmd_use_counter;
            s_cmd_shown_id = id;
            return &s_cmd_slots[i].img;
        }
    }

    /* not resident — kick the loader if it's idle (and past any cooldown) */
    if (s_cmd_thread_run && s_cmd_frame >= s_cmd_retry_at[id] &&
        s_cmd_req_id < 0 && s_cmd_done_id < 0) {
        s_cmd_req_id = id;
        __sync_synchronize();
        LightEvent_Signal(&s_cmd_req_ev);
    }

    /* keep showing the previous character until the new sheet arrives */
    if (s_cmd_shown_id >= 0) {
        for (int i = 0; i < CMD_KEEP_MAX; i++) {
            if (s_cmd_slots[i].id == s_cmd_shown_id)
                return &s_cmd_slots[i].img;
        }
    }
    return NULL; /* caller falls back to the default cover art */
}
extern u8 My_char[2]; /* My_char[0] = P1's CONFIRMED character id (set on the confirm button) */
extern u8 G_No[4];    /* top-level scene; G_No[0]==2 = char-select + match, else menus/title */
extern s8 Cursor_X[2], Cursor_Y[2]; /* P1 character-select grid cursor */
extern s8 ID_of_Face[3][8];         /* select grid -> character id (-1 = empty cell) */

/* --- on-screen text (loading screen + fatal errors) --------------------- */
static C2D_TextBuf s_msg_buf;

static void draw_message_frame(const char *msg) {
    if (!my_gu_init && !s_top_target)
        return;
    if (!s_msg_buf)
        s_msg_buf = C2D_TextBufNew(1024);
    C2D_TextBufClear(s_msg_buf);

    C2D_Text text;
    C2D_TextParse(&text, s_msg_buf, msg);
    C2D_TextOptimize(&text);

    float tw = 0.0f, th = 0.0f;
    C2D_TextGetDimensions(&text, 0.75f, 0.75f, &tw, &th);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top_target, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(s_top_target);
    C2D_DrawText(&text, C2D_WithColor, (SCREEN_WIDTH - tw) * 0.5f,
                 (SCREEN_HEIGHT - th) * 0.5f, 0.5f, 0.75f, 0.75f,
                 C2D_Color32(0xF0, 0xF0, 0xF0, 0xFF));
    C2D_TargetClear(s_bot_target, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(s_bot_target);
    C3D_FrameEnd(0);
}

void showLoadingMessage(const char *msg) {
    draw_message_frame(msg);
}

void fatalMessage(const char *msg) {
    /* A readable error beats a silent black-screen hang (the old while(1)).
     * Keep aptMainLoop running so HOME works; START exits to the launcher. */
    while (aptMainLoop()) {
        draw_message_frame(msg);
        hidScanInput();
        if (hidKeysDown() & (KEY_START | KEY_A | KEY_B))
            break;
    }
    endGu();
    exit(0);
}

void initGu() {
    gfxInitDefault();
    gfxSet3D(false);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* native game renderer (atlas-based) — draws the game's sprites/quads;
     * C2D remains for overlays and the bottom screen */
    SDLGameRenderer_Init();

    /* Optional bottom-screen artwork, baked into romfs. Absent → black.
     * Per-character art (s_cmd_*) is loaded on demand by a low-priority
     * worker thread — see get_cmd_image() — not preloaded here. */
    if (R_SUCCEEDED(romfsInit())) {
        s_bot_sheet = C2D_SpriteSheetLoad("romfs:/bottom.t3x");
        if (s_bot_sheet && C2D_SpriteSheetCount(s_bot_sheet) > 0) {
            s_bot_img = C2D_SpriteSheetGetImage(s_bot_sheet, 0);
            s_have_bot = 1;
        }
        /* read all compressed cmd images into RAM now (boot — invisible),
         * so the loader thread never touches the filesystem mid-game */
        cmd_preload_files();

        /* command-list loader thread: lowest practical priority so it only
         * runs in the main thread's vsync idle time (32KB stack covers the
         * LZ11 decompressor + stdio) */
        for (int i = 0; i < CMD_KEEP_MAX; i++)
            s_cmd_slots[i].id = -1;
        LightEvent_Init(&s_cmd_req_ev, RESET_ONESHOT);
        s_cmd_thread_run = 1;
        s_cmd_thread = threadCreate(cmd_loader_thread, NULL, 32 * 1024, 0x3F, -2, false);
        if (!s_cmd_thread)
            s_cmd_thread_run = 0; /* fall back: no per-character art, default cover only */
    }

    ctrGuInit();

    my_gu_init = 1;
}

void endGu() {
    if (!my_gu_init)
        return;
    if (s_cmd_thread_run) {
        s_cmd_thread_run = 0;
        LightEvent_Signal(&s_cmd_req_ev);
        threadJoin(s_cmd_thread, U64_MAX);
        threadFree(s_cmd_thread);
    }
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    my_gu_init = 0;
}

static u32 abgr_to_c2d(uint32_t c) {
    /* game colors arrive as ABGR8888 which matches C2D's u32 layout */
    return c;
}

void startFrame() {
#if SF3_PERF_LOG
    { /* PORT DIAG: total wall-clock time since the last startFrame — captures
       * game logic + everything else outside our other timed regions. */
        extern void debug_print(const char *fmt, ...);
        static u64 last_start = 0;
        u64 now = svcGetSystemTick();
        if (last_start != 0) {
            double ms = (double)(now - last_start) * 1000.0 / SYSCLOCK_ARM11;
            static int sf;
            if (ms > 20.0 || (++sf & 7) == 0) {
                debug_print("FRAMETOTAL %.1f", ms);
            }
        }
        last_start = now;
    }
#endif

    { /* Animation-stutter triage probe: total melt-decode time last frame.
       * Prints ONLY when a single frame spent >=4ms decoding — silent in
       * steady state, so it can stay in release builds. */
        extern u64 g_melt_frame_ticks;
        if (g_melt_frame_ticks) {
            double ms = (double)g_melt_frame_ticks * 1000.0 / SYSCLOCK_ARM11;
            if (ms >= 4.0) {
                extern void debug_print(const char *fmt, ...);
                debug_print("MELTSPIKE %.1fms", ms);
            }
            g_melt_frame_ticks = 0;
        }
    }

    setupScaling(render_mode);
    pspshim_gu_frame_reset();
    ctrGuFrameBegin();

    /* create pending GPU textures BEFORE the frame (DisplayTransfer is safe
     * here, not between FrameBegin/FrameEnd) */
#if SF3_PERF_LOG
    { /* PORT DIAG: time ProcessPending directly. */
        extern void debug_print(const char *fmt, ...);
        u64 t0 = svcGetSystemTick();
        SDLGameRenderer_ProcessPending();
        double ms = (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11;
        static int pp;
        if (ms > 5.0 || (++pp & 7) == 0) {
            debug_print("PENDMS %.1f pend=%d", ms, SDLGameRenderer_GetPendingCount());
        }
    }
#else
    SDLGameRenderer_ProcessPending();
#endif

#if SF3_PERF_LOG
    { /* PORT DIAG: isolate the vsync/GPU-sync wait itself from compute
       * cost — if this dominates FRAMETOTAL, the bottleneck is presentation
       * pacing (host/emulator-side), not game logic or rendering. */
        extern void debug_print(const char *fmt, ...);
        u64 __diag_t0 = svcGetSystemTick();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        double __diag_ms = (double)(svcGetSystemTick() - __diag_t0) * 1000.0 / 268111856.0;
        static int __diag_ctr;
        if (__diag_ms > 5.0 || (++__diag_ctr & 7) == 0) {
            debug_print("FRAMEBEGIN %.1f", __diag_ms);
        }
    }
#else
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#endif
    C2D_TargetClear(s_top_target, abgr_to_c2d(bg_color));
    C2D_SceneBegin(s_top_target);

    SDLGameRenderer_BeginFrame();

    /* Discard fully-transparent texels (CPS3 sprites have hard 1-bit alpha)
     * so they don't write the depth buffer. Without this, a front sprite's
     * transparent edges write depth and clip sprites drawn behind them,
     * producing rectangular halo artifacts wherever alpha sprites overlap
     * (heavy on the VS clash / fight fx). Re-asserted each frame in case
     * C2D's scene setup touches the alpha-test stage. */
    C3D_AlphaTest(true, GPU_GREATER, 0);

    s_in_frame = 1;

    ppgResetTextureCache();
}

void endFrame() {
    if (s_in_frame) {
        /* flush the native renderer's queued game draws to the top target
         * (its own shader/pipeline), then restore C2D state for the overlay
         * and bottom-screen draws below */
        SDLGameRenderer_RenderFrame();
        C2D_Prepare();

        /* startFrame() left GPU alpha-testing on (GPU_GREATER 0) so the native
         * renderer's game sprites discard hard-transparent texels. That raw
         * C3D state isn't touched by citro2d and otherwise stays active for
         * everything drawn below — crop bars, the bottom screen, HUD/glyph
         * overlays — silently discarding any pixel whose alpha isn't authored
         * as fully opaque (e.g. the bottom-screen PNGs), which is why the
         * bottom screen rendered solid black despite loading correctly. */
        C3D_AlphaTest(false, GPU_GREATER, 0);

        /* crop the centered play area: paint black over the screen-edge margins
         * so edge sprites/background don't bleed into them. Done while the top
         * scene is still active (set in startFrame). */
        ctrGuDrawCropBars(Scale_Off_X, Scale_Off_Y);

        /* Draw this frame's button-config glyph icons now — queued (not drawn)
         * when the game logic called ctrDrawButtonGlyph earlier this frame,
         * since anything drawn there gets painted over once the native
         * renderer's main batch (just submitted above) lands. */
        ctrGuFlushButtonGlyphs();

        /* The native renderer's imm_bind() (run inside SDLGameRenderer_RenderFrame
         * above) sets a hardware scissor rect clipped to the top screen's inner
         * game area (see ctr_game_renderer.c) and never clears it. Left active,
         * it also clips the bottom-screen target below — which has completely
         * different dimensions — discarding every fragment, which is why the
         * bottom-screen draw reported success but nothing ever appeared. */
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

        /* free bottom-screen sheets evicted a few frames ago (deferred so
         * the GPU is guaranteed done reading them) */
        cmd_service_frees();

        /* draw the bottom screen within the same GPU frame. Clear via C2D
         * (proven reliable on its own — the target-switch-then-DRAW step is
         * what silently failed, not the clear), then draw with this
         * renderer's own raw C3D quad path instead of citro2d — see
         * SDLGameRenderer_DrawRawQuadToTarget for why. */
        C2D_TargetClear(s_bot_target, C2D_Color32(0, 0, 0, 0xFF));
        C2D_Image *bimg = NULL;
        /* G_No[1]: 0 = title/attract (auto-demo), 1 = character-select, 2 = match.
         * Show only during select + match: at select preview the HOVERED character
         * (cursor) so it appears on hover; in the match use the confirmed
         * My_char[0] so it persists. The attract/title (G_No[1]==0) and menus
         * (G_No[0]!=2) fall back to the default artwork. */
        if (G_No[0] == 2 && (G_No[1] == 1 || G_No[1] == 2)) {
            s16 c;
            if (G_No[1] == 1) {
                s8 cy = Cursor_Y[0], cx = Cursor_X[0];
                c = (cy >= 0 && cy < 3 && cx >= 0 && cx < 8) ? ID_of_Face[cy][cx] : -1;
                /* Gill (id 0) is not on the select grid; a transient 0 here is an
                 * empty/uninitialized cell that briefly flashed Gill's image. */
                if (c == 0) c = -1;
            } else {
                c = (s16)My_char[0];
            }
            bimg = get_cmd_image(c);
        }
        if (!bimg && s_have_bot)
            bimg = &s_bot_img;
        if (bimg) {
            Tex3DS_SubTexture *st = bimg->subtex;
            SDLGameRenderer_DrawRawQuadToTarget(s_bot_target, bimg->tex, 320, 240,
                                                 st->left, st->top, st->right, st->bottom);
        }
        /* Defensive: flush any citro2d batch still pending from elsewhere
         * (e.g. crop bars) before the raw draw above and C3D_FrameEnd below.
         * The bottom-screen image itself no longer goes through citro2d — see
         * SDLGameRenderer_DrawRawQuadToTarget — so this is a no-op for it. */
        C2D_Flush();
#if SF3_PERF_LOG
        { /* PORT DIAG: time the GPU sync itself — if command submission is
             cheap but GPU execution is backed up, it shows up here, not in
             any of our own CPU-side timers. */
            extern void debug_print(const char *fmt, ...);
            u64 t0 = svcGetSystemTick();
            C3D_FrameEnd(0);
            double ms = (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11;
            static int fe;
            if (ms > 5.0 || (++fe & 7) == 0) {
                debug_print("FRAMEEND %.1f", ms);
            }
        }
#else
        C3D_FrameEnd(0);
#endif

        SDLGameRenderer_EndFrame();

#if SF3_PERF_LOG
        { /* PORT DIAG: surface the renderer's profile via the emulator log. */
            extern void debug_print(const char *fmt, ...);
            extern int cache_fail_invalid_get(void), cache_fail_too_big_get(void),
                       cache_fail_texinit_get(void), cache_fail_noslot_get(void);
            extern int dbg_task_repurposed;
            extern int dbg_regionupd_calls, dbg_regionupd_patched;
            extern unsigned int dbg_settex_miss(void), dbg_settex_create(void),
                                dbg_settex_fail(void), dbg_atlas_evict(void);
            extern int dbg_cache_full_returns;
            static int pf;
            if ((++pf & 7) == 0) {
                debug_print("RENDPROF %s pend=%d inv=%d big=%d tini=%d nslot=%d rep=%d ru=%d/%d stm=%u stc=%u stf=%u aev=%u cf=%d",
                            SDLGameRenderer_GetProfile(), SDLGameRenderer_GetPendingCount(),
                            cache_fail_invalid_get(), cache_fail_too_big_get(),
                            cache_fail_texinit_get(), cache_fail_noslot_get(),
                            dbg_task_repurposed, dbg_regionupd_calls, dbg_regionupd_patched,
                            dbg_settex_miss(), dbg_settex_create(), dbg_settex_fail(), dbg_atlas_evict(),
                            dbg_cache_full_returns);
            }
        }
#endif

        s_in_frame = 0;
    }
}

void endFrameDebug() {
    endFrame();
}

uint32_t getBgColor() {
    return bg_color;
}

void setBgColor(uint32_t color) {
    bg_color = color;
}

int getGuInit() {
    return my_gu_init;
}

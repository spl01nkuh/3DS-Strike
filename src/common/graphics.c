/* graphics.c — citro2d/citro3d frame lifecycle for the 3DS port.
 *
 * Contract preserved from the PSP version: startFrame applies the
 * render_mode scaling globals (Scale_Factor/Off, Min/Max clip bounds,
 * Fade_Pos_tbl) that the decompiled game reads directly, then opens the
 * frame; endFrame presents. Draws flow through src/ctr/gu_draw.c.
 */
#include "graphics.h"
#include "sprites.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "ctr/gu_draw.h"

// just for this project
#include "Game/WORK_SYS.h"
#include "Game/sc_data.h"

extern void ppgResetTextureCache(void);
extern void pspshim_gu_frame_reset(void);

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

void initGu() {
    gfxInitDefault();
    gfxSet3D(false);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* Optional bottom-screen artwork, baked into romfs. Absent → black. */
    if (R_SUCCEEDED(romfsInit())) {
        s_bot_sheet = C2D_SpriteSheetLoad("romfs:/bottom.t3x");
        if (s_bot_sheet && C2D_SpriteSheetCount(s_bot_sheet) > 0) {
            s_bot_img = C2D_SpriteSheetGetImage(s_bot_sheet, 0);
            s_have_bot = 1;
        }
    }

    ctrGuInit();

    my_gu_init = 1;
}

void endGu() {
    if (!my_gu_init)
        return;
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
    setupScaling(render_mode);
    pspshim_gu_frame_reset();
    ctrGuFrameBegin();

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top_target, abgr_to_c2d(bg_color));
    C2D_SceneBegin(s_top_target);

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
        /* crop the centered play area: paint black over the screen-edge margins
         * so edge sprites/background don't bleed into them. Done while the top
         * scene is still active (set in startFrame). */
        ctrGuDrawCropBars(Scale_Off_X, Scale_Off_Y);

        /* draw the bottom screen within the same GPU frame. Always clear AND
         * begin its scene (begin flushes the top batch and makes the bottom
         * the active target) so no stale framebuffer shows through. */
        C2D_TargetClear(s_bot_target, C2D_Color32(0, 0, 0, 0xFF));
        C2D_SceneBegin(s_bot_target);
        if (s_have_bot) {
            float iw = (float)s_bot_img.subtex->width;
            float ih = (float)s_bot_img.subtex->height;
            float sx = iw > 0 ? 320.0f / iw : 1.0f;
            float sy = ih > 0 ? 240.0f / ih : 1.0f;
            C2D_DrawImageAt(s_bot_img, 0.0f, 0.0f, 0.0f, NULL, sx, sy);
        }
        C3D_FrameEnd(0);
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

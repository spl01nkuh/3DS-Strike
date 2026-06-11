/* graphics.c — citro3d implementation of the frame lifecycle for 3DS.
 *
 * Contract preserved from the PSP version:
 *  - startFrame(): present-sync, apply render_mode scaling globals
 *    (Scale_Factor/Off, Min/Max clip bounds, Fade_Pos_tbl), clear.
 *  - endFrame()/endFrameDebug(): finish the frame.
 *  - The decompiled game reads Min_X/Max_X/Min_Y/Max_Y and uses
 *    SCALE_X/SCALE_Y everywhere, so setupScaling stays (pure C, no VFPU).
 *
 * The actual draw calls arrive through the GU-translation layer
 * (sceGuDrawArray & friends) which is wired to citro3d in the video
 * backend; this file owns targets, projection, and frame begin/end.
 */
#include "graphics.h"
#include "sprites.h"

#include <3ds.h>
#include <citro3d.h>

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

/* --- citro3d state --- */
static C3D_RenderTarget *s_top_target;
static C3D_Mtx s_projection;
static int s_in_frame = 0;

/* exposed for the GU translation layer (video backend) */
C3D_Mtx *ctrGetProjection(void) { return &s_projection; }
int ctrInFrame(void) { return s_in_frame; }

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
        Scale_Off_X = (SCREEN_WIDTH - CPS3_WIDTH) / 2.0f;  /* 8 */
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

void initGu() {
    gfxInitDefault();
    gfxSet3D(false);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    /* Top screen color target. The 3DS framebuffer is physically rotated:
       a "400x240" screen is a 240x400 buffer. C3D_RenderTargetCreate takes
       (height, width) in that physical orientation. */
    s_top_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    C3D_RenderTargetSetOutput(s_top_target, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                                  GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    /* 2D orthographic projection in screen coordinates (0,0 top-left). */
    Mtx_OrthoTilt(&s_projection, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 1.0f, true);

    my_gu_init = 1;
}

void endGu() {
    if (!my_gu_init)
        return;
    C3D_Fini();
    gfxExit();
    my_gu_init = 0;
}

static u32 bgr_clear_color(uint32_t argb) {
    /* game color is ABGR-ish via fixARGB; clear wants RGBA8 */
    u32 a = (argb >> 24) & 0xFF;
    u32 b = (argb >> 16) & 0xFF;
    u32 g = (argb >> 8) & 0xFF;
    u32 r = argb & 0xFF;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

void startFrame() {
    setupScaling(render_mode);
    pspshim_gu_frame_reset();

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(s_top_target, C3D_CLEAR_ALL, bgr_clear_color(bg_color), 0);
    C3D_FrameDrawOn(s_top_target);
    s_in_frame = 1;

    ppgResetTextureCache();
}

void endFrame() {
    if (s_in_frame) {
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

#ifndef GRAPHICS_H_ //  include guard
#define GRAPHICS_H_ 1

// libraries
#include <memory.h>
#include <pspdisplay.h>
#include <pspgu.h>

// custom
#include "sprites.h"

// constants
#define BUFFER_WIDTH 512
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272

// Scaling macros — switch between fullscreen and native
extern float Scale_Factor_X;
extern float Scale_Factor_Y;
extern float Scale_Off_X;
extern float Scale_Off_Y;

#define SCALE_X(x) ((x) * Scale_Factor_X + Scale_Off_X)
#define SCALE_Y(y) ((y) * Scale_Factor_Y + Scale_Off_Y)

// colour macros - fix color formats
#define fixARGB(c) ( (c & 0xFF00FF00) + ((c & 0x00FF0000) >> 16) + ((c & 0x000000FF) << 16) )

// c++ guard
#ifdef __cplusplus
extern "C" {
#endif

// Screen mode enums
#define SCREEN_MODE_STRETCH     0
#define SCREEN_MODE_SQUARE      1
#define SCREEN_MODE_NATIVE      2
#define SCREEN_MODE_VERTICAL    3
#define SCREEN_MODE_EXTENDED    4

#define SCREEN_DEFAULT_MODE     SCREEN_MODE_STRETCH
#define SCREEN_DEFAULT_FILTER   1
#define SCREEN_DEFAULT_RTT      0

// CPS3 clipping bounds
extern float Min_X, Max_X, Min_Y, Max_Y;

// variables
extern s16 render_mode;
extern s32 blit_filter;
extern int RTT_Enabled;

// functions
void initGu();
void endGu();

void startFrame();
void endFrame();
void endFrameDebug();
void enableOffscreenMode();

uint32_t getBgColor();
void setBgColor(uint32_t color);
int getGuInit();

// end c++ guard
#ifdef __cplusplus
}
#endif

#endif // GRAPHICS_H_

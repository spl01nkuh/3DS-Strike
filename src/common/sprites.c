/* sprites.c — quad helpers over the 3DS renderer (src/ctr/gu_draw.c).
 *
 * The PSP version built GU vertex arrays (VFPU-scaled); here the same
 * screen-space math (SCALE_X/SCALE_Y) feeds citro2d via ctrGuDrawTexQuad.
 * The bound texture comes from fl.c's flSetTexture (GU CLUT state).
 */
#include "sprites.h"
#include "graphics.h"

#include "ctr/gu_draw.h"

#include <stdlib.h>

TexturePSP *loadTexture(const char *filename, bool inVram) {
    (void)filename;
    (void)inVram;
    /* PNG loading was unused in the PSP build (no callers); keep NULL. */
    return NULL;
}

void setTexture(TexturePSP *texture, int tfx) {
    (void)tfx;
    if (texture)
        sceGuTexImage(0, texture->wRender, texture->hRender, texture->width, texture->data);
}

void drawTextureSet(float x1, float y1, float u1, float v1, float x2, float y2, float u2, float v2, u32 colour) {
    ctrGuDrawTexQuad(SCALE_X(x1), SCALE_Y(y1), u1, v1, SCALE_X(x2), SCALE_Y(y2), u2, v2, colour);
}

void drawTexture(TexturePSP *texture, float x1, float y1, float u1, float v1, float x2, float y2, float u2, float v2,
                 u32 colour) {
    setTexture(texture, 0);
    drawTextureSet(x1, y1, u1, v1, x2, y2, u2, v2, colour);
}

void drawTextureC(TexturePSP *texture, float x, float y, float w, float h) {
    if (texture)
        drawTexture(texture, x, y, 0.0f, 0.0f, x + w, y + h, texture->width, texture->height, 0xFFFFFFFF);
}

void drawTextureF(TexturePSP *texture, float x, float y) {
    if (texture)
        drawTextureC(texture, x, y, texture->width, texture->height);
}

void drawTextureH(TexturePSP *texture, float x, float y, uint32_t color) {
    (void)texture; (void)x; (void)y; (void)color;
    /* horizontal-repeat background draw — unused by the game path */
}

void drawRect(float x, float y, float w, float h, uint32_t colour) {
    ctrGuDrawRectSolid(SCALE_X(x), SCALE_Y(y), w * Scale_Factor_X, h * Scale_Factor_Y, colour);
}

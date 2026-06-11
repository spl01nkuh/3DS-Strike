/* sprites.c — 3DS stub of the PSP immediate-mode helpers.
 *
 * On PSP these built vertex arrays (with VFPU-accelerated scaling) and
 * issued sceGuDrawArray. The 3DS draw path goes through the GU translation
 * layer in the video backend; these helpers are only used by overlay/menu
 * code, so they start as no-ops and gain real bodies with the renderer.
 */
#include "sprites.h"
#include "graphics.h"

#include <stdlib.h>

TexturePSP *loadTexture(const char *filename, bool inVram) {
    (void)filename;
    (void)inVram;
    /* PNG loading was unused in the PSP build (no callers); keep NULL. */
    return NULL;
}

void setTexture(TexturePSP *texture, int tfx) {
    (void)texture;
    (void)tfx;
}

void drawTextureSet(float x1, float y1, float u1, float v1, float x2, float y2, float u2, float v2, u32 colour) {
    (void)x1; (void)y1; (void)u1; (void)v1;
    (void)x2; (void)y2; (void)u2; (void)v2;
    (void)colour;
}

void drawTexture(TexturePSP *texture, float x1, float y1, float u1, float v1, float x2, float y2, float u2, float v2,
                 u32 colour) {
    (void)texture;
    drawTextureSet(x1, y1, u1, v1, x2, y2, u2, v2, colour);
}

void drawTextureC(TexturePSP *texture, float x, float y, float w, float h) {
    (void)texture; (void)x; (void)y; (void)w; (void)h;
}

void drawTextureF(TexturePSP *texture, float x, float y) {
    (void)texture; (void)x; (void)y;
}

void drawTextureH(TexturePSP *texture, float x, float y, uint32_t color) {
    (void)texture; (void)x; (void)y; (void)color;
}

void drawRect(float x, float y, float w, float h, uint32_t colour) {
    (void)x; (void)y; (void)w; (void)h; (void)colour;
}

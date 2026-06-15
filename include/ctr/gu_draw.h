/* gu_draw.h — 3DS renderer behind the residual GU calls.
 *
 * fl.c binds CLUT textures via sceGuTexImage/sceGuClutLoad; sprites.c emits
 * axis-aligned quads; DC_Ghost.c emits untextured triangle lists. This module
 * owns that state, converts indexed PSP textures to native RGB5A1 C3D
 * textures (cached), and draws through citro2d.
 */
#ifndef CTR_GU_DRAW_H_
#define CTR_GU_DRAW_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ctrGuInit(void);
void ctrGuFrameBegin(void);

/* invalidate cached conversions for a source pointer (NULL = everything) */
void ctrGuTexcacheInvalidate(const void *src);

/* draw an axis-aligned textured quad with the currently bound GU texture.
 * coords in screen space, uv in texels, color = ABGR8888 modulate */
void ctrGuDrawTexQuad(float x1, float y1, float u1, float v1,
                      float x2, float y2, float u2, float v2, uint32_t color);

/* solid rect, ABGR color */
void ctrGuDrawRectSolid(float x, float y, float w, float h, uint32_t color);

/* draw opaque black bars over the screen-edge margins outside the centered
 * CPS3 play area, cropping sprite/background bleed (off_x/off_y = per-side
 * margin in screen px; 0 = no crop). Call while the top scene is active. */
void ctrGuDrawCropBars(float off_x, float off_y);

#ifdef __cplusplus
}
#endif

#endif /* CTR_GU_DRAW_H_ */

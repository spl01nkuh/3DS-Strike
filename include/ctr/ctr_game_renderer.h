/* ctr_game_renderer.h — native atlas-based 3DS renderer (ported from the
 * reference build). Keyed by fl texture/palette handles; Quad/Sprite/Sprite2
 * come from this tree's structs.h (identical layouts). */
#ifndef CTR_GAME_RENDERER_H
#define CTR_GAME_RENDERER_H

#include "structs.h"
#include "ctr/port_compat.h"

/* Forward declaration only (C3D_RenderTarget has a real tag to forward-
 * declare against) — avoids pulling <citro3d.h> into every TU that includes
 * this header. C3D_Tex has no tag of its own (anonymous struct typedef in
 * citro3d), so SDLGameRenderer_DrawRawQuadToTarget takes it as void* instead
 * and casts internally. Callers already include <citro3d.h> themselves
 * (needed for C2D_CreateScreenTarget etc.). */
typedef struct C3D_RenderTarget_tag C3D_RenderTarget;

typedef struct SDLGameRenderer_Vertex {
    struct {
        float x;
        float y;
        float z;
        float w;
    } coord;
    unsigned int color;
    TexCoord tex_coord;
} SDLGameRenderer_Vertex;

void SDLGameRenderer_Init(void);
void SDLGameRenderer_BeginFrame();
void SDLGameRenderer_RenderFrame();
void SDLGameRenderer_EndFrame();
void SDLGameRenderer_DrawRawQuadToTarget(C3D_RenderTarget* target, void* tex,
                                          unsigned short target_w, unsigned short target_h,
                                          float u0, float v0, float u1, float v1);
void SDLGameRenderer_DrawGlyphQuad(void* tex, float x0, float y0, float x1, float y1,
                                    float u0, float v0, float u1, float v1);
C3D_RenderTarget *ctrGetTopTarget(void);
void SDLGameRenderer_ProcessPending(void);
void SDLGameRenderer_ProcessPendingBlocking(void);
void SDLGameRenderer_DebugDumpProfile(void);
const char* SDLGameRenderer_GetProfile(void);

void SDLGameRenderer_CreateTexture(unsigned int th);
void SDLGameRenderer_DestroyTexture(unsigned int texture_handle);
void SDLGameRenderer_UnlockTexture(unsigned int th);
void SDLGameRenderer_UpdateTextureRegion(unsigned int th, int x, int y, int w, int h);
void SDLGameRenderer_DirectTileUpload(unsigned int tex_handle, int pal_handle,
                                      const unsigned char* cps3_data, int tile_w, int tile_h,
                                      int pixel_x, int pixel_y);
void SDLGameRenderer_PinTexture(unsigned int th);
void SDLGameRenderer_UnpinTexture(unsigned int th);
void SDLGameRenderer_CreatePalette(unsigned int ph);
void SDLGameRenderer_DestroyPalette(unsigned int palette_handle);
void SDLGameRenderer_UnlockPalette(unsigned int ph);
void SDLGameRenderer_PrewarmTexture(unsigned int th);
void SDLGameRenderer_SetTexture(unsigned int th);
void SDLGameRenderer_DrawTexturedQuad(const Sprite* sprite, unsigned int color);
void SDLGameRenderer_DrawSolidQuad(const Quad* vertices, unsigned int color);
void SDLGameRenderer_DrawSprite(const Sprite* sprite, unsigned int color);
void SDLGameRenderer_DrawSprite2(const Sprite2* sprite2);

/* helpers used by game-side caches (charset portrait hack in the reference) */
void SDLGameRenderer_ZeroCacheTexture(unsigned int th);
void SDLGameRenderer_ClearTileDirty(unsigned int th);
void SDLGameRenderer_RedirectTexturePixels(unsigned int th, const void* pixels);
int SDLGameRenderer_GetPendingCount(void);

#endif

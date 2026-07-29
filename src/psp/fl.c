#include "fl.h"
#include "ctr/ctr_game_renderer.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>

#include <pspdebug.h>
#include <string.h>

#include "AcrSDK/common/fbms.h"
#include "AcrSDK/common/mlPAD.h"
#include "AcrSDK/common/prilay.h"
#include "AcrSDK/common/memfound.h"
#include "Game/color3rd.h"
#include "Game/AcrUtil.h"
#include "psp/PPGFile.h"
#include "psp/flps2etc.h"

#include "common.h"

#include "sdk/libgraph.h"


FLTexture flPalette[FL_PALETTE_MAX];
FLTexture flTexture[FL_TEXTURE_MAX];
//u32 fltex_c[FL_TEXTURE_MAX];
FLPS2State flPs2State;

FL_FMS flFMS;

s32 flFrame;

int debug_mode = 0;

bool skip_frame = 0;

void *vram_particles;


void enableDebug(){
    if(debug_mode == 0){
        pspDebugScreenInit();
        debug_mode = 1;
    }
}

/* Silent in normal builds: every call cost a vsnprintf plus a debug-channel
 * syscall, and the engine logs from a lot of places. Set FL_LOG_ENABLE to 1 to
 * get the running commentary back on the Azahar log / gdb. */
#define FL_LOG_ENABLE 0

s32 flLogOut(s8* format, ...){
#if FL_LOG_ENABLE
    extern void debug_print(const char *fmt, ...);
    char buffer[LOG_BUFFER_SIZE];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    debug_print("%s", buffer);
#else
    (void)format;
#endif
    return 1;
}

s32 flPrintL(s32 posi_x, s32 posi_y, const s8* format, ...) {
    if(DEMMA_DEBUG){
        char buffer[LOG_BUFFER_SIZE];

        va_list args;
        va_start(args, format);

        vsnprintf(buffer, sizeof(buffer), format, args);

        va_end(args);

        enableDebug();
        pspDebugScreenSetXY(posi_x, posi_y);
        pspDebugScreenPrintf("%s", buffer);
    }

    return 1;
}

s32 flPrintColor(u32 col){
    if(DEMMA_DEBUG){
        enableDebug();
        pspDebugScreenSetTextColor(col);
    }

    return 1;
}

s32 flFlip(u32 flag) {
    flFrame++;

    //flLogOut("flFrame %d\n", flFrame);

    return 1;
}

// forward decls
u32 flPS2GetTextureSize(u32 format, s32 dw, s32 dh, s32 bnum);
s32 flPS2LockTexture(Rect* /* unused */, FLTexture* lpflTexture, plContext* lpcontext, u32 flag, s32 /* unused */);
s32 flPS2UnlockTexture(FLTexture*);
static s32 system_work_init();

void swizzle_fast(u8* out, u8* in, unsigned int width, unsigned int height) {
   unsigned int blockx, blocky;
   unsigned int j;
 
   unsigned int width_blocks = (width >> 4);
   unsigned int height_blocks = (height >> 3);
 
   unsigned int src_pitch = (width-16) >> 2;
   unsigned int src_row = width << 3;
 
   u8* ysrc = in;
   u32* dst = (u32*)out;
 
   for (blocky = 0; blocky < height_blocks; ++blocky)
   {
      u8* xsrc = ysrc;
      for (blockx = 0; blockx < width_blocks; ++blockx)
      {
         u32* src = (u32*)xsrc;
         for (j = 0; j < 8; ++j)
         {
            *(dst++) = *(src++);
            *(dst++) = *(src++);
            *(dst++) = *(src++);
            *(dst++) = *(src++);
            src += src_pitch;
         }
         xsrc += 16;
     }
     ysrc += src_row;
   }
}

void swizzle_inplace(void *data, uint32_t width_bytes, uint32_t height) {
    uint32_t rowblocks = width_bytes / 16;
    uint32_t sz = width_bytes * height;
    u8 *tmp = (u8 *)malloc(sz);
    if (!tmp) return;

    u8 *src = (u8 *)data;
    u8 *dst = tmp;
    uint32_t blockx, blocky, j;

    for (blocky = 0; blocky < height; blocky++) {
        for (blockx = 0; blockx < rowblocks; blockx++) {
            uint32_t block_idx = blockx + (blocky / 8) * rowblocks;
            uint32_t block_ofs = block_idx * 16 * 8 + (blocky & 7) * 16;
            for (j = 0; j < 16; j++) {
                dst[block_ofs + j] = *src++;
            }
        }
    }

    memcpy(data, tmp, sz);
    free(tmp);
}

bool swizzle_inplace_correct_5551(void *data, uint32_t width_bytes, uint32_t height) {
    uint32_t rowblocks = width_bytes / 16;
    uint32_t sz = width_bytes * height;

    u8 *tmp = (u8 *)malloc(sz);
    if (!tmp) return false;

    u8 *src = (u8 *)data;
    u8 *dst = tmp;

    for (uint32_t blocky = 0; blocky < height; blocky++) {
        for (uint32_t blockx = 0; blockx < rowblocks; blockx++) {

            uint32_t block_idx = blockx + (blocky / 8) * rowblocks;
            uint32_t block_ofs = block_idx * 16 * 8 + (blocky & 7) * 16;

            // process 16 bytes = 8 pixels (16-bit each)
            u16 *dst16 = (u16 *)(dst + block_ofs);
            u16 *src16 = (u16 *)src;

            for (uint32_t j = 0; j < 8; j++) {
                u16 v = *src16++;

                // swap R (bits 10–14) and B (bits 0–4)
                u16 r = (v >> 10) & 0x1F;
                u16 b = v & 0x1F;

                v &= ~0x7C1F;
                v |= (b << 10) | r;

                dst16[j] = v;
            }

            src += 16; // advance 16 bytes
        }
    }

    memcpy(data, tmp, sz);
    free(tmp);
    return true;
}

u32 flCreateTextureHandle(plContext* bits, u32 flag, u8 mode) {
    FLTexture* lpflTexture;
    u32 th = flPS2GetTextureHandle();

    if (th == 0) {
        return 0;
    }

    //fltex_c[th - 1]++;

    lpflTexture = &flTexture[LO_16_BITS(th) - 1];
    flPS2GetTextureInfoFromContext(bits, 1, th, flag);

    if (bits->ptr == NULL) {
        /* "Empty" placeholder texture (melt/multi-texture sheets): real
         * memory is allocated now, content arrives later via per-tile melt
         * writes (ppgNotifyTextureRegionUpdate). Register it with the
         * renderer immediately so that path has a valid target to mark
         * dirty — without this, these handles never capture a pixels
         * pointer at all and stay permanently invalid (silently invisible:
         * the opening montage, scrolling backgrounds, and anything else
         * built from on-demand melt sheets). */
        lpflTexture->mem_handle = flPS2GetSystemMemoryHandle(lpflTexture->size, 0);
        SDLGameRenderer_CreateTexture(th);
    } else {
        flPS2ConvertTextureFromContext(bits, lpflTexture, 0, mode);
        flPS2CreateTextureHandle(th, flag);
    }

    return th;
}

s32 flPS2GetTextureInfoFromContext(plContext* bits, s32 bnum, u32 th, u32 flag) {
    FLTexture* lpflTexture;
    s32 lp0;
    s32 dw;
    s32 dh;
    plContext* lpcon;

    lpflTexture = &flTexture[LO_16_BITS(th) - 1];

    if (bnum > 1) {
        if (bnum > 7) {
            flLogOut("Not supported mipmap texture @flPS2GetTextureInfoFromContext");
            assert(0);
            return 0;
        }

        lpcon = bits + 1;
        dw = bits->width;
        dh = bits->height;

        for (lp0 = 1; lp0 < bnum; lp0++) {
            dw >>= 1;
            dh >>= 1;

            if ((lpcon->width != dw) || (lpcon->height != dh)) {
                flLogOut("Not supported mipmap texture @flPS2GetTextureInfoFromContext");
                assert(0);
                return 0;
            }

            lpcon += 1;
        }
    }

    lpflTexture->be_flag = 1;
    lpflTexture->flag = flag;
    lpflTexture->desc = bits->desc;
    lpflTexture->width = bits->width;
    lpflTexture->height = bits->height;
    lpflTexture->mem_handle = 0;
    lpflTexture->lock_ptr = 0;
    lpflTexture->lock_flag = 0;
    lpflTexture->tex_num = bnum;
    lpflTexture->swizzeled = 0;

    switch (bits->bitdepth) {
    default:
        flLogOut("Not supported texture bit depth @flPS2GetTextureInfoFromContext");
        assert(0);
        return 0;

    case 0:
        lpflTexture->format = GU_PSM_T4;
        lpflTexture->bitdepth = 0;
        break;

    case 1:
        lpflTexture->format = GU_PSM_T8;
        lpflTexture->bitdepth = 1;
        break;

    case 2:
        lpflTexture->format = GU_PSM_5551;
        lpflTexture->bitdepth = 2;
        break;

    case 3:
        lpflTexture->format = GU_PSM_4444;
        lpflTexture->bitdepth = 2;
        break;

    case 4:
        lpflTexture->format = GU_PSM_8888;
        lpflTexture->bitdepth = 4;
        break;
    }

    switch (bits->width) {
    case 1024:
    case 512:
    case 256:
    case 128:
    case 64:
    case 32:
        break;

    default:
        flLogOut("Not supported width...%d @flPS2GetTextureInfoFromContext", bits->width);
        return 0;
    }

    switch (bits->height) {
    case 1024:
    case 512:
    case 256:
    case 128:
    case 64:
    case 32:
        break;

    default:
        flLogOut("Not supported height...%d @flPS2GetTextureInfoFromContext", bits->height);
        return 0;
    }

    lpflTexture->size =
        flPS2GetTextureSize(lpflTexture->format, lpflTexture->width, lpflTexture->height, lpflTexture->tex_num);
    return 1;
}

s32 flPS2CreateTextureHandle(u32 th, u32 flag) {
    SDLGameRenderer_CreateTexture(th);
    return 1;
}

u32 flPS2GetTextureHandle() {
    s32 i;

    for (i = 0; i < FL_TEXTURE_MAX; i++) {
        if (!flTexture[i].be_flag) {
            break;
        }
    }

    if (i == FL_TEXTURE_MAX) {
        flPrintColor(0xFFFF0000);
        flLogOut("ERROR flPS2GetTextureHandle flps2vram.c\n");
    }

    return i + 1;
}

u32 flCreatePaletteHandle(plContext* lpcontext, u32 flag) {
        FLTexture* lpflPalette;
    u32 ph = flPS2GetPaletteHandle();

    if (ph == 0) {
        return 0;
    }

    lpflPalette = &flPalette[HI_16_BITS(ph) - 1];
    flPS2GetPaletteInfoFromContext(lpcontext, ph, flag);

    lpflPalette->mem_handle = flPS2GetSystemMemoryHandle(lpflPalette->size, 2);
    lpflPalette->wkVram = NULL;
    //lpflPalette->wkVram = memalign(16, lpflPalette->size);

    if (lpcontext->ptr != NULL) {
        u16* src = (u16*) lpcontext->ptr;
        u16* dest = (u16*) flPS2GetSystemBuffAdrs(lpflPalette->mem_handle);
        //u16* dest = (u16*) lpflPalette->wkVram;

        int n = lpflPalette->size / 2;
        
        for(int i = 0; i < n; i++){
            u16 c = src[i];

            u16 a = c & 0x8000;
            u16 r = (c >> 10) & 0x1F;
            u16 g = (c >> 5) & 0x1F;
            u16 b = (c >> 0) & 0x1F;

            dest[i] = a | (b << 10) | (g << 5) | r;
        }

        sceKernelDcacheWritebackRange(dest, lpflPalette->size);
        flPS2CreatePaletteHandle(ph, flag);

        /* new content just written into a (possibly recycled) buffer — make
         * sure the draw-side checksum memo re-hashes it (see flUnlockPalette) */
        {
            extern void ctrGuClutWritten(const void *buf);
            ctrGuClutWritten(dest);
        }
    }

    return ph >> 16;
}

s32 flPS2GetPaletteInfoFromContext(plContext* bits, u32 ph, u32 flag) {
    FLTexture* lpflPalette = &flPalette[((ph & 0xFFFF0000) >> 0x10) - 1];

    if (bits->height != 1) {
        flLogOut("Supported only 1 palette. Unallocatable. @flCreatePaletteHandle");
        return 0;
    }

    switch (bits->bitdepth) {
    default:
        flLogOut("Not supported texture bit depth @flCreatePaletteHandle");
        return 0;

    case 2:
        lpflPalette->format = 2;
        lpflPalette->bitdepth = 2;
        break;

    case 3:
        lpflPalette->format = 1;
        lpflPalette->bitdepth = 3;
        break;

    case 4:
        lpflPalette->format = 0;
        lpflPalette->bitdepth = 4;
        break;
    }

    if (bits->width == 256) {
        lpflPalette->width = 16;
        lpflPalette->height = 16;
    } else {
        lpflPalette->width = 8;
        lpflPalette->height = 2;
    }

    lpflPalette->desc = bits->desc;
    lpflPalette->flag = flag;
    lpflPalette->be_flag = 1;
    lpflPalette->mem_handle = 0;
    lpflPalette->lock_ptr = 0;
    lpflPalette->lock_flag = 0;
    lpflPalette->tex_num = 1;
    lpflPalette->size = flPS2GetTextureSize(lpflPalette->format, lpflPalette->width, lpflPalette->height, lpflPalette->tex_num);
    return 1;
}

s32 flPS2CreatePaletteHandle(u32 ph, u32 flag) {
    SDLGameRenderer_CreatePalette(ph);
    return 1;
}

u32 flPS2GetPaletteHandle() {
    s32 i;

    for (i = 0; i < FL_PALETTE_MAX; i++) {
        if (!flPalette[i].be_flag) {
            break;
        }
    }

    if (i == FL_PALETTE_MAX) {
        flPrintColor(0xFFFF0000);
        flLogOut("ERROR flPS2GetPaletteHandle flps2vram.c\n");
    }

    return (i + 1) << 16;
}

s32 flReleaseTextureHandle(u32 texture_handle) {
    FLTexture* lpflTexture = &flTexture[texture_handle - 1];

    flLogOut("flReleaseTextureHandle %d\n", texture_handle);

    if ((texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX) || (lpflTexture->be_flag == 0)) {
        flPrintColor(0xFFFF0000);
        flLogOut("ERROR flReleaseTextureHandle flps2vram.c\n");
    }

    SDLGameRenderer_DestroyTexture(texture_handle);

    if(lpflTexture->mem_handle != 0){
        /* The GPU-side cache (gu_draw.c) is keyed by the raw CPU source
         * pointer with no relocation/lifetime hook of its own. Once this
         * memory is freed it can be handed back out to an unrelated
         * allocation; without invalidating here first, a later texture that
         * happens to land at this same address could alias a stale cache
         * entry and render completely unrelated content. */
        extern void ctrGuTexcacheInvalidate(const void *src);
        ctrGuTexcacheInvalidate(flPS2GetSystemBuffAdrs(lpflTexture->mem_handle));
        flPS2ReleaseSystemMemory(lpflTexture->mem_handle);
        lpflTexture->mem_handle = 0;
    }


    flMemset(lpflTexture, 0, sizeof(FLTexture));
    return 1;
}

s32 flReleasePaletteHandle(u32 palette_handle) {
    FLTexture* lpflPalette = &flPalette[palette_handle - 1];

    if ((palette_handle == 0) || (palette_handle > FL_PALETTE_MAX) || (lpflPalette->be_flag == 0)) {
        flPrintColor(0xFFFF0000);
        flLogOut("ERROR flReleasePaletteHandle flps2vram.c\n");
    }

    SDLGameRenderer_DestroyPalette(palette_handle);

    if (lpflPalette->mem_handle != 0) {
        /* Same reasoning as flReleaseTextureHandle: invalidate any cache
         * entry keyed by this palette's address before the memory is freed
         * and can be reused by something unrelated. */
        extern void ctrGuTexcacheInvalidate(const void *src);
        ctrGuTexcacheInvalidate(flPS2GetSystemBuffAdrs(lpflPalette->mem_handle));
        flPS2ReleaseSystemMemory(lpflPalette->mem_handle);
    }

    else if (lpflPalette->wkVram != NULL) {
        lpflPalette->wkVram = NULL;
    }

    flMemset(lpflPalette, 0, sizeof(FLTexture));
    return 1;
}

s32 flLockTexture(Rect* lprect, u32 th, plContext* lpcontext, u32 flag) {
    FLTexture* lpflTexture = &flTexture[th - 1];

    if (th > FL_TEXTURE_MAX) {
        return 0;
    }

    if (!lpflTexture->be_flag) {
        return 0;
    }

    return flPS2LockTexture(lprect, lpflTexture, lpcontext, flag, 0);
}

s32 flLockPalette(Rect* lprect, u32 th, plContext* lpcontext, u32 flag) {
    FLTexture* lpflPalette = &flPalette[th - 1];

    if (th > FL_PALETTE_MAX) {
        return 0;
    }

    if (!lpflPalette->be_flag) {
        return 0;
    }

    if (flPS2LockTexture(lprect, lpflPalette, lpcontext, flag, 1) == 0) {
        return 0;
    }

    if ((lpflPalette->width == 16) && (lpflPalette->height == 16)) {
        lpcontext->width = 256;
        lpcontext->height = 1;
    } else {
        lpcontext->width = 16;
        lpcontext->height = 1;
    }

    lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
    return 1;
}

s32 flPS2LockTexture(Rect* /* unused */, FLTexture* lpflTexture, plContext* lpcontext, u32 flag, s32 /* unused */) {
    u8* buff_ptr;
    u8* buff_ptr1;
    plContext src;

    lpflTexture->lock_flag = flag;
    lpcontext->desc = lpflTexture->desc;
    lpcontext->width = lpflTexture->width;
    lpcontext->height = lpflTexture->height;

    switch (flag & 3) {
    case 0:
        if (lpflTexture->mem_handle == 0) {
            buff_ptr1 = mflTemporaryUse(lpflTexture->size * 2);
            buff_ptr = buff_ptr1 + lpflTexture->size;
            // Loading an image from VRAM used to be here
        } else {
            buff_ptr = mflTemporaryUse(lpflTexture->size);
            buff_ptr1 = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
        }

        lpflTexture->lock_ptr = (uintptr_t)buff_ptr;
        lpcontext->ptr = buff_ptr;
        src.desc = lpcontext->desc;
        src.width = lpcontext->width;
        src.height = lpcontext->height;
        src.ptr = buff_ptr1;

        switch (lpflTexture->format) {
        case 20:
            lpcontext->bitdepth = 0;
            lpcontext->pitch = lpcontext->width / 2;
            flMemcpy(buff_ptr, buff_ptr1, lpflTexture->size);
            break;

        case 19:
            lpcontext->bitdepth = 1;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            flMemcpy(buff_ptr, buff_ptr1, lpflTexture->size);
            break;

        case 2:
            lpcontext->bitdepth = 2;
            lpcontext->pixelformat.rl = 5;
            lpcontext->pixelformat.rs = 0xA;
            lpcontext->pixelformat.rm = 0x1F;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gs = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pixelformat.bl = 5;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0x1F;
            lpcontext->pixelformat.al = 1;
            lpcontext->pixelformat.as = 0xF;
            lpcontext->pixelformat.am = 1;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            src.bitdepth = 2;
            src.pixelformat.rl = 5;
            src.pixelformat.rs = 0xA;
            src.pixelformat.rm = 0x1F;
            src.pixelformat.gl = 5;
            src.pixelformat.gs = 5;
            src.pixelformat.gm = 0x1F;
            src.pixelformat.bl = 5;
            src.pixelformat.bs = 0;
            src.pixelformat.bm = 0x1F;
            src.pixelformat.al = 1;
            src.pixelformat.as = 0xF;
            src.pixelformat.am = 1;
            src.pixelformat.rs = 0;
            src.pixelformat.bs = 0xA;
            src.pixelformat.gl = 5;
            src.pixelformat.gm = 0x1F;
            src.pitch = src.width * src.bitdepth;
            plConvertContext(lpcontext, &src);
            break;

        case 1:
            lpcontext->bitdepth = 3;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 0;
            lpcontext->pixelformat.as = 0;
            lpcontext->pixelformat.am = 0;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            src.bitdepth = 3;
            src.pixelformat.rl = 8;
            src.pixelformat.rs = 0x10;
            src.pixelformat.rm = 0xFF;
            src.pixelformat.gl = 8;
            src.pixelformat.gs = 8;
            src.pixelformat.gm = 0xFF;
            src.pixelformat.bl = 8;
            src.pixelformat.bs = 0;
            src.pixelformat.bm = 0xFF;
            src.pixelformat.al = 0;
            src.pixelformat.as = 0;
            src.pixelformat.am = 0;
            src.pixelformat.rs = 0;
            src.pixelformat.bs = 0x10;
            src.pitch = src.width * src.bitdepth;
            plConvertContext(lpcontext, &src);
            break;

        case 0:
            lpcontext->bitdepth = 4;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 8;
            lpcontext->pixelformat.as = 0x18;
            lpcontext->pixelformat.am = 0xFF;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            src.bitdepth = 4;
            src.pixelformat.rl = 8;
            src.pixelformat.rs = 0x10;
            src.pixelformat.rm = 0xFF;
            src.pixelformat.gl = 8;
            src.pixelformat.gs = 8;
            src.pixelformat.gm = 0xFF;
            src.pixelformat.bl = 8;
            src.pixelformat.bs = 0;
            src.pixelformat.bm = 0xFF;
            src.pixelformat.al = 8;
            src.pixelformat.as = 0x18;
            src.pixelformat.am = 0xFF;
            src.pixelformat.rs = 0;
            src.pixelformat.bs = 0x10;
            src.pitch = src.width * src.bitdepth;
            plConvertContext(lpcontext, &src);
            break;
        }

        break;

    case 1:
        buff_ptr = mflTemporaryUse(lpflTexture->size);

        if (lpflTexture->mem_handle == 0) {
            // Loading an image from VRAM used to be here
        } else {
            buff_ptr1 = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
            flMemcpy(buff_ptr, buff_ptr1, lpflTexture->size);
        }

        lpflTexture->lock_ptr = (uintptr_t)buff_ptr;
        lpcontext->ptr = buff_ptr;

        switch (lpflTexture->format) {
        case 20:
            lpcontext->bitdepth = 0;
            lpcontext->pitch = lpcontext->width / 2;
            break;

        case 19:
            lpcontext->bitdepth = 1;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 2:
            lpcontext->bitdepth = 2;
            lpcontext->pixelformat.rl = 5;
            lpcontext->pixelformat.rs = 0xA;
            lpcontext->pixelformat.rm = 0x1F;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gs = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pixelformat.bl = 5;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0x1F;
            lpcontext->pixelformat.al = 1;
            lpcontext->pixelformat.as = 0xF;
            lpcontext->pixelformat.am = 1;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0xA;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 1:
            lpcontext->bitdepth = 3;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 0;
            lpcontext->pixelformat.as = 0;
            lpcontext->pixelformat.am = 0;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0x10;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 0:
            lpcontext->bitdepth = 4;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 8;
            lpcontext->pixelformat.as = 0x18;
            lpcontext->pixelformat.am = 0xFF;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0x10;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;
        }

        break;

    case 2:
        if (lpflTexture->mem_handle == 0) {
            buff_ptr = mflTemporaryUse(lpflTexture->size);
        } else {
            buff_ptr = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
        }

        lpflTexture->lock_ptr = (uintptr_t)buff_ptr;
        lpcontext->ptr = buff_ptr;

        switch (lpflTexture->format) {
        case 20:
            lpcontext->bitdepth = 0;
            lpcontext->pitch = lpcontext->width / 2;
            break;

        case 19:
            lpcontext->bitdepth = 1;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 2:
            lpcontext->bitdepth = 2;
            lpcontext->pixelformat.rl = 5;
            lpcontext->pixelformat.rs = 0xA;
            lpcontext->pixelformat.rm = 0x1F;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gs = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pixelformat.bl = 5;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0x1F;
            lpcontext->pixelformat.al = 1;
            lpcontext->pixelformat.as = 0xF;
            lpcontext->pixelformat.am = 1;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0xA;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 1:
            lpcontext->bitdepth = 3;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 0;
            lpcontext->pixelformat.as = 0;
            lpcontext->pixelformat.am = 0;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0x10;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 0:
            lpcontext->bitdepth = 4;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 8;
            lpcontext->pixelformat.as = 0x18;
            lpcontext->pixelformat.am = 0xFF;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0x10;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;
        }

        break;

    case 3:
        if (lpflTexture->mem_handle == 0) {
            buff_ptr = mflTemporaryUse(lpflTexture->size);
        } else {
            buff_ptr = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
        }

        lpflTexture->lock_ptr = (uintptr_t)buff_ptr;
        lpcontext->ptr = buff_ptr;

        switch (lpflTexture->format) {
        case 20:
            lpcontext->bitdepth = 0;
            lpcontext->pitch = lpcontext->width / 2;
            break;

        case 19:
            lpcontext->bitdepth = 1;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 2:
            lpcontext->bitdepth = 2;
            lpcontext->pixelformat.rl = 5;
            lpcontext->pixelformat.rs = 0xA;
            lpcontext->pixelformat.rm = 0x1F;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gs = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pixelformat.bl = 5;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0x1F;
            lpcontext->pixelformat.al = 1;
            lpcontext->pixelformat.as = 0xF;
            lpcontext->pixelformat.am = 1;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0xA;
            lpcontext->pixelformat.gl = 5;
            lpcontext->pixelformat.gm = 0x1F;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 1:
            lpcontext->bitdepth = 3;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 0;
            lpcontext->pixelformat.as = 0;
            lpcontext->pixelformat.am = 0;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0x10;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;

        case 0:
            lpcontext->bitdepth = 4;
            lpcontext->pixelformat.rl = 8;
            lpcontext->pixelformat.rs = 0x10;
            lpcontext->pixelformat.rm = 0xFF;
            lpcontext->pixelformat.gl = 8;
            lpcontext->pixelformat.gs = 8;
            lpcontext->pixelformat.gm = 0xFF;
            lpcontext->pixelformat.bl = 8;
            lpcontext->pixelformat.bs = 0;
            lpcontext->pixelformat.bm = 0xFF;
            lpcontext->pixelformat.al = 8;
            lpcontext->pixelformat.as = 0x18;
            lpcontext->pixelformat.am = 0xFF;
            lpcontext->pixelformat.rs = 0;
            lpcontext->pixelformat.bs = 0x10;
            lpcontext->pitch = lpcontext->width * lpcontext->bitdepth;
            break;
        }

        break;
    }

    lpcontext->desc = lpcontext->desc | 2;
    return 1;
}

s32 flUnlockTexture(u32 th) {
    FLTexture* lpflTexture = &flTexture[th - 1];

    if (th > FL_TEXTURE_MAX) {
        return 0;
    }

    if (!lpflTexture->be_flag) {
        return 0;
    }

    {
        const s32 ret = flPS2UnlockTexture(lpflTexture);
        SDLGameRenderer_UnlockTexture(th);
        return ret;
    }
}

s32 flUnlockPalette(u32 th) {
    FLTexture* lpflPalette = &flPalette[th - 1];

    if (th > FL_PALETTE_MAX) {
        return 0;
    }

    if (!lpflPalette->be_flag) {
        return 0;
    }

    { /* the palette's content was just rewritten in place (lock/unlock is the
       * game's palette-fade path: color-trans, character flashes). Invalidate
       * the draw-side checksum memo so the new colors are re-hashed on next
       * bind — its 3-entry fingerprint cannot detect writes that keep those
       * entries unchanged (opening DC-ghost row fades = the attract-mode
       * "black box"/frozen-fade corruption). See ctrGuClutWritten. */
        extern void ctrGuClutWritten(const void *buf);
        const void *buf = lpflPalette->wkVram;
        if (buf == NULL)
            buf = flPS2GetSystemBuffAdrs(lpflPalette->mem_handle);
        ctrGuClutWritten(buf);
    }

    {
        const s32 ret = flPS2UnlockTexture(lpflPalette);
        SDLGameRenderer_UnlockPalette(th);
        return ret;
    }
}

s32 flPS2UnlockTexture(FLTexture* lpflTexture) {
    u8* buff_ptr;
    u8* buff_ptr1;
    plContext src;
    plContext dst;

    switch (lpflTexture->lock_flag & 3) {
    case 0:
        if (lpflTexture->mem_handle != 0) {
            buff_ptr = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
            buff_ptr1 = (u8*)lpflTexture->lock_ptr;
        } else {
            buff_ptr = mflTemporaryUse(lpflTexture->size);
            buff_ptr1 = (u8*)lpflTexture->lock_ptr;
        }

        src.desc = lpflTexture->desc;
        src.width = lpflTexture->width;
        src.height = lpflTexture->height;
        src.ptr = buff_ptr1;
        dst.desc = lpflTexture->desc;
        dst.width = lpflTexture->width;
        dst.height = lpflTexture->height;
        dst.ptr = buff_ptr;

        switch (lpflTexture->format) {
        case 20:
        case 19:
            flMemcpy(buff_ptr, buff_ptr1, lpflTexture->size);
            break;

        case 2:
            src.bitdepth = 2;
            src.pixelformat.rl = 5;
            src.pixelformat.rs = 0xA;
            src.pixelformat.rm = 0x1F;
            src.pixelformat.gl = 5;
            src.pixelformat.gs = 5;
            src.pixelformat.gm = 0x1F;
            src.pixelformat.bl = 5;
            src.pixelformat.bs = 0;
            src.pixelformat.bm = 0x1F;
            src.pixelformat.al = 1;
            src.pixelformat.as = 0xF;
            src.pixelformat.am = 1;
            src.pitch = src.width * src.bitdepth;
            dst.bitdepth = 2;
            dst.pixelformat.rl = 5;
            dst.pixelformat.rs = 0xA;
            dst.pixelformat.rm = 0x1F;
            dst.pixelformat.gl = 5;
            dst.pixelformat.gs = 5;
            dst.pixelformat.gm = 0x1F;
            dst.pixelformat.bl = 5;
            dst.pixelformat.bs = 0;
            dst.pixelformat.bm = 0x1F;
            dst.pixelformat.al = 1;
            dst.pixelformat.as = 0xF;
            dst.pixelformat.am = 1;
            dst.pixelformat.rs = 0;
            dst.pixelformat.bs = 0xA;
            dst.pixelformat.gl = 5;
            dst.pixelformat.gm = 0x1F;
            dst.pitch = dst.width * dst.bitdepth;
            plConvertContext(&dst, &src);
            break;

        case 1:
            src.bitdepth = 3;
            src.pixelformat.rl = 8;
            src.pixelformat.rs = 0x10;
            src.pixelformat.rm = 0xFF;
            src.pixelformat.gl = 8;
            src.pixelformat.gs = 8;
            src.pixelformat.gm = 0xFF;
            src.pixelformat.bl = 8;
            src.pixelformat.bs = 0;
            src.pixelformat.bm = 0xFF;
            src.pixelformat.al = 0;
            src.pixelformat.as = 0;
            src.pixelformat.am = 0;
            src.pitch = src.width * src.bitdepth;
            dst.bitdepth = 3;
            dst.pixelformat.rl = 8;
            dst.pixelformat.rs = 0x10;
            dst.pixelformat.rm = 0xFF;
            dst.pixelformat.gl = 8;
            dst.pixelformat.gs = 8;
            dst.pixelformat.gm = 0xFF;
            dst.pixelformat.bl = 8;
            dst.pixelformat.bs = 0;
            dst.pixelformat.bm = 0xFF;
            dst.pixelformat.al = 0;
            dst.pixelformat.as = 0;
            dst.pixelformat.am = 0;
            dst.pixelformat.rs = 0;
            dst.pixelformat.bs = 0x10;
            dst.pitch = dst.width * dst.bitdepth;
            plConvertContext(&dst, &src);
            break;

        case 0:
            src.bitdepth = 4;
            src.pixelformat.rl = 8;
            src.pixelformat.rs = 0x10;
            src.pixelformat.rm = 0xFF;
            src.pixelformat.gl = 8;
            src.pixelformat.gs = 8;
            src.pixelformat.gm = 0xFF;
            src.pixelformat.bl = 8;
            src.pixelformat.bs = 0;
            src.pixelformat.bm = 0xFF;
            src.pixelformat.al = 8;
            src.pixelformat.as = 0x18;
            src.pixelformat.am = 0xFF;
            src.pitch = src.width * src.bitdepth;
            dst.bitdepth = 4;
            dst.pixelformat.rl = 8;
            dst.pixelformat.rs = 0x10;
            dst.pixelformat.rm = 0xFF;
            dst.pixelformat.gl = 8;
            dst.pixelformat.gs = 8;
            dst.pixelformat.gm = 0xFF;
            dst.pixelformat.bl = 8;
            dst.pixelformat.bs = 0;
            dst.pixelformat.bm = 0xFF;
            dst.pixelformat.al = 8;
            dst.pixelformat.as = 0x18;
            dst.pixelformat.am = 0xFF;
            dst.pixelformat.rs = 0;
            dst.pixelformat.bs = 0x10;
            dst.pitch = dst.width * dst.bitdepth;
            plConvertContext(&dst, &src);
            break;
        }

        break;

    case 1:
        if (lpflTexture->mem_handle != 0) {
            buff_ptr = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
            buff_ptr1 = (u8*)lpflTexture->lock_ptr;
            flMemcpy(buff_ptr, buff_ptr1, lpflTexture->size);
        } else {
            buff_ptr = (u8*)lpflTexture->lock_ptr;
        }

        break;

    case 2:
    case 3:
        break;
    }

    lpflTexture->desc &= ~2;

    return 1;
}

u32 flPS2GetTextureSize(u32 format, s32 dw, s32 dh, s32 bnum) {
    u32 tex_size;
    s32 lp0;

    tex_size = 0;

    for (lp0 = 0; lp0 < bnum; lp0++) {
        switch (format) {
        case 0:
        case 1:
            tex_size += dw * dh * 4;
            break;

        case 2:
        case 10:
            tex_size += dw * dh * 2;
            break;

        case 19:
            tex_size += dw * dh;
            break;

        case 20:
            tex_size += (dw * dh) >> 1;
            break;
        }

        dw >>= 1;
        dh >>= 1;
    }

    return tex_size;
}


s32 flInitialize(s32 /* unused */, s32 /* unused */){
    if (system_work_init() == 0) {
        return 0;
    }

    /* PS2-style z range used by flPS2ConvScreenFZ (native renderer depth) */
    flPs2State.ZBuffMax = (f32)65535;

    vram_particles = guGetStaticVramTexture(256, 256, GU_PSM_T8);

    flPS2SystemTmpBuffInit();
    flPADInitialize();

    return 1;
}

/* game z (1..-1 style) -> PS2-style depth for the native renderer */
f32 flPS2ConvScreenFZ(f32 z) {
    z -= 1.0f;
    z = z * -0.5f;
    z *= flPs2State.ZBuffMax;

    return z;
}

void flSetTexture(int th){
    SDLGameRenderer_SetTexture((unsigned int)th);
    int texture_handle = LO_16_BITS(th) - 1;
    FLTexture *flTex = &flTexture[texture_handle];
    int palette_handle = HI_16_BITS(th) - 1;
    FLTexture *flPal = &flPalette[palette_handle];
    u16 *pal = flPal->wkVram;

    void *texData = flTex->wkVram;

    if(texData == NULL)
        texData = flPS2GetSystemBuffAdrs(flTex->mem_handle);

    if(pal == NULL)
        pal = flPS2GetSystemBuffAdrs(flPal->mem_handle);

    if(currentPalette != palette_handle){
        sceGuClutMode(GU_PSM_5551, 0, 255, 0);
        sceGuClutLoad(flPal->size / 16, pal);
        currentPalette = palette_handle;
    }

    if(currentTexture != texData){
        sceGuTexMode(flTex->format, 0, 0, flTex->swizzeled ? GU_TRUE : GU_FALSE);
        sceGuTexImage(0, flTex->width, flTex->height, flTex->width, texData);
        currentTexture = texData;
    }
}

s32 flSetRenderState(enum _FLSETRENDERSTATE func, u32 value) {
    switch (func) {
    case FLRENDER_TEXSTAGE0:
    case FLRENDER_TEXSTAGE1:
    case FLRENDER_TEXSTAGE2:
    case FLRENDER_TEXSTAGE3:

        if (func == FLRENDER_TEXSTAGE0) {
            flSetTexture(value);
        }

        break;

    case FLRENDER_BACKCOLOR:
        setBackGroundColor(value | 0xFF000000);
        break;

    case FLRENDER_ALPHABLENDMODE: {
        extern void ctrGuSetBlendMode(unsigned int mode);
        ctrGuSetBlendMode((unsigned int)value);
        break;
    }

    case FLRENDER_BLENDOPE: {
        extern void ctrGuSetBlendOp(unsigned int op);
        ctrGuSetBlendOp((unsigned int)value);
        break;
    }

    default:
        break;
    }

    return 1;
}

static s32 system_work_init() {
    void* temp;

    flMemset(&flPs2State, 0, sizeof(FLPS2State));
    int temp_size = 0x01800000;
    //int temp_size = 0x01C00000;

    flLogOut("system_work_init: allocating %d KB arena\n", temp_size / 1024);
    temp = memalign(16, temp_size);

    if (temp == NULL) {
        flLogOut("system_work_init malloc FAILED\n");
        while(1);
        return 0;
    }
    flLogOut("system_work_init: arena ok\n");

    fmsInitialize(&flFMS, temp, temp_size, 0x16);
    const int system_memory_size = 0xA00000;
    //const int system_memory_size = 0x1000000;
    temp = flAllocMemoryS(system_memory_size);
    mflInit(temp, system_memory_size, 0x16);

    return 1;
}

s32 flPS2ConvertTextureFromContext(plContext* lpcontext, FLTexture* lpflTexture, u32 type, u8 mode) {
    u8 *dst_ptr;

    if(mode){
        lpflTexture->mem_handle = (u32)lpcontext->ptr;
        lpflTexture->wkVram = NULL;
        dst_ptr = flPS2GetSystemBuffAdrs(lpflTexture->mem_handle);
    }
    else{
        lpflTexture->mem_handle = 0;
        lpflTexture->wkVram = lpcontext->ptr;
        dst_ptr = lpcontext->ptr;
    }

    flLogOut("texconv %d %d %d %d %d\n", lpflTexture - flTexture, lpflTexture->width, lpflTexture->height, lpflTexture->format, mode);

    /* texture data is being (re)written — drop stale GPU-side conversions */
    {
        extern void ctrGuTexcacheInvalidate(const void *src);
        ctrGuTexcacheInvalidate(NULL);
    }

    u8 *base_ptr = dst_ptr;
    s32 tex_size;
    s32 dw = lpflTexture->width;
    s32 dh = lpflTexture->height;
    s32 lp0;

    u16 color;
    u16 *p_color_16;

    for (lp0 = 0; lp0 < lpflTexture->tex_num; lp0++) {
        switch (lpflTexture->format) {
        default:
            flLogOut("Not supported texture bit depth @flPS2ConvertTextureFromContext");
            break;

        case GU_PSM_T4:
            tex_size = (dw * dh) >> 1;
            break;
        case GU_PSM_T8:
            tex_size = dw * dh;
            break;

        case GU_PSM_5551:
            tex_size = (dw * dh) << 1;
            /* 3DS: keep data linear in PS2 channel order — the gu_draw
               converter handles channel order + PICA tiling itself. */
            lpflTexture->swizzeled = 0;
            break;

        case GU_PSM_4444:
            tex_size = (dw * dh) << 1;
            break;

        case GU_PSM_8888:
            tex_size = (dw * dh) << 2;
            break;
        }

        dst_ptr = &dst_ptr[tex_size];
        dw >>= 1;
        dh >>= 1;
        lpcontext++;
    }

    tex_size = dst_ptr - base_ptr;
    dw = lpflTexture->width;
    dh = lpflTexture->height;
    lp0 = lpflTexture - flTexture;

    /* 3DS: no PSP VRAM staging/swizzling — textures stay linear in their
       system-memory buffers; gu_draw converts to native PICA tiles. */
    (void)lp0;


    if(lpflTexture->mem_handle)
        return 1;

    // Flush cache once at load time — textures in main RAM need this
    // VRAM textures (wkVram != NULL) don't need it since VRAM is uncached
    if(!lpflTexture->vram_on_flag)
        sceKernelDcacheWritebackRange(base_ptr, dst_ptr - base_ptr);

    return 1;
}

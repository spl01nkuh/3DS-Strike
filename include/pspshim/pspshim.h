/* pspshim.h — PSP SDK compatibility layer for the 3DS port.
 *
 * The 3s-psp tree calls a small set of PSP kernel/GU APIs from otherwise
 * portable code. Rather than edit 40+ decompiled files, we satisfy those
 * includes/symbols here and implement them over libctru in src/ctr/pspshim.c.
 * Constants use authentic PSP values so behavior-carrying flags (vertex
 * formats, pixel formats, IO flags) keep their original semantics.
 */
#ifndef PSPSHIM_H_
#define PSPSHIM_H_

#include <stdint.h>
#include <stddef.h>

/* The real PSP SDK headers provided u8/u16/u32/... — code that includes only
 * <pspuser.h> relies on that, so pull in the project's types here. */
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sce base types ---- */
typedef int SceUID;
typedef unsigned int SceSize;
typedef int SceSSize;
typedef unsigned char SceUChar;
typedef unsigned int SceUInt;
typedef int SceMode;
typedef long long SceOff;

/* ---- IO (sceIo* over newlib, with PSP device-prefix path translation) ---- */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_NBLOCK 0x0004
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

#define PSP_SEEK_SET 0
#define PSP_SEEK_CUR 1
#define PSP_SEEK_END 2

SceUID sceIoOpen(const char *file, int flags, SceMode mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void *data, SceSize size);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
long sceIoLseek32(SceUID fd, long offset, int whence);

/* ---- Threads / sync (over libctru threads + LightSemaphore) ---- */
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority, int stackSize,
                             SceUInt attr, void *option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelDeleteThread(SceUID thid);
int sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout);
int sceKernelDelayThread(SceUInt delay_us);

SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, void *option);
int sceKernelDeleteSema(SceUID semaid);
int sceKernelSignalSema(SceUID semaid, int signal);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout);
int sceKernelPollSema(SceUID semaid, int signal);

void sceKernelDcacheWritebackRange(const void *p, unsigned int size);
void sceKernelDcacheWritebackInvalidateRange(const void *p, unsigned int size);

/* ---- Display / sync ---- */
int sceDisplayWaitVblankStart(void);
int sceGsSyncV(int mode); /* PS2 leftover in game code; no-op */

/* ---- Controller (only mlPAD.c touches these; real input is src/ctr/pad.c) ---- */
#define PSP_CTRL_MODE_DIGITAL 0
#define PSP_CTRL_MODE_ANALOG  1
typedef struct SceCtrlData {
    unsigned int TimeStamp;
    unsigned int Buttons;
    unsigned char Lx;
    unsigned char Ly;
    unsigned char Rsrv[6];
} SceCtrlData;
int sceCtrlSetSamplingCycle(int cycle);
int sceCtrlSetSamplingMode(int mode);
int sceCtrlReadBufferPositive(SceCtrlData *pad_data, int count);

/* ---- PS2 CD leftovers (GD3rd.c) ---- */
int sceCdGetDiskType(void);
int sceCdGetError(void);

/* ---- Audio (ADX/SPU output path; implemented over ndsp later) ---- */
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_FORMAT_MONO   0x10
#define PSP_AUDIO_NEXT_CHANNEL  (-1)
#define PSP_AUDIO_SAMPLE_MIN    64
#define PSP_AUDIO_SAMPLE_MAX    65472
#define PSP_AUDIO_SAMPLE_ALIGN(s) (((s) + 63) & ~63)
int sceAudioChReserve(int channel, int samplecount, int format);
int sceAudioChRelease(int channel);
int sceAudioOutputBlocking(int channel, int vol, void *buf);
#define PSP_AUDIO_VOLUME_MAX 0x8000
#define PSP_AUDIO_CHANNEL_MAX 8
#define PSP_VOLUME_MAX 0x8000

long long sceIoLseek(SceUID fd, long long offset, int whence);

/* pspaudiolib-compatible callback layer */
typedef void (*pspAudioCallback_t)(void *buf, unsigned int reqn, void *pdata);
int pspAudioInit(void);
void pspAudioEnd(void);
void pspAudioSetChannelCallback(int channel, pspAudioCallback_t callback, void *pdata);
void pspAudioSetVolume(int channel, int left, int right);

/* ---- RTC ---- */
typedef unsigned long long u64_shim;
int sceRtcGetCurrentTick(unsigned long long *tick);
unsigned int sceRtcGetTickResolution(void);

/* ---- Utils (Mt19937 — math.c was rewritten, kept for safety) ---- */
typedef struct {
    unsigned int count;
    unsigned int state[624];
} SceKernelUtilsMt19937Context;
int sceKernelUtilsMt19937Init(SceKernelUtilsMt19937Context *ctx, unsigned int seed);
unsigned int sceKernelUtilsMt19937UInt(SceKernelUtilsMt19937Context *ctx);
long sceKernelLibcTime(long *t);

/* ---- Debug screen (pspdebug.h) ---- */
void pspDebugScreenInit(void);
void pspDebugScreenPrintf(const char *fmt, ...);
void pspDebugScreenSetTextColor(unsigned int color);
void pspDebugScreenSetXY(int x, int y);

/* ---- GU VRAM helpers ---- */
void *guGetStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm);
void *guGetStaticVramTexture(unsigned int width, unsigned int height, unsigned int psm);

/* ---- Power ---- */
int scePowerSetClockFrequency(int cpufreq, int ramfreq, int busfreq);

/* =====================  GU (Graphics Unit)  ===================== */
/* Kept only for the few residual calls in fl.c / PPGFile.c; the real
 * renderer lives in src/common/graphics.c + sprites.c (citro3d). */

/* Primitive types */
#define GU_POINTS         0
#define GU_LINES          1
#define GU_LINE_STRIP     2
#define GU_TRIANGLES      3
#define GU_TRIANGLE_STRIP 4
#define GU_TRIANGLE_FAN   5
#define GU_SPRITES        6

/* Vertex declaration flags (authentic bit layout) */
#define GU_TEXTURE_SHIFT  0
#define GU_TEXTURE_8BIT   (1 << 0)
#define GU_TEXTURE_16BIT  (2 << 0)
#define GU_TEXTURE_32BITF (3 << 0)
#define GU_COLOR_SHIFT    2
#define GU_COLOR_5650     (4 << 2)
#define GU_COLOR_5551     (5 << 2)
#define GU_COLOR_4444     (6 << 2)
#define GU_COLOR_8888     (7 << 2)
#define GU_VERTEX_SHIFT   7
#define GU_VERTEX_8BIT    (1 << 7)
#define GU_VERTEX_16BIT   (2 << 7)
#define GU_VERTEX_32BITF  (3 << 7)
#define GU_TRANSFORM_SHIFT 23
#define GU_TRANSFORM_3D   (0 << 23)
#define GU_TRANSFORM_2D   (1 << 23)

/* Pixel storage modes */
#define GU_PSM_5650 0
#define GU_PSM_5551 1
#define GU_PSM_4444 2
#define GU_PSM_8888 3
#define GU_PSM_T4   4
#define GU_PSM_T8   5
#define GU_PSM_T16  6
#define GU_PSM_T32  7

/* States */
#define GU_ALPHA_TEST   0
#define GU_DEPTH_TEST   1
#define GU_SCISSOR_TEST 2
#define GU_BLEND        4
#define GU_CULL_FACE    5
#define GU_DITHER       6
#define GU_CLIP_PLANES  8
#define GU_TEXTURE_2D   9

/* Texture wrap/filter/effects */
#define GU_REPEAT  0
#define GU_CLAMP   1
#define GU_NEAREST 0
#define GU_LINEAR  1
#define GU_TFX_MODULATE 0
#define GU_TFX_DECAL    1
#define GU_TFX_BLEND    2
#define GU_TFX_REPLACE  3
#define GU_TFX_ADD      4
#define GU_TCC_RGB  0
#define GU_TCC_RGBA 1

#define GU_TRUE  1
#define GU_FALSE 0

void sceGuDrawArray(int prim, int vtype, int count, const void *indices, const void *vertices);
void *sceGuGetMemory(int size);
void sceGuTexImage(int mipmap, int width, int height, int tbw, const void *tbp);
void sceGuTexMode(int tpsm, int maxmips, int a2, int swizzle);
void sceGuTexWrap(int u, int v);
void sceGuTexFilter(int min, int mag);
void sceGuTexFunc(int tfx, int tcc);
void sceGuClutMode(unsigned int cpsm, unsigned int shift, unsigned int mask, unsigned int a3);
void sceGuClutLoad(int num_blocks, const void *cbp);
void sceGuEnable(int state);
void sceGuDisable(int state);

#ifdef __cplusplus
}
#endif

#endif /* PSPSHIM_H_ */

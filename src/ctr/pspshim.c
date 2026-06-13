/* pspshim.c — PSP SDK shim implementations over libctru/newlib.
 *
 * IO: PSP device prefixes are rewritten to the game data dir on SD.
 * Threads/semaphores: libctru Thread + LightSemaphore, table-indexed
 * so SceUID stays a small int like the originals expect.
 * GU residuals: no-ops; the citro3d renderer replaces that path.
 */
#include <pspshim.h>

#include <3ds.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ------------------------------------------------------------------ IO -- */

#define SHIM_DATA_DIR "sdmc:/3ds/sf3"

/* Rewrite PSP/PS2-era device paths to our SD data dir. */
static void shim_translate_path(const char *in, char *out, size_t outsz) {
    const char *prefixes[] = { "ms0:/", "disc0:/", "host0:/", "umd0:/", "cdrom0:\\", "cdrom0:/" };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncasecmp(in, prefixes[i], n) == 0) {
            snprintf(out, outsz, SHIM_DATA_DIR "/%s", in + n);
            return;
        }
    }
    if (in[0] == '/') {
        snprintf(out, outsz, SHIM_DATA_DIR "%s", in);
    } else if (strchr(in, ':') == NULL) {
        /* bare relative path (e.g. "resources/SF33RD.AFS") */
        snprintf(out, outsz, SHIM_DATA_DIR "/%s", in);
    } else {
        /* already has a scheme newlib understands (sdmc:/, romfs:/) */
        snprintf(out, outsz, "%s", in);
    }
}

SceUID sceIoOpen(const char *file, int flags, SceMode mode) {
    (void)mode;
    char path[512];
    shim_translate_path(file, path, sizeof(path));

    int oflags = 0;
    switch (flags & 0x3) {
    case PSP_O_RDONLY: oflags = O_RDONLY; break;
    case PSP_O_WRONLY: oflags = O_WRONLY; break;
    case PSP_O_RDWR:   oflags = O_RDWR;   break;
    }
    if (flags & PSP_O_CREAT)  oflags |= O_CREAT;
    if (flags & PSP_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & PSP_O_APPEND) oflags |= O_APPEND;

    int fd = open(path, oflags, 0644);
    return (fd < 0) ? -1 : fd;
}

int sceIoClose(SceUID fd) { return (fd >= 0) ? close(fd) : -1; }

int sceIoRead(SceUID fd, void *data, SceSize size) {
    int n = read(fd, data, size);
    return (n < 0) ? -1 : n;
}

int sceIoWrite(SceUID fd, const void *data, SceSize size) {
    int n = write(fd, data, size);
    return (n < 0) ? -1 : n;
}

long sceIoLseek32(SceUID fd, long offset, int whence) {
    return (long)lseek(fd, offset, whence); /* PSP whence values match newlib (0/1/2) */
}

long long sceIoLseek(SceUID fd, long long offset, int whence) {
    return (long long)lseek(fd, (off_t)offset, whence);
}

/* ------------------------------------------------------- threads/sync -- */

#define SHIM_MAX_THREADS 8
#define SHIM_MAX_SEMAS   8
#define SHIM_THREAD_BASE 0x1000
#define SHIM_SEMA_BASE   0x2000

typedef struct {
    Thread thread;
    SceKernelThreadEntry entry;
    int stack_size;
    int in_use;
    int started;
    /* arg copy for start */
    void *argp;
    SceSize arglen;
} ShimThread;

typedef struct {
    LightSemaphore sema;
    int in_use;
} ShimSema;

static ShimThread s_threads[SHIM_MAX_THREADS];
static ShimSema s_semas[SHIM_MAX_SEMAS];

static void shim_thread_tramp(void *arg) {
    ShimThread *t = (ShimThread *)arg;
    t->entry(t->arglen, t->argp);
}

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority, int stackSize,
                             SceUInt attr, void *option) {
    (void)name; (void)initPriority; (void)attr; (void)option;
    for (int i = 0; i < SHIM_MAX_THREADS; i++) {
        if (!s_threads[i].in_use) {
            s_threads[i].in_use = 1;
            s_threads[i].started = 0;
            s_threads[i].entry = entry;
            s_threads[i].stack_size = (stackSize < 0x2000) ? 0x2000 : stackSize;
            return SHIM_THREAD_BASE + i;
        }
    }
    return -1;
}

int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp) {
    int i = thid - SHIM_THREAD_BASE;
    if (i < 0 || i >= SHIM_MAX_THREADS || !s_threads[i].in_use)
        return -1;
    ShimThread *t = &s_threads[i];
    t->arglen = arglen;
    t->argp = argp;
    /* priority 0x30 ≈ below main (0x30 is typical app priority on 3DS);
       run on default core, fall back handled by libctru */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    t->thread = threadCreate(shim_thread_tramp, t, t->stack_size, prio + 1, -2, false);
    if (!t->thread) {
        return -1;
    }
    t->started = 1;
    return 0;
}

int sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout) {
    (void)timeout;
    int i = thid - SHIM_THREAD_BASE;
    if (i < 0 || i >= SHIM_MAX_THREADS || !s_threads[i].in_use || !s_threads[i].started)
        return -1;
    threadJoin(s_threads[i].thread, U64_MAX);
    return 0;
}

int sceKernelDeleteThread(SceUID thid) {
    int i = thid - SHIM_THREAD_BASE;
    if (i < 0 || i >= SHIM_MAX_THREADS || !s_threads[i].in_use)
        return -1;
    if (s_threads[i].started && s_threads[i].thread) {
        threadFree(s_threads[i].thread);
        s_threads[i].thread = NULL;
    }
    s_threads[i].in_use = 0;
    return 0;
}

int sceKernelDelayThread(SceUInt delay_us) {
    svcSleepThread((s64)delay_us * 1000);
    return 0;
}

SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, void *option) {
    (void)name; (void)attr; (void)option;
    for (int i = 0; i < SHIM_MAX_SEMAS; i++) {
        if (!s_semas[i].in_use) {
            s_semas[i].in_use = 1;
            LightSemaphore_Init(&s_semas[i].sema, initVal, maxVal);
            return SHIM_SEMA_BASE + i;
        }
    }
    return -1;
}

int sceKernelDeleteSema(SceUID semaid) {
    int i = semaid - SHIM_SEMA_BASE;
    if (i < 0 || i >= SHIM_MAX_SEMAS) return -1;
    s_semas[i].in_use = 0;
    return 0;
}

int sceKernelSignalSema(SceUID semaid, int signal) {
    int i = semaid - SHIM_SEMA_BASE;
    if (i < 0 || i >= SHIM_MAX_SEMAS || !s_semas[i].in_use) return -1;
    LightSemaphore_Release(&s_semas[i].sema, signal);
    return 0;
}

int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout) {
    (void)timeout;
    int i = semaid - SHIM_SEMA_BASE;
    if (i < 0 || i >= SHIM_MAX_SEMAS || !s_semas[i].in_use) return -1;
    for (int n = 0; n < signal; n++)
        LightSemaphore_Acquire(&s_semas[i].sema, 1);
    return 0;
}

int sceKernelPollSema(SceUID semaid, int signal) {
    int i = semaid - SHIM_SEMA_BASE;
    if (i < 0 || i >= SHIM_MAX_SEMAS || !s_semas[i].in_use) return -1;
    return LightSemaphore_TryAcquire(&s_semas[i].sema, signal) == 0 ? 0 : -1;
}

void sceKernelDcacheWritebackRange(const void *p, unsigned int size) { (void)p; (void)size; }
void sceKernelDcacheWritebackInvalidateRange(const void *p, unsigned int size) { (void)p; (void)size; }

/* ------------------------------------------------------------ display -- */

int sceDisplayWaitVblankStart(void) {
    gspWaitForVBlank();
    return 0;
}

int sceGsSyncV(int mode) { (void)mode; return 0; }

/* --------------------------------------------------------- controller -- */

int sceCtrlSetSamplingCycle(int cycle) { (void)cycle; return 0; }
int sceCtrlSetSamplingMode(int mode) { (void)mode; return 0; }
int sceCtrlReadBufferPositive(SceCtrlData *pad_data, int count) {
    if (pad_data && count > 0) memset(pad_data, 0, sizeof(*pad_data));
    return count;
}

/* ----------------------------------------------------------- cd (ps2) -- */

int sceCdGetDiskType(void) { return 0x14; /* SCECdPS2DVD */ }
int sceCdGetError(void) { return 0; /* SCECdErNO */ }

/* --------------------------------------------------------------- rtc -- */

int sceRtcGetCurrentTick(unsigned long long *tick) {
    if (tick) *tick = svcGetSystemTick() / CPU_TICKS_PER_USEC;
    return 0;
}
unsigned int sceRtcGetTickResolution(void) { return 1000000; }

/* ------------------------------------------------------------- utils -- */

int sceKernelUtilsMt19937Init(SceKernelUtilsMt19937Context *ctx, unsigned int seed) {
    ctx->count = 0;
    ctx->state[0] = seed;
    for (unsigned i = 1; i < 624; i++)
        ctx->state[i] = 1812433253u * (ctx->state[i - 1] ^ (ctx->state[i - 1] >> 30)) + i;
    return 0;
}

unsigned int sceKernelUtilsMt19937UInt(SceKernelUtilsMt19937Context *ctx) {
    /* standard MT19937 */
    if (ctx->count >= 624) ctx->count = 0;
    if (ctx->count == 0) {
        for (unsigned i = 0; i < 624; i++) {
            unsigned int y = (ctx->state[i] & 0x80000000u) | (ctx->state[(i + 1) % 624] & 0x7fffffffu);
            ctx->state[i] = ctx->state[(i + 397) % 624] ^ (y >> 1);
            if (y & 1) ctx->state[i] ^= 2567483615u;
        }
    }
    unsigned int y = ctx->state[ctx->count++];
    y ^= y >> 11;
    y ^= (y << 7) & 2636928640u;
    y ^= (y << 15) & 4022730752u;
    y ^= y >> 18;
    return y;
}

long sceKernelLibcTime(long *t) {
    long now = (long)time(NULL);
    if (t) *t = now;
    return now;
}

/* ------------------------------------------------------------- debug -- */

void pspDebugScreenInit(void) {}
void pspDebugScreenPrintf(const char *fmt, ...) { (void)fmt; }
void pspDebugScreenSetTextColor(unsigned int color) { (void)color; }
void pspDebugScreenSetXY(int x, int y) { (void)x; (void)y; }

/* PSP VRAM allocators — plain heap on 3DS; pixel size depends on format. */
static size_t gu_psm_bytes(unsigned int psm) {
    switch (psm) {
    case GU_PSM_T4: return 0; /* caller divides; handled below */
    case GU_PSM_T8: return 1;
    case GU_PSM_5650:
    case GU_PSM_5551:
    case GU_PSM_4444:
    case GU_PSM_T16: return 2;
    default: return 4; /* 8888 / T32 */
    }
}

void *guGetStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm) {
    size_t bpp = gu_psm_bytes(psm);
    size_t size = (psm == GU_PSM_T4) ? ((size_t)width * height / 2) : ((size_t)width * height * bpp);
    return calloc(1, size ? size : 1);
}

void *guGetStaticVramTexture(unsigned int width, unsigned int height, unsigned int psm) {
    return guGetStaticVramBuffer(width, height, psm);
}

/* ------------------------------------------------------------- power -- */

int scePowerSetClockFrequency(int cpufreq, int ramfreq, int busfreq) {
    (void)cpufreq; (void)ramfreq; (void)busfreq;
    return 0;
}

/* audio: implemented in src/ctr/audio.c (ndsp backend) */

/* -------------------------------------------------------- GU residual -- */

static unsigned char s_gu_scratch[64 * 1024];
static size_t s_gu_scratch_off = 0;

void *sceGuGetMemory(int size) {
    /* per-frame scratch ring; reset each frame from graphics.c */
    if (s_gu_scratch_off + (size_t)size > sizeof(s_gu_scratch))
        s_gu_scratch_off = 0;
    void *p = &s_gu_scratch[s_gu_scratch_off];
    s_gu_scratch_off += (size + 15) & ~15;
    return p;
}

void pspshim_gu_frame_reset(void) { s_gu_scratch_off = 0; }

/* Remaining sceGu* entry points (TexImage/ClutLoad/DrawArray/...) are
 * implemented by the renderer in src/ctr/gu_draw.c. */

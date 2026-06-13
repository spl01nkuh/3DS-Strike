/* ctr/audio.c — ndsp audio backend implementing the pspaudiolib contract.
 *
 * The game's sound code is pull-based, exactly like pspaudiolib: it
 * registers per-channel callbacks that fill `reqn` interleaved stereo
 * s16 frames at 44100 Hz.
 *   channel 0 = SPU2 emulator (sound effects)  — src/port/sound/spu.c
 *   channel 1 = CRI ADX decoder (BGM)          — src/psp/adx.c
 *
 * On PSP these were two independent hardware voices the SPU mixed. The
 * 3DS DSP also mixes, but driving two ndsp channels with independent pull
 * cadences is awkward — and the SPU's internal frame timer wants a single
 * steady reqn stream. So we run ONE ndsp stereo channel fed by a mixing
 * thread that pulls both callbacks for each output buffer and sums them.
 *
 * On real hardware ndspInit needs a DSP firmware dump (dspfirm.cdc) which
 * Azahar HLEs away; if init fails we degrade gracefully to silence so the
 * game still runs.
 */
#include <pspshim.h>

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void debug_print(const char *fmt, ...);

#define AUDIO_RATE     44100
#define AUDIO_CHANNEL  0       /* ndsp channel index we own */
#define FRAMES         1024    /* stereo frames per output buffer (~23ms) */
#define NUM_WBUF       3       /* triple-buffered */
#define NUM_LOGICAL    2       /* pspaudiolib channels 0 and 1 */

typedef struct {
    pspAudioCallback_t cb;
    void *pdata;
    int vol_l, vol_r; /* 0..PSP_VOLUME_MAX (0x8000) */
} LogicalChannel;

static LogicalChannel s_chan[NUM_LOGICAL];

static int s_inited = 0;
static volatile int s_running = 0;

static ndspWaveBuf s_wbuf[NUM_WBUF];
static s16 *s_buf_pcm;       /* NUM_WBUF * FRAMES * 2, linear (DSP-visible) */
static s16 *s_mix0;          /* scratch for channel 0 callback */
static s16 *s_mix1;          /* scratch for channel 1 callback */

static Thread s_thread;
static LightEvent s_event;   /* signalled by the ndsp frame callback */

static inline s16 clamp16(s32 x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (s16)x;
}

/* Fill one wave buffer: pull both logical callbacks, apply volume, sum. */
static void render_buffer(s16 *out) {
    int have0 = s_chan[0].cb != NULL;
    int have1 = s_chan[1].cb != NULL;

    if (have0)
        s_chan[0].cb(s_mix0, FRAMES, s_chan[0].pdata);
    if (have1)
        s_chan[1].cb(s_mix1, FRAMES, s_chan[1].pdata);

    if (!have0 && !have1) {
        memset(out, 0, FRAMES * 2 * sizeof(s16));
        return;
    }

    /* volumes as Q15 (0x8000 = unity) */
    s32 l0 = have0 ? s_chan[0].vol_l : 0, r0 = have0 ? s_chan[0].vol_r : 0;
    s32 l1 = have1 ? s_chan[1].vol_l : 0, r1 = have1 ? s_chan[1].vol_r : 0;

    for (int i = 0; i < FRAMES; i++) {
        s32 l = 0, r = 0;
        if (have0) {
            l += (s_mix0[i * 2]     * l0) >> 15;
            r += (s_mix0[i * 2 + 1] * r0) >> 15;
        }
        if (have1) {
            l += (s_mix1[i * 2]     * l1) >> 15;
            r += (s_mix1[i * 2 + 1] * r1) >> 15;
        }
        out[i * 2]     = clamp16(l);
        out[i * 2 + 1] = clamp16(r);
    }
}

static void queue_buffer(int idx) {
    s16 *dst = s_buf_pcm + (size_t)idx * FRAMES * 2;
    render_buffer(dst);
    DSP_FlushDataCache(dst, FRAMES * 2 * sizeof(s16));

    memset(&s_wbuf[idx], 0, sizeof(s_wbuf[idx]));
    s_wbuf[idx].data_pcm16 = dst;
    s_wbuf[idx].nsamples = FRAMES;
    ndspChnWaveBufAdd(AUDIO_CHANNEL, &s_wbuf[idx]);
}

/* ndsp fires this each DSP frame; just wake the mixer. */
static void ndsp_frame_cb(void *arg) {
    (void)arg;
    LightEvent_Signal(&s_event);
}

static void audio_thread(void *arg) {
    (void)arg;
    while (s_running) {
        LightEvent_Wait(&s_event);
        for (int i = 0; i < NUM_WBUF; i++) {
            if (s_wbuf[i].status == NDSP_WBUF_DONE || s_wbuf[i].status == NDSP_WBUF_FREE)
                queue_buffer(i);
        }
    }
}

/* libctru's ndspInit normally locates the DSP firmware via the system,
 * which fails under Citra/Azahar HLE (and on any console where it isn't
 * exposed). Loading dspfirm.cdc ourselves and handing it to libctru via
 * ndspUseComponent() satisfies that step on both emulator and hardware. */
static void *s_dspfirm;
static int load_dsp_firmware(void) {
    static const char *paths[] = {
        "sdmc:/3ds/sf3/dspfirm.cdc",
        "sdmc:/3ds/dspfirm.cdc",
        "romfs:/dspfirm.cdc",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }
        s_dspfirm = malloc(sz);
        if (!s_dspfirm) { fclose(f); return 0; }
        size_t rd = fread(s_dspfirm, 1, sz, f);
        fclose(f);
        if (rd != (size_t)sz) { free(s_dspfirm); s_dspfirm = NULL; continue; }
        ndspUseComponent(s_dspfirm, sz, 0xFFFF, 0xFFFF);
        debug_print("audio: loaded DSP firmware %s (%ld bytes)", paths[i], sz);
        return 1;
    }
    debug_print("audio: dspfirm.cdc not found on SD — ndsp may fail");
    return 0;
}

int pspAudioInit(void) {
    if (s_inited)
        return 0;

    for (int i = 0; i < NUM_LOGICAL; i++) {
        s_chan[i].cb = NULL;
        s_chan[i].pdata = NULL;
        s_chan[i].vol_l = PSP_VOLUME_MAX;
        s_chan[i].vol_r = PSP_VOLUME_MAX;
    }

    load_dsp_firmware();
    Result ndsp_rc = ndspInit();
    if (R_FAILED(ndsp_rc)) {
        /* No DSP firmware (real HW without dump) — run silent. */
        debug_print("audio: ndspInit failed rc=0x%08lX (desc=%ld mod=%ld lvl=%ld) — silent",
                    (unsigned long)ndsp_rc, (long)R_DESCRIPTION(ndsp_rc),
                    (long)R_MODULE(ndsp_rc), (long)R_LEVEL(ndsp_rc));
        s_inited = 1; /* still mark inited so callbacks register harmlessly */
        return 0;
    }

    s_buf_pcm = (s16 *)linearAlloc(NUM_WBUF * FRAMES * 2 * sizeof(s16));
    s_mix0 = (s16 *)malloc(FRAMES * 2 * sizeof(s16));
    s_mix1 = (s16 *)malloc(FRAMES * 2 * sizeof(s16));
    if (!s_buf_pcm || !s_mix0 || !s_mix1) {
        debug_print("audio: buffer alloc failed");
        ndspExit();
        s_inited = 1;
        return 0;
    }
    memset(s_buf_pcm, 0, NUM_WBUF * FRAMES * 2 * sizeof(s16));

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHANNEL, (float)AUDIO_RATE);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f; /* front left  */
    mix[1] = 1.0f; /* front right */
    ndspChnSetMix(AUDIO_CHANNEL, mix);

    LightEvent_Init(&s_event, RESET_ONESHOT);
    ndspSetCallback(ndsp_frame_cb, NULL);

    s_running = 1;

    /* Prime all buffers so playback starts immediately. */
    for (int i = 0; i < NUM_WBUF; i++)
        queue_buffer(i);

    /* Pin the mixer to the system core (1) so the SPU2 tick + ADX decode
     * run parallel to the render thread on core 0 instead of stealing its
     * time — recovers the ~2fps the audio work was costing. Falls back to
     * the default core if core 1 is unavailable. Priority just above main. */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_thread = threadCreate(audio_thread, NULL, 16 * 1024, prio - 1, 1, false);
    if (!s_thread) {
        debug_print("audio: core-1 thread create failed; retrying default core");
        s_thread = threadCreate(audio_thread, NULL, 16 * 1024, prio - 1, -1, false);
    }
    if (!s_thread)
        debug_print("audio: thread create failed (DSP will underrun)");

    s_inited = 1;
    debug_print("audio: ndsp backend up (%d Hz, %d-frame buffers)", AUDIO_RATE, FRAMES);
    return 0;
}

void pspAudioEnd(void) {
    if (!s_inited)
        return;
    s_running = 0;
    LightEvent_Signal(&s_event);
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    ndspExit();
    if (s_buf_pcm) { linearFree(s_buf_pcm); s_buf_pcm = NULL; }
    free(s_mix0); s_mix0 = NULL;
    free(s_mix1); s_mix1 = NULL;
    s_inited = 0;
}

void pspAudioSetChannelCallback(int channel, pspAudioCallback_t callback, void *pdata) {
    if (channel < 0 || channel >= NUM_LOGICAL)
        return;
    /* publish pdata before cb so the audio thread never sees a cb with a
     * stale/!matching pdata */
    s_chan[channel].pdata = pdata;
    __dmb();
    s_chan[channel].cb = callback;
}

void pspAudioSetVolume(int channel, int left, int right) {
    if (channel < 0 || channel >= NUM_LOGICAL)
        return;
    if (left < 0) left = 0; if (left > PSP_VOLUME_MAX) left = PSP_VOLUME_MAX;
    if (right < 0) right = 0; if (right > PSP_VOLUME_MAX) right = PSP_VOLUME_MAX;
    s_chan[channel].vol_l = left;
    s_chan[channel].vol_r = right;
}

/* ---- direct-output path (adx.c top comment references it; the live path
 * is the callback above, but keep these correct in case a code path calls
 * them) ---------------------------------------------------------------- */

int sceAudioChReserve(int channel, int samplecount, int format) {
    (void)samplecount; (void)format;
    return (channel == PSP_AUDIO_NEXT_CHANNEL) ? 0 : channel;
}
int sceAudioChRelease(int channel) { (void)channel; return 0; }
int sceAudioOutputBlocking(int channel, int vol, void *buf) {
    (void)channel; (void)vol; (void)buf;
    /* Not used by the live callback path. Pace like real output would. */
    svcSleepThread(20 * 1000 * 1000LL);
    return 0;
}

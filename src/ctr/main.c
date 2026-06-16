/* ctr/main.c — 3DS entry point. Replaces psp/pspmain.c.
 *
 * Same shape as the PSP version: init graphics + audio, hand control to
 * AcrMain() (the game). PSP power/suspend callbacks become apt hooks.
 * PSP CPU-clock requests map to nothing on Old 3DS; on New 3DS we simply
 * run at 804MHz the whole time via osSetSpeedupEnable.
 */
#include <3ds.h>

#include "common/audio.h"
#include "common/graphics.h"
#include "psp/adx.h"
#include "psp/afs.h"

#include "Game/main.h"

/* The whole game engine runs on the main thread (main -> AcrMain -> the game
 * loop and everything it calls). libctru's default 3dsx stack is only 32 KiB,
 * far too small for the decompiled engine's call depth and large stack locals.
 * On real hardware that overflows and faults at boot; Azahar's looser memory
 * map lets the overflow scribble into adjacent memory and survive — the exact
 * "runs in the emulator, crashes on a real 3DS at startup" signature. 1 MiB is
 * negligible against the 64/124 MiB app heap and leaves generous headroom. */
unsigned int __stacksize__ = 1024 * 1024;

/* global variables (game + backends reference these) */
volatile int g_request_pause = 0;
extern int RTT_Enabled;

extern void adxSuspend(void);
extern void adxResume(void);

volatile int clock_mode = CLOCK_300;
volatile int clock_mode_temp = -1;

void updateClock(void) {
    /* PSP changed CPU frequency per scene; the 3DS runs at a fixed clock.
       New 3DS speedup is enabled once in main. */
    clock_mode_temp = clock_mode;
}

void setClock(int clockMode) {
    clock_mode = clockMode;
}

void forceClock(int clockMode) {
    clock_mode_temp = -1;
    setClock(clockMode);
}

static void apt_hook_cb(APT_HookType hook, void *param) {
    (void)param;
    switch (hook) {
    case APTHOOK_ONSUSPEND:
    case APTHOOK_ONSLEEP:
        g_request_pause = 1;
        adxSuspend();
        break;
    case APTHOOK_ONRESTORE:
    case APTHOOK_ONWAKEUP:
        g_request_pause = 1;
        adxResume();
        break;
    default:
        break;
    }
}

static aptHookCookie s_apt_cookie;

int main(void) {
    /* New 3DS: 804MHz + L2 cache. No-op on Old 3DS. */
    osSetSpeedupEnable(true);

    /* Let the app spawn worker threads on the system core (core 1). Without
     * this, threadCreate on core 1 fails on real hardware and the audio mixer
     * (SPU2 + ADX) falls back onto the render core (0), stealing frame time.
     * 30% of the syscore is plenty for the periodic mix bursts and leaves the
     * OS its services. No-op-safe if it fails. */
    APT_SetAppCpuTimeLimit(30);

    svcOutputDebugString("[SF3] main: start", sizeof("[SF3] main: start") - 1);

    /* Graphics (citro3d) — implemented in common/graphics.c */
    initGu();
    svcOutputDebugString("[SF3] main: initGu done", sizeof("[SF3] main: initGu done") - 1);

    aptHook(&s_apt_cookie, apt_hook_cb, NULL);

    /* Audio backend init is owned by the sound path (SPU/ADX) for now. */
    pspAudioInit();

    svcOutputDebugString("[SF3] main: entering AcrMain", sizeof("[SF3] main: entering AcrMain") - 1);
    AcrMain();
    svcOutputDebugString("[SF3] main: AcrMain returned", sizeof("[SF3] main: AcrMain returned") - 1);

    endGu();
    return 0;
}

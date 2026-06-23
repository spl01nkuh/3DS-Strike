/* ctr/pad.c — input backend. Replaces psp/pspPAD.c (same API + bit mapping).
 *
 * Game-facing bit layout (tp->sw), inherited from the PSP/PS2 port:
 *   0x0001 up   0x0002 down  0x0004 left  0x0008 right
 *   0x0100 LP(square) 0x0200 MP(triangle) 0x0400 HP(L)
 *   0x0010 LK(cross)  0x0020 MK(circle)   0x0040 HK(R)
 *   0x8000 start      0x4000 select(coin)
 *
 * 3DS mapping (PlayStation face button -> 3DS face button, Nintendo-style so
 * A=confirm/B=cancel; positions match a PS pad rotated to the Nintendo layout):
 *   Square(LP)=X  Triangle(MP)=Y  HP=L
 *   Cross(LK)=A   Circle(MK)=B    HK=R
 *   d-pad + circle pad = stick.
 */
#include <3ds.h>
#include <string.h>

#include "psp/pspPAD.h"

u8 playAsP2 = 0;
extern volatile int g_request_pause;

TARPAD tarpad_root[2];

s32 tarPADInit(void) {
    memset(tarpad_root, 0, sizeof(tarpad_root));

    for (int i = 0; i < 2; i++) {
        tarpad_root[i].kind = 1 - i; /* pad 0 connected, pad 1 disconnected */
        tarpad_root[i].anstate = 0x60;
        tarpad_root[i].state = i;
        tarpad_root[i].conn.port = i;
        tarpad_root[i].conn.slot = 0;
    }

    return 1;
}

void tarPADDestroy(void) {}

void tarPADRead(void) {
    hidScanInput();
    u32 held = hidKeysHeld();

    TARPAD *tp;

    if (playAsP2) {
        tp = &tarpad_root[1];
        tarpad_root[0].kind = 0;
        tarpad_root[1].kind = 1;
    } else {
        tp = &tarpad_root[0];
        tarpad_root[0].kind = 1;
        tarpad_root[1].kind = 0;
    }

    u32 sw = 0;

    circlePosition cp;
    hidCircleRead(&cp);

    if ((held & KEY_DUP) || cp.dy > 40) sw |= 0x0001;
    if ((held & KEY_DDOWN) || cp.dy < -40) sw |= 0x0002;
    if ((held & KEY_DLEFT) || cp.dx < -40) sw |= 0x0004;
    if ((held & KEY_DRIGHT) || cp.dx > 40) sw |= 0x0008;

    if (held & KEY_X) sw |= 0x0100; /* LP = Square */
    if (held & KEY_Y) sw |= 0x0200; /* MP = Triangle */
    if (held & KEY_L) sw |= 0x0400; /* HP */
    if (held & KEY_A) sw |= 0x0010; /* LK = Cross  (confirm) */
    if (held & KEY_B) sw |= 0x0020; /* MK = Circle (cancel) */
    if (held & KEY_R) sw |= 0x0040; /* HK */

    if (held & KEY_START) sw |= 0x8000;
    if (held & KEY_SELECT) sw |= 0x4000;

    /* New 3DS extras: ZL/ZR drive the game's two otherwise-unreachable config
     * slots (SWK_LEFT_SHOULDER / SWK_LEFT_TRIGGER -> Shot[3] / Shot[7]) so they
     * show up as fully remappable buttons in BUTTON CONFIG. They default to
     * 3P / 3K (see Setup_IO_ConvDataDefault in SYS_sub.c). */
    if (held & KEY_ZL) sw |= 0x0080; /* SWK_LEFT_SHOULDER -> Shot[3] (default 3P) */
    if (held & KEY_ZR) sw |= 0x0800; /* SWK_LEFT_TRIGGER  -> Shot[7] (default 3K) */

    tp->sw = sw;

    tp->stick[1].x = 0;
    tp->stick[1].y = 0;
}

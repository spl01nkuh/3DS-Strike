/* ctr/save.c — save-data backend. Replaces psp/savesub.c.
 *
 * First-boot version: report "no save / completed" so the game proceeds
 * without persisting. Real SD-card persistence lands with the polish pass
 * (the game state machine drives SaveInit/SaveMove around menu flows).
 */
#include "types.h"

void SaveInit(s32 file_type, s32 save_mode) {
    (void)file_type;
    (void)save_mode;
}

s32 SaveMove(void) {
    /* 0 = operation finished/idle in the PSP flow */
    return 0;
}

void SaveMsg(u16 x, u16 y) {
    (void)x;
    (void)y;
}

/* Game/menu.c renders the unlock-progress page through the save module. */
void displayGameProgress(void) {
}

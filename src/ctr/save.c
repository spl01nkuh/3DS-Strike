/* ctr/save.c — SD-card save persistence. Replaces psp/savesub.c.
 *
 * Settings, button config, sound/screen options, rankings, unlock progress and
 * player colors persist to sdmc:/3ds/sf3/SETTINGS.BIN. The game drives
 * SaveInit(file_type, save_mode); even save_mode = load, odd = save (see the
 * SAVEMODE_* enum in the PSP backend). File I/O is fast on the SD card, so we
 * do it synchronously inside SaveInit and report "finished/idle" from SaveMove,
 * which the boot/menu state machines treat as "operation complete"
 * (e.g. Init_Task_Aload: SaveInit(0,2) then waits for SaveMove() <= 0).
 *
 * Only file_type 0 (DATA) is persisted; replay/directions saves are no-ops.
 *
 * The marshalling mirrors psp/savesub.c's save/load helpers plus the
 * game-side Save_Game_Data()/Copy_Save_w() (in Game/SYS_sub.c). */
#include "types.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "common/graphics.h"
#include "Game/WORK_SYS.h"
#include "Game/RANKING.h"
#include "Game/color3rd.h"

extern void Save_Game_Data(void);
extern void Copy_Save_w(void);

#define SAVE_DIR0    "sdmc:/3ds"
#define SAVE_DIR1    "sdmc:/3ds/sf3"
#define SAVE_PATH    "sdmc:/3ds/sf3/SETTINGS.BIN"
#define SAVE_VERSION 5u

/* On-disk layout — self-contained to this build (not PSP save-compatible). */
typedef struct {
    u32 version;
    u8 RTT_Enabled;
    u8 blit_filter;
    s16 render_mode;
    u8 Auto_Save;
    u8 SoundMode;
    u8 BGM_Level;
    u8 SE_Level;
    u8 BgmType;
    RANK_DATA Ranking[20];
    u8 Extra_Option;
    u8 PL_Color[2][20];
    _EXTRA_OPTION extra_option;
    u8 CRT_COLOR;
    _PAD_INFOR Pad_Infor[2];
} SaveData;

static SaveData save_data;

/* ---- game state -> save_data (mirrors savesub.c save*() helpers) ---- */
static void marshal_out(void) {
    int ix;

    save_data.version = SAVE_VERSION;
    save_data.Auto_Save = save_w[1].Auto_Save;
    Save_Game_Data(); /* fold the option-menu buffers back into save_w[1] */

    save_data.RTT_Enabled = (u8)RTT_Enabled;
    save_data.blit_filter = (u8)blit_filter;
    save_data.render_mode = render_mode;

    save_data.SoundMode = sys_w.sound_mode;
    save_data.BGM_Level = save_w[1].BGM_Level;
    save_data.SE_Level = save_w[1].SE_Level;
    save_data.BgmType = sys_w.bgm_type;

    for (ix = 0; ix < 20; ix++) save_data.Ranking[ix] = Ranking_Data[ix];
    for (ix = 0; ix < 20; ix++) save_data.PL_Color[0][ix] = save_w[1].PL_Color[0][ix];
    for (ix = 0; ix < 20; ix++) save_data.PL_Color[1][ix] = save_w[1].PL_Color[1][ix];
    save_data.Extra_Option = save_w[1].Extra_Option;
    save_data.extra_option = save_w[1].extra_option;

    save_data.CRT_COLOR = CRT_COLOR;

    save_data.Pad_Infor[0] = save_w[1].Pad_Infor[0];
    save_data.Pad_Infor[1] = save_w[1].Pad_Infor[1];
}

/* ---- save_data -> game state (mirrors savesub.c load*() + Copy_Save_w) ---- */
static void marshal_in(void) {
    int ix, iy;

    RTT_Enabled = save_data.RTT_Enabled;
    blit_filter = save_data.blit_filter;
    render_mode = save_data.render_mode;

    sys_w.sound_mode = save_data.SoundMode;
    save_w[1].BGM_Level = save_data.BGM_Level;
    save_w[1].SE_Level = save_data.SE_Level;
    sys_w.bgm_type = save_data.BgmType;

    for (ix = 0; ix < 20; ix++) save_w[1].Ranking[ix] = save_data.Ranking[ix];
    for (iy = 0; iy < 6; iy++) {
        for (ix = 0; ix < 20; ix++) save_w[iy].PL_Color[0][ix] = save_data.PL_Color[0][ix];
        for (ix = 0; ix < 20; ix++) save_w[iy].PL_Color[1][ix] = save_data.PL_Color[1][ix];
    }
    save_w[1].Extra_Option = save_data.Extra_Option;
    save_w[1].extra_option = save_data.extra_option;

    CRT_COLOR = save_data.CRT_COLOR;

    for (iy = 0; iy < 6; iy++) {
        save_w[iy].Pad_Infor[0] = save_data.Pad_Infor[0];
        save_w[iy].Pad_Infor[1] = save_data.Pad_Infor[1];
    }

    Copy_Save_w();
}

void SaveInit(s32 file_type, s32 save_mode) {
    /* Only the main DATA file is persisted; replay/directions are not stored. */
    if (file_type != 0)
        return;

    if (save_mode & 1) {
        /* odd save_mode = SAVE */
        FILE *f;
        marshal_out();
        mkdir(SAVE_DIR0, 0777); /* harmless if they already exist */
        mkdir(SAVE_DIR1, 0777);
        f = fopen(SAVE_PATH, "wb");
        if (f) {
            fwrite(&save_data, sizeof(save_data), 1, f);
            fclose(f);
        }
    } else {
        /* even save_mode = LOAD */
        FILE *f = fopen(SAVE_PATH, "rb");
        if (f) {
            size_t n = fread(&save_data, 1, sizeof(save_data), f);
            fclose(f);
            if (n == sizeof(save_data) && save_data.version == SAVE_VERSION)
                marshal_in();
        }
    }
}

s32 SaveMove(void) {
    return 0; /* work done synchronously in SaveInit; report finished/idle */
}

void SaveMsg(u16 x, u16 y) {
    (void)x;
    (void)y;
}

/* Game/menu.c renders the unlock-progress page through the save module. */
void displayGameProgress(void) {
}

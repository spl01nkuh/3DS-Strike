#ifndef GAME_SOUND_SOUND3RD_H
#define GAME_SOUND_SOUND3RD_H

#include "structs.h"
#include "types.h"

extern s16 bgm_level;
extern s16 se_level;
extern s8* sdbd[3];
extern SoundEvent* cseTSBDataTable[];
extern s8* csePHDDataTable[];

/* Master volume multiplier (0.0 = mute, 1.0 = full). */
extern float g_master_volume;

void Init_sound_system(void);
s32 sndCheckVTransStatus(s32 type);
void sndInitialLoad(void);
void checkAdxFileLoaded(void);
void Exit_sound_system(void);
void Init_bgm_work(void);
void sound_all_off(void);
void setSeVolume(s16 level);
void setupSoundMode(void);
void BGM_Server(void);
void setupAlwaysSeamlessFlag(s16 flag);
s32 adx_now_playend(void);
s32 bgm_play_status(void);
s32 bgmSkipCheck(s32 code);
void SsAllNoteOff(void);
void SsRequest(u16 ReqNumber);
void SsRequest_CC(u16 num);
void Standby_BGM(u16 num);
void Go_BGM(void);
void SsBgmFadeOut(u16 time);
void SsBgmHalfVolume(s16 flag);
void SE_cursor_move(void);
void SE_selected(void);
void SE_dir_cursor_move(void);
void SE_dir_selected(void);
void SsBgmControl(s8 unused, s8 VOLUME);
void SsRequestPan(u16 reqNum, s16 start, s16 unused1, s32 unused2, s32 unused3);
void SsBgmOff(void);
void SsBgmFadeIn(u16 ReqNumber, u16 FadeSpeed);
void spu_all_off(void);

#endif

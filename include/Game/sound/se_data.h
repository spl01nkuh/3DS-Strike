#ifndef GAME_SOUND_SE_DATA_H
#define GAME_SOUND_SE_DATA_H

#include "structs.h"
#include "types.h"

#define SE_DISPATCH_TABLE_SIZE 768

typedef enum {
    SE_HANDLER_CALL_SE = 0,
    SE_HANDLER_SHOCK,
    SE_HANDLER_MYSELF,
    SE_HANDLER_MYSELF_DIE,
    SE_HANDLER_LET,
    SE_HANDLER_LET_SP,
    SE_HANDLER_TERM,
    SE_HANDLER_DUMMY,
} SeHandlerType;

extern const SeHandlerType se_handler_type[SE_DISPATCH_TABLE_SIZE];
extern const u16 Bonus_Voice_Data[768];

void Se_Dispatch(u16 index, u16 code, WORK_Other* ewk);

#endif

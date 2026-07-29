#ifndef PSP_AFS_H
#define PSP_AFS_H

#include <pspiofilemgr.h>
#include "types.h"

#define AFS_MAX_ENTRIES 1536

typedef struct {
    u32 offset;
    u32 size;
} AFSEntry;

typedef struct {
    SceUID fd;          /* shared fd for sync reads */
    SceUID async_fd;    /* dedicated fd for async reads */
    u32 entry_count;
    volatile s32 async_busy;    /* 1 = async read in progress */
    volatile s32 async_result;  /* bytes read, or <0 on error */
    AFSEntry entries[AFS_MAX_ENTRIES];
} AFS;

/* Initialize AFS from file path (call from main thread) */
s32 afsInit(const char* path);
s32 afsIsReady(void);
void afsClose(void);

/* Get file size from AFS index (instant — cached from header) */
u32 afsGetFileSize(u16 fnum);

/* Get sector count for a file */
u32 afsGetSectorCount(u16 fnum);

/* Read file data synchronously (blocks until complete) */
s32 afsReadSync(u16 fnum, void* buf, u32 size);

/* Async: start a non-blocking read */
s32 afsReadAsync(u16 fnum, void* buf, u32 size);

/* Async: check if read is done (0=still reading, 1=done) */
s32 afsCheckRead(void);

/* Close fds on PSP suspend (unblocks hung I/O reads) */
void afsSuspend(void);

/* Re-open file descriptors after PSP sleep/resume */
void afsReopen(void);

#endif

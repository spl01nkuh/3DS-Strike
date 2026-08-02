#include "psp/afs.h"
#include <string.h>
#include <stdlib.h>
#include <pspthreadman.h>

#define AFS_MAGIC 0x00534641  /* "AFS\0" little-endian */
#define SECTOR_SIZE 2048

static AFS afs;
static s32 afs_ready = 0;
static char afs_path[256];

/* Background I/O thread for async reads */
static SceUID io_thread_id = -1;
static SceUID io_sema = -1;
static volatile u16 io_fnum;
static volatile void* io_buf;
static volatile u32 io_size;
static volatile s32 io_shutdown = 0;
static volatile s32 afs_suspending = 0;  /* set during PSP sleep transition */

/* Forward declarations for self-healing fd reopen */
static void afs_reopen_sync_fd(void);
static void afs_reopen_async_fd(void);
static void afs_cache_reset(void);

/* Prefetch staging state (defined with the cache below; the I/O thread needs
 * visibility to publish stage completion). */
static volatile s32 pf_fnum;
static volatile u32 pf_len;
static volatile u8 pf_ready;
static volatile u8 io_is_prefetch;

/* IOSPIKE attribution (read by pspshim.c's probe): which fd is the main-thread
 * sync path vs the I/O thread, and the fnum each is currently reading. */
volatile int g_afs_sync_fd = -1, g_afs_async_fd = -1;
volatile int g_afs_sync_fnum = -1, g_afs_io_fnum = -1;

static int afs_io_thread(SceSize args, void* argp) {
    (void)args; (void)argp;

    while (!io_shutdown) {
        /* Wait for work */
        sceKernelWaitSema(io_sema, 1, NULL);
        if (io_shutdown) break;

        u16 fnum = io_fnum;
        void* buf = (void*)io_buf;
        u32 size = io_size;

        if (fnum >= afs.entry_count || afs.async_fd < 0 || !buf) {
            afs.async_result = -1;
            afs.async_busy = 0;
            continue;
        }

        u32 read_size = size;
        if (read_size > afs.entries[fnum].size) {
            read_size = afs.entries[fnum].size;
        }

        g_afs_async_fd = afs.async_fd;
        g_afs_io_fnum = fnum;
        sceIoLseek32(afs.async_fd, afs.entries[fnum].offset, PSP_SEEK_SET);
        s32 result = sceIoRead(afs.async_fd, buf, read_size);

        /* Self-heal after PSP sleep — async fd may be stale.
           Don't reopen if we're in the suspend transition — afsReopen()
           handles it on RESUME_COMPLETE. */
        if (result < 0 && !afs_suspending) {
            afs_reopen_async_fd();
            if (afs.async_fd >= 0) {
                g_afs_async_fd = afs.async_fd;
                sceIoLseek32(afs.async_fd, afs.entries[fnum].offset, PSP_SEEK_SET);
                result = sceIoRead(afs.async_fd, buf, read_size);
            }
        }

        afs.async_result = result;
        if (io_is_prefetch) {
            pf_len = (result > 0) ? (u32)result : 0;
            if (result <= 0) pf_fnum = -1; /* failed stage — forget it */
        }
        __sync_synchronize(); /* publish the buffer write before clearing the flags */
        if (io_is_prefetch) pf_ready = (result > 0);
        afs.async_busy = 0;
    }

    return 0;
}

s32 afsInit(const char* path) {
    u32 magic;

    /* The game re-inits on menu/mode transitions (measured 3x in one boot
     * flow). The archive is immutable — wiping the read cache each time cost
     * ~720ms of boot-set re-reads per wipe and threw away preloader work
     * (plus re-created the I/O thread). Already open with the same path:
     * keep everything. */
    if (afs_ready && afs.fd >= 0 && strncmp(afs_path, path, sizeof(afs_path) - 1) == 0) {
        return 1;
    }

    memset(&afs, 0, sizeof(afs));
    afs.fd = -1;
    afs.async_fd = -1;
    afs_cache_reset();
    strncpy(afs_path, path, sizeof(afs_path) - 1);

    /* Primary fd for sync reads */
    afs.fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (afs.fd < 0) {
        return 0;
    }

    sceIoRead(afs.fd, &magic, 4);
    if (magic != AFS_MAGIC) {
        sceIoClose(afs.fd);
        afs.fd = -1;
        return 0;
    }

    sceIoRead(afs.fd, &afs.entry_count, 4);
    if (afs.entry_count > AFS_MAX_ENTRIES) {
        afs.entry_count = AFS_MAX_ENTRIES;
    }

    sceIoRead(afs.fd, afs.entries, afs.entry_count * sizeof(AFSEntry));

    /* Second fd for async reads (separate seek position) */
    afs.async_fd = sceIoOpen(path, PSP_O_RDONLY, 0);

    /* Create I/O thread and semaphore */
    io_shutdown = 0;
    io_sema = sceKernelCreateSema("afsIO", 0, 0, 1, NULL);
    io_thread_id = sceKernelCreateThread("afsIO", afs_io_thread, 0x18, 0x4000, 0, NULL);
    if (io_thread_id >= 0) {
        sceKernelStartThread(io_thread_id, 0, NULL);
    }

    afs_ready = 1;
    return 1;
}

s32 afsIsReady(void) {
    return afs_ready;
}

void afsClose(void) {
    /* Shut down I/O thread */
    if (io_thread_id >= 0) {
        io_shutdown = 1;
        sceKernelSignalSema(io_sema, 1);
        sceKernelWaitThreadEnd(io_thread_id, NULL);
        sceKernelDeleteThread(io_thread_id);
        io_thread_id = -1;
    }
    if (io_sema >= 0) {
        sceKernelDeleteSema(io_sema);
        io_sema = -1;
    }

    if (afs.fd >= 0) {
        sceIoClose(afs.fd);
        afs.fd = -1;
    }
    if (afs.async_fd >= 0) {
        sceIoClose(afs.async_fd);
        afs.async_fd = -1;
    }
    afs_cache_reset();
    afs_ready = 0;
}

void afsSuspend(void) {
    /* Called from power callback on SUSPENDING.
       Close fds to unblock any hung sceIoRead in the I/O thread.
       Reads will fail with EBADF instead of hanging forever. */
    afs_suspending = 1;

    if (afs.fd >= 0) { sceIoClose(afs.fd); afs.fd = -1; }
    if (afs.async_fd >= 0) { sceIoClose(afs.async_fd); afs.async_fd = -1; }
}

void afsReopen(void) {
    /* Re-open file descriptors after PSP sleep/resume. */
    if (afs_path[0] == 0) return;

    /* Close any leftover stale fds */
    if (afs.fd >= 0) sceIoClose(afs.fd);
    if (afs.async_fd >= 0) sceIoClose(afs.async_fd);

    afs.fd = sceIoOpen(afs_path, PSP_O_RDONLY, 0);
    afs.async_fd = sceIoOpen(afs_path, PSP_O_RDONLY, 0);
    /* Force-clear async state — I/O thread may have been stuck */
    afs.async_busy = 0;
    afs.async_result = -1;
    afs_suspending = 0;
}

u32 afsGetFileSize(u16 fnum) {
    if (fnum >= afs.entry_count) return 0;
    return afs.entries[fnum].size;
}

u32 afsGetSectorCount(u16 fnum) {
    u32 size = afsGetFileSize(fnum);
    return (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
}

/* Try to reopen just the sync fd (thread-safe — only touches afs.fd) */
static void afs_reopen_sync_fd(void) {
    if (afs_path[0] == 0) return;
    if (afs.fd >= 0) sceIoClose(afs.fd);
    afs.fd = sceIoOpen(afs_path, PSP_O_RDONLY, 0);
}

/* Try to reopen just the async fd (called from I/O thread only) */
static void afs_reopen_async_fd(void) {
    if (afs_path[0] == 0) return;
    if (afs.async_fd >= 0) sceIoClose(afs.async_fd);
    afs.async_fd = sceIoOpen(afs_path, PSP_O_RDONLY, 0);
}

/* ── AFS read cache ───────────────────────────────────────────────────────
 * On-demand content (sprites, fight fx, music) is re-read from the AFS file
 * each time its decompressed copy is evicted from the texture cache. Every
 * read is a blocking sceIoRead on the main thread (~5 MB/s), stalling the
 * frame 8–32 ms — the periodic in-fight hitch. AFS entries are immutable, so
 * cache file data by index: a re-read becomes a memcpy instead of disk I/O.
 * Budget-bounded LRU; only small files are cached (big scene loads are
 * one-shot and bypass the cache). No threads — avoids the async deadlock. */
/* Budget was 44MB, sized to hold the ~30MB boot preload set — but that preload
   was deleted (2026-07-28), so the justification went with it. On Old 3DS the
   app heap is ~64MB and the fl system arena already takes 24MB of it, so a
   44MB cache can never be satisfied: it simply grew until malloc started
   failing. Measured symptom — heap used climbed 41MB -> 55MB with only 140KB
   free and a largest free block of 8-31KB, at which point BGM's 191KB segment
   allocation failed, adxPlayInternal bailed, and music went silent for
   seconds at a time while the game respun the segment queue.

   MAX_FILE was 9MB, which cached multi-MB one-shot stage/fx files and is what
   wrecked the hit rate: measured 3 hits against 15 misses while holding 13MB.
   That also contradicted this file's own header comment ("only small files are
   cached, big scene loads are one-shot and bypass the cache") — restoring a
   small cap restores the documented design. Small files are the ones actually
   re-read, and they still fit comfortably. */
#define AFS_CACHE_BUDGET   (8 * 1024 * 1024)
#define AFS_CACHE_MAX_FILE (512 * 1024)
#define AFS_CACHE_SLOTS    512
typedef struct { s32 fnum; u32 size; u8* data; u32 last_use; } AfsCacheSlot;
static AfsCacheSlot afs_cache[AFS_CACHE_SLOTS];
static u32 afs_cache_bytes;
static u32 afs_cache_clock;

/* Background prefetch of the next consecutive index. The slow sprite/stage
 * streams read fnum N, N+1, ... ~one every couple of seconds via the SYNC path;
 * staging N+1 on the (otherwise idle) I/O thread turns the next read into a
 * memcpy instead of a ~37ms main-thread block. */
#define AFS_PREFETCH_MAX (3 * 1024 * 1024) /* big enough for the 1.5-1.8MB one-shot files that caused 280-330ms SYNC stalls */
static u8* pf_buf;
static volatile s32 pf_fnum = -1;     /* index staged (or being staged) in pf_buf */
static volatile u32 pf_len;           /* bytes actually staged (set by the I/O thread) */
static volatile u8 pf_ready;          /* stage completed OK; pf_buf holds pf_len bytes of pf_fnum */
static volatile u8 io_is_prefetch;    /* the in-flight I/O-thread job is a prefetch stage */

static void afs_cache_reset(void) {
    for (int i = 0; i < AFS_CACHE_SLOTS; i++) {
        if (afs_cache[i].data) free(afs_cache[i].data);
        afs_cache[i].data = NULL;
        afs_cache[i].fnum = -1;
        afs_cache[i].size = 0;
    }
    afs_cache_bytes = 0;
    afs_cache_clock = 0;
    if (pf_buf) { free(pf_buf); pf_buf = NULL; }
    pf_fnum = -1;
    pf_len = 0;
    pf_ready = 0;
}

static AfsCacheSlot* afs_cache_find(u16 fnum) {
    for (int i = 0; i < AFS_CACHE_SLOTS; i++)
        if (afs_cache[i].data && afs_cache[i].fnum == (s32)fnum)
            return &afs_cache[i];
    return NULL;
}

/* Take ownership of a malloc'd full-file buffer; evict LRU until it fits. */
static void afs_cache_insert(u16 fnum, u8* owned, u32 fullsize) {
    for (;;) {
        AfsCacheSlot* slot = NULL;
        for (int i = 0; i < AFS_CACHE_SLOTS; i++)
            if (!afs_cache[i].data) { slot = &afs_cache[i]; break; }
        if (slot && afs_cache_bytes + fullsize <= AFS_CACHE_BUDGET) {
            slot->data = owned; slot->fnum = (s32)fnum; slot->size = fullsize;
            slot->last_use = ++afs_cache_clock; afs_cache_bytes += fullsize;
            return;
        }
        AfsCacheSlot* lru = NULL;
        for (int i = 0; i < AFS_CACHE_SLOTS; i++)
            if (afs_cache[i].data && (!lru || afs_cache[i].last_use < lru->last_use))
                lru = &afs_cache[i];
        if (!lru) { free(owned); return; } /* empty yet won't fit (shouldn't happen) */
        afs_cache_bytes -= lru->size; free(lru->data); lru->data = NULL;
    }
}

/* Drop any cached entry for an index (before replacing it with a larger read). */
static void afs_cache_remove(u16 fnum) {
    for (int i = 0; i < AFS_CACHE_SLOTS; i++)
        if (afs_cache[i].data && afs_cache[i].fnum == (s32)fnum) {
            afs_cache_bytes -= afs_cache[i].size;
            free(afs_cache[i].data);
            afs_cache[i].data = NULL;
        }
}

/* Stage the next consecutive index on the I/O thread (best-effort). Only small
 * stream files; skips if the thread is busy (LDREQ or a prior stage), already
 * cached, or already staged. */
static void afs_prefetch(u16 fnum) {
    if (!afs_ready || fnum >= afs.entry_count) return;
    u32 fsz = afs.entries[fnum].size;
    if (fsz == 0 || fsz > AFS_PREFETCH_MAX) return;
    if (afs.async_busy) return;
    if (pf_fnum == (s32)fnum) return; /* already staged (async_busy==0 ⇒ complete) */
    if (io_thread_id < 0 || io_sema < 0) return;

    /* A completed stage the game never consumed still cost a disk read — keep
     * it: promote into the LRU cache before reusing the staging buffer. This is
     * what lets a big file staged at one scene survive until its use a minute
     * later (measured: fnum 559, staged after 558, wanted 42s later — the old
     * code dropped it and ate a 281ms mid-fight stall). */
    if (pf_ready && pf_fnum >= 0 && pf_len > 0 && !afs_cache_find((u16)pf_fnum)) {
        u8* keep = (u8*)malloc(pf_len);
        if (keep) {
            memcpy(keep, pf_buf, pf_len);
            afs_cache_insert((u16)pf_fnum, keep, pf_len);
        }
    }
    pf_ready = 0;
    pf_fnum = -1;

    if (afs_cache_find(fnum)) return;
    if (!pf_buf) { pf_buf = (u8*)malloc(AFS_PREFETCH_MAX); if (!pf_buf) return; }
    pf_fnum = (s32)fnum;
    pf_len = 0;
    io_is_prefetch = 1;
    io_fnum = fnum;
    io_buf = pf_buf;
    io_size = fsz;
    afs.async_result = 0;
    afs.async_busy = 1;
    sceKernelSignalSema(io_sema, 1);
}

s32 afsReadSync(u16 fnum, void* buf, u32 size) {
    if (fnum >= afs.entry_count || !afs_ready) return 0;

    u32 file_size = afs.entries[fnum].size;
    if (file_size == 0) return 0;

    u32 read_size = (size < file_size) ? size : file_size;

    /* 1) LRU cache hit. The game reads a fixed prefix of each index, so the
     * cached span normally covers the request; a rarer larger request falls
     * through to re-read + re-cache. */
    AfsCacheSlot* hit = afs_cache_find(fnum);
    if (hit && hit->size >= read_size) {
        memcpy(buf, hit->data, read_size);
        hit->last_use = ++afs_cache_clock;
        afs_prefetch((u16)(fnum + 1));
        return 1;
    }

    /* 2) The wanted index is mid-stage on the I/O thread — wait for it instead
     * of issuing a duplicate blocking read. (Measured: stream onsets ask for N
     * and N+1 in the same frame; the duplicate read cost a full stall AND
     * orphaned the stage, making N+2 miss too.) Bounded: the wait can never
     * exceed the blocking read we'd otherwise do; 1.5s cap as a safety net. */
    if (pf_fnum == (s32)fnum && !pf_ready && afs.async_busy && io_is_prefetch) {
        for (int spin = 0; spin < 7500 && afs.async_busy && !pf_ready; spin++)
            sceKernelDelayThread(200); /* 0.2ms tick */
    }

    /* 3) Prefetch hit: staged earlier (or just waited on) — serve from RAM and
     * promote to the LRU cache, then stage the next index. */
    if (pf_fnum == (s32)fnum && pf_ready && pf_len >= read_size) {
        memcpy(buf, pf_buf, read_size);
        { u8* c = (u8*)malloc(pf_len);
          if (c) { memcpy(c, pf_buf, pf_len); afs_cache_remove(fnum); afs_cache_insert(fnum, c, pf_len); } }
        pf_ready = 0;
        pf_fnum = -1;
        afs_prefetch((u16)(fnum + 1));
        return 1;
    }

    /* 4) Miss — blocking read on the main thread (what 1-3 exist to avoid). */
    if (afs.fd < 0) afs_reopen_sync_fd();
    if (afs.fd < 0) return 0;

    /* Kick the NEXT index onto the I/O thread FIRST so it reads in parallel
     * with our own blocking read — stream onsets ask for N then N+1 within a
     * frame, and this overlaps the two stalls into one. */
    afs_prefetch((u16)(fnum + 1));

    /* Read the span we'll serve; for a small file read the whole thing so a later
     * full read of a header-first index also hits. Cache the bytes read, keyed by
     * index — works for a small prefix of a multi-MB file (music/voice/fx). */
    u32 want = (file_size <= AFS_CACHE_MAX_FILE) ? file_size : read_size;

    g_afs_sync_fd = afs.fd;
    g_afs_sync_fnum = (int)fnum;

    if (want <= AFS_CACHE_MAX_FILE) {
        u8* full = (u8*)malloc(want);
        if (full) {
            sceIoLseek32(afs.fd, afs.entries[fnum].offset, PSP_SEEK_SET);
            s32 rd = sceIoRead(afs.fd, full, want);
            if (rd < 0) {
                afs_reopen_sync_fd();
                if (afs.fd >= 0) {
                    g_afs_sync_fd = afs.fd;
                    sceIoLseek32(afs.fd, afs.entries[fnum].offset, PSP_SEEK_SET);
                    rd = sceIoRead(afs.fd, full, want);
                }
            }
            if (rd >= 0) {
                memcpy(buf, full, read_size); /* read_size <= want */
                afs_cache_remove(fnum);
                afs_cache_insert(fnum, full, want); /* transfers ownership */
                afs_prefetch((u16)(fnum + 1));
                return 1;
            }
            free(full);
            return 0;
        }
        /* malloc failed — fall through to a direct read */
    }

    sceIoLseek32(afs.fd, afs.entries[fnum].offset, PSP_SEEK_SET);
    s32 read = sceIoRead(afs.fd, buf, read_size);

    /* Self-heal after PSP sleep — fd may be stale */
    if (read < 0) {
        afs_reopen_sync_fd();
        if (afs.fd < 0) return 0;
        g_afs_sync_fd = afs.fd;
        sceIoLseek32(afs.fd, afs.entries[fnum].offset, PSP_SEEK_SET);
        read = sceIoRead(afs.fd, buf, read_size);
    }
    if (read < 0) return 0;
    afs_prefetch((u16)(fnum + 1));
    return 1;
}

s32 afsReadAsync(u16 fnum, void* buf, u32 size) {
    if (fnum >= afs.entry_count || !afs_ready) return 0;

    u32 file_size = afs.entries[fnum].size;
    if (file_size == 0) return 0;

    u32 read_size = (size < file_size) ? size : file_size;

    /* Cache hit — satisfy on the spot. async_busy stays 0, so the poller
     * (afsCheckRead/fsCheckCommandExecuting) sees it as already complete. */
    AfsCacheSlot* hit = afs_cache_find(fnum);
    if (hit && hit->size >= read_size) {
        memcpy(buf, hit->data, read_size);
        hit->last_use = ++afs_cache_clock;
        afs.async_result = (s32)read_size;
        afs.async_busy = 0;
        return 1;
    }

    /* No I/O thread (init failed) or one already in flight — do it synchronously
     * so the contract still holds (the queue is serial, so "in flight" is rare). */
    if (io_thread_id < 0 || io_sema < 0 || afs.async_busy) {
        return afsReadSync(fnum, buf, size);
    }

    /* Kick the background read on the I/O thread (afs_io_thread reads via the
     * dedicated async_fd and clears async_busy when done). The game polls for
     * completion, so the main thread never blocks on the disk read — the whole
     * point: mid-fight streamed sprites/stage frames load off the render core
     * instead of freezing it ~8-37ms each. */
    io_is_prefetch = 0;
    io_fnum = fnum;
    io_buf = buf;
    io_size = size;
    afs.async_result = 0;
    afs.async_busy = 1;
    sceKernelSignalSema(io_sema, 1);
    return 1;
}

s32 afsCheckRead(void) {
    if (afs.async_busy) {
        return 0;  /* Still reading */
    }
    return 1;  /* Done */
}

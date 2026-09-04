/*
 * The log file on the SD card.
 *
 * The USB Gecko console is the port's debug channel under Dolphin, and it has
 * carried every diagnosis so far. On real hardware it is not there: the user
 * boots a .dol from Swiss on a console with no second machine attached, no
 * serial cable and no way to read a printf. Everything the heartbeat knows --
 * dropped opcodes, audio peak levels, the thread block address, the stack
 * canary -- becomes invisible at exactly the moment it matters most, because
 * hardware is where the port's remaining defects actually have to be found.
 *
 * So the same text goes to a file. gc_log and gc_fatal tee into here, and the
 * file lands beside the executable as dkr.log.
 *
 * Three properties this has to have, and how each is met:
 *
 * 1. It must survive the power switch. Nobody exits a GameCube game; they turn
 *    it off. A FILE* held open for the session would lose everything still in
 *    libfat's cache, and the directory entry would never be updated. So a
 *    flush is a full open/append/close cycle: after it returns, what has been
 *    logged is on the card, including the file's size. That is more work than
 *    an fflush, and it is the only version of this worth having.
 *
 * 2. It must not stall the game. A FAT write costs milliseconds. Log text goes
 *    into a fixed buffer, and the buffer is flushed on a schedule -- at the end
 *    of each heartbeat, when it is close to full, and immediately on a fatal.
 *    Nothing in the render or audio path ever waits on the card.
 *
 * 3. It must be safe to call from any thread. gc_log is called from the boot
 *    thread, gc_fatal from wherever the failure was, and the crash handler from
 *    an exception context. The buffer is guarded by a mutex, and the crash path
 *    can bypass it -- see gc_logfile_flush_unlocked.
 *
 * If no card is mounted the whole thing degrades to nothing: gc_log still goes
 * to the console, and every call here becomes a cheap early return. That is the
 * Dolphin case, where the Gecko is the better channel anyway.
 */

#include <ultra64.h>

/* <ogc/mutex.h> rather than <gccore.h>: the wider header drags in gu.h and
 * gx.h, whose Mtx and Vtx collide with the ones in PR/gbi.h that ultra64.h
 * has already defined. The same rule as everywhere else on this side of the
 * port -- see the type-collision note in PORTING.md. */
#include <ogc/mutex.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gc_ultra.h"

#if GC_SDLOG

/*
 * Where the log is written, in the same order and for the same reasons as the
 * asset image in gc_main.c: the folder the dist target produces, then a reader
 * in either slot.
 */
static const char *const kLogPaths[] = {
    "sd:/dkr/dkr.log",
    "carda:/dkr/dkr.log",
    "cardb:/dkr/dkr.log",
};

/*
 * 16 KB. One heartbeat at its most verbose (every counter block enabled, the
 * texture dumps included) is a little over 4 KB, so this holds several beats
 * and never truncates one; a flush that happens mid-beat is legible, just
 * split. The buffer is static because the game's heap belongs to the game.
 */
#define LOG_BUF_SIZE (16 * 1024)

/* Flush once the buffer is this full even if no beat has ended, so that a
 * burst of tracing between beats cannot overflow it. */
#define LOG_FLUSH_MARK ((LOG_BUF_SIZE * 3) / 4)

/*
 * The file is created once at its full size and never grows.
 *
 * This is a direct response to the user's SD card being corrupted badly enough
 * to need reformatting, after several runs that each ended in a CPU fault.
 *
 * Appending is the dangerous shape: every `fopen(path, "a")` that extends the
 * file makes libfat allocate a cluster and rewrite the FAT, and the port was
 * doing that about once a second on a machine that keeps dying mid-run. A fault
 * between the FAT update and the directory update leaves the volume
 * inconsistent -- not just this file.
 *
 * Preallocating removes that entirely. The cluster chain is written once, at
 * boot, before any game code runs; every flush afterwards is `fopen("r+b")`,
 * seek, overwrite, close, which touches file *data* and one directory entry and
 * never the allocation table. The worst a crash mid-flush can now do is garble
 * the tail of the log.
 *
 * 256 KB holds about a minute of GC_DEBUG heartbeat, which is far longer than
 * any run that has reached this point, and costs a second or two of boot on an
 * SD Gecko. The padding is newlines rather than NULs so the file still opens
 * cleanly as text when the log stops part-way through it.
 *
 * None of this can be tested under Dolphin, which has no GameCube SD card at
 * all -- the same trap that cost a hardware run on gc_logfile_mark. So every
 * step of it degrades to the append path that was already working rather than
 * to no log: a short write here, or a failed open at flush time, sets
 * sAppendMode and the port carries on the old way.
 */
#define LOG_FILE_SIZE (256 * 1024)

static char sBuf[LOG_BUF_SIZE];
static u32 sLen;
static u32 sDropped;

/* How far into the preallocated file the next flush writes. */
static u32 sOffset;
/* Set when preallocation failed and the log falls back to plain appending. */
static BOOL sAppendMode;

static const char *sPath;
static BOOL sReady;
static mutex_t sLock = LWP_MUTEX_NULL;

/*
 * Crash mode: stop taking the lock.
 *
 * The crash handler writes a couple of hundred lines through gc_logfile_printf,
 * and each one would take this mutex. If the thread that faulted was itself
 * inside a gc_log at the time -- the boot thread printing a heartbeat is the
 * obvious case -- the mutex is already held by a thread that will never release
 * it, and the handler deadlocks on its first line. A total freeze, and no
 * report: strictly worse than the empty log it was meant to fix.
 *
 * Once this is set there is exactly one writer left, so the lock protects
 * nothing and skipping it costs nothing. It is never cleared: after a crash
 * there is no "back to normal".
 */
static volatile BOOL sCrashMode;

void gc_logfile_set_crash_mode(void) {
    sCrashMode = TRUE;
}

/* ---- the card ------------------------------------------------------------ */

/* The write itself. The caller holds the lock, or is the crash handler and has
 * decided that racing is better than not writing at all. */
static void flush_locked(void) {
    FILE *f;
    u32 len = sLen;
    u32 dropped = sDropped;

    if (!sReady || (len == 0 && dropped == 0)) {
        return;
    }

    sLen = 0;
    sDropped = 0;

    /* The card is shared with the save path and, on this user's console, with
     * the volume the game booted from. One writer at a time -- see gc_fs_lock.
     * The crash handler skips it: the thread that faulted may be holding it. */
    if (!sCrashMode) {
        gc_fs_lock();
    }

    /* "r+b" and a seek, not "a": see LOG_FILE_SIZE. The file was created at
     * full size at boot, so this never asks libfat to allocate a cluster. */
    f = fopen(sPath, sAppendMode ? "a" : "r+b");
    if (f == NULL && !sAppendMode) {
        /* The preallocated path is untestable off hardware, so it is never
         * allowed to be the reason there is no log. Fall back to what worked
         * before and keep going. */
        sAppendMode = TRUE;
        f = fopen(sPath, "a");
    }
    if (f == NULL) {
        /* The card was pulled, or the filesystem gave up. Stop trying: a
         * failing fopen every second is a much worse stall than no log. */
        sReady = FALSE;
        if (!sCrashMode) {
            gc_fs_unlock();
        }
        return;
    }
    if (!sAppendMode) {
        if (sOffset + len > LOG_FILE_SIZE) {
            /* The preallocated file is full. Stop rather than grow it: growing
             * is the operation this whole scheme exists to avoid. */
            len = 0;
            dropped += sLen;
        }
        fseek(f, (long) sOffset, SEEK_SET);
    }
    if (len != 0) {
        fwrite(sBuf, 1, len, f);
        sOffset += len;
    }
    if (dropped != 0) {
        /* A fixed string, not fprintf: this runs on the crash path too, and
         * newlib's printf family touches the FPU -- which an exception context
         * does not have. See the comment on crash_puts in gc_crash.c. */
        static const char kDropped[] = "\n[log buffer overran; some lines lost]\n";

        fwrite(kDropped, 1, sizeof(kDropped) - 1, f);
        sOffset += (u32) (sizeof(kDropped) - 1);
    }
    fclose(f); /* commits the directory entry, which fflush does not */

    if (!sCrashMode) {
        gc_fs_unlock();
    }
}

/* ---- the buffer ---------------------------------------------------------- */

void gc_logfile_write(const char *text, u32 len) {
    if (!sReady || len == 0) {
        return;
    }
    if (!sCrashMode && LWP_MutexLock(sLock) != 0) {
        return;
    }

    /*
     * Buffer only. This used to flush when the buffer filled, which meant any
     * thread that logged enough found itself inside libfat -- and the log is
     * written to the very card the game boots from. All the I/O is on the boot
     * thread now, which calls gc_logfile_flush once per retrace, so a burst of
     * tracing reaches the card within 20 ms without the tracing thread ever
     * opening a file.
     *
     * Crash mode is the exception: the boot thread may never run again, so the
     * handler drains the buffer itself.
     */
    if (sLen + len > LOG_BUF_SIZE) {
        /* Full and nobody has drained it. Count the loss rather than storing a
         * truncated line that a reader would take at face value. */
        sDropped += len;
    } else {
        memcpy(sBuf + sLen, text, len);
        sLen += len;
    }

    if (sCrashMode && sLen >= LOG_FLUSH_MARK) {
        flush_locked();
    }
    if (!sCrashMode) {
        LWP_MutexUnlock(sLock);
    }
}

void gc_logfile_printf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    int n;

    if (!sReady) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }
    if ((u32) n >= sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    gc_logfile_write(line, (u32) n);
}

void gc_logfile_vprintf(const char *fmt, va_list ap) {
    char line[512];
    int n;

    if (!sReady) {
        return;
    }
    n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) {
        return;
    }
    if ((u32) n >= sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    gc_logfile_write(line, (u32) n);
}

/*
 * Called every retrace from gc_main, so the empty case has to be free: no lock,
 * no call, just two compares. Only when something is buffered does this cost an
 * open/append/close, and that is about once a second.
 */
void gc_logfile_flush(void) {
    if (!sReady || (sLen == 0 && sDropped == 0)) {
        return;
    }
    if (sCrashMode) {
        flush_locked();
        return;
    }
    if (LWP_MutexLock(sLock) != 0) {
        return;
    }
    flush_locked();
    LWP_MutexUnlock(sLock);
}

/*
 * The crash path's flush.
 *
 * An exception handler runs on a broken machine: the mutex may be held by the
 * thread that just faulted, and blocking on it would replace a crash report
 * with a hang. Writing without the lock can interleave with a log call that was
 * in progress, which costs one garbled line. That is the right trade -- the
 * whole point of this file is to get the last few lines onto the card.
 */
void gc_logfile_flush_unlocked(void) {
    flush_locked();
}

/*
 * Truncate (or create) the log and record which path worked.
 *
 * Opening for write here rather than appending is deliberate: a log that grows
 * across boots turns "what happened this run" into a search. The user reads the
 * file after the run that misbehaved, so the file is that run.
 */
void gc_logfile_init(void) {
    u32 i;

    if (sReady) {
        return;
    }
    if (sLock == LWP_MUTEX_NULL && LWP_MutexInit(&sLock, FALSE) != 0) {
        return;
    }

    for (i = 0; i < sizeof(kLogPaths) / sizeof(kLogPaths[0]); i++) {
        FILE *f = fopen(kLogPaths[i], "wb");
        static char pad[4096];
        u32 written = 0;

        if (f == NULL) {
            continue;
        }

        /*
         * Write the whole file once, here, before any game code runs. From now
         * on every flush overwrites bytes inside it and the allocation table is
         * never touched again -- see LOG_FILE_SIZE. Newline padding so the file
         * still reads as text once the log stops part-way through it.
         */
        memset(pad, '\n', sizeof(pad));
        while (written < LOG_FILE_SIZE) {
            if (fwrite(pad, 1, sizeof(pad), f) != sizeof(pad)) {
                break;
            }
            written += (u32) sizeof(pad);
        }
        fclose(f);

        /* A short card, or a write that failed: fall back to appending rather
         * than to no log at all. */
        sAppendMode = (written < LOG_FILE_SIZE);
        sOffset = 0;
        sPath = kLogPaths[i];
        sReady = TRUE;
        break;
    }

    if (!sReady) {
        /* No card: not an error. Under Dolphin this is the expected outcome
         * and the Gecko console carries everything. */
        return;
    }

    gc_logfile_printf("=== DKR-GC ===\n");
    gc_logfile_printf("built " __DATE__ " " __TIME__ "\n");
    gc_logfile_printf("log   %s (%u KB preallocated%s)\n", sPath,
                      (unsigned) (LOG_FILE_SIZE / 1024),
                      sAppendMode ? ", FALLBACK: appending" : "");
    gc_logfile_flush();
}

/*
 * A breadcrumb: one line, written and flushed immediately.
 *
 * For the handful of places that have to be readable even if the machine dies a
 * millisecond later -- the first time a newly written subsystem is entered, the
 * boot trace, a crash marker. The per-beat flush is not enough for those,
 * because the first hardware crash happened before the first beat and the log
 * therefore said nothing about the run at all.
 *
 * It flushes from the calling thread, and that is deliberate -- it was briefly
 * changed to buffer-only and that was a mistake worth recording.
 *
 * The reasoning for buffering was sound: three threads had started touching the
 * card, so move all the I/O onto the boot thread. But it destroyed the one
 * thing these marks exist for. If the game thread hangs at a priority above the
 * boot thread, or the machine stops with interrupts disabled, the boot thread
 * never runs again and everything still in the buffer is lost -- which is
 * exactly what the third hardware run produced: a log ending at
 * `boot: game running` with no marks at all, from a build whose marks had
 * reached the card on the run before.
 *
 * gc_fs_lock was the right answer, not buffering. The lock is what makes a
 * flush from any thread safe; taking it here costs a few milliseconds at a
 * once-per-boot event and keeps the breadcrumb readable even when the caller
 * never returns.
 */
void gc_logfile_mark(const char *fmt, ...) {
    va_list ap;

    /*
     * To the console as well, and unconditionally -- note this runs even when
     * no card is mounted, which is the Dolphin case. A breadcrumb that only
     * exists on hardware cannot be tested before it is needed, and these were
     * added precisely because a hardware run had to be spent finding out
     * something a Dolphin run could have told us.
     */
    if (gc_console_on()) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }

    if (!sReady) {
        return;
    }
    va_start(ap, fmt);
    gc_logfile_vprintf(fmt, ap);
    va_end(ap);
    gc_logfile_flush();
}

BOOL gc_logfile_active(void) {
    return sReady;
}

const char *gc_logfile_path(void) {
    return sReady ? sPath : NULL;
}

#else /* !GC_SDLOG */

void gc_logfile_init(void) {
}

void gc_logfile_flush(void) {
}

void gc_logfile_flush_unlocked(void) {
}

void gc_logfile_write(const char *text, u32 len) {
    (void) text;
    (void) len;
}

void gc_logfile_printf(const char *fmt, ...) {
    (void) fmt;
}

void gc_logfile_mark(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void gc_logfile_set_crash_mode(void) {
}

void gc_logfile_vprintf(const char *fmt, va_list ap) {
    (void) fmt;
    (void) ap;
}

BOOL gc_logfile_active(void) {
    return FALSE;
}

const char *gc_logfile_path(void) {
    return NULL;
}

#endif /* GC_SDLOG */

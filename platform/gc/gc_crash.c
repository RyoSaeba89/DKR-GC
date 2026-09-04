/*
 * The crash handler.
 *
 * DKR shipped with one. src/thread0_epc.c runs a priority-0 thread that waits
 * on OS_EVENT_FAULT; when the CPU faults it stops every other thread, writes
 * the faulting OSThread's context and stack to a Controller Pak file called
 * "CORE", and spins. On the *next* boot get_lockup_status finds that file,
 * reads it back, deletes it, and thread3_main renders the register dump on
 * screen -- four pages the developer could photograph.
 *
 * That whole design assumes three things the GameCube does not have: MIPS
 * exception vectors, libultra's fault event, and a Controller Pak. The
 * translation units that implement it (main.c, thread0_epc.c,
 * get_stack_pointer.c) are in GAME_EXCLUDE for exactly that reason, and until
 * now the seven entry points the rest of the game still calls were answered
 * with "never crashed" placeholders in stubs.c.
 *
 * This file is the port of the feature, not a re-stubbing of it, and it keeps
 * the original's shape because that shape is right: catch the fault, write the
 * evidence somewhere that survives the reset, replay it on the next boot.
 * Three substitutions do the whole job.
 *
 *   MIPS vectors      -> the PowerPC exception vectors libogc already installs.
 *   OS_EVENT_FAULT    -> c_default_exceptionhandler, intercepted at link time.
 *   the Controller Pak -> the SD card: a human-readable report appended to
 *                        dkr.log, and a binary record in dkr.crash for the
 *                        next boot to find.
 *
 * ---- How the fault is intercepted --------------------------------------
 *
 * libogc copies a small stub over each PowerPC exception vector at 0x100,
 * 0x200 ... and that stub ends in `mtsrr0; rfi` to whatever address sits in
 * _exceptionhandlertable. The entries are therefore *raw vector routines*, not
 * C functions -- registering a C function through __exception_sethandler would
 * rfi straight into it with no frame saved. (Disassembling exception.o is how
 * that was settled; it is not what the usual homebrew snippet claims.)
 *
 * So the hook goes one level up instead. libogc's own vector routine,
 * default_exceptionhandler, saves the complete frame_context and then calls
 * c_default_exceptionhandler(frame_context *) -- an ordinary C call, from a
 * separate object file, to an undefined symbol. `-Wl,--wrap` catches exactly
 * that: __wrap_c_default_exceptionhandler runs first, writes the report, and
 * then hands the frame to __real_c_default_exceptionhandler so libogc's own
 * on-screen dump still happens. Nothing is reimplemented and nothing is
 * patched at runtime.
 */

#include <ultra64.h>

#include <ogc/context.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include "gc_ultra.h"

#include "macros.h"
#include "memory.h"
#include "printf.h"
#include "thread0_epc.h"

/* ---- the object breadcrumb trail ---------------------------------------- */

/*
 * gObjectStackTrace, kept for real.
 *
 * objects.c writes into it at every spawn, update and draw (nine call sites),
 * and the original's crash screen printed it as "setup 42 control 17" -- which
 * object the game was busy with when it died. It costs one store, it is the
 * only game-level context a register dump has, and stubbing it out was pure
 * loss. Volatile because the reader is an exception handler.
 */
volatile s32 gObjectStackTrace[3] = { -1, -1, -1 };

static const char *const kObjectStage[3] = { "spawn", "update", "draw" };

void update_object_stack_trace(s32 index, s32 value) {
    if (index >= OBJECT_SPAWN && index <= OBJECT_DRAW) {
        gObjectStackTrace[index] = value;
    }
}

/* ---- the stack pointer -------------------------------------------------- */

/*
 * src/get_stack_pointer.c reads MIPS $sp. Here it is r1, and the register is
 * fixed by the ABI rather than by an asm constraint, so this is one
 * instruction and no inline-asm subtleties.
 *
 * memory.c calls it as stack_pointer()->sp, reading the sixth word of the
 * StackInfo it points at -- that is a *cast of the stack itself*, not a struct
 * the port has to build, so returning r1 reproduces it exactly: the value that
 * ends up in the report is the caller's frame plus 20 bytes, which is what the
 * N64 build reported too.
 */
StackInfo *stack_pointer(void) {
    register StackInfo *sp __asm__("r1");

    __asm__ __volatile__("" : "=r"(sp));
    return sp;
}

/* ---- the game thread's stack -------------------------------------------- */

/*
 * thread3_verify_stack, called from thread3_main once per frame.
 *
 * The original incremented a word at each end of the game thread's stack and
 * complained if they ever disagreed -- a wraparound check, on a stack whose
 * size was chosen for MIPS. The port allocates that stack itself and already
 * writes a sentinel below every thread's stack for gc_stack_overflowed(), so
 * the check that means something here is the sentinel, and it covers every
 * thread rather than only this one.
 *
 * It is reported once. A stack overflow is not transient, and a line per frame
 * would push everything else out of a 16 KB log buffer.
 */
void thread3_verify_stack(void) {
    static BOOL sReported;
    s32 id = gc_stack_overflowed();

    if (id >= 0 && !sReported) {
        sReported = TRUE;
        gc_logfile_printf("\n*** STACK OVERFLOW on thread %d ***\n", (int) id);
        gc_logfile_flush();
    }
}

#ifdef GC_DEBUG
/*
 * The pool's health, because a failure in it is completely silent.
 *
 * `mempool_slot_find` reports "No more slots available" and "No suitable block
 * found for allocation" -- through `stubbed_printf`, which `include/types.h`
 * defines as nothing. And the two callers that matter most do not use
 * `mempool_alloc_safe`: `object_model_init` and `texture_load` both call plain
 * `mempool_alloc` and return NULL on failure, silently, leaving the game with a
 * model or a texture that simply is not there.
 *
 * That is exactly the shape of "the menu text and the 2D sprites are missing
 * and nothing in the log says why", so the pool stops being unobservable.
 * Three numbers separate the three ways it fails: slots exhausted (1600 of
 * them, and the allocator refuses *everything* once `curNumSlots + 1` reaches
 * the maximum, which is a cliff and not a slope), space exhausted, and
 * fragmentation -- plenty free in total but no single block big enough, which
 * is the one a total would hide.
 *
 * Walked from the free list rather than the slot array, because the slot array
 * holds unlinked spares.
 */
void gc_pool_report(u32 *slotsUsed, u32 *slotsMax, u32 *freeBytes, u32 *largestFree,
                    u32 *usedBytes) {
    extern MemoryPool gMemoryPools[POOL_COUNT];
    const MemoryPool *pool = &gMemoryPools[POOL_MAIN];
    const MemoryPoolSlot *slots = pool->slots;
    s32 i = 0;
    u32 free = 0, largest = 0, used = 0;
    u32 guard = 0;

    *slotsUsed = (u32) pool->curNumSlots;
    *slotsMax = (u32) pool->maxNumSlots;
    *freeBytes = 0;
    *largestFree = 0;
    *usedBytes = 0;

    if (slots == NULL || pool->maxNumSlots == 0) {
        return;
    }

    /* The list is walked with a bound: this runs once a second on a machine
     * whose whole problem may be corrupt memory, and a broken nextIndex must
     * not turn a diagnostic into a hang. */
    while (i != MEMSLOT_NONE && guard++ < (u32) pool->maxNumSlots) {
        const MemoryPoolSlot *s = &slots[i];

        if (s->flags == SLOT_FREE) {
            free += (u32) s->size;
            if ((u32) s->size > largest) {
                largest = (u32) s->size;
            }
        } else {
            used += (u32) s->size;
        }
        i = s->nextIndex;
    }

    *freeBytes = free;
    *largestFree = largest;
    *usedBytes = used;
}

#endif

/* ---- the crash record ---------------------------------------------------- */

/*
 * What is kept across the reset.
 *
 * The N64 wrote 0x440 bytes to the Controller Pak: an OSThread, 0x200 bytes of
 * the faulting stack and 0x400 bytes of a ring buffer. This is the same idea
 * sized for what is actually readable on screen -- the registers, and enough
 * stack words to recognise a call chain with addr2line.
 *
 * `magic` carries the layout version. A record written by an older build is
 * discarded rather than displayed, because a register dump that is subtly
 * misaligned is worse than none.
 */
#define CRASH_MAGIC 0x444B5243u /* 'DKRC' */
#define CRASH_VERSION 1
#define CRASH_STACK_WORDS 192

#define CRASH_PATH_COUNT 3

static const char *const kCrashPaths[CRASH_PATH_COUNT] = {
    "sd:/dkr/dkr.crash",
    "carda:/dkr/dkr.crash",
    "cardb:/dkr/dkr.crash",
};

typedef struct GcCrashRecord {
    u32 magic;
    u32 version;

    u32 nExcept;
    u32 srr0; /* the faulting instruction */
    u32 srr1; /* MSR as it was */
    u32 dsisr;
    u32 dar; /* the address that could not be accessed */
    u32 cr, lr, ctr, xer, msr, dabr;
    u32 gpr[32];

    s32 objTrace[3];

    /* Set instead of a real exception when mempool_alloc_safe fails: the
     * original's "cause 0xFFFFFFFF, mmAlloc(size, tag)" page. */
    u32 allocFailed;
    u32 allocSize;
    u32 allocTag;

    u32 stack[CRASH_STACK_WORDS];
} GcCrashRecord;

static GcCrashRecord sRecord;

/* Set when a record was read back at boot: the equivalent of the original
 * finding its "CORE" file on the pak. */
static BOOL sHaveCrash;

/* Which of the four pages the display is showing, and the timer that turns
 * them. Same names and same cadence as thread0_epc.c. */
static s32 sLockupPage = EPC_PAGE_REGISTER;
static s32 sLockupDelay;

/* ---- writing the record -------------------------------------------------- */

/*
 * Copy CRASH_STACK_WORDS from the faulting stack, refusing anything that is not
 * a plausible MEM1 address.
 *
 * A crash handler that faults while collecting evidence produces nothing at
 * all, and the stack pointer is one of the things a crash is likely to have
 * ruined, so it is range-checked rather than trusted.
 */
static void capture_stack(GcCrashRecord *rec, u32 sp) {
    u32 i;

    memset(rec->stack, 0, sizeof(rec->stack));
    if (sp < 0x80000000u || sp >= 0x81800000u || (sp & 3) != 0) {
        return;
    }
    for (i = 0; i < CRASH_STACK_WORDS; i++) {
        u32 addr = sp + i * 4;

        if (addr >= 0x81800000u) {
            break;
        }
        rec->stack[i] = *(const volatile u32 *) addr;
    }
}

static const char *exception_name(u32 n) {
    switch (n) {
        case EX_MACH_CHECK: return "machine check";
        case EX_DSI:        return "DSI (bad data address)";
        case EX_ISI:        return "ISI (bad instruction address)";
        case EX_ALIGN:      return "alignment";
        case EX_PRG:        return "program (illegal instruction / trap)";
        case EX_FP:         return "floating point unavailable";
        case EX_DEC:        return "decrementer";
        case EX_SYS_CALL:   return "system call";
        case EX_TRACE:      return "trace";
        case EX_PERF:       return "performance monitor";
        case EX_IABR:       return "instruction breakpoint";
        case EX_THERM:      return "thermal";
        default:            return "unknown";
    }
}

/* ---- formatting without newlib ------------------------------------------- *
 *
 * The report is written with these three functions and not with printf, and
 * that is the whole reason the first four hardware crashes produced no report.
 *
 * A PowerPC exception leaves MSR[FP] clear, and libogc's vector does not set
 * it. newlib's vsnprintf reaches _svfprintf_r, which touches a floating-point
 * register whatever the format string says -- so the very first
 * gc_logfile_printf in this handler took a *second* exception, "Floating Point
 * unavailable", and that second fault overwrote SRR0/SRR1 with its own. The
 * screen then showed the crash handler crashing instead of the crash, and the
 * log stayed empty. The user's photograph is what made it visible:
 * `Exception (Floating Point) occurred!` with `SRR1 00009030` -- no MSR_FP --
 * and a stack of _svfprintf_r inside __wrap_c_default_exceptionhandler.
 *
 * MSR[FP] is now enabled below as well, so printf would in fact work. These
 * stay anyway: the report is the one thing that must not depend on the C
 * library being usable on a machine that has just faulted, and hex digits and
 * a decimal integer are all it needs.
 */
static void crash_puts(const char *s) {
    u32 n = 0;

    while (s[n] != 0) {
        n++;
    }
    gc_logfile_write(s, n);
    /* And to the console, through write() rather than printf -- same reason.
     * This is what makes the crash path testable at all: under Dolphin there is
     * no SD card, so without this the report would exist only on hardware, and
     * an instrument that only exists where it is needed cannot be validated
     * before it is needed. GC_CRASHTEST exercises it. */
    write(1, s, n);
}

static void crash_hex(u32 v) {
    static const char kDigits[] = "0123456789abcdef";
    char out[8];
    int i;

    for (i = 7; i >= 0; i--) {
        out[i] = kDigits[v & 0xF];
        v >>= 4;
    }
    gc_logfile_write(out, 8);
}

static void crash_dec(s32 v) {
    char out[12];
    int i = (int) sizeof(out);
    u32 u = (v < 0) ? (u32) -v : (u32) v;

    do {
        out[--i] = (char) ('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    if (v < 0) {
        out[--i] = '-';
    }
    gc_logfile_write(out + i, (u32) ((int) sizeof(out) - i));
}

/* The human-readable half: written into dkr.log, which is where the user will
 * look. Written before the binary record, because if the card is going to
 * refuse a write it should refuse the less important one. */
static void report_to_log(const GcCrashRecord *rec) {
    u32 i;

    crash_puts("\n=================== CRASH ===================\n");
    if (rec->allocFailed) {
        crash_puts("cause    mempool_alloc_safe failed: ");
        crash_dec((s32) rec->allocSize);
        crash_puts(" bytes, tag 0x");
        crash_hex(rec->allocTag);
        crash_puts("\ncaller   ");
        crash_hex(rec->srr0);
        crash_puts("   <- addr2line this\ninside   ");
        crash_hex(rec->lr);
        crash_puts("\nsp       ");
        crash_hex(rec->dar);
        crash_puts("\n");
    } else {
        crash_puts("cause    exception ");
        crash_dec((s32) rec->nExcept);
        crash_puts(", ");
        crash_puts(exception_name(rec->nExcept));
        crash_puts("\nsrr0     ");
        crash_hex(rec->srr0);
        crash_puts("   <- the faulting instruction, feed to addr2line\nsrr1     ");
        crash_hex(rec->srr1);
        crash_puts("   msr ");
        crash_hex(rec->msr);
        crash_puts("\ndsisr    ");
        crash_hex(rec->dsisr);
        crash_puts("   dar ");
        crash_hex(rec->dar);
        crash_puts("\nlr       ");
        crash_hex(rec->lr);
        crash_puts("   ctr ");
        crash_hex(rec->ctr);
        crash_puts("   cr ");
        crash_hex(rec->cr);
        crash_puts("   xer ");
        crash_hex(rec->xer);
        crash_puts("\n");
    }

    crash_puts("object   ");
    for (i = 0; i < 3; i++) {
        if (rec->objTrace[i] != OBJECT_CLEAR) {
            crash_puts(kObjectStage[i]);
            crash_puts(" ");
            crash_dec(rec->objTrace[i]);
            crash_puts("  ");
        }
    }
    crash_puts("\n");

    for (i = 0; i < 32; i++) {
        crash_puts("r");
        crash_dec((s32) i);
        crash_puts(" ");
        crash_hex(rec->gpr[i]);
        crash_puts(((i & 3) == 3) ? "\n" : "   ");
    }

    /*
     * The stack, raw. There is no symbol table on the console, so this is not a
     * backtrace -- it is the words to feed to
     *   powerpc-eabi-addr2line -e build/gc/dkr.elf <addr>
     * which is how every other address in this port has been resolved. Only
     * words that look like code in MEM1 are worth printing; the rest is data
     * and would bury the ones that matter.
     */
    crash_puts("stack (candidate return addresses, feed to addr2line)\n");
    for (i = 0; i < CRASH_STACK_WORDS; i++) {
        u32 w = rec->stack[i];

        if (w >= 0x80003000u && w < 0x81800000u && (w & 3) == 0) {
            crash_puts("  [");
            crash_dec((s32) i);
            crash_puts("] ");
            crash_hex(w);
            crash_puts("\n");
        }
    }
    crash_puts("=============================================\n");
}

/* The binary half, for the next boot's on-screen replay. Returns whether it
 * reached the card, because the handler has a second attempt to make if not. */
static BOOL record_to_card(const GcCrashRecord *rec) {
    u32 i;

    for (i = 0; i < CRASH_PATH_COUNT; i++) {
        FILE *f = fopen(kCrashPaths[i], "wb");

        if (f != NULL) {
            fwrite(rec, 1, sizeof(*rec), f);
            fclose(f);
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * The second chance, and why there is one.
 *
 * Writing a file from inside an exception handler is the least reliable thing
 * this port does. The first hardware crash proved it: the log stopped at the
 * boot trace and no dkr.crash appeared beside it, because MSR[EE] was clear and
 * libfat cannot work that way. Enabling interrupts (below) should fix that --
 * but "should" is not a measurement, and there is no spare hardware run to find
 * out with.
 *
 * So the report is also handed to the boot thread. The handler sets this flag,
 * tries the write itself, and clears the flag only if it actually reached the
 * card; gc_crash_poll -- called from gc_main's retrace loop, in an ordinary
 * thread context with a healthy stack -- writes it otherwise. The handler then
 * waits briefly for that to happen before letting libogc take the screen.
 *
 * Two independent attempts, and the one that runs in a normal context is the
 * one more likely to work.
 */
static volatile BOOL sCrashPending;

static void write_report(const GcCrashRecord *rec) {
    report_to_log(rec);
    gc_logfile_flush_unlocked();
    if (record_to_card(rec) && gc_logfile_active()) {
        sCrashPending = FALSE;
    }
}

void gc_crash_poll(void) {
    if (sCrashPending) {
        sCrashPending = FALSE; /* one attempt from here, whatever happens */
        write_report(&sRecord);
    }
}

/* ---- the interception ---------------------------------------------------- */

void __real_c_default_exceptionhandler(frame_context *ctx);

void __wrap_c_default_exceptionhandler(frame_context *ctx) {
    static volatile BOOL sInHandler;
    GcCrashRecord *rec = &sRecord;
    u32 msr;
    u32 i;

    /*
     * Re-entrancy. If writing the report faults, the vector runs again and we
     * would loop forever producing nothing. One flag, and the second time
     * through we go straight to libogc's dump, which touches no filesystem.
     */
    if (!sInHandler) {
        sInHandler = TRUE;

        memset(rec, 0, sizeof(*rec));
        rec->magic = CRASH_MAGIC;
        rec->version = CRASH_VERSION;
        rec->nExcept = ctx->nExcept;
        rec->srr0 = ctx->srr0;
        rec->srr1 = ctx->srr1;
        rec->cr = ctx->cr;
        rec->lr = ctx->lr;
        rec->ctr = ctx->ctr;
        rec->xer = ctx->xer;
        rec->msr = ctx->msr;
        rec->dabr = ctx->dabr;
        /* Not in frame_context, and still valid: the vector routine has not
         * executed a load or store that would overwrite them. */
        rec->dsisr = mfspr(18); /* DSISR */
        rec->dar = mfspr(19);   /* DAR: the address the access faulted on */

        for (i = 0; i < 32; i++) {
            rec->gpr[i] = ctx->gpr[i];
        }
        for (i = 0; i < 3; i++) {
            rec->objTrace[i] = gObjectStackTrace[i];
        }
        capture_stack(rec, ctx->gpr[1]);

        /*
         * Interrupts back on before touching the card, and this is the whole
         * reason the first hardware crash produced an empty log.
         *
         * A PowerPC exception clears MSR[EE], and libogc's vector never puts it
         * back -- it does not need to, because everything c_default_exception-
         * handler does (kprintf to a framebuffer console, SI_Sync, PAD_Sync,
         * udelay) is polled. Writing a file is not: libfat takes an LWP mutex
         * per partition, newlib's fopen allocates under another, and the SD
         * card's EXI transfers complete on an interrupt. With EE clear all
         * three either fail or never complete, and the first hardware run
         * showed exactly that -- a log that stopped at "boot: game running"
         * with no CRASH block and no dkr.crash beside it.
         *
         * The risk this takes is real and worth naming: with interrupts on, the
         * scheduler can run other threads while this one is inside the handler,
         * on a machine that has already faulted. That is acceptable because the
         * alternative measured out at zero information, and because the threads
         * that keep running (boot, audio) are the ones whose output we want.
         */
        /* No more locking on the log: see gc_logfile_set_crash_mode. */
        gc_logfile_set_crash_mode();

        /*
         * Interrupts on, and the FPU on with them.
         *
         * A PowerPC exception leaves MSR[FP] clear and libogc's vector does not
         * restore it. Anything that touches a floating-point register from here
         * -- and newlib's vsnprintf does, whatever the format string says --
         * takes a second exception that overwrites SRR0/SRR1 with its own. That
         * is exactly what happened on hardware for four runs: the screen showed
         * `Exception (Floating Point) occurred!` inside _svfprintf_r inside
         * this function, the original fault's address was lost, and the log
         * stayed empty. The report itself is written without printf now (see
         * crash_puts), so this is the belt to that pair of braces.
         *
         * Interrupts stay on so the boot thread can still run gc_crash_poll:
         * libogc's dump never returns, it loops polling the pad, so with them
         * off nothing else would ever run again.
         */
        msr = mfmsr();
        mtmsr(msr | MSR_FP | MSR_EE);

        /*
         * A marker first, flushed on its own. If the machine dies during the
         * report itself, the log still says the handler ran and which
         * exception it was -- which is the difference between "the crash
         * handler does not work" and "the crash handler could not finish".
         */
        /* A marker first, flushed on its own, so that even a machine that dies
         * during the report itself leaves the exception and its address behind. */
        crash_puts("\n*** exception ");
        crash_dec((s32) rec->nExcept);
        crash_puts(" (");
        crash_puts(exception_name(rec->nExcept));
        crash_puts(") at ");
        crash_hex(rec->srr0);
        crash_puts(" ***\n");
        gc_logfile_flush();

        sCrashPending = TRUE;
        write_report(rec);

        /*
         * Interrupts off again before libogc takes the screen -- and this is a
         * correction of a correction, so it is worth saying why twice.
         *
         * They were first left on so that gc_crash_poll on the boot thread
         * could write the report if this handler could not. That reasoning was
         * sound while the handler was unreliable, and it stopped being sound
         * the moment the handler was fixed (MSR[FP] above, no printf on the
         * report path). What it cost, meanwhile, was the evidence: libogc's
         * dump never returns, it loops polling the pad, so with interrupts on
         * every other thread keeps running on a machine that has already
         * faulted -- and the *next* exception draws its own dump over this one.
         *
         * That is exactly what the user photographed: an `Exception
         * (Interrupt)` in the framebuffer console, on the game thread, while
         * `DAR` still held `A4600010` from the DSI that had actually started
         * all this. One crash, one report, nothing running afterwards to
         * overwrite it.
         *
         * FP stays enabled: libogc's own handler is about to format a screenful
         * of registers.
         */
        mtmsr(msr | MSR_FP);
    }

    /* libogc's own dump goes to the screen and the USB Gecko, and it is the
     * only half of this that a user without a card reader can read. */
    __real_c_default_exceptionhandler(ctx);
}

/* ---- the allocation failure --------------------------------------------- */

/*
 * dump_memory_to_cpak, from memory.c's mempool_alloc_safe.
 *
 * It reports and returns. It used to report and halt, and that was wrong -- it
 * is what stopped the game on the console once the two real crashes were fixed.
 *
 * The original (src/thread0_epc.c:160) writes a "CORE" file and then spins, but
 * only `if (get_filtered_cheats() & CHEAT_EPC_LOCK_UP_DISPLAY)`. Without that
 * cheat -- which is to say always, in a normal session -- the whole function is
 * a no-op and mempool_alloc_safe carries on:
 *
 *     void *mempool_alloc_safe(s32 size, u32 colourTag) {   // src/memory.c:100
 *         if (size == 0) dump_memory_to_cpak(...);
 *         addr = mempool_slot_find(POOL_MAIN, size, colourTag);
 *         if (addr == NULL) dump_memory_to_cpak(...);
 *         return addr;
 *     }
 *
 * So a zero-byte request is a *tolerated* condition in this game, not a fatal
 * one, and it happens for real: the fifth hardware run reported
 * `mempool_alloc_safe(0, 0x7f7f7fff)` -- COLOUR_TAG_GREY, a perfectly ordinary
 * tag, from one of asset_loading.c's three call sites, i.e. an asset whose
 * recorded size is zero. On the N64 that goes straight through to
 * mempool_slot_find. Halting on it was a behaviour change this port had no
 * business making, and it cost a run.
 *
 * What is kept is the report, because a genuine out-of-memory is a real
 * port-specific risk (the N64's pool ran to the top of 4 MB of hardware;
 * GC_MAIN_POOL_MB is a number this port chose) and it should not be a silent
 * NULL. It is written once, so a caller in a loop cannot fill the card.
 *
 * And the zero-byte case is now separated out entirely -- one log line, no
 * record, no crash page. It is not a failure: the US 1.0 asset table has three
 * sections of length zero, so it fires every boot, and letting it consume the
 * single report meant a real allocation failure would never have been seen.
 */
void dump_memory_to_cpak(s32 epc, s32 size, u32 colourTag) {
    static BOOL sReported;
    static BOOL sSaidZero;
    GcCrashRecord *rec = &sRecord;
    u32 i;

    /*
     * A zero-byte request is not a failure and must not spend the one report.
     *
     * The asset lookup table in the US 1.0 ROM has three sections of length
     * zero -- SCREENS, EMPTY_14 and EMPTY_37 -- so asset_table_load asks for
     * zero bytes as a matter of course, on every boot, well before anything
     * interesting has happened. Writing the full record for it put a CRASH
     * page on the screen at the next boot for a condition the game tolerates,
     * and, worse, latched `sReported` so that a *genuine* out-of-memory later
     * in the run would have been silent. One log line, and get out of the way.
     */
    if (size == 0) {
        if (!sSaidZero) {
            sSaidZero = TRUE;
            gc_logfile_printf("mm: zero-byte allocation, tag %08lx, from %08lx "
                              "(expected: three asset sections are empty)\n",
                              (unsigned long) colourTag,
                              (unsigned long) (u32) __builtin_return_address(1));
            gc_logfile_flush();
        }
        return;
    }

    if (sReported) {
        return;
    }
    sReported = TRUE;

    memset(rec, 0, sizeof(*rec));
    rec->magic = CRASH_MAGIC;
    rec->version = CRASH_VERSION;
    rec->allocFailed = 1;
    rec->allocSize = (u32) size;
    rec->allocTag = colourTag;

    /*
     * Name the caller properly.
     *
     * The first version recorded `epc`, which mempool_alloc_safe builds as
     * `stack_pointer()->sp` -- the sixth word of the current stack frame. That
     * came out zero on hardware and the report said `caller 00000000`, which is
     * exactly as useless as it sounds. The return addresses are what addr2line
     * can turn into a function name: level 0 is inside mempool_alloc_safe,
     * level 1 is the game code that asked for the allocation.
     */
    rec->srr0 = (u32) __builtin_return_address(1);
    rec->lr = (u32) __builtin_return_address(0);
    rec->dar = (u32) epc;

    for (i = 0; i < 3; i++) {
        rec->objTrace[i] = gObjectStackTrace[i];
    }
    /* From the real stack pointer, not from `epc`. */
    capture_stack(rec, (u32) stack_pointer());

    report_to_log(rec);
    gc_logfile_flush();
    record_to_card(rec);
}

/* ---- the replay on the next boot ---------------------------------------- */

/*
 * Read back a record left by the previous run, and delete it.
 *
 * Deleting on read is the original's behaviour (get_lockup_status calls
 * delete_file once it has the data) and it is the right one: the screen is
 * shown once, for the crash that just happened, and a stale record must not
 * greet the user on every future boot.
 */
void gc_crash_init(void) {
    u32 i;

    for (i = 0; i < CRASH_PATH_COUNT; i++) {
        FILE *f = fopen(kCrashPaths[i], "rb");
        size_t got;

        if (f == NULL) {
            continue;
        }
        got = fread(&sRecord, 1, sizeof(sRecord), f);
        fclose(f);
        remove(kCrashPaths[i]);

        if (got == sizeof(sRecord) && sRecord.magic == CRASH_MAGIC &&
            sRecord.version == CRASH_VERSION) {
            sHaveCrash = TRUE;
            gc_logfile_printf("previous run crashed; showing its report on screen\n");
        }
        return;
    }
}

s32 get_lockup_status(void) {
    return sHaveCrash ? TRUE : FALSE;
}

/*
 * The page timer. Identical to thread0_epc.c: one page a second at 60 updates
 * a second, wrapping through EPC_PAGE_EXIT back to the register page.
 */
void mode_lockup(s32 updateRate) {
    sLockupDelay += updateRate;
    if (sLockupDelay > 60) {
        sLockupDelay = 0;
        sLockupPage++;
        if (sLockupPage > EPC_PAGE_EXIT) {
            sLockupPage = EPC_PAGE_REGISTER;
        }
    }
}

/*
 * The crash screen.
 *
 * Same four pages as the original, with PowerPC in place of MIPS: the fault and
 * the registers, then the stack in three screenfuls. The layout follows
 * thread0_epc.c's rather than being redesigned, because the original chose it
 * for a 640x480 television and that has not changed.
 */
void render_epc_lock_up_display(void) {
    const GcCrashRecord *rec = &sRecord;
    s32 named = FALSE;
    u32 offset;
    u32 i;

    set_render_printf_position(16, 32);

    switch (sLockupPage) {
        case EPC_PAGE_REGISTER:
            if (rec->allocFailed) {
                render_printf(" caller\t\t0x%08x\n", rec->srr0);
                render_printf(" cause\t\tmmAlloc(%d,0x%08x)\n", (s32) rec->allocSize,
                              rec->allocTag);
            } else {
                render_printf(" %s\n", exception_name(rec->nExcept));
                render_printf(" srr0\t\t0x%08x\n", rec->srr0);
                render_printf(" srr1\t\t0x%08x\n", rec->srr1);
                render_printf(" dsisr\t\t0x%08x\n", rec->dsisr);
                render_printf(" dar\t\t0x%08x\n", rec->dar);
                render_printf(" lr\t\t0x%08x  ctr 0x%08x\n", rec->lr, rec->ctr);
            }

            for (i = 0; i < 3; i++) {
                if (rec->objTrace[i] != OBJECT_CLEAR) {
                    if (!named) {
                        named = TRUE;
                        render_printf(" object\t\t");
                    }
                    render_printf("%s %d ", kObjectStage[i], rec->objTrace[i]);
                }
            }
            render_printf("\n");

            for (i = 0; i < 32; i += 3) {
                if (i + 2 < 32) {
                    render_printf(" r%-2u 0x%08x r%-2u 0x%08x r%-2u 0x%08x\n", i, rec->gpr[i],
                                  i + 1, rec->gpr[i + 1], i + 2, rec->gpr[i + 2]);
                } else {
                    render_printf(" r%-2u 0x%08x r%-2u 0x%08x\n", i, rec->gpr[i], i + 1,
                                  rec->gpr[i + 1]);
                }
            }
            break;

        case EPC_PAGE_STACK_TOP:    /* fall through */
        case EPC_PAGE_STACK_MIDDLE: /* fall through */
        case EPC_PAGE_STACK_BOTTOM:
            offset = (u32) (sLockupPage - EPC_PAGE_STACK_TOP) * 48;
            for (i = 0; i < 16 && offset + 32 < CRASH_STACK_WORDS; i++) {
                render_printf("   %08x %08x %08x\n", rec->stack[offset],
                              rec->stack[offset + 16], rec->stack[offset + 32]);
                offset++;
            }
            break;

        case EPC_PAGE_UNK04:
            render_printf(" full report on the card: dkr.log\n");
            render_printf(" resolve addresses with\n");
            render_printf("   powerpc-eabi-addr2line -e dkr.elf <addr>\n");
            break;

        default:
            break;
    }
}

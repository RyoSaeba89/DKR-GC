/*
 * System-level libultra services: the boot globals the game reads, address
 * translation, the interrupt mask, and the diagnostic printf paths.
 */

#include <ultra64.h>

#include <ogc/system.h>
#include <ogc/video.h>
#include <ogc/consol.h>
#include <ogc/machine/processor.h>

#include <stdarg.h>
#include <stdio.h>

#include <ogc/irq.h>

#include "gc_ultra.h"

/* ---- boot globals -------------------------------------------------------- */
/*
 * The game reads osTvType directly (video.c picks the refresh rate and aspect
 * ratio from it, and the ported gc_video.c keeps doing the same), so it has to
 * reflect the real display standard rather than a fixed value. The rest are
 * present because the headers declare them and a handful of debug paths touch
 * them; nothing on this platform depends on their values.
 */
s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0; /* cold reset */
s32 osCicId = 6102;  /* what a retail DKR cartridge reports */
u32 osMemSize = 24 * 1024 * 1024;
void *osRomBase = NULL;
s32 osAppNMIBuffer[16];

void osInitialize(void) {
#if GC_FORCE_PAL
    /* GC_FORCE_PAL: see gc_video.c. libogc reads the TV standard from the SRAM
     * globals, not from the mode that was configured, so forcing the video mode
     * alone would leave the game believing it is on NTSC and the two halves of
     * the port would disagree about the screen height. */
    osTvType = OS_TV_PAL;
    osMemSize = SYS_GetArenaHi() - SYS_GetArenaLo();
    return;
#endif
    switch (VIDEO_GetCurrentTvMode()) {
        case VI_PAL:
        case VI_EURGB60:
            osTvType = OS_TV_PAL;
            break;
        case VI_MPAL:
            osTvType = OS_TV_MPAL;
            break;
        default:
            osTvType = OS_TV_NTSC;
            break;
    }

    osMemSize = SYS_GetArenaHi() - SYS_GetArenaLo();
}

void osExit(void) {
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    for (;;) {
    }
}

u32 osGetMemSize(void) {
    return osMemSize;
}

s32 osAfterPreNMI(void) {
    return 0;
}

/* ---- address translation ------------------------------------------------- */
/*
 * These are the identity on this port, and that is a deliberate choice rather
 * than a shortcut.
 *
 * On the N64 the caller converts a KSEG0 pointer to a physical address because
 * it is about to hand that address to the RSP or the PI, which do not go
 * through the CPU's mapping. The clearest case is audiomgr.c, which converts
 * the audio output buffer before passing it to alAudioFrame so the audio
 * microcode can write samples into it.
 *
 * In this port there is no second processor on the other end: the audio
 * command list is executed by platform/gc/audio on the CPU, and it has to
 * write through a pointer it can actually dereference. Returning a translated
 * address would hand the mixer something unusable. Keeping the conversion an
 * identity means every such round trip stays valid without touching the game
 * code that performs it.
 */
u32 osVirtualToPhysical(void *addr) {
    return (u32) addr;
}

void *osPhysicalToVirtual(u32 addr) {
    return (void *) addr;
}

/* ---- interrupts ---------------------------------------------------------- */
/*
 * The game brackets short critical sections with osSetIntMask(OS_IM_NONE) and
 * a restore of whatever the first call returned. Rather than map the N64's
 * per-source mask onto the Gekko's, this collapses to "all off" versus "back
 * to what they were" -- which is all those sections rely on.
 *
 * What the returned value is matters more than it looks. It is libogc's own
 * IRQ level, handed straight back to IRQ_Restore, so a restore inside an outer
 * critical section leaves interrupts off instead of turning them back on.
 * audiosfx.c nests these, and an earlier version that answered a flat
 * OS_IM_ALL/OS_IM_NONE got that case backwards: the outer section came back to
 * interrupts already enabled, and one that had been entered with them off
 * disabled them a second time and never undid it, which stops the retrace
 * interrupt and with it the entire machine.
 *
 * IRQ_Disable never returns zero for "they were on", so OS_IM_NONE (0) staying
 * the sentinel for "disable" is safe.
 */
OSIntMask osSetIntMask(OSIntMask mask) {
    if (mask == OS_IM_NONE) {
        return (OSIntMask) IRQ_Disable();
    }

    IRQ_Restore((u32) mask);
    return OS_IM_ALL;
}

/* ---- diagnostics --------------------------------------------------------- */

/*
 * Whether printf is worth what it costs.
 *
 * libogc's framebuffer console scrolls by memcpy, and the disassembly of
 * __console_write says how much: `src = destbuffer + stride*16`,
 * `len = stride*con_yres - 16`. On the user's PAL console that is **737 264
 * bytes of uncached memcpy per scrolled line**, into the very framebuffer GX is
 * copying into -- and GC_DEBUG's heartbeat prints about sixty lines a beat.
 *
 * The fifth hardware log measured the result: `clock: 759 ms since last beat`
 * on the first beat, then 1659 ms and 1600 ms once the console had filled and
 * started scrolling. Sixty retraces should be 1200 ms on a 50 Hz console.
 *
 * And after the first frame nobody can read it anyway: GX owns the screen, the
 * text is overwritten immediately, and on this user's machine there is no USB
 * Gecko either (slot B is the SD card -- see gc_main.c). So once the renderer
 * is up and no Gecko is listening, the port stops printing. The SD log is
 * unaffected: it never went through printf.
 */
static BOOL sConsoleWorthIt = TRUE;

void gc_console_set(BOOL on) {
    sConsoleWorthIt = on;
}

BOOL gc_console_on(void) {
    return sConsoleWorthIt;
}

void osSyncPrintf(const char *fmt, ...) {
    va_list ap;

    if (!sConsoleWorthIt) {
        return;
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void rmonPrintf(const char *fmt, ...) {
    va_list ap;

    if (!sConsoleWorthIt) {
        return;
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void osLogEvent(OSLog *log, s16 code, s16 numArgs, ...) {
    (void) log;
    (void) code;
    (void) numArgs;
}

/*
 * Both of these print twice: once to whatever console the build has, and once
 * into the SD log. The second copy is the only one that exists on real
 * hardware, where there is no USB Gecko -- see platform/gc/gc_logfile.c. When
 * no card is mounted the log calls cost a compare and a return.
 */
void gc_fatal(const char *fmt, ...) {
    va_list ap;

    printf("\n*** DKR-GC fatal ***\n");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");

    gc_logfile_printf("\n*** DKR-GC fatal ***\n");
    va_start(ap, fmt);
    gc_logfile_vprintf(fmt, ap);
    va_end(ap);
    gc_logfile_printf("\n");
    /* The machine stops here, so this is the last chance to reach the card. */
    gc_logfile_flush();

    for (;;) {
        VIDEO_WaitVSync();
    }
}

#ifdef GC_DEBUG
void gc_log(const char *fmt, ...) {
    va_list ap;

    if (sConsoleWorthIt) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }

    va_start(ap, fmt);
    gc_logfile_vprintf(fmt, ap);
    va_end(ap);
}
#endif

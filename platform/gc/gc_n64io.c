/*
 * The N64's memory-mapped registers, answered rather than dereferenced.
 *
 * `IO_READ` and `IO_WRITE` (include/PR/rcp.h) dereference a physical N64
 * register address through PHYS_TO_K1 -- 0x04600010 becomes 0xA4600010. On the
 * GameCube the Gekko BATs cover 0x80000000-0x8FFFFFFF and 0xC0000000-0xCFFFFFFF
 * and there is no page table, so that address is unmapped and the load faults.
 *
 * platform/gc/include/PR/rcp.h redirects both macros here. The comment there
 * carries the diagnosis; the short version is that the user's console was
 * faulting with `DAR A4600010` inside `cam_init`, and Dolphin never reproduced
 * it because it does not emulate the MMU for homebrew.
 *
 * ---- What the answers are, and where they come from -----------------------
 *
 * Every value below is what a real cartridge would have returned, for the same
 * reason `D_B0000578` in stubs.c holds the real ROM word: the game's checks
 * should pass because the answer is right, not because they were patched out.
 *
 * An address this file does not know is answered with zero and counted. That
 * counter is the point -- an unimplemented register must show up in the
 * heartbeat as a number, not on a television as an exception.
 */

#include <ultra64.h>

#include "gc_ultra.h"

/*
 * Register addresses, spelt out rather than taken from PR/rcp.h.
 *
 * This file is on the libogc side of the type split and cannot include the
 * game's headers cleanly, and the constants are three lines. Each is named with
 * the macro it corresponds to so a reader can check it against include/PR/rcp.h.
 */
#define N64_SP_DMEM_START 0x04000000u /* SP_DMEM_START */
#define N64_SP_IMEM_START 0x04001000u /* SP_IMEM_START */
#define N64_PI_STATUS_REG 0x04600010u /* PI_BASE_REG + 0x10 */

/* Cartridge words the anti-piracy checks compare against, from the US 1.0 ROM.
 * All four sites are inside `#ifdef ANTI_TAMPER`, which Makefile.gc does not
 * define, so none of them is compiled today -- they are answered anyway so that
 * turning the flag on is not a new way to crash. */
#define N64_ROM_0X200 0x00000200u /* src/tracks.c:344 */
#define N64_ROM_0X284 0x00000284u /* src/object_functions.c:3433 */

#ifdef GC_DEBUG
u32 gGcIoReads;
u32 gGcIoWrites;
u32 gGcIoUnknown;
u32 gGcIoLastUnknown;
#define IO_COUNT(v) ((v)++)
#else
#define IO_COUNT(v) ((void) 0)
#endif

unsigned int gc_io_read(unsigned int physAddr) {
    IO_COUNT(gGcIoReads);

    switch (physAddr) {
        /*
         * The PI is idle, always.
         *
         * WAIT_ON_IOBUSY spins until (status & (DMA_BUSY | IO_BUSY)) is clear.
         * There is no Peripheral Interface here and no cartridge DMA in flight
         * -- the port's osPiStartDma is an ARQ transfer that has already
         * completed by the time it returns -- so "idle" is not a convenient
         * lie, it is the truth about this machine. Returning anything else
         * would hang the caller in that spin loop forever.
         */
        case N64_PI_STATUS_REG:
            return 0;

        /*
         * drm_validate_dmem wants DMEM's first word to be all ones, and
         * drm_validate_imem wants IMEM's to be the CIC id. Both are properties
         * of a booted N64, and osCicId in ultra/os_system.c already reports the
         * same 6102 a retail cartridge does.
         */
        case N64_SP_DMEM_START:
            return 0xFFFFFFFFu;

        case N64_SP_IMEM_START:
            return 6102u;

        case N64_ROM_0X200:
            return 0xAC290000u;

        case N64_ROM_0X284:
            return 0x240B17D7u;

        default:
            IO_COUNT(gGcIoUnknown);
#ifdef GC_DEBUG
            gGcIoLastUnknown = physAddr;
#endif
            return 0;
    }
}

/*
 * Writes are dropped.
 *
 * Nothing in the compiled tree writes an N64 register -- the RSP, RDP and PI
 * status writes all live in translation units the port excludes or in libultra
 * sources it does not build. This exists so that the macro pair stays a pair,
 * and so that a write that does appear one day is counted rather than faulting.
 */
void gc_io_write(unsigned int physAddr, unsigned int value) {
    (void) value;
    IO_COUNT(gGcIoWrites);
    switch (physAddr) {
        case N64_PI_STATUS_REG:
            break;
        default:
            IO_COUNT(gGcIoUnknown);
#ifdef GC_DEBUG
            gGcIoLastUnknown = physAddr;
#endif
            break;
    }
}

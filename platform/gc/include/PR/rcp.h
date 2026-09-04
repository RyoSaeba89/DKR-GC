#ifndef GC_PR_RCP_H
#define GC_PR_RCP_H

/*
 * GameCube shadow for PR/rcp.h: the N64 hardware registers.
 *
 * platform/gc/include comes before include/ on the search path, so every
 * `#include <PR/rcp.h>` in the tree lands here. This file takes the real header
 * whole and then replaces two macros.
 *
 * ---- Why -----------------------------------------------------------------
 *
 * `IO_READ(addr)` is `*(vu32 *) PHYS_TO_K1(addr)` -- a dereference of an N64
 * memory-mapped register, at an address like 0xA4600010. On the GameCube the
 * Gekko BATs cover 0x80000000-0x8FFFFFFF and 0xC0000000-0xCFFFFFFF and there is
 * no page table, so 0xA4600010 is unmapped and the load is a DSI.
 *
 * That is not hypothetical. It is what the user's console had been faulting on:
 * the crash screen showed `DAR A4600010` -- PI_BASE_REG + 0x10, PI_STATUS_REG,
 * spelt through PHYS_TO_K1 -- with the game thread inside `cam_init`, which
 * does `WAIT_ON_IOBUSY(stat)` (src/camera.c:148) before reading the cartridge
 * word its anti-piracy check wants. `WAIT_ON_IOBUSY` (include/PRinternal/
 * piint.h:127) spins on IO_READ(PI_STATUS_REG) until the PI reports idle, and
 * the very first read faulted.
 *
 * It never showed up under Dolphin, which does not emulate the MMU for homebrew
 * and simply returns data for unmapped reads.
 *
 * ---- Why here rather than in the game -------------------------------------
 *
 * The port does not modify src/. Redirecting the two macros keeps every
 * existing call site intact and puts the decision where the rest of the
 * hardware substitutions already live -- and it catches accesses this port has
 * not thought about, instead of letting them fault: gc_io_read counts every
 * address it does not recognise, so the heartbeat says which N64 registers the
 * game reached for rather than leaving it to be discovered on a television.
 *
 * `PHYS_TO_K1` itself is left alone. It is used for ordinary addresses too, and
 * only these two macros dereference what it produces.
 */

#include_next <PR/rcp.h>

#ifndef _LANGUAGE_ASSEMBLY

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in platform/gc/gc_n64io.c. Declared here rather than pulled in from
 * gc_ultra.h because this header is reached from game translation units that
 * must not see the platform layer's internals. */
unsigned int gc_io_read(unsigned int physAddr);
void gc_io_write(unsigned int physAddr, unsigned int value);

#ifdef __cplusplus
}
#endif

#undef IO_READ
#undef IO_WRITE

#define IO_READ(addr) gc_io_read((unsigned int) (addr))
#define IO_WRITE(addr, data) gc_io_write((unsigned int) (addr), (unsigned int) (data))

#endif /* !_LANGUAGE_ASSEMBLY */

#endif /* GC_PR_RCP_H */

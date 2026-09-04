/**
 * The last symbols standing between the port and a linked binary.
 *
 * Everything here falls into one of three buckets, and the section comments say
 * which. Some of it is finished work; most of it is a placeholder that exists so
 * the link succeeds and the game can be run and watched. Nothing in this file is
 * silently wrong -- each placeholder is written to fail in the safe direction
 * (report "absent", return zero, do nothing) rather than to look plausible.
 *
 * Grep for PORT-TODO to find what still has to be written.
 */
#include <ogc/irq.h>

#include "macros.h"
#include "memory.h"
#include "objects.h"
#include "types.h"
#include "ultra64.h"

/* ===================================================================== *
 * Done: libultra primitives that libogc already provides
 * ===================================================================== */

/* The N64 pair for "briefly make this critical section atomic". libogc's IRQ
 * calls have the same shape, including returning the previous level so that
 * nesting works. */
u32 __osDisableInt(void) {
    return IRQ_Disable();
}

void __osRestoreInt(u32 level) {
    IRQ_Restore(level);
}

/* The anti-piracy check in camera.c reads the cartridge at 0xB0000578 and wants
 * 0x8965 in the low half. This is that word, taken from the US 1.0 ROM, so the
 * check passes for the reason it was meant to rather than by being patched out. */
s32 D_B0000578 = 0x34218965;

/* ===================================================================== *
 * Inert by design: N64 hardware with no GameCube counterpart
 * ===================================================================== */

/* Signal-processor and display-processor status registers. The port never runs
 * RSP or RDP tasks -- os_sched.c intercepts the tasks and services them on the
 * CPU and GX -- so there is no register here to write and nothing to emulate. */
void __osSpSetStatus(UNUSED u32 status) {
}

void osDpSetStatus(UNUSED u32 status) {
}

/* Microcode images. On the N64 these bracket blobs of RSP code copied into IMEM.
 * The port replaces what they did (F3DDKR with GX, the audio microcode with the
 * software mixer), so they are never loaded; they exist only because the task
 * structures the game fills in still name them. Kept one element wide rather
 * than zero, which is not valid C. */
long long int rspbootTextStart[1];
long long int rspbootTextEnd[1];
long long int rspF3DDKRXbusStart[1];
long long int rspF3DDKRDataXbusStart[1];
long long int aspMainTextStart[1];
long long int aspMainDataStart[1];

/* The buffer the RSP would spill a pre-empted graphics task into. Nothing
 * pre-empts anything here, but the task struct still points at it. */
u8 gGfxTaskYieldData[OS_YIELD_DATA_SIZE];

/* End of the N64 ROM image. The port's assets live in ARAM, not in a ROM the
 * CPU can address, so there is no meaningful value. */
u8 *__ROM_END = NULL;

/* libultra/src/gu/libm_vals.s, which is MIPS. cosf.c and sinf.c return this for
 * arguments outside their domain. */
float __libm_qnan_f = 0.0f / 0.0f;

/* ===================================================================== *
 * Done: the Controller Pak
 * ===================================================================== *
 *
 * Every osPfs* entry point used to return PFS_ERR_NOPACK from here, so the game
 * ran and simply never offered to save a ghost. They now live in
 * platform/gc/ultra/os_pfs.c, which emulates the pak's API over a 32 KB image
 * kept in one GameCube memory card file (platform/gc/gc_storage.c, with the SD
 * card behind it). The heading stays to record that the category is closed.
 */

/* ===================================================================== *
 * Done: the crash handler
 * ===================================================================== *
 *
 * main.c, thread0_epc.c and get_stack_pointer.c are in GAME_EXCLUDE because
 * they are the N64 boot path and the MIPS exception handler, so the seven
 * entry points the rest of the game still calls had to come from somewhere.
 * They were placeholders here -- "never locked up", a NULL stack pointer, a
 * breadcrumb trail that recorded nothing -- until 2026-09-04.
 *
 * They now live in platform/gc/gc_crash.c, which ports the feature rather than
 * answering for it: the PowerPC exception vectors take the place of the MIPS
 * ones, libogc's c_default_exceptionhandler is intercepted at link time, and
 * the SD card takes the place of the Controller Pak the report used to be
 * written to. The heading is kept, like the one below it, to record that the
 * category is closed.
 */

/* ===================================================================== *
 * PORT-TODO: hand-written MIPS with no C anywhere in the repo
 * ===================================================================== *
 *
 * src/hasm holds six .s files; only collision.s and math_util.s have C beside
 * them. The functions that existed solely as MIPS have all been rewritten from
 * the assembly now, and nothing in this category is left:
 *
 *   obj_animate                        -> src/hasm/obj_animate.c
 *   obj_shade_fast                     -> src/hasm/obj_shade_fast.c
 *   calc_dynamic_lighting_for_object_2 -> src/hasm/obj_shade_fast.c
 *
 * Keeping the heading is deliberate: it records that this class of gap is
 * closed, so a later reader does not go looking for a stub that is no longer
 * here.
 */

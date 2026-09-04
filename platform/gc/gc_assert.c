/*
 * `__assert`, with the argument order the game actually uses.
 *
 * This is not a convenience. Linking the game's assertions against newlib's
 * `__assert` is a latent crash on every one of them, and one of them fires.
 *
 * The decompilation declares (libultra/src/debug/assert.h):
 *
 *     void __assert(const char *exp, const char *filename, int line);
 *
 * newlib declares (powerpc-eabi/include/assert.h):
 *
 *     void __assert(const char *file, int line, const char *failedexpr);
 *
 * Same name, three arguments each, **different order**. With no definition of
 * its own the port resolved the game's calls to newlib's function, so
 * `__assert("samples >= 0", "env.c", 104)` arrived as file = "samples >= 0",
 * line = the address of "env.c" printed as a decimal, and failedexpr = the
 * integer 104 -- which newlib then hands to `fiprintf("%s")`. A read of address
 * 104. And having printed, newlib's assert calls `abort()`.
 *
 * There are exactly three such calls in the linked binary, all in
 * libultra/src/audio/env.c (lines 107, 109 and 378), and the decompilation says
 * outright what they are: "Something must have gone wrong when compiling this
 * file, and the asserts got left in." They are written as
 * `if (cond) {} else { __assert(...); }` rather than through the macro, so
 * -DNDEBUG does not remove them.
 *
 * env.c runs on the audio thread, once per voice per audio frame. The sixth
 * hardware run crashed there with `__console_write` and a 686 KB memcpy on the
 * stack -- the console cost of newlib's assert message -- so at least one of
 * the three is firing.
 *
 * ---- What this does instead -----------------------------------------------
 *
 * Records the failure in the SD log, once per site, and **returns**.
 *
 * Returning is deliberate, and it is the second time today this port has had to
 * learn it: making a condition fatal that the original tolerated is a behaviour
 * change, and `dump_memory_to_cpak` halting on a zero-byte allocation already
 * cost a hardware run. An assertion that "got left in" is a diagnostic, not a
 * contract -- the retail cartridge ran this code. Log it and let the game
 * continue; the log says which one fired, which is the thing worth knowing.
 *
 * Once per site because the audio thread would otherwise write to the card
 * fifty times a second.
 */

#include <ultra64.h>

#include <string.h>

#include "gc_ultra.h"

#define ASSERT_SITES 8

#ifdef GC_DEBUG
u32 gGcAsserts;
#endif

void __assert(const char *exp, const char *filename, int line) {
    static const char *sSeen[ASSERT_SITES];
    static u32 sCount;
    u32 i;

#ifdef GC_DEBUG
    gGcAsserts++;
#endif

    if (exp == NULL) {
        exp = "?";
    }
    if (filename == NULL) {
        filename = "?";
    }

    /* Identified by the expression's address: each call site passes a distinct
     * string literal, so pointer equality is exactly "this site again". */
    for (i = 0; i < sCount; i++) {
        if (sSeen[i] == exp) {
            return;
        }
    }
    if (sCount < ASSERT_SITES) {
        sSeen[sCount++] = exp;
    }

    gc_logfile_mark("assert: %s failed (%s:%d)\n", exp, filename, line);
}

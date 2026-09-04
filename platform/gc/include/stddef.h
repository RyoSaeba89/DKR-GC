/**
 * stddef.h shim -- restores the N64 definition of NULL.
 *
 * The game was written against libultra, where include/PR/ultratypes.h defines
 * NULL as plain 0. GCC's own stddef.h, which the libogc headers pull in after
 * it, does `#undef NULL` / `#define NULL ((void *)0)` and wins. The game then
 * stops compiling, because it uses NULL as a general zero: `return NULL` from a
 * function returning s32, `{ NULL }` initialising an enum field, and so on.
 * Some of those are only warnings, but initialising an enum from void * is a
 * hard error no -Wno- flag can turn off.
 *
 * platform/gc/include comes first on the include path, so this file shadows the
 * compiler's stddef.h everywhere, defers to it for everything real, and then
 * puts NULL back the way the game expects. 0 is a valid null pointer constant
 * in C, so pointer use is unaffected; both machines are 32-bit, so passing it
 * through varargs is too.
 */
#include_next <stddef.h>

#undef NULL
#define NULL 0

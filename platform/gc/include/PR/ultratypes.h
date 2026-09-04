#ifndef _ULTRATYPES_H_
#define _ULTRATYPES_H_

/*
 * GameCube replacement for PR/ultratypes.h.
 *
 * platform/gc/include comes before include/ on the search path, so every
 * `#include <PR/ultratypes.h>` in the tree lands here instead of the N64
 * header. Nothing else in include/PR is shadowed.
 *
 * Why this file has to exist at all: the N64 header spells the fixed-width
 * types in terms of `long` (u32 == unsigned long), while libogc's <gctypes.h>
 * spells them in terms of <stdint.h> (u32 == unsigned int). Both are 32 bits
 * on this ABI, but they are *different types* to the compiler, so a single
 * translation unit that pulls in both headers fails to compile on the
 * duplicate typedefs. Since the platform layer has to talk to libogc and the
 * game at the same time, one of the two spellings has to give, and libogc's
 * is the one the linked-against library was built with.
 *
 * The practical consequence for game code is limited to printf-style format
 * checking (%lu vs %u). The game routes its own text through src/printf.c,
 * which does not go through GCC's format attribute, so nothing warns.
 */

#include <gctypes.h>
#include <stddef.h> /* size_t; the N64 header defined it inline instead */

/* <gctypes.h> stops short of these three. */
typedef volatile float  vf32;
typedef volatile double vf64;

#endif /* _ULTRATYPES_H_ */

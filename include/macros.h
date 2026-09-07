#ifndef _MACROS_H_
#define _MACROS_H_

#include <PRinternal/macros.h>

#ifndef __sgi
#define GLOBAL_ASM(...)
#endif

#if !defined(__sgi) && (!defined(NON_MATCHING) || !defined(AVOID_UB))
// asm-process isn't supported outside of IDO, and undefined behavior causes
// crashes.
// #error Matching build is only possible on IDO; please build with NON_MATCHING=1.
#endif

#define ARRAY_COUNT(arr) (s32)(sizeof(arr) / sizeof(arr[0]))

#define GLUE(a, b) a ## b
#define GLUE2(a, b) GLUE(a, b)

// Avoid undefined behaviour for non-returning functions
#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

// Static assertions
#ifdef __GNUC__
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define STATIC_ASSERT(cond, msg) typedef char GLUE2(static_assertion_failed, __LINE__)[(cond) ? 1 : -1]
#endif

// Align to 8-byte boundary for DMA requirements
#ifdef __GNUC__
#define ALIGNED8 __attribute__((aligned(8)))
#else
#define ALIGNED8
#endif

// Align to 16-byte boundary for audio lib requirements
#ifdef __GNUC__
#define ALIGNED16 __attribute__((aligned(16)))
#else
#define ALIGNED16
#endif

// Use built-in pseudocode where possible in NON_MATCHING builds
#if defined(__GNUC__) && !defined(CC_CHECK)
#define abs(x)      __builtin_abs(x)
#define fabsf(x)    __builtin_fabsf(x)
#define sqrtf(f)    __builtin_sqrtf(f)
#undef ABS
#undef ABSF
#define ABS(x)      __builtin_abs(x)
#define ABSF(x)     __builtin_fabsf(x)
#else
#define inline
#define ABSF(x) (x < 0.f ? -x : x)
#define ABS(x) ((x) >= 0 ? (x) : -(x))
#endif


// convert a virtual address to physical.
#define VIRTUAL_TO_PHYSICAL(addr)   ((uintptr_t)(addr) & 0x1FFFFFFF)

// convert a physical address to virtual.
#define PHYSICAL_TO_VIRTUAL(addr)   ((uintptr_t)(addr) | 0x80000000)

// another way of converting virtual to physical
#define VIRTUAL_TO_PHYSICAL2(addr)  ((u8 *)(addr) - 0x80000000U)

// Used to suppress warnings in the ./generate_ctx.sh script.
#define INCONSISTENT 

// Used to make a u32 colour value look clearer. Transforms 0xFF0000FF to 255, 0, 0, 255
#define COLOUR_RGBA32(r, g, b, a) ((u32)((r << 24) | (g << 16) |  (b << 8) | a))

// A few systems in the game use an array as a cache table. This gives you the asset ID
#define ASSETCACHE_ID(x)    ((x << 1) + 0)
// A few systems in the game use an array as a cache table. This gives you the pointer to the asset
#define ASSETCACHE_PTR(x)   ((x << 1) + 1)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/*
 * A left shift the way MIPS does one.
 *
 * `sllv` takes the count from the low five bits of the register, so `1 << 32`
 * is `1 << 0` on the N64 -- and the game leans on that. objects.c writes
 * `tajFlags |= 1 << (j + 31)` with j in 1..3 to set bits 0..2, and game.c does
 * the same to remember which world-key cutscene has played.
 *
 * PowerPC's `slw` reads six bits of the count and produces *zero* for anything
 * from 32 up. So on the GameCube those flags were never set: Taj kept offering
 * a challenge already won, and the key cutscene played every time the hub was
 * entered. Nothing was wrong with the save -- the bit never reached it.
 *
 * Only the GameCube build is changed. The N64 build must keep matching, and
 * there the shift already behaves this way.
 */
#ifdef TARGET_GC
#define MIPS_SHL(value, count) ((value) << ((count) & 31))
#else
#define MIPS_SHL(value, count) ((value) << (count))
#endif

#endif

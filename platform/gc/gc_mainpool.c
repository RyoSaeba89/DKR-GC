/*
 * The game's heap.
 *
 * On the N64, `gMainMemoryPool` was not an object: it was a linker symbol
 * parked at the end of BSS, and mempool_init_main took everything from there
 * up to RAM_END (0x80400000, the top of the console's 4 MB) as the pool. That
 * works because nothing else on the machine owns memory -- there is no
 * allocator underneath the game.
 *
 * On the GameCube there is. libogc's arena starts at the end of BSS and backs
 * malloc, the framebuffers, the FAT cache and every thread stack. Handing the
 * game "end of BSS up to the top of RAM" would hand it the arena as well, and
 * the two would quietly write over each other.
 *
 * So the pool stops being the leftovers of the address space and becomes an
 * ordinary BSS object of a fixed size. libogc's arena then begins above it on
 * its own, with no runtime negotiation: the linker has already kept them
 * apart. RAM_END follows from the object rather than being a constant, which
 * is what the #ifdef in src/memory.h is for.
 *
 * memory.h declares `extern MemoryPoolSlot gMainMemoryPool;` -- a single slot,
 * because on the N64 the declaration only had to name an address. C cannot
 * define an object larger than its declared type, so the storage is defined
 * here, in the one file that deliberately never sees that declaration, and the
 * two meet at link time exactly as they did before.
 */

#include <ultra64.h>

#ifndef GC_MAIN_POOL_MB
#define GC_MAIN_POOL_MB 4
#endif

#define GC_MAIN_POOL_SIZE (GC_MAIN_POOL_MB * 1024 * 1024)

/* 32-byte aligned: the pool hands out buffers that become DMA destinations for
 * asset reads, and the ARAM engine wants that alignment to take the fast path
 * in gc_assets_read. mempool_init only guarantees 16. */
u8 gMainMemoryPool[GC_MAIN_POOL_SIZE] __attribute__((aligned(32)));

/*
 * What RAM_END means here. The N64's constant described the top of the
 * machine; this describes the top of the pool, which is the only thing the
 * game ever used it for -- mempool_init_main sizes the pool with it, and
 * audiomgr.c places its fixed audio heap just below it.
 */
u32 gc_ram_end(void) {
    return (u32) gMainMemoryPool + sizeof(gMainMemoryPool);
}

/*
 * Cache maintenance.
 *
 * On the N64 these calls exist so the RSP and RDP, which read main memory
 * without going through the CPU cache, see what the CPU just wrote. Almost all
 * of them are therefore about display lists and audio command lists, and in
 * this port neither is read by a coprocessor: the display list is walked by
 * platform/gc/gfx on the CPU, and the audio command list by platform/gc/audio,
 * both through the normal cached mapping.
 *
 * That makes osWritebackDCacheAll a no-op here, which matters for more than
 * tidiness. The game calls it once per graphics task, and the GameCube has no
 * flush-everything instruction -- honouring it literally would mean walking
 * all 24 MB of MEM1 through dcbf every frame, several milliseconds of pure
 * waste.
 *
 * The memory that genuinely does need flushing is what the graphics processor
 * DMAs: the GX FIFO and the vertex and texture buffers. That is all produced
 * inside platform/gc/gfx, which flushes it at the point it is handed over.
 * Range calls are still honoured, because they are cheap and because a caller
 * that named a range usually meant it.
 */

#include <ultra64.h>

#include <ogc/cache.h>

void osWritebackDCacheAll(void) {
}

void osWritebackDCache(void *addr, s32 nbytes) {
    if (nbytes > 0) {
        DCFlushRange(addr, (u32) nbytes);
    }
}

/*
 * Invalidate, without throwing away a neighbour's data.
 *
 * The N64 callers invalidate a DMA destination whose address and length obey
 * no alignment rule at all: asset_loading.c's dmacopy is handed
 * `objMdl + modelSize - compressedSize` and a byte count straight out of the
 * asset table. On MIPS that was free -- the range was rounded to cache lines
 * and the machine had one owner for all of memory.
 *
 * Here it is not free. `dcbi` discards a whole 32 byte line *without writing
 * it back*, and the two lines at the ends of an unaligned range are shared
 * with whatever the allocator handed out on either side. Invalidating them
 * silently reverts up to 31 bytes of a live, recently written neighbour to
 * whatever main memory held before -- a corruption that leaves no trace, that
 * happens on every asset load, and that an emulator with no cache model can
 * never reproduce.
 *
 * So the two partial lines are flushed instead (`dcbf` writes back *and*
 * invalidates, which is correct for both owners) and only the lines this range
 * covers outright are discarded.
 */
void osInvalDCache(void *addr, s32 nbytes) {
    u32 start, end, firstWhole, lastWhole;

    if (nbytes <= 0) {
        return;
    }

    start = (u32) addr;
    end = start + (u32) nbytes;
    firstWhole = (start + 31) & ~31u; /* start of the first fully covered line */
    lastWhole = end & ~31u;           /* end of the last fully covered line */

    if (firstWhole >= lastWhole) {
        /* Shorter than a line, or straddling one boundary: every line is
         * shared, so write all of them back. */
        DCFlushRange((void *) (start & ~31u), ((end + 31) & ~31u) - (start & ~31u));
        return;
    }

    if (start != firstWhole) {
        DCFlushRange((void *) (start & ~31u), 32);
    }
    if (end != lastWhole) {
        DCFlushRange((void *) lastWhole, 32);
    }
    DCInvalidateRange((void *) firstWhole, lastWhole - firstWhole);
}

void osInvalICache(void *addr, s32 nbytes) {
    if (nbytes > 0) {
        ICInvalidateRange(addr, (u32) nbytes);
    }
}

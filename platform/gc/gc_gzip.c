/*
 * Asset decompression, replacing src/gzip.c.
 *
 * This is the one module the port replaces for a reason that is neither
 * hardware nor endianness: the work is all in MIPS. src/gzip.c holds the
 * Huffman table builder and the driver in C, but `gzip_inflate_block` -- the
 * inflate loop itself -- exists only as 781 lines of assembly in
 * src/hasm/gzip_asm.s, with no C counterpart anywhere in the repository. It is
 * also not optional in the way the other missing assembly is: with it stubbed
 * out, `gzip_inflate` spins zero times and every compressed asset comes back
 * empty. Fonts, textures, models and tracks are all compressed, so the game
 * runs its whole loop over nothing. That is what an empty screen looks like
 * from the inside.
 *
 * The original is Mark Adler's public-domain inflate -- src/gzip.c's
 * `gzip_huft_build` is that code verbatim, down to the variable names -- and
 * it decodes an ordinary DEFLATE stream. zlib is already in devkitPro's
 * portlibs and already linked, so rather than translate the assembly this file
 * hands the stream to zlib and keeps the four entry points the rest of the
 * game calls. Everything else in src/gzip.c exists only to serve the inflate
 * loop and goes away with it.
 *
 * The container. src/gzip.c documents it in one line -- "The compression
 * header is 5 bytes" -- and the two call sites agree on what is in it:
 * asset_loading.c reads the uncompressed size with `byteswap32` from the same
 * pointer it later hands to `gzip_inflate`, so the first four bytes are that
 * size, stored little-endian, and the raw DEFLATE stream begins at byte five.
 * It is not a gzip file: there is no 1f 8b magic and no trailing CRC, which is
 * why zlib is asked for a raw stream (-MAX_WBITS) rather than a gzip one.
 */

#include <ultra64.h>

#include <zlib.h>

#include <string.h>

#include "asset_loading.h"
#include "gc_ultra.h"
#include "gzip.h"
#include "memory.h"

/* Bytes of container in front of the DEFLATE stream: a little-endian
 * uncompressed size and one byte the game never reads. */
#define GZIP_HEADER_SIZE 5

/* Scratch for gzip_size_uncompressed, which DMAs eight bytes of an asset in
 * order to read the size out of the front of it. Allocated from the game's
 * pool at init, exactly as src/gzip.c did. */
static s32 *sPackedHeader;

#ifdef GC_DEBUG
/*
 * Decompressions that worked, and decompressions that did not.
 *
 * The log only ever spoke when inflate failed, so "no gzip line this run" was
 * indistinguishable from "the code path never ran". A positive count is the
 * statement that matters, and it is worth having next to the offline result:
 * every compressed asset in the US 1.0 ROM -- 136 object maps, 55 level models,
 * 390 object models, 470 animations, 906 2D textures and 1393 3D textures,
 * 3350 in all -- was decompressed offline with exactly the container this file
 * implements, and all 3350 came back at their stated length. So a failure here
 * cannot be the format. It can only be a buffer or a pointer.
 */
u32 gGcGzipOk;
u32 gGcGzipFail;
#endif

/*
 * How far past the end of the compressed data zlib may look.
 *
 * The callers know how big the compressed blob is; gzip_inflate is not told.
 * Since inflate stops at the end-of-stream symbol rather than at the end of
 * the buffer, the bound only has to be an upper limit it will never actually
 * reach. Compressed is smaller than uncompressed for every asset in this game,
 * and a stored block costs five bytes per 64 KB, so the uncompressed size plus
 * a page is comfortably past the end of the stream and still inside the pool
 * the caller allocated.
 */
#define INPUT_SLACK 0x1000

void gzip_init(void) {
    sPackedHeader = (s32 *) mempool_alloc_safe(0x10, COLOUR_TAG_BLACK);
}

/*
 * Reads a 32-bit little-endian value a byte at a time.
 *
 * Kept because three other translation units call it, and kept byte-wise
 * rather than as a load-and-swap because the pointer it is given has no
 * alignment guarantee: textures_sprites.c passes the address of a field
 * inside a header read straight out of the asset image.
 */
s32 byteswap32(u8 *arg0) {
    s32 value;

    value = *arg0++;
    value |= (*arg0++ << 8);
    value |= (*arg0++ << 16);
    value |= (*arg0 << 24);
    return value;
}

/* The uncompressed size of an asset, read out of the front of it. */
s32 gzip_size_uncompressed(s32 assetIndex, s32 assetOffset) {
    asset_load(assetIndex, (u32) sPackedHeader, assetOffset, 8);
    return byteswap32((u8 *) sPackedHeader);
}

/*
 * Where the bytes in front of gzip_inflate were supposed to come from, and
 * what is at that place in ARAM right now.
 *
 * The eighth hardware run reported `head dd 4a dd 4a dd 0a` -- and that six
 * byte sequence does not occur anywhere in the 12 MB ROM, which was checked
 * offline. So the buffer does not hold the wrong asset; it holds something
 * that never came out of the asset image at all. Three things can produce
 * that, and an address alone separates none of them:
 *
 *   - the read that should have filled the buffer never happened, or landed
 *     somewhere else -- then no entry in the ring covers this address;
 *   - it happened and ARAM answers correctly now -- then the bytes were right
 *     when they were fetched and something overwrote them, or the cache is
 *     handing back a stale line;
 *   - it happened and ARAM answers with the same rubbish -- then the image in
 *     ARAM is wrong at that offset, and the boot-time verification says
 *     whether it is wrong everywhere.
 *
 * Re-reading is safe here: the failure path is already the slow path, and
 * gc_assets_read is reentrant.
 */
static void report_source(const u8 *at) {
    u32 rom, dst, len, seq, slow;
    u8 fresh[32] __attribute__((aligned(32)));
    u32 skew, i;
    char hex[3 * 16 + 1];

    if (!gc_assets_find_read((u32) at, &rom, &dst, &len, &seq, &slow)) {
        gc_log("gzip: no asset read covers %08x (%lu reads served) -- "
               "nothing ever filled this buffer\n",
               (unsigned) (u32) at, (unsigned long) gc_assets_read_seq());
        return;
    }

    skew = (u32) at - dst;
    gc_log("gzip: filled by read #%lu: rom %08lx -> %08lx, %lu bytes, %s path; "
           "src is +%lu into it, so rom %08lx\n",
           (unsigned long) seq, (unsigned long) rom, (unsigned long) dst,
           (unsigned long) len, slow ? "bounce" : "direct", (unsigned long) skew,
           (unsigned long) (rom + skew));

    gc_assets_read(rom + skew, fresh, sizeof(fresh));
    for (i = 0; i < 16; i++) {
        static const char kDigits[] = "0123456789abcdef";

        hex[i * 3 + 0] = kDigits[fresh[i] >> 4];
        hex[i * 3 + 1] = kDigits[fresh[i] & 0xF];
        hex[i * 3 + 2] = ' ';
    }
    hex[16 * 3] = '\0';
    gc_log("gzip: aram now says %s\n", hex);
}

/*
 * Decompresses one asset in place of the microcode-era inflate loop.
 *
 * Returns the output pointer it was given, the way the original did, because
 * callers use it as the result of the whole operation. A failure cannot be
 * reported through that signature and the game has no path for one anyway, so
 * a broken stream leaves the output buffer as it was and says so through
 * gc_log; under a normal build it degrades to an asset that did not load,
 * which is what the stub did for every asset.
 */
u8 *gzip_inflate(u8 *compressedInput, u8 *decompressedOutput) {
    z_stream strm;
    u32 uncompressedSize;
    int rc;

    if (compressedInput == NULL || decompressedOutput == NULL) {
        return decompressedOutput;
    }

    uncompressedSize = (u32) byteswap32(compressedInput);
    if (uncompressedSize == 0) {
        return decompressedOutput;
    }

    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *) (compressedInput + GZIP_HEADER_SIZE);
    strm.avail_in = uncompressedSize + INPUT_SLACK;
    strm.next_out = (Bytef *) decompressedOutput;
    strm.avail_out = uncompressedSize;

    /* A negative window size is zlib's way of saying "raw DEFLATE, no
     * container": there is no zlib header to parse and no checksum to verify,
     * which is exactly what sits after this asset's five-byte prefix. */
    rc = inflateInit2(&strm, -MAX_WBITS);
    if (rc != Z_OK) {
        gc_log("gzip: inflateInit2 failed (%d)\n", rc);
        return decompressedOutput;
    }

    rc = inflate(&strm, Z_FINISH);
#ifdef GC_DEBUG
    if (rc == Z_STREAM_END || strm.total_out == uncompressedSize) {
        gGcGzipOk++;
    } else {
        gGcGzipFail++;
    }
#endif
    if (rc != Z_STREAM_END && strm.total_out != uncompressedSize) {
        /*
         * The source address and the caller, not just the bytes.
         *
         * The seventh hardware run reported `inflate -3, 2 of 1256016605 bytes,
         * head dd 4a dd 4a dd 0a`: 1256016605 is 0x4ADD4ADD, which is the same
         * repeating pattern as the head, so the four bytes read as the
         * uncompressed size are part of the data rather than a size field. That
         * says the buffer holds something other than the asset expected here --
         * but not *where* it came from, and without an address there is nothing
         * to compare against the ROM. Both are one argument each.
         */
        gc_log("gzip: inflate %d, %lu of %lu bytes, src %08x, from %08x, "
               "head %02x %02x %02x %02x %02x %02x\n",
               rc, (unsigned long) strm.total_out, (unsigned long) uncompressedSize,
               (unsigned) (u32) compressedInput, (unsigned) (u32) __builtin_return_address(0),
               compressedInput[0], compressedInput[1], compressedInput[2], compressedInput[3],
               compressedInput[4], compressedInput[5]);
        report_source(compressedInput);
    }

    inflateEnd(&strm);
    return decompressedOutput;
}

/*
 * The asset image, linked into the executable.
 *
 * The port's first plan was to read dkr.assets off an SD card, which is how it
 * would ship for Swiss on real hardware. That cannot be tested under Dolphin:
 * its GameCube EXI device list has no SD card entry at all (Dolphin's SD
 * support is Wii-only), so sd:/ and carda:/ can never resolve in GC mode and
 * the game stops at fatInitDefault before it has run a single frame.
 *
 * Linking the image in removes the question of where it comes from. It costs
 * 12 MB of MEM1's 24, which is affordable precisely because the image does not
 * stay there: gc_assets_open_embedded streams it into ARAM through the same
 * path the SD version uses, so both routes converge on one reader and the SD
 * one keeps working untouched.
 *
 * .incbin rather than a generated C array: the assembler emits the bytes
 * directly instead of a 12 MB source file that would have to be parsed. The
 * path arrives as ASSETS_BLOB from the Makefile because the build directory is
 * not fixed.
 *
 * The section is .rodata.dkr_assets so it lands inside .rodata with everything
 * else read-only, which keeps it below BSS and therefore out of libogc's
 * arena. Nothing has to be reserved at runtime.
 */

#ifndef ASSETS_BLOB
#error "ASSETS_BLOB must name the asset image to link in"
#endif

    .section .rodata.dkr_assets, "a", @progbits
    .balign 32

    .globl gc_assets_blob
gc_assets_blob:
    .incbin ASSETS_BLOB

    .balign 32
    .globl gc_assets_blob_end
gc_assets_blob_end:

    .size gc_assets_blob, gc_assets_blob_end - gc_assets_blob

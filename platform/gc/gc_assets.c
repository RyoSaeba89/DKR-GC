/*
 * The asset image: getting it into ARAM and reading pieces back out.
 *
 * See platform/gc/ultra/os_pi.c for why the image lives in ARAM rather than in
 * main memory. This file covers what that choice costs.
 *
 * Getting it there. There are two sources and they converge immediately. The
 * image can be linked into the executable (platform/gc/assets_blob.S), which
 * is the only route that works under Dolphin, or read off an SD card, which is
 * how it ships for Swiss on hardware. Either way it ends up in ARAM through
 * the same upload, so the reader below has one code path to be right about.
 * The SD route streams through a bounce buffer a chunk at a time, so its peak
 * main-memory cost is the chunk rather than the image.
 *
 * Getting pieces back out. The ARAM DMA engine works in 32 byte units and
 * wants both addresses aligned to 32 bytes. The game's transfers respect no
 * such rule -- an asset can start at any 8 byte boundary and be any length --
 * so a read whose offset, destination or length is misaligned is widened to
 * the enclosing aligned window, DMA'd into a bounce buffer, and the requested
 * bytes copied out. Aligned reads, which are the common case for the large
 * transfers that matter, go straight to the caller's buffer with no copy.
 *
 * A note on AR_Alloc. libogc's ARAM allocator only exists if AR_Init is handed
 * an array to track blocks in; called as AR_Init(NULL, 0) -- "I will manage
 * ARAM myself" -- AR_Alloc has nowhere to record anything. This port allocates
 * exactly one region, for the lifetime of the process, so it takes the second
 * option outright and uses AR_GetBaseAddress/AR_GetSize directly.
 */

#include <ultra64.h>

#include <ogc/aram.h>
#include <ogc/arqueue.h>
#include <ogc/cache.h>
#include <ogc/mutex.h>

#include <dirent.h>
#include <fat.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "gc_ultra.h"

#define ARAM_ALIGN 32
#define ALIGN_DOWN(x) ((x) & ~(ARAM_ALIGN - 1))
#define ALIGN_UP(x) (((x) + ARAM_ALIGN - 1) & ~(ARAM_ALIGN - 1))

/* Large enough that streaming the image off a card is dominated by the card
 * rather than by per-chunk overhead, small enough to be a rounding error
 * against the game's heap. */
#define LOAD_CHUNK (256 * 1024)

/* Sized for the largest misaligned read the game makes in one call. Anything
 * larger is split. */
#define BOUNCE_SIZE (64 * 1024)

static u32 sAramBase;
static u32 sAramSize;
static u8 *sBounce;

/* The unpadded image, and a checksum of it taken from the source side while it
 * was being uploaded. gc_assets_verify reads the image back out of ARAM and
 * folds it the same way, which is the only way to tell "the bytes in ARAM are
 * wrong" apart from "the bytes were right and something else went wrong". */
static u32 sImageSize;
static u32 sSrcSum;
static const char *sImageRoute = "none";

/* Additive with a rotate, so a transposition or a repeated block still moves
 * it. It is a smoke test for a 12 MB DMA, not a CRC. */
static u32 fold(u32 sum, const u8 *p, u32 len) {
    u32 i;

    for (i = 0; i < len; i++) {
        sum = ((sum << 1) | (sum >> 31)) + p[i];
    }
    return sum;
}

/*
 * The lock on the read path, and why it has to exist.
 *
 * gc_assets_read has two callers on two threads. The game thread pulls tracks,
 * models and textures through it. The *audio* thread pulls ADPCM sample data
 * through it as well, from __amDMA in src/audiomgr.c -- once per voice that
 * needs new samples, up to fifty times per audio frame -- and that thread runs
 * at priority 12 against the game thread's 10, so it preempts.
 *
 * Both of them go down the slow path, because the slow path is not the
 * exception it looks like: the audio fetches use a ROM offset that __amDMA
 * only ever rounds down to an *even* address, never to 32 bytes. So the two
 * threads were taking turns inside a single shared `sBounce`, with no
 * serialisation at all: DMA into it, then memcpy out of it, with a preemption
 * point in between.
 *
 * What that does is not subtle once it is written down. The game thread DMAs a
 * model into sBounce; the audio thread preempts and overwrites sBounce with
 * sample data; the game thread resumes and copies *audio samples* into its
 * vertex buffer. Corrupt geometry, corrupt normals, corrupt headers -- wrong
 * shading, and a crash once a bad pointer is finally dereferenced. In the other
 * direction the audio thread's samples are replaced by asset bytes, which is
 * heard as crackle.
 *
 * The aram_dma helper below already reasons its way to exactly this hazard for
 * the ARQRequest structure and puts it on the stack. The same reasoning was
 * simply never carried across to the buffer.
 *
 * A mutex rather than a per-thread buffer: ARQ is one engine and serialises the
 * transfers anyway, so the only thing being added is making the DMA and the
 * copy that reads it one indivisible step.
 */
static mutex_t sReadLock = LWP_MUTEX_NULL;

#ifdef GC_DEBUG
/* Reads, how many took the shared-buffer path, and how many arrived to find
 * the lock already held. The last one is the measurement that says whether the
 * race above was real traffic or a theoretical hazard. */
u32 gGcAssetReads;
u32 gGcAssetSlow;
u32 gGcAssetContended;
#endif

/*
 * The last few reads, kept so that a RAM address can be turned back into the
 * ROM offset it was filled from.
 *
 * gzip_inflate is handed a pointer and nothing else. When the stream at that
 * pointer is not a stream, the question that decides everything is "what was
 * this buffer supposed to contain, and did the read that filled it ever
 * happen" -- and neither the address nor the caller answers it. This ring
 * does: it names the read whose destination covers the address, so the offset
 * can be compared against the ROM and, more usefully, re-read from ARAM on the
 * spot.
 *
 * Kept unconditionally rather than under GC_DEBUG. It is four words per read
 * into a fixed array and no I/O, and its whole value is being there when
 * something has already gone wrong.
 */
#define READ_LOG_COUNT 32

typedef struct GcAssetRead {
    u32 rom;
    u32 dst;
    u32 len;
    u32 seq;
    u32 slow;
} GcAssetRead;

static GcAssetRead sReadLog[READ_LOG_COUNT];
static u32 sReadSeq;

static void read_log_add(u32 rom, u32 dst, u32 len, u32 slow) {
    u32 seq = ++sReadSeq;
    GcAssetRead *e = &sReadLog[seq & (READ_LOG_COUNT - 1)];

    e->rom = rom;
    e->dst = dst;
    e->len = len;
    e->slow = slow;
    e->seq = seq;
}

/*
 * The newest recorded read whose destination covers `addr`.
 *
 * Returns FALSE when no read in the ring wrote there, which is itself an
 * answer: it means the transfer the caller believes it made either never
 * happened or landed somewhere else.
 */
BOOL gc_assets_find_read(u32 addr, u32 *rom, u32 *dst, u32 *len, u32 *seq, u32 *slow) {
    const GcAssetRead *best = NULL;
    u32 i;

    for (i = 0; i < READ_LOG_COUNT; i++) {
        const GcAssetRead *e = &sReadLog[i];

        if (e->seq == 0 || e->len == 0) {
            continue;
        }
        if (addr < e->dst || addr >= e->dst + e->len) {
            continue;
        }
        if (best == NULL || e->seq > best->seq) {
            best = e;
        }
    }

    if (best == NULL) {
        return FALSE;
    }
    *rom = best->rom;
    *dst = best->dst;
    *len = best->len;
    *seq = best->seq;
    *slow = best->slow;
    return TRUE;
}

/* How many reads have been served, for "was this read even the most recent
 * thing to touch that buffer". */
u32 gc_assets_read_seq(void) {
    return sReadSeq;
}

/*
 * One ARAM transfer.
 *
 * The request structure is a stack local rather than a static: ARQ_PostRequest
 * writes the whole request into it and then blocks, and asset reads come from
 * the game thread and from the scheduler both, so a shared one could be
 * overwritten mid-transfer. It must not be NULL -- libogc dereferences it
 * immediately, and on this machine address 0 is ordinary RAM, so the corruption
 * would be silent.
 */
static void aram_dma(u32 dir, u32 aramAddr, void *mramAddr, u32 len) {
    ARQRequest req;

    ARQ_PostRequest(&req, 0, dir, ARQ_PRIO_HI, aramAddr, (u32) mramAddr, len);
}

/*
 * Claims ARAM for an image of `size` bytes and prepares the bounce buffer.
 *
 * The bottom of ARAM belongs to the operating system, which is what
 * AR_GetBaseAddress reports past; everything from there up is ours, and the
 * image is placed at the start of it.
 */
static void aram_reserve(u32 size) {
    u32 usable;

    AR_Init(NULL, 0);
    ARQ_Init();

    sAramBase = AR_GetBaseAddress();
    usable = AR_GetSize() - sAramBase;
    sAramSize = ALIGN_UP(size);

    if (sAramSize > usable) {
        gc_fatal("ARAM has no room for a %lu byte asset image (%lu available)",
                 (unsigned long) sAramSize, (unsigned long) usable);
    }

    if (sBounce == NULL) {
        sBounce = memalign(ARAM_ALIGN, BOUNCE_SIZE);
        if (sBounce == NULL) {
            gc_fatal("out of memory staging the asset image");
        }
    }

    /* The lock guarding sBounce, created alongside it. Both are set up here,
     * before the game or the audio thread exists, so the read path never has to
     * race to create it. */
    if (sReadLock == LWP_MUTEX_NULL) {
        if (LWP_MutexInit(&sReadLock, FALSE) != 0) {
            gc_fatal("could not create the asset read lock");
        }
    }
}

/*
 * The linked-in image. Present only when the build embedded one; the weak
 * declarations let the same source serve a build that did not, where both
 * symbols come out zero and the function reports "nothing embedded".
 */
extern const u8 gc_assets_blob[] __attribute__((weak));
extern const u8 gc_assets_blob_end[] __attribute__((weak));

BOOL gc_assets_open_embedded(void) {
    u32 size;

    if (gc_assets_blob == NULL || gc_assets_blob_end == NULL) {
        return FALSE;
    }

    size = (u32) (gc_assets_blob_end - gc_assets_blob);
    if (size == 0) {
        return FALSE;
    }

    aram_reserve(size);

    /* The DMA reads main memory behind the cache. The image is read-only and
     * was written by the loader rather than by us, but it still has to be
     * pushed out of the data cache before the engine looks at it. */
    DCFlushRange((void *) gc_assets_blob, ALIGN_UP(size));
    aram_dma(ARQ_MRAMTOARAM, sAramBase, (void *) gc_assets_blob, ALIGN_UP(size));

    sImageSize = size;
    sSrcSum = fold(0, gc_assets_blob, size);
    sImageRoute = "embedded";
    return TRUE;
}

/*
 * Mount the SD card, once.
 *
 * fatInitDefault registers a device per interface, so calling it twice is not
 * free and not obviously idempotent. It also used to be called only from
 * gc_assets_open, which meant an embedded-assets build never mounted anything
 * -- and then the EEPROM file in ultra/os_eeprom.c and the log in gc_logfile.c
 * had no filesystem to open, silently, on the very builds the user runs on
 * hardware. So the mount is its own operation with its own answer, and gc_main
 * performs it before anything that might want a card.
 */
/*
 * One lock for everything that talks to an EXI card, and the reason it exists.
 *
 * Until 2026-09-04 exactly one thread ever touched a filesystem, at boot, and
 * the question did not arise. Now three do: the boot thread flushes the log,
 * the game thread reads and writes the save through gc_storage.c, and the game
 * thread also drops breadcrumbs into the log. libfat's SD Gecko driver and
 * libogc's CARD_* both drive the same EXI channels, and the user's reader is in
 * slot B -- the same slot the log is written to.
 *
 * That is a lot of new concurrency introduced in one build, and that build
 * crashed on hardware within 100 ms of the game entering the Controller Pak
 * code, with the crash handler itself unable to write. "Two threads inside
 * libfat" explains both halves at once, so the whole of it is serialised here.
 *
 * The lock is not fine-grained and does not need to be: a card access is
 * milliseconds and happens at save points and once a second, never per frame.
 */
static mutex_t sFsLock = LWP_MUTEX_NULL;

void gc_fs_lock(void) {
    if (sFsLock != LWP_MUTEX_NULL) {
        LWP_MutexLock(sFsLock);
    }
}

void gc_fs_unlock(void) {
    if (sFsLock != LWP_MUTEX_NULL) {
        LWP_MutexUnlock(sFsLock);
    }
}

/*
 * Which EXI slots libfat has taken.
 *
 * `carda:` is slot A and `cardb:` is slot B, so a device libfat mounted is a
 * device CARD_* must never probe or mount: the SD Gecko the game booted from
 * would be handed to the memory card driver mid-transfer. gc_storage.c reads
 * this before it touches a slot.
 *
 * Probed by opening the volume's root, which is the cheapest question libfat
 * answers truthfully about "is this device there".
 */
static u32 sFatSlots;

u32 gc_fat_slots(void) {
    return sFatSlots;
}

BOOL gc_fat_mount(void) {
    static BOOL sTried;
    static BOOL sMounted;

    if (!sTried) {
        sTried = TRUE;
        if (sFsLock == LWP_MUTEX_NULL) {
            LWP_MutexInit(&sFsLock, FALSE);
        }
        sMounted = fatInitDefault() ? TRUE : FALSE;

        if (sMounted) {
            static const char *const kVolumes[2] = { "carda:/", "cardb:/" };
            u32 i;

            for (i = 0; i < 2; i++) {
                DIR *d = opendir(kVolumes[i]);

                if (d != NULL) {
                    closedir(d);
                    sFatSlots |= 1u << i;
                }
            }
        }
    }
    return sMounted;
}

BOOL gc_assets_open(const char *path) {
    FILE *f;
    u8 *chunk;
    long size;
    u32 offset;

    if (!gc_fat_mount()) {
        return FALSE;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    aram_reserve((u32) size);
    sImageSize = (u32) size;
    sSrcSum = 0;
    sImageRoute = path;

    chunk = memalign(ARAM_ALIGN, LOAD_CHUNK);
    if (chunk == NULL) {
        fclose(f);
        gc_fatal("out of memory staging the asset image");
    }

    for (offset = 0; offset < (u32) size; offset += LOAD_CHUNK) {
        u32 want = (u32) size - offset;
        if (want > LOAD_CHUNK) {
            want = LOAD_CHUNK;
        }

        if (fread(chunk, 1, want, f) != want) {
            fclose(f);
            free(chunk);
            gc_fatal("short read at offset %lu of the asset image", (unsigned long) offset);
        }

        sSrcSum = fold(sSrcSum, chunk, want);
        DCFlushRange(chunk, ALIGN_UP(want));
        aram_dma(ARQ_MRAMTOARAM, sAramBase + offset, chunk, ALIGN_UP(want));
    }

    free(chunk);
    fclose(f);
    return TRUE;
}

/*
 * Read the whole image back out of ARAM and compare it with what was uploaded.
 *
 * Worth the ~12 MB of DMA it costs, once, at boot. Everything downstream --
 * every asset offset, every decompression, every texture -- is built on the
 * assumption that ARAM holds the ROM byte for byte, and that assumption has
 * never actually been checked. When a buffer turns out to hold bytes that do
 * not occur anywhere in the ROM, this line is what says whether the image or
 * the path to it is at fault, and it says so before any hypothesis is formed.
 */
void gc_assets_verify(void) {
    u32 sum = 0;
    u32 offset;

    if (sAramSize == 0 || sImageSize == 0) {
        gc_logfile_printf("aram: no image to verify\n");
        return;
    }

    for (offset = 0; offset < sImageSize; offset += BOUNCE_SIZE) {
        u32 want = sImageSize - offset;
        u32 span;

        if (want > BOUNCE_SIZE) {
            want = BOUNCE_SIZE;
        }
        span = ALIGN_UP(want);

        DCInvalidateRange(sBounce, span);
        aram_dma(ARQ_ARAMTOMRAM, sAramBase + offset, sBounce, span);
        sum = fold(sum, sBounce, want);
    }

    gc_logfile_printf("aram: image %lu bytes from %s, src sum %08lx, readback %08lx -- %s\n",
                      (unsigned long) sImageSize, sImageRoute, (unsigned long) sSrcSum,
                      (unsigned long) sum, (sum == sSrcSum) ? "identical" : "*** DIFFERENT ***");
}

void gc_assets_close(void) {
    sAramBase = 0;
    sAramSize = 0;
    free(sBounce);
    sBounce = NULL;
}

void gc_assets_read(u32 romOffset, void *dst, u32 len) {
    u32 dstAddr = (u32) dst;

    if (sAramSize == 0) {
        gc_fatal("asset read before the image was opened");
    }
    if (romOffset + len > sAramSize) {
        gc_fatal("asset read past the end of the image (offset %lu, %lu bytes)",
                 (unsigned long) romOffset, (unsigned long) len);
    }

#ifdef GC_DEBUG
    gGcAssetReads++;
#endif

    /* Fast path: everything already lines up, so the DMA lands directly in the
     * caller's buffer. No shared state, so no lock. */
    if ((romOffset % ARAM_ALIGN) == 0 && (dstAddr % ARAM_ALIGN) == 0 && (len % ARAM_ALIGN) == 0) {
        read_log_add(romOffset, dstAddr, len, 0);
        DCInvalidateRange(dst, len);
        aram_dma(ARQ_ARAMTOMRAM, sAramBase + romOffset, dst, len);
        return;
    }

    read_log_add(romOffset, dstAddr, len, 1);

#ifdef GC_DEBUG
    gGcAssetSlow++;
    if (sReadLock != LWP_MUTEX_NULL && LWP_MutexTryLock(sReadLock) != 0) {
        gGcAssetContended++;
    } else if (sReadLock != LWP_MUTEX_NULL) {
        LWP_MutexUnlock(sReadLock);
    }
#endif

    /*
     * Slow path: widen to the enclosing aligned window and copy out the part
     * that was asked for, in pieces the bounce buffer can hold. The DMA and the
     * copy that reads it back are one step -- see the note on sReadLock.
     *
     * The lock is taken per window, not around the whole loop. A level load
     * reads a model of a megabyte or two through here, and holding the lock
     * for the whole of it kept the audio thread -- which needs the same buffer
     * for a few hundred bytes of ADPCM per voice -- waiting for the entire
     * transfer plus its memcpy. The log measured that as `gap 17..19 cb`: the
     * producer silent for ninety to a hundred milliseconds against a 64 ms
     * ring, three times a run, every time at a level load. Releasing between
     * windows bounds the wait at one 64 KB window, about two milliseconds.
     */
    while (len > 0) {
        u32 alignedStart = ALIGN_DOWN(romOffset);
        u32 skew = romOffset - alignedStart;
        u32 window = BOUNCE_SIZE - ARAM_ALIGN;
        u32 take = (len < window) ? len : window;
        u32 span = ALIGN_UP(skew + take);

        if (sReadLock != LWP_MUTEX_NULL) {
            LWP_MutexLock(sReadLock);
        }
        DCInvalidateRange(sBounce, span);
        aram_dma(ARQ_ARAMTOMRAM, sAramBase + alignedStart, sBounce, span);
        memcpy((u8 *) dst, sBounce + skew, take);
        if (sReadLock != LWP_MUTEX_NULL) {
            LWP_MutexUnlock(sReadLock);
        }

        romOffset += take;
        dst = (u8 *) dst + take;
        len -= take;
    }
}

/*
 * Persistent storage: the GameCube memory card, with the SD card behind it.
 *
 * The game saves two things, through two completely different N64 devices:
 *
 *   - the cartridge EEPROM, 512 bytes, holding records and settings
 *     (ultra/os_eeprom.c);
 *   - the Controller Pak, 32 KB, holding ghost data as up to sixteen "notes"
 *     (ultra/os_pfs.c).
 *
 * Neither device exists here, and both are the same thing on a GameCube: a
 * named blob on a memory card. So the two save paths meet at this file, which
 * knows nothing about either format -- it reads and writes a fixed-size blob by
 * name, and that is the whole interface.
 *
 * Two backends, tried in that order:
 *
 * 1. **The memory card**, through libogc's CARD_* API. This is the native
 *    place for a save and the only one that exists on an unmodified console.
 *    Both slots are tried, because a user with a card in slot B should not have
 *    to know that the port looked only in A.
 *
 * 2. **A file on the SD card**, which is what the port used before and what
 *    keeps working under Swiss with no memory card plugged in at all. It is
 *    also the only route that survives if the card is full: the fallback is
 *    per-operation, not chosen once at boot.
 *
 * A memory card write is slow (tens of milliseconds, and it stops the EXI bus),
 * so nothing here is called from the render or audio path -- the game saves at
 * menu transitions, which is where it always did. Every entry point takes
 * gc_fs_lock: the boot thread may be flushing the log to the same card.
 *
 * GC_MEMCARD=0 turns the whole file off -- reads and writes fail, and nothing
 * probes or mounts anything. The EEPROM then lives in RAM for the session and
 * the Controller Pak reports absent, which is precisely where the port stood
 * before 2026-09-04. It is the isolation build for a hardware failure that has
 * to be told apart from "the storage subsystem is new".
 *
 * ---- Why the blob is not one card file per ghost -----------------------
 *
 * A GameCube memory card allocates in 8 KB blocks, and its directory holds 127
 * entries. DKR's ghosts are a few kilobytes each and it wants sixteen of them.
 * One card file per ghost would burn 128 KB of a 512 KB card and fill the
 * user's save list with sixteen entries for one game. So the Controller Pak is
 * emulated whole, in ultra/os_pfs.c, and lands here as a single 32 KB blob --
 * one save file, the size the pak actually was.
 */

#include <ultra64.h>

#include <ogc/card.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>

#include <stdio.h>
#include <string.h>

#include "gc_ultra.h"

/*
 * The identity the card writes into the file's directory entry. Four
 * characters and two, as the hardware requires. "GDKP" reads as Diddy Kong
 * racing, Port; "HB" is the convention homebrew uses so that a save from this
 * port is not mistaken for one written by a retail disc.
 */
#define CARD_GAMECODE "GDKP"
#define CARD_COMPANY "HB"

/*
 * CARD_Mount needs CARD_WORKAREA (40 KB) of 32-byte-aligned scratch, and
 * CARD_Read/CARD_Write need a 32-byte-aligned buffer whose length is a multiple
 * of CARD_READSIZE. Both are static: the game's heap belongs to the game, and
 * an allocation failure inside the save path would be a poor way to discover
 * that the pool is tight.
 *
 * GC_STORAGE_MAX bounds the blob, and the Controller Pak image is the largest
 * of the two at 32 KB.
 */
#define GC_STORAGE_MAX (32 * 1024)

static u8 sWorkArea[CARD_WORKAREA] __attribute__((aligned(32)));
static u8 sBounce[GC_STORAGE_MAX] __attribute__((aligned(32)));

static BOOL sCardInited;

#define ROUND_UP(v, a) (((v) + (a) -1) & ~((u32) (a) -1))

/* ---- the memory card ----------------------------------------------------- */

static void card_init_once(void) {
    if (!sCardInited) {
        sCardInited = TRUE;
        CARD_Init(CARD_GAMECODE, CARD_COMPANY);
    }
}

/*
 * Is there a real memory card in this slot?
 *
 * CARD_ProbeEx, not CARD_Probe. libogc defines CARD_Probe as a plain branch to
 * EXI_Probe -- verified by disassembling card.o -- so it answers "some EXI
 * device is attached", which on this user's hardware is very likely an SD
 * Gecko in slot B rather than a memory card. CARD_ProbeEx reads the device's
 * identity and returns CARD_ERROR_WRONGDEVICE for anything that is not a card.
 *
 * Getting that wrong would not have lost data -- the SD fallback below would
 * still have caught the write -- but osPfsIsPlug answers the game with this,
 * and telling the game a Controller Pak is plugged in because a card *reader*
 * is plugged in is the kind of near-miss that is very hard to diagnose later.
 */
static BOOL card_slot_has_card(s32 chn) {
    s32 memSize = 0;
    s32 sectorSize = 0;

    /*
     * Never touch a slot libfat is using, and this is not caution -- it is the
     * hazard the 2026-09-04 hardware crash pointed at.
     *
     * `carda:` is slot A and `cardb:` is slot B. The user boots from an SD
     * Gecko in slot B and the log is written to `cardb:`, so probing slot B
     * with CARD_* hands libogc's memory card driver the EXI channel that
     * libfat's SD driver owns and is actively using. There is no memory card
     * there to find, and the only possible outcome is disturbing the volume the
     * game and the log both depend on.
     */
    if (gc_fat_slots() & (1u << chn)) {
        return FALSE;
    }

    card_init_once();
    return CARD_ProbeEx(chn, &memSize, &sectorSize) == CARD_ERROR_READY;
}

/*
 * Mount a slot, or say why not.
 *
 * The probe comes first because CARD_Mount on an empty slot is an EXI round
 * trip that costs milliseconds, and this runs on both slots for every read and
 * write.
 */
static BOOL card_mount(s32 chn) {
    if (!card_slot_has_card(chn)) {
        return FALSE;
    }
    return CARD_Mount(chn, sWorkArea, NULL) >= 0;
}

static BOOL card_read_blob(s32 chn, const char *name, void *buf, u32 size) {
    card_file file;
    u32 padded = ROUND_UP(size, CARD_READSIZE);
    BOOL ok = FALSE;

    if (CARD_Open(chn, name, &file) < 0) {
        return FALSE;
    }
    /* Read into the aligned bounce buffer: CARD_Read requires both the length
     * and the destination to be aligned, and the caller's buffer is whatever
     * the game happened to give us. */
    if (CARD_Read(&file, sBounce, padded, 0) >= 0) {
        memcpy(buf, sBounce, size);
        ok = TRUE;
    }
    CARD_Close(&file);
    return ok;
}

static BOOL card_write_blob(s32 chn, const char *name, const void *buf, u32 size) {
    card_file file;
    u32 sectorSize = 0;
    u32 padded;
    BOOL created = FALSE;
    BOOL ok = FALSE;

    if (CARD_GetSectorSize(chn, &sectorSize) < 0 || sectorSize == 0) {
        sectorSize = 8192;
    }

    if (CARD_Open(chn, name, &file) < 0) {
        /* A file has to be a whole number of the card's sectors. 32 KB of pak
         * image on an 8 KB-sector card is four blocks, which is what a retail
         * game with ghost data would have taken too. */
        if (CARD_Create(chn, name, ROUND_UP(size, sectorSize), &file) < 0) {
            return FALSE;
        }
        created = TRUE;
    }

    padded = ROUND_UP(size, CARD_READSIZE);
    memset(sBounce, 0, padded);
    memcpy(sBounce, buf, size);
    DCFlushRange(sBounce, padded);

    if (CARD_Write(&file, sBounce, padded, 0) >= 0) {
        ok = TRUE;
    }
    CARD_Close(&file);

    if (!ok && created) {
        /* Do not leave an empty save behind that a later read would find and
         * take for real data. */
        CARD_Delete(chn, name);
    }
    return ok;
}

/* ---- the SD fallback ----------------------------------------------------- */

static void sd_path(char *out, u32 outSize, const char *name) {
    snprintf(out, outSize, "sd:/dkr/%s", name);
}

/* Both called with gc_fs_lock already held by the entry point above. */
static BOOL sd_read_blob(const char *name, void *buf, u32 size) {
    char path[64];
    FILE *f;
    size_t got;

    if (!gc_fat_mount()) {
        return FALSE;
    }
    sd_path(path, sizeof(path), name);
    f = fopen(path, "rb");
    if (f == NULL) {
        return FALSE;
    }
    got = fread(buf, 1, size, f);
    fclose(f);
    return got == size;
}

static BOOL sd_write_blob(const char *name, const void *buf, u32 size) {
    char path[64];
    FILE *f;
    size_t put;

    if (!gc_fat_mount()) {
        return FALSE;
    }
    sd_path(path, sizeof(path), name);
    f = fopen(path, "wb");
    if (f == NULL) {
        return FALSE;
    }
    put = fwrite(buf, 1, size, f);
    fclose(f);
    return put == size;
}

/* ---- the interface ------------------------------------------------------- */

/*
 * Both of these run on the game thread, at the game's own save points, while
 * the boot thread may be flushing the log to the same card. gc_fs_lock is what
 * keeps exactly one of them inside libfat or CARD_* at a time; see the comment
 * on it in gc_assets.c for what happened without it.
 */
BOOL gc_storage_read(const char *name, void *buf, u32 size) {
    BOOL result = FALSE;
    s32 chn;

    if (!GC_MEMCARD || size == 0 || size > GC_STORAGE_MAX) {
        return FALSE;
    }

    gc_fs_lock();
    for (chn = CARD_SLOTA; chn <= CARD_SLOTB; chn++) {
        if (card_mount(chn)) {
            BOOL ok = card_read_blob(chn, name, buf, size);

            CARD_Unmount(chn);
            if (ok) {
                gc_fs_unlock();
                return TRUE;
            }
            /* Mounted but no such file: keep looking. The other slot, or the
             * SD card, may have the save. */
        }
    }

    result = sd_read_blob(name, buf, size);
    gc_fs_unlock();
    return result;
}

BOOL gc_storage_write(const char *name, const void *buf, u32 size) {
    BOOL result;
    s32 chn;

    if (!GC_MEMCARD || size == 0 || size > GC_STORAGE_MAX) {
        return FALSE;
    }

    gc_fs_lock();
    for (chn = CARD_SLOTA; chn <= CARD_SLOTB; chn++) {
        if (card_mount(chn)) {
            BOOL ok = card_write_blob(chn, name, buf, size);

            CARD_Unmount(chn);
            if (ok) {
                gc_fs_unlock();
                return TRUE;
            }
        }
    }

    result = sd_write_blob(name, buf, size);
    gc_fs_unlock();
    return result;
}

/*
 * Whether anything can be saved at all.
 *
 * osPfsIsPlug answers the game's "is there a Controller Pak" with this, so it
 * has to be a real question about the hardware and not an optimistic TRUE: the
 * game's menus offer to save ghosts on the strength of it, and offering when
 * nothing will persist is worse than saying no.
 *
 * The answer is cached for a second, because the caller is not careful with
 * it: save_data.c asks on its way through the save menus, and a bare
 * CARD_Probe on both slots per frame would put an EXI transaction in the
 * game's frame time for no new information. A card plugged in mid-menu is
 * noticed within a second, which is faster than a user can act on it.
 */
BOOL gc_storage_present(void) {
    static u64 sChecked;
#if !GC_MEMCARD
    return FALSE;
#else
    static BOOL sPresent;
    static BOOL sHave;
    u64 now = gettime();
    s32 chn;

    if (sHave && diff_msec(sChecked, now) < 1000) {
        return sPresent;
    }
    sHave = TRUE;
    sChecked = now;
    sPresent = FALSE;

    gc_fs_lock();
    for (chn = CARD_SLOTA; chn <= CARD_SLOTB; chn++) {
        if (card_slot_has_card(chn)) {
            sPresent = TRUE;
            gc_fs_unlock();
            return sPresent;
        }
    }
    gc_fs_unlock();

    sPresent = gc_fat_mount();
    return sPresent;
#endif
}

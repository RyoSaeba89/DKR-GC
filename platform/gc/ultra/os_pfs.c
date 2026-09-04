/*
 * The Controller Pak.
 *
 * DKR saves its ghost data to a Controller Pak: a 32 KB block device with a
 * sixteen-entry directory, where a "note" is identified by a company code, a
 * game code, a sixteen-character name and a four-character extension.
 * src/save_data.c is written straight against that API -- osPfsInit,
 * osPfsFindFile, osPfsAllocateFile, osPfsReadWriteFile and the rest -- and
 * until now every one of them answered PFS_ERR_NOPACK from stubs.c, which the
 * game handles gracefully by never offering to save a ghost.
 *
 * This file is the pak, emulated. Two decisions shape it.
 *
 * **The API is emulated, not the media.** Nothing outside libultra ever reads a
 * pak's inode table, page chain or ID block; save_data.c touches exactly one
 * field of OSPfs (`status & PFS_INITIALIZED`, at save_data.c:1184) and
 * otherwise goes through the twelve calls below. So the implementation is a
 * directory and a page allocator, not a reproduction of the on-media format.
 * Page accounting is kept exact, because the game shows the user how many
 * pages are free and refuses to save a ghost that will not fit.
 *
 * **The pak is one blob, not sixteen card files.** A GameCube memory card
 * allocates in 8 KB blocks with a 127-entry directory; sixteen ghosts of a few
 * kilobytes each would take 128 KB and sixteen directory entries for one game.
 * So the whole 32 KB image is a single save file, written through
 * gc_storage.c, which puts it on a memory card if there is one and on the SD
 * card otherwise. That is also what a GameCube game with ghost data would have
 * done.
 *
 * The image is loaded once, kept in memory, and written back after every
 * operation that changes it -- the same policy as the EEPROM in os_eeprom.c
 * and for the same reason: on a console the user pulls the power rather than
 * quitting, so a save has to be durable the moment the game believes it is.
 */

#include <ultra64.h>

#include <string.h>

#include "macros.h"

#include "../gc_ultra.h"

/*
 * The pak's own geometry, from include/PR/os_pfs.h.
 *
 * A 256 kbit pak is 32 KB of 32-byte blocks; PFS_ONE_PAGE blocks make a page,
 * so a page is 256 bytes. Of the 128 pages, five are the label, the two inode
 * tables and the two directory tables, leaving 123 for data. That 123 is the
 * number the game divides by when it tells the user how much room is left, so
 * it is reproduced exactly rather than rounded to something tidier.
 */
#define PFS_PAGE_SIZE (BLOCKSIZE * PFS_ONE_PAGE) /* 256 bytes */
#define PFS_DATA_PAGES 123
#define PFS_DIR_ENTRIES 16

#define PFS_DATA_SIZE (PFS_DATA_PAGES * PFS_PAGE_SIZE)

/* The blob's name on the memory card, and its layout version. Bumping the
 * version invalidates old images rather than misreading them. */
#define PAK_SAVE_NAME "dkr_ghosts"
#define PAK_MAGIC 0x444B5250u /* 'DKRP' */
#define PAK_VERSION 1

typedef struct PakDirEntry {
    u32 used; /* 0 = free slot */
    u32 gameCode;
    u32 companyCode;
    u32 fileSize;  /* bytes, as the game asked for them */
    u32 startPage; /* first data page */
    u32 pageCount;
    char name[PFS_FILE_NAME_LEN];
    char ext[PFS_FILE_EXT_LEN];
} PakDirEntry;

typedef struct PakImage {
    u32 magic;
    u32 version;
    PakDirEntry dir[PFS_DIR_ENTRIES];
    u8 data[PFS_DATA_SIZE];
} PakImage;

static PakImage sPak;
static BOOL sLoaded;

/* ---- the image ----------------------------------------------------------- */

static void pak_format(void) {
    memset(&sPak, 0, sizeof(sPak));
    sPak.magic = PAK_MAGIC;
    sPak.version = PAK_VERSION;
}

static void pak_load(void) {
    if (sLoaded) {
        return;
    }
    sLoaded = TRUE;

    if (!gc_storage_read(PAK_SAVE_NAME, &sPak, sizeof(sPak)) || sPak.magic != PAK_MAGIC ||
        sPak.version != PAK_VERSION) {
        /* No save yet, or one this build cannot read. A blank pak is what the
         * game sees on a freshly formatted one, and it knows what to do. */
        pak_format();
    }
}

static void pak_store(void) {
    gc_storage_write(PAK_SAVE_NAME, &sPak, sizeof(sPak));
}

/* ---- the page allocator -------------------------------------------------- */

/*
 * Files are kept contiguous and the data area is compacted on delete.
 *
 * The real pak chains pages through an inode table, so its files are scattered
 * and it never has to compact. Contiguous plus compaction is simpler, cannot
 * fragment, and is invisible from the API -- nobody outside can observe where a
 * note's pages are. The cost is a memmove of at most 31 KB when a ghost is
 * deleted, on a machine that is about to write 32 KB to a memory card anyway.
 */
static u32 pages_used(void) {
    u32 used = 0;
    u32 i;

    for (i = 0; i < PFS_DIR_ENTRIES; i++) {
        if (sPak.dir[i].used) {
            used += sPak.dir[i].pageCount;
        }
    }
    return used;
}

static u32 pages_for(u32 bytes) {
    return (bytes + PFS_PAGE_SIZE - 1) / PFS_PAGE_SIZE;
}

/* The first page not covered by any entry. Entries are kept packed from page
 * zero upward, so this is the end of the last one. */
static u32 first_free_page(void) {
    u32 end = 0;
    u32 i;

    for (i = 0; i < PFS_DIR_ENTRIES; i++) {
        if (sPak.dir[i].used) {
            u32 e = sPak.dir[i].startPage + sPak.dir[i].pageCount;

            if (e > end) {
                end = e;
            }
        }
    }
    return end;
}

static void pak_release(s32 fileNo) {
    PakDirEntry *victim = &sPak.dir[fileNo];
    u32 gapStart = victim->startPage;
    u32 gapPages = victim->pageCount;
    u32 tailStart = gapStart + gapPages;
    u32 tailEnd = first_free_page();
    u32 i;

    if (tailEnd > tailStart) {
        memmove(&sPak.data[gapStart * PFS_PAGE_SIZE], &sPak.data[tailStart * PFS_PAGE_SIZE],
                (tailEnd - tailStart) * PFS_PAGE_SIZE);
    }
    for (i = 0; i < PFS_DIR_ENTRIES; i++) {
        if (sPak.dir[i].used && sPak.dir[i].startPage > gapStart) {
            sPak.dir[i].startPage -= gapPages;
        }
    }
    memset(victim, 0, sizeof(*victim));
}

/* ---- name comparison ----------------------------------------------------- */

/*
 * The game passes names as arrays of font codes, not NUL-terminated strings,
 * and always exactly PFS_FILE_NAME_LEN and PFS_FILE_EXT_LEN long -- see
 * save_data.c:1948, which builds fileNameAsFontCodes and hands over the array.
 * So the comparison is a fixed-length one and the stored copy is not a C
 * string either.
 */
static BOOL name_matches(const PakDirEntry *e, u16 companyCode, u32 gameCode, const u8 *name,
                         const u8 *ext) {
    if (!e->used || e->gameCode != gameCode || e->companyCode != companyCode) {
        return FALSE;
    }
    if (name != NULL && memcmp(e->name, name, PFS_FILE_NAME_LEN) != 0) {
        return FALSE;
    }
    if (ext != NULL && memcmp(e->ext, ext, PFS_FILE_EXT_LEN) != 0) {
        return FALSE;
    }
    return TRUE;
}

static s32 find_entry(u16 companyCode, u32 gameCode, const u8 *name, const u8 *ext) {
    s32 i;

    for (i = 0; i < PFS_DIR_ENTRIES; i++) {
        if (name_matches(&sPak.dir[i], companyCode, gameCode, name, ext)) {
            return i;
        }
    }
    return -1;
}

/* ---- breadcrumbs --------------------------------------------------------- */

/*
 * One line the first time each entry point is reached.
 *
 * Answering osPfsIsPlug with "yes" is what admits the game to save_data.c's
 * Controller Pak and rumble code, none of which had executed in this port
 * before 2026-09-04 -- and the first two hardware runs of that build died
 * within 100 ms of getting there. Until that is understood, the log has to say
 * which of these twelve calls was the last one to return.
 *
 * The marks are free of I/O: gc_logfile_mark buffers, and the boot thread puts
 * the buffer on the card once a retrace.
 */
#define PFS_MARK(flag, ...)                                                                            do {                                                                                                   static BOOL flag;                                                                                  if (!flag) {                                                                                           flag = TRUE;                                                                                       gc_logfile_mark(__VA_ARGS__);                                                                  }                                                                                              } while (0)

/* ---- the API ------------------------------------------------------------- */

/*
 * Whether there is anywhere to save.
 *
 * This is the one place the emulation must not be optimistic. The game's menus
 * offer to record a ghost on the strength of this answer, and a console with no
 * memory card and no SD card cannot keep one -- reporting a pak that quietly
 * discards everything would be worse than reporting none, which is a state the
 * game already handles.
 */
static BOOL pak_available(void) {
#if !GC_MEMCARD
    /*
     * GC_MEMCARD=0 puts the game back where it was before 2026-09-04: no pack,
     * ever. It exists because enabling the pak turned on a large amount of game
     * code that had never executed in this port -- save_data.c's Controller Pak
     * and rumble paths -- in the same build as several other changes, and the
     * first hardware run of that build crashed. This is the one-build A/B that
     * separates "the pak emulation is at fault" from "something else is".
     */
    return FALSE;
#else
    return gc_storage_present();
#endif
}

s32 osPfsInit(UNUSED OSMesgQueue *mq, OSPfs *pfs, int channel) {
    if (pfs == NULL) {
        return PFS_ERR_INVALID;
    }
    /* Only controller 1 carries the pak here: there is one memory card set and
     * one SD card, and pretending four players each have their own would let
     * the game write four ghosts into the same blob. */
    if (channel != 0 || !pak_available()) {
        pfs->status = 0;
        return PFS_ERR_NOPACK;
    }

    pak_load();

    PFS_MARK(sInitSaid, "pfs: osPfsInit ch%d ok, %u/%u pages used\n", channel,
             (unsigned) pages_used(), (unsigned) PFS_DATA_PAGES);

    memset(pfs, 0, sizeof(*pfs));
    pfs->status = PFS_INITIALIZED;
    pfs->queue = NULL;
    pfs->channel = channel;
    pfs->version = OS_PFS_VERSION;
    pfs->dir_size = PFS_DIR_ENTRIES;
    pfs->banks = PFS_BANKS_256K;
    pfs->activebank = 0;
    return 0;
}

/*
 * Formatting. The game offers this when it decides a pak is unusable, and the
 * honest translation is to throw the image away and write a blank one -- which
 * is exactly what the user asked for when they chose to format.
 */
s32 osPfsReFormat(OSPfs *pfs, UNUSED OSMesgQueue *mq, int channel) {
    PFS_MARK(sSaid, "pfs: osPfsReFormat ch%d\n", channel);
    if (channel != 0 || !pak_available()) {
        return PFS_ERR_NOPACK;
    }
    pak_format();
    sLoaded = TRUE;
    pak_store();
    if (pfs != NULL) {
        pfs->status = PFS_INITIALIZED;
    }
    return 0;
}

/*
 * The consistency check. On the N64 this repaired a half-written inode table
 * after a pak was pulled mid-write. There is no half-written state here: the
 * image is replaced whole, by one gc_storage_write, and a torn write leaves a
 * blob whose magic fails and which pak_load discards. So the pak is consistent
 * by construction and this reports so.
 */
s32 osPfsChecker(UNUSED OSPfs *pfs) {
    PFS_MARK(sSaid, "pfs: osPfsChecker\n");
    if (!pak_available()) {
        return PFS_ERR_NOPACK;
    }
    pak_load();
    return 0;
}

s32 osPfsAllocateFile(OSPfs *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName,
                      int fileSize, s32 *fileNo) {
    PFS_MARK(sSaid, "pfs: osPfsAllocateFile %d bytes\n", fileSize);
    u32 need;
    s32 slot = -1;
    s32 i;

    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    if (fileSize <= 0) {
        return PFS_ERR_INVALID;
    }
    pak_load();

    if (find_entry(companyCode, gameCode, gameName, extName) >= 0) {
        return PFS_ERR_EXIST;
    }

    for (i = 0; i < PFS_DIR_ENTRIES; i++) {
        if (!sPak.dir[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return PFS_DIR_FULL;
    }

    need = pages_for((u32) fileSize);
    if (pages_used() + need > PFS_DATA_PAGES) {
        return PFS_DATA_FULL;
    }

    sPak.dir[slot].used = 1;
    sPak.dir[slot].gameCode = gameCode;
    sPak.dir[slot].companyCode = companyCode;
    sPak.dir[slot].fileSize = (u32) fileSize;
    sPak.dir[slot].startPage = first_free_page();
    sPak.dir[slot].pageCount = need;
    if (gameName != NULL) {
        memcpy(sPak.dir[slot].name, gameName, PFS_FILE_NAME_LEN);
    }
    if (extName != NULL) {
        memcpy(sPak.dir[slot].ext, extName, PFS_FILE_EXT_LEN);
    }
    memset(&sPak.data[sPak.dir[slot].startPage * PFS_PAGE_SIZE], 0, need * PFS_PAGE_SIZE);

    if (fileNo != NULL) {
        *fileNo = slot;
    }
    pak_store();
    return 0;
}

s32 osPfsFindFile(OSPfs *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName,
                  s32 *fileNo) {
    s32 found;
    PFS_MARK(sSaid, "pfs: osPfsFindFile\n");

    if (fileNo != NULL) {
        *fileNo = -1;
    }
    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    pak_load();

    found = find_entry(companyCode, gameCode, gameName, extName);
    if (found < 0) {
        return PFS_ERR_INVALID; /* "file not exist", per os_pfs.h */
    }
    if (fileNo != NULL) {
        *fileNo = found;
    }
    return 0;
}

s32 osPfsDeleteFile(OSPfs *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName) {
    s32 found;
    PFS_MARK(sSaid, "pfs: osPfsDeleteFile\n");

    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    pak_load();

    found = find_entry(companyCode, gameCode, gameName, extName);
    if (found < 0) {
        return PFS_ERR_INVALID;
    }
    pak_release(found);
    pak_store();
    return 0;
}

s32 osPfsReadWriteFile(OSPfs *pfs, s32 fileNo, u8 flag, int offset, int nbytes, u8 *dataBuffer) {
    PakDirEntry *e;
    u32 capacity;
    PFS_MARK(sSaid, "pfs: osPfsReadWriteFile no%d flag%u off%d n%d\n", (int) fileNo,
             (unsigned) flag, offset, nbytes);

    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    if (fileNo < 0 || fileNo >= PFS_DIR_ENTRIES || dataBuffer == NULL || offset < 0 ||
        nbytes <= 0) {
        return PFS_ERR_INVALID;
    }
    pak_load();

    e = &sPak.dir[fileNo];
    if (!e->used) {
        return PFS_ERR_INVALID;
    }

    /*
     * The bound is the allocated pages, not the size the game asked for.
     * save_data.c writes ghosts in variable-length pieces and reads them back
     * with a different length than it wrote (save_data.c:1214 passes ghostSize
     * as the offset), so the file's usable extent has to be what was actually
     * reserved -- which is what the real pak did too, since a note occupies
     * whole pages.
     */
    capacity = e->pageCount * PFS_PAGE_SIZE;
    if ((u32) offset + (u32) nbytes > capacity) {
        return PFS_ERR_INVALID;
    }

    if (flag == PFS_WRITE) {
        memcpy(&sPak.data[e->startPage * PFS_PAGE_SIZE + offset], dataBuffer, (size_t) nbytes);
        pak_store();
    } else {
        memcpy(dataBuffer, &sPak.data[e->startPage * PFS_PAGE_SIZE + offset], (size_t) nbytes);
    }
    return 0;
}

s32 osPfsFileState(OSPfs *pfs, s32 fileNo, OSPfsState *state) {
    const PakDirEntry *e;
    PFS_MARK(sSaid, "pfs: osPfsFileState no%d\n", (int) fileNo);

    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    if (fileNo < 0 || fileNo >= PFS_DIR_ENTRIES || state == NULL) {
        return PFS_ERR_INVALID;
    }
    pak_load();

    e = &sPak.dir[fileNo];
    if (!e->used) {
        return PFS_ERR_INVALID;
    }

    state->file_size = e->pageCount * PFS_PAGE_SIZE;
    state->game_code = e->gameCode;
    state->company_code = (u16) e->companyCode;
    memcpy(state->ext_name, e->ext, PFS_FILE_EXT_LEN);
    memcpy(state->game_name, e->name, PFS_FILE_NAME_LEN);
    return 0;
}

/*
 * Which controller slots have a pak.
 *
 * One bit per channel. Only channel 0 ever reports one, for the reason given
 * at osPfsInit: there is a single blob behind this, and four players each
 * "having a pak" would mean four of them sharing one set of ghost slots
 * without knowing it.
 */
s32 osPfsIsPlug(UNUSED OSMesgQueue *mq, u8 *pattern) {
    BOOL have = pak_available();

    /*
     * A breadcrumb, once. Answering this with "yes" is what lets the game into
     * save_data.c's Controller Pak and rumble code, none of which had ever run
     * in this port before 2026-09-04. If a hardware crash happens near here, the
     * log has to say whether the game was let in.
     */
    PFS_MARK(sPlugSaid, "pfs: osPfsIsPlug -> %s\n", have ? "pack present" : "no pack");

    if (pattern != NULL) {
        *pattern = have ? 1 : 0;
    }
    return have ? 0 : PFS_ERR_NOPACK;
}

s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytesNotUsed) {
    PFS_MARK(sSaid, "pfs: osPfsFreeBlocks\n");
    if (bytesNotUsed != NULL) {
        *bytesNotUsed = 0;
    }
    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    pak_load();

    if (bytesNotUsed != NULL) {
        *bytesNotUsed = (s32) ((PFS_DATA_PAGES - pages_used()) * PFS_PAGE_SIZE);
    }
    return 0;
}

s32 osPfsNumFiles(OSPfs *pfs, s32 *maxFiles, s32 *filesUsed) {
    s32 used = 0;
    PFS_MARK(sSaid, "pfs: osPfsNumFiles\n");
    s32 i;

    if (maxFiles != NULL) {
        *maxFiles = 0;
    }
    if (filesUsed != NULL) {
        *filesUsed = 0;
    }
    if (pfs == NULL || !(pfs->status & PFS_INITIALIZED)) {
        return PFS_ERR_NOPACK;
    }
    pak_load();

    for (i = 0; i < PFS_DIR_ENTRIES; i++) {
        if (sPak.dir[i].used) {
            used++;
        }
    }
    if (maxFiles != NULL) {
        *maxFiles = PFS_DIR_ENTRIES;
    }
    if (filesUsed != NULL) {
        *filesUsed = used;
    }
    return 0;
}

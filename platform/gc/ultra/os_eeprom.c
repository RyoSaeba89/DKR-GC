/*
 * Save data.
 *
 * Diddy Kong Racing saves to a 4 kbit EEPROM on the cartridge: 64 blocks of 8
 * bytes, addressed by block number. save_data.c is written directly against
 * that geometry, so the port keeps it and backs the 512 bytes with a file
 * rather than reshaping the save format.
 *
 * The whole EEPROM is held in memory and written back after each modification.
 * At 512 bytes that costs nothing, and it means a save is durable the moment
 * the game thinks it is -- which matters on a console where the user pulls the
 * power rather than quitting.
 *
 * Where those 512 bytes actually land is gc_storage.c's problem: a GameCube
 * memory card if one is plugged in, and a file on the SD card otherwise. That
 * was the plan when this file was written -- "only save_load and save_store
 * need to change" -- and it turned out to be true; they are four lines each.
 * The Controller Pak in ultra/os_pfs.c goes through the same door.
 */

#include <ultra64.h>

#include <string.h>

#include "gc_ultra.h"

#define EEPROM_BLOCK_SIZE 8
#define EEPROM_BLOCKS 64
#define EEPROM_SIZE (EEPROM_BLOCK_SIZE * EEPROM_BLOCKS)

/* The blob's name. gc_storage.c turns it into a memory card file or into
 * sd:/dkr/dkr.eep, and the name is the same either way so a save copied
 * between them keeps working. */
#define SAVE_NAME "dkr.eep"

static u8 sEeprom[EEPROM_SIZE];
static BOOL sLoaded;

static void save_load(void) {
    if (sLoaded) {
        return;
    }
    sLoaded = TRUE;

    if (!gc_storage_read(SAVE_NAME, sEeprom, sizeof(sEeprom))) {
        /* Nothing saved yet is a fresh cartridge, not an error: the game's own
         * checksum pass will see the zeroes and format itself. */
        memset(sEeprom, 0, sizeof(sEeprom));
    }
}

static void save_store(void) {
    gc_storage_write(SAVE_NAME, sEeprom, sizeof(sEeprom));
}

s32 osEepromProbe(OSMesgQueue *mq) {
    (void) mq;

    /*
     * Marked on both sides, because what happens between them is the newest
     * code on the game's boot path: save_load now goes through gc_storage.c,
     * which reaches a memory card and a filesystem. If a hardware log stops
     * between these two lines, that is where to look.
     */
    {
        static BOOL sSaid;

        if (!sSaid) {
            sSaid = TRUE;
            gc_logfile_mark("init: osEepromProbe, loading save\n");
            save_load();
            gc_logfile_mark("init: osEepromProbe done\n");
            return EEPROM_TYPE_4K;
        }
    }
    save_load();
    return EEPROM_TYPE_4K;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer) {
    (void) mq;

    if (address >= EEPROM_BLOCKS) {
        return -1;
    }
    save_load();
    memcpy(buffer, &sEeprom[address * EEPROM_BLOCK_SIZE], EEPROM_BLOCK_SIZE);
    return 0;
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer) {
    (void) mq;

    if (address >= EEPROM_BLOCKS) {
        return -1;
    }
    save_load();
    memcpy(&sEeprom[address * EEPROM_BLOCK_SIZE], buffer, EEPROM_BLOCK_SIZE);
    save_store();
    return 0;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int length) {
    (void) mq;

    if (address * EEPROM_BLOCK_SIZE + length > EEPROM_SIZE) {
        return -1;
    }
    save_load();
    memcpy(buffer, &sEeprom[address * EEPROM_BLOCK_SIZE], length);
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int length) {
    (void) mq;

    if (address * EEPROM_BLOCK_SIZE + length > EEPROM_SIZE) {
        return -1;
    }
    save_load();
    memcpy(&sEeprom[address * EEPROM_BLOCK_SIZE], buffer, length);
    save_store();
    return 0;
}

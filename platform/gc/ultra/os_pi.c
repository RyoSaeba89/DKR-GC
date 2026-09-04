/*
 * The PI (cartridge DMA) half of libultra.
 *
 * Diddy Kong Racing does not load its assets into memory up front. It keeps a
 * lookup table of ROM offsets and DMAs pieces of the cartridge in on demand --
 * a track, a texture set, a model -- decompressing as it goes. asset_loading.c
 * and memory.c are built entirely around that, and they are large, so the port
 * keeps the model rather than rewriting them: the asset image stays in the
 * original ROM layout and osPiStartDma keeps meaning "copy from offset X of
 * that image into RAM".
 *
 * What changes is where the image lives. The obvious choice is main memory,
 * but the image is around 12 MB against MEM1's 24 MB, and the game wants a
 * large heap. ARAM is the better home: it is 16 MB of memory the CPU cannot
 * address directly and that is otherwise almost unused in a port like this,
 * and reaching it means a DMA -- which is exactly the operation being emulated.
 * The N64's cartridge and the GameCube's ARAM end up playing the same role.
 *
 * gc_assets.c owns getting the image into ARAM; this file is the DMA itself.
 */

#include <ultra64.h>

#include <ogc/arqueue.h>
#include <ogc/cache.h>

#include <string.h>

#include "gc_ultra.h"

/* The PI address space on the N64 starts at the cartridge base; the game's
 * offsets carry it, so it is masked off before indexing the image. */
#define PI_DOM1_ADDR2 0x10000000

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt) {
    (void) pri;
    (void) cmdQ;
    (void) cmdBuf;
    (void) cmdMsgCnt;
    /* There is no PI manager thread here: transfers complete inside
     * osPiStartDma before it returns. */
}

/*
 * Performs the transfer and posts the completion message.
 *
 * libultra's osPiStartDma is asynchronous -- it queues the transfer and the PI
 * manager posts to `mq` when the hardware finishes. Callers in this game
 * always block on that message immediately afterwards (see dmacopy in
 * asset_loading.c), so completing synchronously and then posting is
 * indistinguishable from the outside, and it avoids standing up a manager
 * thread to serialise something ARQ already serialises.
 */
s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 direction, u32 devAddr, void *vAddr, u32 nbytes,
                 OSMesgQueue *mq) {
    (void) pri;
    {
        static BOOL sSaid;

        if (!sSaid) {
            sSaid = TRUE;
            gc_logfile_mark("init: first osPiStartDma (asset DMA)\n");
        }
    }

    if (direction != OS_READ) {
        /* Nothing in the game writes back to the cartridge. */
        return -1;
    }

    gc_assets_read(devAddr & ~PI_DOM1_ADDR2, vAddr, nbytes);

    if (mq != NULL) {
        osSendMesg(mq, (OSMesg) mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 osEPiStartDma(OSPiHandle *pihandle, OSIoMesg *mb, s32 direction) {
    (void) pihandle;
    return osPiStartDma(mb, OS_MESG_PRI_NORMAL, direction, mb->devAddr, mb->dramAddr, mb->size,
                        mb->hdr.retQueue);
}

u32 osPiGetStatus(void) {
    return 0;
}

s32 osPiGetDeviceType(void) {
    return 0;
}

s32 osPiRawStartDma(s32 direction, u32 devAddr, void *vAddr, u32 nbytes) {
    if (direction != OS_READ) {
        return -1;
    }
    gc_assets_read(devAddr & ~PI_DOM1_ADDR2, vAddr, nbytes);
    return 0;
}

s32 osPiReadIo(u32 devAddr, u32 *data) {
    gc_assets_read(devAddr & ~PI_DOM1_ADDR2, data, sizeof(u32));
    return 0;
}

s32 osPiWriteIo(u32 devAddr, u32 data) {
    (void) devAddr;
    (void) data;
    return -1;
}

OSPiHandle *osCartRomInit(void) {
    return NULL;
}

OSPiHandle *osLeoDiskInit(void) {
    return NULL;
}

OSPiHandle *osDriveRomInit(void) {
    return NULL;
}

s32 osEPiLinkHandle(OSPiHandle *pihandle) {
    (void) pihandle;
    return 0;
}

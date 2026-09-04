/*
 * Video: VI and framebuffer management, replacing src/video.c.
 *
 * The structure of the original is kept deliberately, because the game's frame
 * pacing lives inside it. fb_update counts how many retrace messages piled up
 * while the frame was being built, decides from that whether the game should
 * be running its logic at 60, 30 or 20 Hz, and then blocks until the frame it
 * just drew has been shown for long enough. That is game behaviour, not
 * hardware behaviour, and rewriting it would change how the game feels. It is
 * reproduced here with the same shape and the same counters.
 *
 * What is genuinely different is what a "framebuffer" means. On the N64 the
 * game allocated two 16-bit colour buffers out of its own pool, the RDP drew
 * into them, and the VI was pointed at whichever one was finished. GX does not
 * work that way: it draws into the embedded framebuffer on the GPU and then
 * copies out to an external framebuffer in a YUV format the VI understands.
 *
 * Rather than pretend, this file keeps both. The game's own u16 buffers are
 * still allocated and still paged between, because parts of the game read and
 * write them directly -- screen_asset.c grabs the finished frame for the
 * pause-menu backdrop, and the display list carries their addresses as
 * segments. The XFBs that the television actually sees are separate, owned
 * here, and the flip in gc_video_swap is a GX copy-out followed by pointing
 * the VI at the other one.
 */

#include <ultra64.h>
#include <PR/sched.h>

#include <ogc/video.h>
#include <ogc/color.h>
#include <ogc/system.h>
#include <ogc/cache.h>

#include <string.h>

#include "gc_ultra.h"
#include "gfx/gfx_gx.h"
#include "memory.h"
#include "video.h"

/************ globals the game reads ************/

u16 *gVideoDepthBuffer = NULL;
s32 gVideoRefreshRate;
f32 gVideoAspectRatio;
f32 gVideoHeightRatio;
OSMesg gVideoMesgBuf[8];
OSMesgQueue gVideoMesgQueue[8];
OSViMode gTvViMode;
s32 gVideoFbWidths[2];
s32 gVideoFbHeights[2];
u16 *gVideoFramebuffers[2];
s32 gVideoCurrFbIndex;
s32 gVideoModeIndex;
u16 *gVideoCurrFramebuffer;
u16 *gVideoLastFramebuffer;
u16 *gVideoCurrDepthBuffer;
u16 *gVideoLastDepthBuffer;
u8 gVideoDeltaCounter;
u8 gVideoDeltaTime;
OSScClient gVideoSched;

VideoModeResolution gVideoModeResolutions[] = {
    { SCREEN_WIDTH, SCREEN_HEIGHT },
    { SCREEN_WIDTH, SCREEN_HEIGHT },
    { HIGH_RES_SCREEN_WIDTH, SCREEN_HEIGHT },
    { HIGH_RES_SCREEN_WIDTH, SCREEN_HEIGHT },
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT },
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT },
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT },
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT },
};

#define NUM_RESOLUTION_MODES ((s32) (sizeof(gVideoModeResolutions) / sizeof(VideoModeResolution)) - 1)

static s32 sBlackScreenTimer;
static u8 sMaxUpdateRate = 3;

/************ the real display ************/

static GXRModeObj *sRmode;
static void *sXfb[2];
static u32 sXfbIndex;
static BOOL sVideoReady;

static void retrace_callback(u32 count) {
    gc_video_retrace(count);
}

/*
 * Brings up the television and the VI. Called once from gc_main before any
 * game code runs, because the game's own video_init assumes the VI is already
 * alive. GX itself is initialised by platform/gc/gfx, which owns the FIFO.
 */
void gc_video_init(void) {
    VIDEO_Init();

#if GC_FORCE_PAL
    /*
     * Pretend this is a PAL console.
     *
     * The port has only ever been driven on an NTSC machine -- Dolphin's
     * default -- and the user's console is PAL: their log opens with
     * `video ok (640x576)`. PAL is not a cosmetic difference here. The game
     * adds PAL_HEIGHT_DIFFERENCE to every resolution in video_init, so it draws
     * into a 320x264 screen space instead of 320x240, and libogc's PAL mode has
     * a 528-line EFB scaled to a 576-line external framebuffer where NTSC is
     * 480 to 480. Every scale factor between the game's coordinates and the EFB
     * changes.
     *
     * Forcing the mode is the only way to exercise that without a console:
     * Dolphin's FallbackRegion did not move VIDEO_GetPreferredMode off NTSC,
     * and libogc reads the TV standard from the SRAM globals rather than from
     * the mode that was configured, so osTvType has to be forced alongside it
     * (see ultra/os_system.c). TVPal576IntDfScale is what the user's console
     * reports: 640x576 external, 528-line EFB.
     *
     * A diagnostic, not an option. Nobody should ship this.
     */
    sRmode = &TVPal576IntDfScale;
#else
    sRmode = VIDEO_GetPreferredMode(NULL);
#endif
    sXfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sRmode));
    sXfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sRmode));
    VIDEO_ClearFrameBuffer(sRmode, sXfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(sRmode, sXfb[1], COLOR_BLACK);

    VIDEO_Configure(sRmode);
    VIDEO_SetNextFramebuffer(sXfb[0]);
    VIDEO_SetPostRetraceCallback(retrace_callback);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (sRmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }

    sXfbIndex = 0;
    sVideoReady = TRUE;
}

/*
 * The framebuffer the television is showing right now.
 *
 * Only the boot console wants this. CON_Init draws characters straight into a
 * framebuffer the caller names, so it has to be handed a real one -- given NULL
 * it writes to address 0, which on this machine is ordinary RAM: no fault, no
 * output, and a black screen that looks exactly like a hang.
 */
void *gc_video_xfb(void) {
    return sXfb[sXfbIndex];
}

GXRModeObj *gc_video_mode(void) {
    return sRmode;
}

/*
 * Presents a finished frame.
 *
 * The argument is the game's own colour buffer and is deliberately unused: the
 * pixels the television sees came from GX, not from that buffer. It is kept in
 * the signature because both callers -- osViSwapBuffer and the scheduler's
 * OS_SC_SWAPBUFFER path -- name it, and because a software-rendered fallback
 * would want it.
 */
#ifdef GC_DEBUG
u32 gGcSwapIn, gGcSwapOut;
#endif

void gc_video_swap(void *framebuffer) {
    (void) framebuffer;

    if (!sVideoReady) {
        return;
    }

#ifdef GC_DEBUG
    gGcSwapIn++;
#endif
    sXfbIndex ^= 1;
    gc_gfx_copy_display(sXfb[sXfbIndex]);

    VIDEO_SetNextFramebuffer(sXfb[sXfbIndex]);
    VIDEO_Flush();
#ifdef GC_DEBUG
    gGcSwapOut++;
#endif
}

/************ the game-facing API, mirroring src/video.c ************/

void video_init(s32 videoModeIndex, OSSched *sc) {
    if (osTvType == OS_TV_PAL) {
        gVideoRefreshRate = REFRESH_50HZ;
        gVideoAspectRatio = ASPECT_RATIO_PAL;
        gVideoHeightRatio = HEIGHT_RATIO_PAL;
    } else if (osTvType == OS_TV_MPAL) {
        gVideoRefreshRate = REFRESH_60HZ;
        gVideoAspectRatio = ASPECT_RATIO_MPAL;
        gVideoHeightRatio = HEIGHT_RATIO_MPAL;
    } else {
        gVideoRefreshRate = REFRESH_60HZ;
        gVideoAspectRatio = ASPECT_RATIO_NTSC;
        gVideoHeightRatio = HEIGHT_RATIO_NTSC;
    }

    if (osTvType == OS_TV_PAL) {
        s32 i;
        for (i = 0; i <= NUM_RESOLUTION_MODES; i++) {
            gVideoModeResolutions[i].height += PAL_HEIGHT_DIFFERENCE;
        }
    }

    video_delta_reset();
    fb_mode_set(videoModeIndex);
    gVideoFramebuffers[0] = NULL;
    gVideoFramebuffers[1] = NULL;
    fb_alloc(0);
    fb_alloc(1);
    gVideoCurrFbIndex = 1;
    fb_swap();
    osCreateMesgQueue((OSMesgQueue *) &gVideoMesgQueue, gVideoMesgBuf,
                      (s32) (sizeof(gVideoMesgBuf) / sizeof(gVideoMesgBuf[0])));
    osScAddClient(sc, &gVideoSched, (OSMesgQueue *) &gVideoMesgQueue, OS_SC_ID_VIDEO);
    sMaxUpdateRate = 3;
}

void fb_mode_set(s32 videoModeIndex) {
    gVideoModeIndex = videoModeIndex;
}

/*
 * The resolution the game believes it is drawing at.
 *
 * The game builds its display list in its own screen space -- 320x240 in the
 * low-res modes, taller under PAL -- while GX rasterises into a 640x480 EFB.
 * The renderer needs both numbers to set up its projection and to scale the
 * scissor rectangles the list carries, and it cannot include this file's
 * headers (see gfx/gfx_gx.h), so it asks through plain ints.
 */
void gc_video_game_resolution(int *width, int *height) {
    *width = gVideoFbWidths[gVideoCurrFbIndex];
    *height = gVideoFbHeights[gVideoCurrFbIndex];

    /* Before video_init has run there is no framebuffer to describe yet. */
    if (*width <= 0 || *height <= 0) {
        *width = SCREEN_WIDTH;
        *height = SCREEN_HEIGHT;
    }
}

s32 fb_size(void) {
    return (gVideoFbHeights[gVideoCurrFbIndex] << 16) | gVideoFbWidths[gVideoCurrFbIndex];
}

/*
 * On the N64 this reprogrammed the VI timings for the chosen resolution. The
 * GameCube raster was already configured from the console's TV standard in
 * gc_video_init and does not change per resolution mode, so there is nothing
 * left for it to do.
 */
void fb_init_vi(void) {
}

void fb_alloc(s32 index) {
    s32 width, height;

    if (gVideoFramebuffers[index] != NULL) {
        mempool_locked_unset((u8 *) gVideoFramebuffers[index]);
        mempool_free(gVideoFramebuffers[index]);
    }

    gVideoFbWidths[index] = gVideoModeResolutions[gVideoModeIndex & NUM_RESOLUTION_MODES].width;
    gVideoFbHeights[index] = gVideoModeResolutions[gVideoModeIndex & NUM_RESOLUTION_MODES].height;

    if (gVideoModeIndex >= VIDEO_MODE_MIDRES_MASK) {
        width = HIGH_RES_SCREEN_WIDTH;
        height = HIGH_RES_SCREEN_HEIGHT;
    } else {
        width = gVideoFbWidths[index];
        height = gVideoFbHeights[index];
    }

    gVideoFramebuffers[index] = mempool_alloc_safe((width * height * 2) + 0x30, COLOUR_TAG_WHITE);
    gVideoFramebuffers[index] = FBALIGN(gVideoFramebuffers[index]);

    if (gVideoDepthBuffer == NULL) {
        gVideoDepthBuffer = mempool_alloc_safe((width * height * 2) + 0x30, COLOUR_TAG_WHITE);
        gVideoDepthBuffer = FBALIGN(gVideoDepthBuffer);
    }
}

void video_delta_reset(void) {
    gVideoDeltaCounter = 0;
    gVideoDeltaTime = LOGIC_30FPS;
}

void func_8007AB24(u8 arg0) {
    sMaxUpdateRate = arg0;
}

s32 vi_refresh_rate(void) {
    return gVideoRefreshRate;
}

void fb_swap(void) {
    gVideoLastFramebuffer = gVideoFramebuffers[gVideoCurrFbIndex];
    gVideoLastDepthBuffer = gVideoDepthBuffer;
    gVideoCurrFbIndex ^= 1;
    gVideoCurrFramebuffer = gVideoFramebuffers[gVideoCurrFbIndex];
    gVideoCurrDepthBuffer = gVideoDepthBuffer;
}

void fb_memcpy(u8 *src, u8 *dest, s32 len) {
    memcpy(dest, src, len);
}

/*
 * Frame pacing. Reproduced from the original, because the adaptive logic rate
 * it computes is what keeps the game's physics stable when a frame runs long.
 * The only substitution is the flip at the end.
 */
#ifdef GC_DEBUG
u32 gGcFbIn, gGcFbOut, gGcFbMesgs;
#endif

s32 fb_update(s32 mesg) {
    u8 tempUpdateRate = LOGIC_60FPS;

#ifdef GC_DEBUG
    gGcFbIn++;
#endif

    if (sBlackScreenTimer) {
        sBlackScreenTimer--;
        if (sBlackScreenTimer == 0) {
            osViBlack(FALSE);
        }
    }

    if (mesg != MESG_SKIP_BUFFER_SWAP) {
        fb_swap();
    }

    /* Every retrace that arrived while this frame was being built means the
     * frame took an extra field, so the logic rate has to drop to match. */
    while (osRecvMesg((OSMesgQueue *) gVideoMesgQueue, NULL, OS_MESG_NOBLOCK) != -1) {
#ifdef GC_DEBUG
        gGcFbMesgs++;
#endif
        tempUpdateRate++;
    }

    if (tempUpdateRate < gVideoDeltaTime) {
        if (gVideoDeltaCounter < 20) {
            gVideoDeltaCounter++;
        }
        if (gVideoDeltaCounter == 20) {
            gVideoDeltaTime = tempUpdateRate;
            gVideoDeltaCounter = 0;
        }
    } else {
        gVideoDeltaCounter = 0;
        if ((gVideoDeltaTime < tempUpdateRate) && (sMaxUpdateRate >= tempUpdateRate)) {
            gVideoDeltaTime = tempUpdateRate;
        }
    }

    while (tempUpdateRate < gVideoDeltaTime) {
        osRecvMesg((OSMesgQueue *) gVideoMesgQueue, NULL, OS_MESG_BLOCK);
        tempUpdateRate++;
    }

    osViSwapBuffer(gVideoLastFramebuffer);
    osRecvMesg((OSMesgQueue *) gVideoMesgQueue, NULL, OS_MESG_BLOCK);
#ifdef GC_DEBUG
    gGcFbMesgs++;
    gGcFbOut++;
#endif
    return tempUpdateRate;
}

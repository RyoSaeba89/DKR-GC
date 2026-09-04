/*
 * The VI half of libultra, mapped onto libogc's VIDEO subsystem.
 *
 * The N64 VI is programmed by handing it a mode table entry that describes the
 * whole raster -- timing, filtering, scale factors -- and a framebuffer
 * pointer. libogc works at a higher level: it picks a GXRModeObj for the
 * console's TV standard and the game only chooses framebuffers. So the mode
 * table entries here are not real timings, they are just distinct objects for
 * the game to pass around and for osViSetMode to identify by index; the actual
 * raster is set up once by gc_video_init.
 *
 * The one piece of behaviour that has to survive intact is osViSetEvent. The
 * scheduler registers a message with it and everything downstream -- the game
 * loop, the audio manager, the frame pacing -- runs off that message arriving
 * once per retrace.
 */

#include <ultra64.h>

#include <ogc/video.h>

#include "gc_ultra.h"

/*
 * Mode table.
 *
 * Zeroed on purpose: nothing reads the fields. video.c and thread3_main.c use
 * these only as tokens -- `osViSetMode(&osViModeTable[viMode])` and comparisons
 * against the named globals -- and the port resolves the real video standard
 * from the console rather than from the token.
 */
OSViMode osViModeTable[64];

OSViMode osViModeNtscLpn1;
OSViMode osViModeNtscLan1;
OSViMode osViModePalLpn1;
OSViMode osViModePalLan1;
OSViMode osViModeMpalLpn1;
OSViMode osViModeMpalLan1;

#ifdef GC_DEBUG
u32 gGcViMesgs;
u32 gGcViDrops;
#endif

static OSMesgQueue *sViQueue;
static OSMesg sViMesg;
static u32 sViFieldsPerMessage = 1;
static u32 sViFieldCounter;

void osCreateViManager(OSPri pri) {
    (void) pri; /* libogc's retrace callback replaces the VI manager thread */
}

void osViSetMode(OSViMode *mode) {
    (void) mode;
}

void osViSetSpecialFeatures(u32 func) {
    (void) func; /* AA/dedither/gamma are RDP-side on the N64; GX handles its own */
}

void osViSetXScale(f32 value) {
    (void) value;
}

void osViSetYScale(f32 value) {
    (void) value;
}

void osViExtendVStart(u32 value) {
    (void) value;
}

void osViRepeatLine(u8 active) {
    (void) active;
}

void osViFade(u8 active, u16 factor) {
    (void) active;
    (void) factor;
}

void osViBlack(u8 active) {
    VIDEO_SetBlack(active ? TRUE : FALSE);
    VIDEO_Flush();
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount) {
    sViQueue = mq;
    sViMesg = msg;
    sViFieldsPerMessage = (retraceCount == 0) ? 1 : retraceCount;
    sViFieldCounter = 0;
}

void osViSwapBuffer(void *framebuffer) {
    gc_video_swap(framebuffer);
}

/*
 * Called from the retrace interrupt.
 *
 * `retraceCount` in osViSetEvent means "post a message every N fields", which
 * the game uses to run at 30 Hz on a 60 Hz display, so the division happens
 * here rather than in the game loop.
 */
void gc_video_retrace(u32 retraceCount) {
    (void) retraceCount;

    gc_event_fire(OS_EVENT_VI);

    if (sViQueue == NULL) {
        return;
    }
    if (++sViFieldCounter >= sViFieldsPerMessage) {
        sViFieldCounter = 0;
        if (osSendMesg(sViQueue, sViMesg, OS_MESG_NOBLOCK) != 0) {
#ifdef GC_DEBUG
            gGcViDrops++;
#endif
        } else {
#ifdef GC_DEBUG
            gGcViMesgs++;
#endif
        }
    }
}

u32 osViGetStatus(void) {
    return 0;
}

u32 osViGetCurrentMode(void) {
    return 0;
}

u32 osViGetCurrentLine(void) {
    return VIDEO_GetCurrentLine();
}

u32 osViGetCurrentField(void) {
    return VIDEO_GetNextField();
}

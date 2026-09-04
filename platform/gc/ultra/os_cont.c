/*
 * Controllers: the N64 SI interface expressed in terms of libogc's PAD driver.
 *
 * The N64 pad and the GameCube pad differ enough that the mapping is a design
 * decision rather than a translation, and the choices below are the ones that
 * keep Diddy Kong Racing playable:
 *
 *   Z  -> L trigger.  The N64 Z is under the middle grip and is held down for
 *         long stretches (it is the game's brake and its weapon fire). The
 *         GameCube's Z is a small shoulder button that is awkward to hold, so
 *         the analogue L trigger, read as a digital press, takes its place.
 *   L  -> Z button.    What is left over goes where the N64's L was: rarely
 *         used, so the awkward button is the right home for it.
 *   R  -> R trigger.   Positionally identical.
 *   C  -> C stick.     The four C buttons become directions on the C stick,
 *         with a deliberately high threshold so a resting thumb does not
 *         register.
 *
 * The GameCube stick reads a little further than the N64's, so it is scaled
 * and clamped to the range the game's handling code was tuned against.
 */

#include <ultra64.h>

#include <ogc/pad.h>

#include <string.h>

#include "gc_ultra.h"

#define MAX_CONTROLLERS 4

/* How far the C stick must be pushed before it counts as a C button press. */
#define CSTICK_THRESHOLD 40

/* The N64 stick saturates around 80 in each axis; the GameCube's reads higher,
 * so readings are clamped rather than rescaled, which keeps the centre
 * sensitivity the game expects. */
#define STICK_LIMIT 80

static OSContPad sPads[MAX_CONTROLLERS];
static OSMesgQueue *sContQueue;
static u8 sContBitmask;

static u16 map_buttons(u16 gc, s8 cstickX, s8 cstickY) {
    u16 out = 0;

    if (gc & PAD_BUTTON_A) out |= CONT_A;
    if (gc & PAD_BUTTON_B) out |= CONT_B;
    if (gc & PAD_TRIGGER_L) out |= CONT_G;     /* N64 Z */
    if (gc & PAD_TRIGGER_Z) out |= CONT_L;     /* N64 L */
    if (gc & PAD_TRIGGER_R) out |= CONT_R;
    if (gc & PAD_BUTTON_START) out |= CONT_START;
    if (gc & PAD_BUTTON_UP) out |= CONT_UP;
    if (gc & PAD_BUTTON_DOWN) out |= CONT_DOWN;
    if (gc & PAD_BUTTON_LEFT) out |= CONT_LEFT;
    if (gc & PAD_BUTTON_RIGHT) out |= CONT_RIGHT;

    if (cstickY > CSTICK_THRESHOLD) out |= CONT_E;   /* C-up */
    if (cstickY < -CSTICK_THRESHOLD) out |= CONT_D;  /* C-down */
    if (cstickX < -CSTICK_THRESHOLD) out |= CONT_C;  /* C-left */
    if (cstickX > CSTICK_THRESHOLD) out |= CONT_F;   /* C-right */

    /* The GameCube X and Y have no N64 counterpart. X doubles for A so a
     * player can brake and accelerate without stretching. */
    if (gc & PAD_BUTTON_X) out |= CONT_A;

    return out;
}

static s8 clamp_stick(s8 v) {
    if (v > STICK_LIMIT) return STICK_LIMIT;
    if (v < -STICK_LIMIT) return -STICK_LIMIT;
    return v;
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status) {
    s32 i;
    u32 connected;

    {
        static BOOL sSaid;

        if (!sSaid) {
            sSaid = TRUE;
            gc_logfile_mark("init: osContInit\n");
        }
    }
    PAD_Init();
    PAD_ScanPads();
    connected = PAD_ScanPads();

    sContQueue = mq;
    sContBitmask = 0;

    for (i = 0; i < MAX_CONTROLLERS; i++) {
        memset(&status[i], 0, sizeof(OSContStatus));
        if (connected & (1 << i)) {
            sContBitmask |= (1 << i);
            status[i].type = CONT_ABSOLUTE | CONT_JOYPORT;
            status[i].status = 0;
            status[i].errno = 0;
        } else {
            status[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }

    *bitpattern = sContBitmask;
    return 0;
}

s32 osContStartReadData(OSMesgQueue *mq) {
    s32 i;

    PAD_ScanPads();

    for (i = 0; i < MAX_CONTROLLERS; i++) {
        s8 stickX = PAD_StickX(i);
        s8 stickY = PAD_StickY(i);

        sPads[i].button = map_buttons(PAD_ButtonsHeld(i), PAD_SubStickX(i), PAD_SubStickY(i));
        sPads[i].stick_x = clamp_stick(stickX);
        sPads[i].stick_y = clamp_stick(stickY);
        sPads[i].errno = (sContBitmask & (1 << i)) ? 0 : CONT_NO_RESPONSE_ERROR;
    }

    /* The read completes here, so the caller's wait on `mq` returns at once. */
    if (mq != NULL) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    }
    return 0;
}

void osContGetReadData(OSContPad *pad) {
    memcpy(pad, sPads, sizeof(sPads));
}

s32 osContStartQuery(OSMesgQueue *mq) {
    return osContStartReadData(mq);
}

void osContGetQuery(OSContStatus *status) {
    s32 i;

    for (i = 0; i < MAX_CONTROLLERS; i++) {
        memset(&status[i], 0, sizeof(OSContStatus));
        if (sContBitmask & (1 << i)) {
            status[i].type = CONT_ABSOLUTE | CONT_JOYPORT;
        } else {
            status[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContSetCh(u8 ch) {
    (void) ch;
    return 0;
}

s32 osContReset(OSMesgQueue *mq, OSContStatus *status) {
    return osContStartQuery(mq), osContGetQuery(status), 0;
}

/* ---- rumble -------------------------------------------------------------- */
/*
 * The N64's Rumble Pak was an accessory in the controller's memory slot; the
 * GameCube pad has a motor built in, so the "is one plugged in" question is
 * answered by whether the pad itself is present.
 */
s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel) {
    (void) mq;

    if (!(sContBitmask & (1 << channel))) {
        return PFS_ERR_NOPACK;
    }
    /*
     * osMotorStart and osMotorStop below address the pad through pfs->channel,
     * so it has to be recorded here -- libultra's osMotorInit does the same.
     * It did not matter until 2026-09-04, because osPfsIsPlug reported no pack
     * and save_data.c's rumble loop never ran; now it does, and without this
     * every player's rumble would be sent to pad 0.
     */
    if (pfs != NULL) {
        pfs->channel = channel;
        pfs->status |= PFS_MOTOR_INITIALIZED;
    }
    return 0;
}

s32 osMotorStart(OSPfs *pfs) {
    PAD_ControlMotor(pfs->channel, PAD_MOTOR_RUMBLE);
    return 0;
}

s32 osMotorStop(OSPfs *pfs) {
    PAD_ControlMotor(pfs->channel, PAD_MOTOR_STOP);
    return 0;
}

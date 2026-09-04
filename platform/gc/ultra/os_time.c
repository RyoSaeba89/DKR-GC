/*
 * libultra time services on the Gekko time base.
 *
 * The N64's COP0 count register advances at 46.875 MHz and the game treats
 * osGetCount()/osGetTime() as being in those units -- it converts to and from
 * real time with the OS_CYCLES_TO_* macros, which bake that rate in. The
 * GameCube time base runs at TB_TIMER_CLOCK, a quarter of the 162 MHz bus, so
 * every reading is rescaled here rather than at each call site. The ratio is
 * exactly 125/108, so the conversion is a widening multiply with no drift.
 */

#include <ultra64.h>

#include <ogc/system.h>
#include <ogc/timesupp.h>
#include <ogc/machine/processor.h>

#include <string.h>

#include "gc_ultra.h"

/* 46875000 / (TB_TIMER_CLOCK * 1000) reduced: 46875000/40500000 == 125/108. */
#define N64_COUNT_NUM 125
#define N64_COUNT_DEN 108

/* Applied by osSetTime so that the sequence stays monotonic across a reset of
 * the game's clock, which is what the N64 register would have done. */
static OSTime sTimeOffset;

static OSTime raw_count(void) {
    return (gettime() * N64_COUNT_NUM) / N64_COUNT_DEN;
}

OSTime osGetTime(void) {
    return raw_count() + sTimeOffset;
}

void osSetTime(OSTime t) {
    sTimeOffset = t - raw_count();
}

u32 osGetCount(void) {
    return (u32) osGetTime();
}

/*
 * Timers.
 *
 * Diddy Kong Racing never calls these -- the game paces itself off the retrace
 * message instead -- but libultra callers inside the tree may, and leaving
 * them unimplemented would fail at link time rather than somewhere visible.
 * They are backed by libogc's alarm objects, one per OSTimer.
 */

#define MAX_TIMERS 8

typedef struct {
    OSTimer *timer;
    syswd_t alarm;
    OSMesgQueue *mq;
    OSMesg msg;
    BOOL periodic;
} TimerSlot;

static TimerSlot sTimers[MAX_TIMERS];

static void timer_fired(syswd_t alarm, void *arg) {
    TimerSlot *slot = (TimerSlot *) arg;

    (void) alarm;
    if (slot->mq != NULL) {
        osSendMesg(slot->mq, slot->msg, OS_MESG_NOBLOCK);
    }
    if (!slot->periodic) {
        slot->timer = NULL;
    }
}

static void count_to_timespec(OSTime count, struct timespec *ts) {
    /* count is in N64 cycles; convert back to nanoseconds. */
    u64 ns = (count * N64_COUNT_DEN * 1000ull) / (N64_COUNT_NUM * 40500ull / 1000ull);

    ts->tv_sec = (time_t) (ns / 1000000000ull);
    ts->tv_nsec = (long) (ns % 1000000000ull);
}

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg) {
    struct timespec start, period;
    TimerSlot *slot = NULL;
    s32 i;

    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].timer == NULL) {
            slot = &sTimers[i];
            break;
        }
    }
    if (slot == NULL) {
        return -1;
    }

    slot->timer = t;
    slot->mq = mq;
    slot->msg = msg;
    slot->periodic = (interval != 0);

    t->interval = interval;
    t->value = countdown;
    t->mq = mq;
    t->msg = msg;

    count_to_timespec(countdown, &start);
    count_to_timespec(interval, &period);

    SYS_CreateAlarm(&slot->alarm);
    if (slot->periodic) {
        SYS_SetPeriodicAlarm(slot->alarm, &start, &period, timer_fired, slot);
    } else {
        SYS_SetAlarm(slot->alarm, &start, timer_fired, slot);
    }
    return 0;
}

int osStopTimer(OSTimer *t) {
    s32 i;

    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].timer == t) {
            SYS_RemoveAlarm(sTimers[i].alarm);
            memset(&sTimers[i], 0, sizeof(sTimers[i]));
            return 0;
        }
    }
    return -1;
}

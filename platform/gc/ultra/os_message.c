/*
 * libultra message queues on top of libogc's MQ subsystem.
 *
 * The two APIs line up almost exactly -- both are bounded queues of pointer
 * sized messages with blocking and non-blocking variants -- so this file is
 * mostly a shim. Two details are worth knowing before changing anything here:
 *
 * 1. Where the handle lives. OSMesgQueue is allocated by the game, usually
 *    statically, and we cannot grow it. Rather than keep a side table keyed on
 *    the queue address, the libogc handle is stashed in `mtqueue`, one of the
 *    struct's internal scheduler fields. The game never reads those fields --
 *    the one exception is `validCount`, which save_data.c polls to decide
 *    whether a controller message is waiting -- so `validCount` is the only
 *    field this file has to keep truthful.
 *
 * 2. Interrupt context. osSendMesg is called from interrupt handlers on the
 *    N64 (retrace, PI completion) and the ports of those handlers in
 *    platform/gc keep doing so. libogc's MQ is RTEMS-derived and defers thread
 *    dispatch when called with interrupts disabled, so MQ_Send is safe there;
 *    a mutex would not be. Do not "fix" the validCount bookkeeping below by
 *    reaching for LWP_MutexLock.
 */

#include <ultra64.h>

#include <ogc/message.h>
#include <ogc/machine/processor.h>

#include "gc_ultra.h"

#define MQ_OF(mq) ((mqbox_t) (u32) (mq)->mtqueue)
#define SET_MQ(mq, box) ((mq)->mtqueue = (OSThread *) (u32) (box))

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgBuf, s32 count) {
    mqbox_t box = MQ_BOX_NULL;

    /* libogc allocates the ring itself, so msgBuf goes unused. It is kept in
     * the struct because the game hands us the same buffer it sized `count`
     * from, and a future debugger build may want to walk it. */
    if (MQ_Init(&box, (u32) count) != 0) {
        gc_fatal("osCreateMesgQueue: MQ_Init failed (count=%d)", (int) count);
    }

    SET_MQ(mq, box);
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    u32 level;

    if (!MQ_Send(MQ_OF(mq), (mqmsg_t) msg, flags == OS_MESG_BLOCK ? MQ_MSG_BLOCK : MQ_MSG_NOBLOCK)) {
        return -1;
    }

    _CPU_ISR_Disable(level);
    mq->validCount++;
    _CPU_ISR_Restore(level);
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    u32 level;

    if (!MQ_Jam(MQ_OF(mq), (mqmsg_t) msg, flags == OS_MESG_BLOCK ? MQ_MSG_BLOCK : MQ_MSG_NOBLOCK)) {
        return -1;
    }

    _CPU_ISR_Disable(level);
    mq->validCount++;
    _CPU_ISR_Restore(level);
    return 0;
}

#ifdef GC_DEBUG
volatile u32 gGcBlockPc;
volatile u32 gGcBlockQ;
volatile u32 gGcBlockSeq;

/* The game runs on the thread the port creates with libultra id 3; every other
 * thread here blocks on a queue by design (the scheduler on its interrupt
 * queue, the audio manager on its reply queue) and would drown the breadcrumb
 * in noise. */
#define GAME_THREAD_ID 3

static void block_enter(OSMesgQueue *mq, void *pc) {
    OSThread *self = gc_current_thread();

    if (self != NULL && self->id == GAME_THREAD_ID) {
        gGcBlockQ = (u32) mq;
        gGcBlockPc = (u32) pc;
        gGcBlockSeq++;
    }
}

static void block_exit(void) {
    OSThread *self = gc_current_thread();

    if (self != NULL && self->id == GAME_THREAD_ID) {
        gGcBlockPc = 0;
    }
}
#endif

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flags) {
    mqmsg_t received = NULL;
    u32 level;

#ifdef GC_DEBUG
    if (flags == OS_MESG_BLOCK) {
        block_enter(mq, __builtin_return_address(0));
    }
#endif

    if (!MQ_Receive(MQ_OF(mq), &received, flags == OS_MESG_BLOCK ? MQ_MSG_BLOCK : MQ_MSG_NOBLOCK)) {
#ifdef GC_DEBUG
        if (flags == OS_MESG_BLOCK) {
            block_exit();
        }
#endif
        return -1;
    }

#ifdef GC_DEBUG
    if (flags == OS_MESG_BLOCK) {
        block_exit();
    }
#endif

    _CPU_ISR_Disable(level);
    if (mq->validCount > 0) {
        mq->validCount--;
    }
    _CPU_ISR_Restore(level);

    /* libultra tolerates a NULL destination: callers that only care that
     * *something* arrived pass NULL rather than a scratch variable. */
    if (msg != NULL) {
        *msg = (OSMesg) received;
    }
    return 0;
}

/*
 * Event registration.
 *
 * On the N64 these bind a hardware interrupt to a queue. Here the interrupt
 * sources are libogc callbacks living in os_vi.c, os_pi.c and gfx/, and they
 * look the binding up through gc_event_queue(). Events nothing on this
 * platform can raise (SP break, CPU fault, the rmon events) are still recorded
 * so that a stray osSetEventMesg does not need a special case.
 */
static OSMesgQueue *sEventQueue[OS_NUM_EVENTS];
static OSMesg sEventMesg[OS_NUM_EVENTS];

void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg) {
    if ((u32) event >= OS_NUM_EVENTS) {
        return;
    }
    sEventQueue[event] = mq;
    sEventMesg[event] = msg;
}

void gc_event_fire(OSEvent event) {
    if ((u32) event < OS_NUM_EVENTS && sEventQueue[event] != NULL) {
        osSendMesg(sEventQueue[event], sEventMesg[event], OS_MESG_NOBLOCK);
    }
}

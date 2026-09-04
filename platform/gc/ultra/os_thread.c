/*
 * libultra threads on top of libogc's LWP subsystem.
 *
 * Three differences between the two models drive everything in this file.
 *
 * Start semantics. osCreateThread produces a thread in OS_STATE_STOPPED that
 * only runs once osStartThread is called, while LWP_CreateThread starts
 * running immediately. Suspending right after creating is a race -- the thread
 * can reach its entry point first -- so each thread instead begins life
 * blocked on a start gate semaphore that osStartThread posts. Once a thread
 * has been started the gate is done with, and stop/start becomes plain
 * suspend/resume.
 *
 * Stacks. The game passes the top of a statically sized u64 array, with no
 * length, and those arrays are sized for MIPS frames. PowerPC frames are
 * larger, and the EABI keeps more state per call, so reusing them would
 * overflow. The game's array is left alone -- thread3_verify_stack() still
 * writes its sentinels into it and still reports them consistent -- while the
 * real stack is allocated here at GC_THREAD_STACK_SIZE.
 *
 * Priorities. Both systems agree on direction (larger number wins) but
 * libultra runs 0..255 and LWP runs 0..127, so priorities are halved. The
 * relative order of everything the game creates is preserved, which is all the
 * game depends on.
 */

#include <ultra64.h>

#include <ogc/lwp.h>
#include <ogc/semaphore.h>
#include <ogc/machine/processor.h>

#include <malloc.h>
#include <string.h>

#include "gc_ultra.h"

#define MAX_THREADS 16

typedef struct {
    OSThread *osThread;  /* NULL marks the slot free */
    lwp_t lwp;
    sem_t gate;      /* posted by the first osStartThread */
    void (*entry)(void *);
    void *arg;
    void *stack;
    u32 stackSize;
    u8 started;
} ThreadSlot;

/*
 * How big a stack a thread gets.
 *
 * The game hands osCreateThread the top of a statically sized array and no
 * length, so the size cannot be read off the call; and those arrays are sized
 * for MIPS frames anyway, which are smaller than PowerPC EABI ones. The port
 * therefore picks the size, and one thread is not like the others: thread 3 is
 * the game -- it runs the whole simulation, recurses through the object update
 * tree and builds the display list on the way back out. Giving it the same
 * stack as the audio manager or the background loader overflows it into
 * whatever malloc put underneath, which does not fault on this machine and so
 * shows up much later as an unexplained freeze rather than as a crash.
 */
#define GAME_THREAD_ID 3

static u32 stack_size_for(OSId id) {
    return (id == GAME_THREAD_ID) ? GC_GAME_STACK_SIZE : GC_THREAD_STACK_SIZE;
}

/*
 * A word at the low end of every stack, checked by gc_stack_overflowed().
 * PowerPC stacks grow down, so this is the first thing an overflow destroys.
 * It costs one store per thread and turns a silent memory corruption into a
 * question that can be answered.
 */
#define STACK_CANARY 0x444B5243 /* 'DKRC' */

static ThreadSlot sThreads[MAX_THREADS];

static u8 to_lwp_priority(OSPri pri) {
    s32 p = (s32) pri >> 1;

    /* LWP_PRIO_IDLE (0) is reserved for libogc's own idle thread; a game
     * thread asking for OS_PRIORITY_IDLE still has to be schedulable. */
    if (p < LWP_PRIO_LOWEST) {
        p = LWP_PRIO_LOWEST;
    } else if (p > LWP_PRIO_HIGHEST) {
        p = LWP_PRIO_HIGHEST;
    }
    return (u8) p;
}

static ThreadSlot *slot_for(OSThread *t) {
    s32 i;

    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i].osThread == t) {
            return &sThreads[i];
        }
    }
    return NULL;
}

static ThreadSlot *slot_for_self(void) {
    lwp_t self = LWP_GetSelf();
    s32 i;

    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i].osThread != NULL && sThreads[i].lwp == self) {
            return &sThreads[i];
        }
    }
    return NULL;
}

OSThread *gc_current_thread(void) {
    ThreadSlot *slot = slot_for_self();

    return (slot != NULL) ? slot->osThread : NULL;
}

static void *thread_trampoline(void *arg) {
    ThreadSlot *slot = (ThreadSlot *) arg;

    LWP_SemWait(slot->gate);
    slot->osThread->state = OS_STATE_RUNNING;
    slot->entry(slot->arg);

    /* libultra threads are expected never to return; the ones in this game do
     * not. If one ever does, park it rather than letting LWP tear down a stack
     * the game still believes in. */
    slot->osThread->state = OS_STATE_STOPPED;
    LWP_SuspendThread(slot->lwp);
    return NULL;
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) {
    ThreadSlot *slot = slot_for(NULL);

    (void) sp; /* see the stack note in the file header */

    if (slot == NULL) {
        gc_fatal("osCreateThread: more than %d threads", MAX_THREADS);
    }

    slot->osThread = t;
    slot->entry = entry;
    slot->arg = arg;
    slot->started = FALSE;
    slot->stackSize = stack_size_for(id);
    slot->stack = memalign(32, slot->stackSize);
    if (slot->stack == NULL) {
        gc_fatal("osCreateThread: out of memory for a %lu byte stack",
                 (unsigned long) slot->stackSize);
    }

    t->id = id;
    t->priority = pri;
    t->next = NULL;
    t->queue = NULL;
    t->state = OS_STATE_STOPPED;
    t->flags = 0;

    if (LWP_SemInit(&slot->gate, 0, 1) != 0) {
        gc_fatal("osCreateThread: LWP_SemInit failed");
    }
    if (LWP_CreateThread(&slot->lwp, thread_trampoline, slot, slot->stack, slot->stackSize,
                         to_lwp_priority(pri)) != 0) {
        gc_fatal("osCreateThread: LWP_CreateThread failed");
    }

    /* After LWP_CreateThread, not before: it lays out the initial frame in the
     * block it was given, and the canary has to be the last word written. */
    *(u32 *) slot->stack = STACK_CANARY;
}

/*
 * The libultra id of the first thread whose stack has been run off the bottom,
 * or -1 if they are all intact.
 */
s32 gc_stack_overflowed(void) {
    s32 i;

    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i].osThread != NULL && sThreads[i].stack != NULL &&
            *(u32 *) sThreads[i].stack != STACK_CANARY) {
            return sThreads[i].osThread->id;
        }
    }
    return -1;
}

void osStartThread(OSThread *t) {
    ThreadSlot *slot = slot_for(t);

    if (slot == NULL) {
        return;
    }

    t->state = OS_STATE_RUNNABLE;
    if (!slot->started) {
        slot->started = TRUE;
        LWP_SemPost(slot->gate);
    } else {
        LWP_ResumeThread(slot->lwp);
    }
}

void osStopThread(OSThread *t) {
    ThreadSlot *slot = (t == NULL) ? slot_for_self() : slot_for(t);

    if (slot == NULL) {
        return;
    }

    slot->osThread->state = OS_STATE_STOPPED;
    LWP_SuspendThread(slot->lwp);
}

void osDestroyThread(OSThread *t) {
    ThreadSlot *slot = (t == NULL) ? slot_for_self() : slot_for(t);

    if (slot == NULL) {
        return;
    }

    LWP_SuspendThread(slot->lwp);
    LWP_SemDestroy(slot->gate);
    free(slot->stack);
    slot->osThread->state = OS_STATE_STOPPED;
    memset(slot, 0, sizeof(*slot));
}

void osYieldThread(void) {
    LWP_YieldThread();
}

OSId osGetThreadId(OSThread *t) {
    ThreadSlot *slot = (t == NULL) ? slot_for_self() : slot_for(t);

    return (slot != NULL) ? slot->osThread->id : 0;
}

void osSetThreadPri(OSThread *t, OSPri pri) {
    ThreadSlot *slot = (t == NULL) ? slot_for_self() : slot_for(t);

    if (slot == NULL) {
        return;
    }

    slot->osThread->priority = pri;
    LWP_SetThreadPriority(slot->lwp, to_lwp_priority(pri));
}

OSPri osGetThreadPri(OSThread *t) {
    ThreadSlot *slot = (t == NULL) ? slot_for_self() : slot_for(t);

    return (slot != NULL) ? slot->osThread->priority : 0;
}

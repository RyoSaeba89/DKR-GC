/*
 * The libultra scheduler, reimplemented for a machine with no RSP or RDP.
 *
 * This file is the seam of the whole port. On the N64 the scheduler thread
 * arbitrates two coprocessors: it queues graphics and audio tasks, decides
 * which may run given what is already busy, kicks them off, and waits for the
 * SP and DP interrupts that say they finished. The game's only view of any of
 * that is a message on a queue when its task is done.
 *
 * Here neither coprocessor exists. A graphics task is a display list that
 * platform/gc/gfx walks on the CPU and turns into GX commands; an audio task
 * is a command list that platform/gc/audio renders into a sample buffer, also
 * on the CPU. Both are synchronous, so all the arbitration collapses: a task
 * is executed the moment it arrives and its completion message goes out
 * immediately afterwards. The queue heads and the curRSPTask/curRDPTask fields
 * of OSSched are kept updated anyway, because the game reads some of them, but
 * nothing ever has to wait on them.
 *
 * What must be preserved exactly is the *messaging*, since that is what the
 * game's timing is built on:
 *
 *   - every retrace, each registered client gets a retrace message; this is
 *     what wakes the video code and the audio manager, and is what paces the
 *     game loop.
 *   - a finished task replies to the queue named in the task, with the message
 *     named in the task.
 *   - OS_SC_SWAPBUFFER on a finished graphics task means the framebuffer it
 *     drew into should now be shown.
 *
 * Note the two ways a task can reach the scheduler. libultra expects tasks on
 * cmdQ, and the audio manager sends them there, but DKR's graphics path sends
 * them straight to interruptQ (see gfxtask_run_xbus in src/rcp_dkr.c) mixed in
 * with the interrupt notifications. They are told apart the same way the
 * original does it: interrupt notifications are small integer constants cast
 * to OSMesg, anything else is a task pointer.
 */

#include <ultra64.h>
#include <PR/sched.h>

#include "gc_ultra.h"

/* The values libultra uses for the notifications it posts to interruptQ. They
 * are duplicated here rather than shared because sched.c is not compiled for
 * this target. */
#define VIDEO_MSG    666
#define RSP_DONE_MSG 667
#define RDP_DONE_MSG 668
#define PRE_NMI_MSG  669

#define SCHED_THREAD_ID 0

#ifdef GC_DEBUG
u32 gGcGfxTasks;
u32 gGcAudioTasks;
u32 gGcSwaps;
u32 gGcSchedMsgs;
u32 gGcSchedRetraces;
u32 gGcClientSends;
#define COUNT(x) ((x)++)
#else
#define COUNT(x) ((void) 0)
#endif

static void sched_main(void *arg);

/* Alternates every retrace; see handle_retrace. */
static u8 sAudioRetraceParity;

void osCreateScheduler(OSSched *sc, void *stack, OSPri priority, u8 mode, u8 numFields) {
    (void) mode; /* the video mode is chosen by gc_video_init from the real TV standard */
    {
        static BOOL sSaid;

        if (!sSaid) {
            sSaid = TRUE;
            gc_logfile_mark("init: osCreateScheduler\n");
        }
    }

    sc->curRSPTask = NULL;
    sc->curRDPTask = NULL;
    sc->clientList = NULL;
    sc->audioListHead = NULL;
    sc->gfxListHead = NULL;
    sc->audioListTail = NULL;
    sc->gfxListTail = NULL;
    sc->frameCount = 0;
    sc->unkTask = NULL;
    sc->doAudio = 0;
    sc->retraceMsg.type = OS_SC_RETRACE_MSG;
    sc->prenmiMsg.type = OS_SC_PRE_NMI_MSG;

    osCreateMesgQueue(&sc->interruptQ, sc->intBuf, OS_SC_MAX_MESGS);
    osCreateMesgQueue(&sc->cmdQ, sc->cmdMsgBuf, OS_SC_MAX_MESGS);

    /* The SP and DP events can never fire on this platform, but registering
     * them keeps osSetEventMesg's table in the shape the game expects and
     * costs nothing. The VI event is the one that matters: gc_video_retrace
     * raises it from the retrace interrupt. */
    osSetEventMesg(OS_EVENT_SP, &sc->interruptQ, (OSMesg) RSP_DONE_MSG);
    osSetEventMesg(OS_EVENT_DP, &sc->interruptQ, (OSMesg) RDP_DONE_MSG);
    osSetEventMesg(OS_EVENT_PRENMI, &sc->interruptQ, (OSMesg) PRE_NMI_MSG);
    osViSetEvent(&sc->interruptQ, (OSMesg) VIDEO_MSG, numFields);

    osCreateThread(&sc->thread, SCHED_THREAD_ID, sched_main, sc, stack, priority);
    osStartThread(&sc->thread);
}

void osScAddClient(OSSched *sc, OSScClient *c, OSMesgQueue *msgQ, u8 id) {
    OSIntMask mask = osSetIntMask(OS_IM_NONE);

    c->msgQ = msgQ;
    c->next = sc->clientList;
    c->id = id;
    sc->clientList = c;

    osSetIntMask(mask);
}

void osScRemoveClient(OSSched *sc, OSScClient *c) {
    OSIntMask mask = osSetIntMask(OS_IM_NONE);
    OSScClient *client = sc->clientList;
    OSScClient *prev = NULL;

    while (client != NULL) {
        if (client == c) {
            if (prev != NULL) {
                prev->next = c->next;
            } else {
                sc->clientList = c->next;
            }
            break;
        }
        prev = client;
        client = client->next;
    }

    osSetIntMask(mask);
}

OSMesgQueue *osScGetCmdQ(OSSched *sc) {
    return &sc->cmdQ;
}

OSMesgQueue *osScGetInterruptQ(OSSched *sc) {
    return &sc->interruptQ;
}

/*
 * Runs one task to completion and answers it.
 *
 * The reply is what unblocks the caller: the game loop waits on
 * gGfxTaskMesgQueue after submitting a display list, and the audio manager
 * waits on its own reply queue after submitting a command list. Both must get
 * exactly one message per task or the loop stalls.
 */
/*
 * What the scheduler replies with when a task carries no message of its own.
 * `D_800DE730` in libultra/src/sc/sched.c:68, and the two words matter: the
 * waiter reads the second one and hands it to fb_update as gScreenStatus, where
 * OSMESG_SWAP_BUFFER (0) means "present this frame" and MESG_SKIP_BUFFER_SWAP
 * (8) means "do not". Static because the game keeps the pointer only until it
 * has read through it, but there is no reason to make that a race.
 */
static s32 sSwapBufferMsg[2] = { OSMESG_SWAP_BUFFER, OSMESG_SWAP_BUFFER };

static void run_task(OSSched *sc, OSScTask *task) {
    switch (task->list.t.type) {
        case M_GFXTASK:
            sc->curRSPTask = task;
            sc->curRDPTask = task;
            COUNT(gGcGfxTasks);
            {
                /* First frame ever submitted. With the boot trace and the pak
                 * marks around it, this is what says whether the game reached
                 * its render loop at all before whatever happens next. */
                static BOOL sSaid;

                if (!sSaid) {
                    sSaid = TRUE;
                    gc_logfile_mark("sched: first graphics task\n");
                }
            }
            gc_gfx_run_dl(task->list.t.data_ptr);
            {
                /* Paired with the mark above, so the log distinguishes "died
                 * inside the first display list" from "got past it". The
                 * fourth hardware run stopped exactly at the opening mark and
                 * there was no way to tell which. */
                static BOOL sDone;

                if (!sDone) {
                    sDone = TRUE;
                    gc_logfile_mark("sched: first graphics task done\n");
                }
            }
            sc->curRSPTask = NULL;
            sc->curRDPTask = NULL;

            if (task->flags & OS_SC_SWAPBUFFER) {
                COUNT(gGcSwaps);
                gc_video_swap(task->framebuffer);
            }
            sc->frameCount++;
            break;

        case M_AUDTASK:
            sc->curRSPTask = task;
            COUNT(gGcAudioTasks);
            {
                static BOOL sSaid;

                if (!sSaid) {
                    sSaid = TRUE;
                    gc_logfile_mark("sched: first audio task\n");
                }
            }
            gc_audio_run_cmds(task->list.t.data_ptr, task->list.t.data_size);
            sc->curRSPTask = NULL;
            break;

        default:
            /* Nothing else reaches the scheduler in this game. Dropping an
             * unknown task silently would hang whoever is waiting on it, so
             * still send the reply. */
            break;
    }

    /*
     * The reply, and the substitution that has to go with it.
     *
     * `osSendMesg(task->msgQ, task->msg, ...)` looks like the obvious thing and
     * it is wrong. libultra/src/sc/sched.c:522 -- the real scheduler, which is
     * in this repository and should have been read before this function was
     * written -- does:
     *
     *     if (t->unk68 || t->msg) osSendMesg(t->msgQ, t->msg, ...);
     *     else                    osSendMesg(t->msgQ, &D_800DE730, ...);
     *
     * with `s32 D_800DE730[] = { OSMESG_SWAP_BUFFER, OSMESG_SWAP_BUFFER }`.
     *
     * That branch is not an edge case here, it is the normal path for every
     * frame the game draws. `gfxtask_run_xbus` (src/rcp_dkr.c:166) is the only
     * submitter that runs, it sets `mesgQueue` but **never sets `mesg`**, and
     * `gGfxTaskBuf` is a BSS array -- so `task->msg` is NULL on every graphics
     * task DKR ever submits. And the waiter dereferences what it receives:
     *
     *     s32 gfxtask_wait(void) {          // src/rcp_dkr.c:365
     *         OSMesg *mesg = NULL;
     *         ...
     *         osRecvMesg(&gGfxTaskMesgQueue, (OSMesg) &mesg, OS_MESG_BLOCK);
     *         return (s32) mesg[1];
     *     }
     *
     * `mesg[1]` on a NULL pointer is a load from address 4. That is the crash
     * the user's console had been taking on every boot: the photograph of the
     * exception screen showed `DAR 00000004`, and the interrupted frame was
     * gfxtask_wait immediately after its `bl osRecvMesg`.
     *
     * It never reproduced under Dolphin because Dolphin does not emulate the
     * MMU for homebrew -- reads of unmapped addresses simply return data
     * instead of faulting, which was confirmed by making the port write to
     * address 4 and to 0xDEADBEE0 and watching it carry on. Four hardware runs
     * and a photograph of the television were what it took, and the answer was
     * in this repository the whole time.
     */
    if (task->msgQ != NULL) {
        if (task->unk68 || task->msg != NULL) {
            osSendMesg(task->msgQ, task->msg, OS_MESG_NOBLOCK);
        } else {
            osSendMesg(task->msgQ, (OSMesg) sSwapBufferMsg, OS_MESG_NOBLOCK);
        }
    }
}

/* Audio tasks arrive on cmdQ rather than interruptQ, so it is drained on every
 * pass rather than waited on. */
static void drain_cmd_queue(OSSched *sc) {
    OSMesg msg;

    while (osRecvMesg(&sc->cmdQ, &msg, OS_MESG_NOBLOCK) == 0) {
        run_task(sc, (OSScTask *) msg);
    }
}

/*
 * Retrace and PRE_NMI go to different clients, and this is not libultra
 * behaviour.
 *
 * Stock libultra broadcasts both to every client on the list. Rareware added
 * an `id` field to OSScClient -- the comment in include/PR/sched.h says so
 * outright -- and routes by it, because DKR registers a client whose queue is
 * a reset button rather than a frame clock:
 *
 *     osScAddClient(&gMainSched, ..., &gNMIMesgQueue, OS_SC_ID_PRENMI);
 *
 * and is_reset_pressed() treats *any* message arriving on that queue as the
 * reset button having been pressed. Broadcasting the retrace to it makes the
 * game believe it was reset on the very first frame: thread3_main then kills
 * audio and background loading and spins in a `while (1) ;` forever, which
 * from outside looks exactly like a renderer that draws nothing.
 *
 * So the retrace goes to everyone except the PRE_NMI client, and PRE_NMI goes
 * only to it. The audio and video managers, the two that pace the game, are
 * ids 1 and 2 and keep getting every retrace as before.
 */
static void handle_retrace(OSSched *sc) {
    OSScClient *client;

    COUNT(gGcSchedRetraces);
    sAudioRetraceParity ^= 1;

    for (client = sc->clientList; client != NULL; client = client->next) {
        if (client->id == OS_SC_ID_PRENMI) {
            continue;
        }

        /*
         * The audio client gets one retrace in two, and that is not a tuning
         * choice -- it is what the game's own arithmetic requires.
         *
         * audiomgr sizes an audio frame as `outputRate * 2 / refreshRate`: two
         * video frames' worth of samples, 736 at 22050 Hz and 60 Hz. The
         * synthesiser's clock, `drvr->curSamples`, advances by exactly that per
         * audio frame, and the sequencer turns note durations into samples
         * through the same `outputRate`. So one audio frame per video frame
         * makes the clock run at 44160 samples per second against an
         * outputRate of 22050 -- every note half as long as written, the whole
         * score at double speed.
         *
         * That was measured, not deduced. With one retrace per audio frame the
         * synthesiser's own clock read:
         *
         *     curSamples/s 44640  (real time would be 22050)  ratio 2.02
         *
         * One frame per two retraces gives 30 * 736 = 22080 samples per second,
         * which is real time to within the rounding in frameSize. It is also
         * what makes the producer and the DAC agree: at 60 the game offered
         * ~43000 stereo frames a second against 22050 of consumption, and the
         * surplus had to be thrown away somewhere -- which is what made the
         * sound crackle on top of running fast.
         *
         * Only the audio client is halved. The video client keeps every
         * retrace, because the game's frame pacing and fb_update ride on it.
         */
        if (client->id == OS_SC_ID_AUDIO && sAudioRetraceParity) {
            continue;
        }

        COUNT(gGcClientSends);
        osSendMesg(client->msgQ, (OSMesg) &sc->retraceMsg, OS_MESG_NOBLOCK);
    }
}

static void handle_prenmi(OSSched *sc) {
    OSScClient *client;

    for (client = sc->clientList; client != NULL; client = client->next) {
        if (client->id != OS_SC_ID_PRENMI) {
            continue;
        }
        osSendMesg(client->msgQ, (OSMesg) &sc->prenmiMsg, OS_MESG_NOBLOCK);
    }
}

static void sched_main(void *arg) {
    OSSched *sc = (OSSched *) arg;
    OSMesg msg = NULL;

    for (;;) {
        osRecvMesg(&sc->interruptQ, &msg, OS_MESG_BLOCK);
        COUNT(gGcSchedMsgs);

        switch ((s32) msg) {
            case VIDEO_MSG:
                handle_retrace(sc);
                break;

            case PRE_NMI_MSG:
                handle_prenmi(sc);
                break;

            case RSP_DONE_MSG:
            case RDP_DONE_MSG:
                /* Unreachable: tasks complete inside run_task. */
                break;

            default:
                run_task(sc, (OSScTask *) msg);
                break;
        }

        drain_cmd_queue(sc);
    }
}

/*
 * The audio interface: the N64's AI expressed through libogc's AI/DMA path.
 *
 * The game's audio manager is a producer that runs once per retrace. It asks
 * how many samples are still queued (osAiGetLength), generates just enough to
 * refill (alAudioFrame, executed here by platform/gc/audio), and hands the
 * result over (osAiSetNextBuffer). Nothing about that has to change; what has
 * to change is the sample rate underneath it.
 *
 * The N64 could clock its DAC at whatever frequency the game asked for, and
 * DKR asks for a rate in the low twenty thousands. The GameCube's AI runs at
 * 32 or 48 kHz and nothing else, so the game's stream is resampled on the way
 * out, and the quality of that resampling turned out to matter a great deal:
 * linear interpolation leaves an image of every input tone at (22050 - f) only
 * seven decibels down at 9 kHz, which is what "the music crackles" was. See the
 * note on RS_TAPS.
 *
 * Two rates therefore coexist in this file, and mixing them up is the easy
 * mistake: everything the game sees -- the length it queries, the buffer it
 * passes -- is in *its* rate, while everything in the ring buffer and the DMA
 * is at OUTPUT_RATE_HZ. The conversions are confined to samples_to_game() and
 * its inverse.
 */

#include <ultra64.h>

#include <ogc/audio.h>
#include <ogc/cache.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#include <math.h>
#include <string.h>

#include "gc_ultra.h"

#define OUTPUT_RATE_HZ 48000

/* One DMA block. The AI wants a multiple of 32 bytes; at 48 kHz stereo 16-bit
 * this is a little over 5 ms, short enough to keep latency invisible and long
 * enough that the callback overhead is irrelevant. */
/*
 * 512 rather than 256. The AI raises its interrupt when a block finishes and
 * immediately starts whatever block was latched next; if the callback that
 * latches it has not run by then -- interrupts held off by a card transfer,
 * a long critical section, anything -- the hardware replays the block it has.
 * That is a click at any volume, and one that no ring-depth counter can see,
 * because the ring is full the whole time. Ten milliseconds of block is twice
 * the margin for the same latency; the ring already holds sixty. `ai cb late`
 * in the heartbeat says whether it ever happens.
 */
#define DMA_FRAMES 512
#define DMA_BYTES (DMA_FRAMES * 4)

/*
 * The output ring.
 *
 * Sized from the operating point the game actually steers for, now that the
 * audio manager runs at its intended one frame per two retraces (see the note
 * in ultra/os_sched.c). Its budget is
 *
 *     frameSamples = (16 + (frameSize - samplesLeft + EXTRA_SAMPLES)) & ~0xf
 *
 * which settles where frameSamples equals what one audio frame consumes: 736
 * samples, leaving a backlog of 112 -- about 244 frames here, some 5 ms. One
 * whole buffer is 1568 frames, and the ring has to have that much free at
 * every push or the guard in osAiSetNextBuffer refuses it. Both smaller sizes
 * were measured and rejected: 2048 truncates to a third of a buffer (12.5 % of
 * DAC frames silent, synthesiser starved to 0.89 x real time), 4096 settles
 * where only ~995 of 1568 frames fit (6.8 % silent). 8192 leaves a full buffer
 * free at every push, settles around 5400 frames, and measures 2.4 % -- which
 * is the supply shortfall from frameSamples sitting on its 720 floor, not
 * truncation. The cost is about 112 ms of latency.
 *
 * It was 8192 for a while, to paper over a producer running at twice the
 * consumption rate; that was treating the symptom. With the cadence right the
 * ring does not need to absorb a surplus, because there is no surplus.
 */
#define RING_FRAMES 8192

/*
 * The depth the rate loop steers the ring towards, in output frames.
 *
 * 3072 frames is 64 ms, and it is chosen against two measurements rather than
 * as a round number. It is deep enough that the twelve callbacks between two
 * pushes at the worst normal cadence never reach the bottom, and it absorbs
 * most of what a level load costs -- the log shows the producer going quiet for
 * `gap 17` and `gap 18` callbacks three times in a sixty-second run, which is
 * the one dropout this does not cover. It is also *less* latency than the port
 * had before: the ring used to settle wherever the drift left it, around 5400
 * frames, which is the 112 ms this document has been quoting.
 */
#define RING_TARGET 3072

/*
 * How far two adjacent output samples may be apart before it is a click.
 *
 * A third of full scale. Music at 22 kHz resampled to 48 kHz moves in small
 * steps between neighbours; a jump this large is a splice, not a note. The
 * threshold is deliberately generous -- the aim is to count only what would be
 * audible as a click, not to measure high-frequency content.
 */
#define AUDIO_STEP_THRESHOLD 11000

/*
 * The resampler, and why linear interpolation was the crackle.
 *
 * The recording the user made settled it, and the arithmetic behind it is not
 * subtle once it is written down. Upsampling 22050 to 48000 by linear
 * interpolation leaves an image of every input tone at (22050 - f), attenuated
 * only by the triangle filter's sinc^2. Measured, image against signal:
 *
 *      tone      linear            32-tap windowed sinc
 *     1000 Hz   -52.7 dB            -51.8 dB
 *     3000 Hz   -33.0 dB            -42.3 dB
 *     5000 Hz   -20.7 dB            -38.6 dB
 *     7000 Hz   -13.1 dB            -36.3 dB
 *     9000 Hz    -7.4 dB            -37.8 dB
 *
 * A harmonic at 9 kHz came back with a companion at 13 kHz **seven decibels
 * below it** -- inharmonic, metallic, and riding on top of the note. That is
 * exactly "the music crackles, and only some notes", because only notes with
 * strong high harmonics produce an audible image.
 *
 * The file's own header said "linear interpolation is enough here ... the
 * artefacts sit well below the material". That was an assertion, never
 * measured, and it was wrong by thirty decibels.
 *
 * The fix is a low-pass, because the source has nothing above 11 kHz: every
 * component above that in the output is an image and can be removed without
 * touching anything real. A windowed-sinc polyphase interpolator is that
 * low-pass and the interpolation in one step. 32 taps at 64 phases costs
 * 64 multiply-adds per output frame, about 3 M/s, which is noise against a
 * task that measures 5 to 8 ms in 40.
 */
#define RS_TAPS 32
#define RS_HALF (RS_TAPS / 2)
#define RS_PHASES 64

/* The cutoff, as a fraction of the input rate: 10.5 kHz against 11.025 kHz of
 * input Nyquist. Written as the sinc argument's scale, so h(k) = sinc(RS_BW*k)
 * windowed. Wide enough to keep the material, narrow enough to put the first
 * image in the stopband. */
#define RS_BW 0.9524f

typedef struct {
    s16 l, r;
} Frame;

static Frame sRing[RING_FRAMES] __attribute__((aligned(32)));
static volatile u32 sRingRead;
static volatile u32 sRingWrite;

static u8 sDmaBuf[2][DMA_BYTES] __attribute__((aligned(32)));
static volatile u32 sDmaIndex;

static u32 sGameRate = 22050;
static BOOL sStarted;

/* Fractional read position into the game's buffer, carried across calls so the
 * resampler does not click at buffer boundaries. */
static u32 sResamplePhase = (RS_HALF - 1) << 16;

/* The rate loop's smoothed view of the ring depth, in 24.8 output frames. */
static s32 sDepthAvgQ8;

/* The largest buffer the game has handed over, in its own samples.
 * osAiGetLength reports no more backlog than this -- see the note there. */
static u32 sLastBufferGameSamples = 1;

/* The filter bank, the input tail it reaches back into, and the scratch the two
 * are joined in. Rebuilt only when the game changes its rate. */
static f32 sBank[RS_PHASES][RS_TAPS];
static u32 sBankRate;
static Frame sHist[RS_TAPS];
#define RS_WORK_FRAMES 4096
static Frame sWork[RS_WORK_FRAMES];

/*
 * GC_AUDIOTEST: replace the game's audio with a pure 440 Hz tone, at one of
 * two depths, and listen.
 *
 * Every audio counter this port has says the delivery is correct -- `ai drops
 * 0`, `ringMin` above 2400, `under` frozen at its initial fill, the mixer
 * diffed against ref-sm64gc line by line -- and the console still crackles.
 * When every number says a thing is fine and the ear says it is not, the
 * numbers are measuring the wrong quantity, and the way out is a signal whose
 * correctness needs no counter at all: a sine wave either sounds clean or it
 * does not, and anyone can hear the difference in two seconds.
 *
 * The two levels bisect the pipeline:
 *
 *   1  the tone is written into the ring in place of the resampler's output,
 *      so the rate loop, the ring and the DMA all still run exactly as they
 *      do normally. Clean here means everything below the mixer is sound.
 *   2  the tone is written straight into the DMA block, so the ring and the
 *      push path are bypassed entirely. This is the AI, the interrupt and the
 *      double buffer, and nothing else.
 *
 * Crackle at 2 puts the defect in the delivery, which would clear the mixer
 * and the whole game side outright. Clean at 2 and crackle at 1 puts it in
 * the ring or the rate loop. Clean at both puts it upstream, in what the
 * mixer produces. One session, three possibilities, no counters.
 *
 * Never ship either at anything but 0.
 */
#ifndef GC_AUDIOTEST
#define GC_AUDIOTEST 0
#endif

#if GC_AUDIOTEST
/*
 * The tone has to be exact, and the first version of it was not.
 *
 * It was 440 Hz read out of a 256-entry table with a 16.16 phase accumulator
 * and no interpolation. The phase is then truncated to the nearest table entry
 * on every sample, and the resulting amplitude error reaches 2.7 % of full
 * scale -- **-31.5 dB of harmonic distortion**, computed against an exact sine
 * offline. That is not a subtle artefact; it is plainly audible as a buzz on
 * an otherwise pure tone, and it means the first run of this test measured the
 * instrument rather than the port. Both levels "crackled" and neither result
 * could be believed. This file's own history has the same lesson twice over
 * (the peak counter, the heartbeat's second), and it caught us again.
 *
 * The fix removes the error rather than shrinking it. 48000 / 480 is exactly
 * 100, so a 480 Hz tone is 100 samples per cycle with no remainder: a table of
 * one hundred entries, stepped by one and wrapped at one hundred, replays the
 * *same* hundred samples for ever. There is no phase accumulator, no
 * truncation and no approximation -- every sample that leaves this function is
 * the exactly-rounded value of the sine at that point.
 *
 * The table is built in ai_start, on an ordinary thread. Building it lazily in
 * the DMA callback would put sinf, and therefore the FPU, inside an interrupt
 * handler whose context libogc does not save.
 */
#define TEST_CYCLE 100 /* samples per cycle: 48000 / 480 exactly */

static s16 sTestTable[TEST_CYCLE];
static u32 sTestPos;

static void test_tone_init(void) {
    u32 i;

    for (i = 0; i < TEST_CYCLE; i++) {
        sTestTable[i] =
            (s16) (8000.0f * sinf(2.0f * 3.14159265358979f * (f32) i / (f32) TEST_CYCLE));
    }
}

static void test_tone(Frame *out, u32 frames) {
    u32 i;

    for (i = 0; i < frames; i++) {
        s16 v = sTestTable[sTestPos];

        out[i].l = v;
        out[i].r = v;
        if (++sTestPos >= TEST_CYCLE) {
            sTestPos = 0;
        }
    }
}
#endif

static s16 clamp_s16(f32 v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (s16) v;
}

/*
 * Build the polyphase bank: for phase p, the filter is a sinc delayed by p/N of
 * a sample, windowed and normalised to unity gain so that a constant input
 * comes out constant.
 */
static void resample_bank_build(void) {
    u32 p, k;

    if (sBankRate == sGameRate) {
        return;
    }
    sBankRate = sGameRate;

    for (p = 0; p < RS_PHASES; p++) {
        f32 d = (f32) p / (f32) RS_PHASES;
        f32 sum = 0.0f;

        for (k = 0; k < RS_TAPS; k++) {
            f32 t = (f32) ((s32) k - (RS_HALF - 1)) - d;
            f32 x = RS_BW * t;
            f32 h;

            if (x > -1e-6f && x < 1e-6f) {
                h = 1.0f;
            } else {
                h = sinf(3.14159265358979f * x) / (3.14159265358979f * x);
            }
            /* Blackman, which buys stopband depth at a little transition
             * width -- the right trade when the stopband is where the defect
             * lives. */
            h *= 0.42f - 0.5f * cosf(2.0f * 3.14159265358979f * (f32) k / (f32) (RS_TAPS - 1)) +
                 0.08f * cosf(4.0f * 3.14159265358979f * (f32) k / (f32) (RS_TAPS - 1));
            sBank[p][k] = h;
            sum += h;
        }
        if (sum != 0.0f) {
            for (k = 0; k < RS_TAPS; k++) {
                sBank[p][k] /= sum;
            }
        }
    }
}

/*
 * How many frames are queued.
 *
 * Written out rather than as `(write - read) % RING_FRAMES`, because that form
 * is only correct when RING_FRAMES is a power of two: it relies on the
 * unsigned wrap at 2^32 being a multiple of the modulus. During the sizing work
 * above the ring was briefly 1536, and 2^32 mod 1536 is 1024, not 0 -- so the
 * moment the write index wrapped past the read
 * index the count came out 512 short, the ring was reported nearly empty while
 * full, pushes overwrote unread frames, and 88 % of DAC frames underran. That
 * was measured, not reasoned about: `under` climbing 46700/s against 57000
 * frames served while `pushed` climbed 81000/s -- more pushed than played,
 * which is only possible if the writer is trampling the reader.
 *
 * This form has no such dependency and costs a branch.
 */
static u32 ring_used(void) {
    u32 w = sRingWrite;
    u32 r = sRingRead;

    return (w >= r) ? (w - r) : (RING_FRAMES - r + w);
}

static u32 ring_free(void) {
    return RING_FRAMES - 1 - ring_used();
}

/* Converts a count of output-rate frames into the game's rate, which is the
 * unit osAiGetLength answers in. */
static u32 frames_to_game(u32 frames) {
    return (u32) (((u64) frames * sGameRate) / OUTPUT_RATE_HZ);
}

#ifdef GC_DEBUG
/* The reader side, counted. osAiGetLength was pinned at a full ring forever,
 * which has exactly two causes: the writer outruns the DAC, or the DAC is not
 * draining at all because this callback never fires. `cb` separates them in
 * one run, and a callback that fires but finds nothing to play shows up in
 * `under`. */
u32 gGcAiCallbacks;
u32 gGcAiUnderruns;
/* Underrun *events* rather than frames, the longest single run, how empty the
 * ring gets when the consumer looks, and the longest the producer has ever
 * left it unfed. See the note in dma_callback. */
u32 gGcAiUnderEvents;
u32 gGcAiUnderMax;
u32 gGcAiCbLate;
u32 gGcAiCbMaxUs;
u32 gGcAiRingMin = 0xFFFFFFFFu;
u32 gGcAiPushGapMax;
/* The rate loop's two state variables, so the heartbeat can show it working:
 * the corrected 16.16 step and the smoothed ring depth it is steering. */
u32 gGcAiStep;
u32 gGcAiDepthAvg;
/* Discontinuities in the delivered waveform, and the largest one. This is the
 * crackle measured as sound rather than as delivery. */
u32 gGcAiSteps;
u32 gGcAiStepMax;
static s16 sPrevL;
static u32 sCbSincePush;
u32 gGcAiPushed;
u32 gGcAiRingUsed;
u32 gGcAiRejected;
u32 gGcAiRefusedFull;
/* The ground truth nobody had measured: how many stereo frames the game
 * actually offers per second, against the rate it asked the DAC to run at.
 * Every theory about "the game over-produces" stands or falls on this one
 * ratio, so it gets counted rather than inferred from two other numbers. */
u32 gGcAiOffers;
u32 gGcAiOfferedFrames;
u32 gGcAiGameRate;
#endif

/*
 * Fill one DMA block from the ring.
 *
 * Split out of the callback because *when* this runs is the whole point: it
 * has to happen after the next block is already latched, not before. See
 * dma_callback.
 */
static void fill_block(u8 *buf) {
    Frame *out = (Frame *) buf;
    u32 i;
#ifdef GC_DEBUG
    u32 runHere = 0;
#endif

#if GC_AUDIOTEST == 2
    /* The ring and everything above it are bypassed: what reaches the DAC is
     * generated here, one block at a time, with continuous phase. */
    test_tone(out, DMA_FRAMES);
    DCFlushRange(buf, DMA_BYTES);
    return;
#endif

    for (i = 0; i < DMA_FRAMES; i++) {
        if (sRingRead != sRingWrite) {
            out[i] = sRing[sRingRead];
#ifdef GC_DEBUG
            {
                s32 d = (s32) out[i].l - (s32) sPrevL;

                if (d < 0) {
                    d = -d;
                }
                if (d > AUDIO_STEP_THRESHOLD) {
                    gGcAiSteps++;
                    if ((u32) d > gGcAiStepMax) {
                        gGcAiStepMax = (u32) d;
                    }
                }
                sPrevL = out[i].l;
            }
#endif
            sRingRead = (sRingRead + 1) % RING_FRAMES;
#ifdef GC_DEBUG
            if (runHere > gGcAiUnderMax) {
                gGcAiUnderMax = runHere;
            }
            runHere = 0;
#endif
        } else {
#ifdef GC_DEBUG
            gGcAiUnderruns++;
            if (runHere++ == 0) {
                gGcAiUnderEvents++;
            }
#endif
            /* Underrun. Silence is the right answer: repeating the last frame
             * turns a dropout into a buzz, which is far more audible. */
            out[i].l = 0;
            out[i].r = 0;
        }
    }

#ifdef GC_DEBUG
    if (runHere > gGcAiUnderMax) {
        gGcAiUnderMax = runHere;
    }
#endif

    DCFlushRange(buf, DMA_BYTES);
}

/*
 * The AI has finished a block. Latch the next one FIRST.
 *
 * This order is the fix for the crackle, and the measurement that named it is
 * `ai cb late 8, longest 14260 us` against a 10666 us block: eight times a
 * beat the callback was arriving later than a whole block, which means the AI
 * had reached the end of its buffer with no next address in its registers and
 * had nothing defined to play. Seven or eight such holes a second is exactly
 * "ça grésille".
 *
 * The reason was the order here. The callback used to fill a block from the
 * ring and only then hand it to the hardware, so the AI was left idle for the
 * whole of the fill -- 512 frames of copy, plus a per-sample comparison under
 * GC_DEBUG -- and the next block could not start until that finished. The
 * buffer the AI plays next is now always one that was filled during the
 * *previous* callback, so the register write is the first thing that happens
 * and the fill has a whole block of slack to complete in.
 *
 * That is the ordinary double-buffer discipline for a DMA engine with a single
 * "next" register pair, and it is what ref-sm64gc's audio_ogc.c does with its
 * queue of ready buffers. The port had the two buffers and used them as one.
 */
static void dma_callback(void) {
    u8 *next = sDmaBuf[sDmaIndex ^ 1]; /* filled by the previous callback */

    AUDIO_InitDMA((u32) next, DMA_BYTES);
    AUDIO_StartDMA();
    sDmaIndex ^= 1;

#ifdef GC_DEBUG
    {
    u32 usedAtEntry = ring_used();

    gGcAiCallbacks++;
    {
        /* How long since the previous callback. One block is DMA_FRAMES /
         * 48000 s; anything much past that means the previous block was
         * replayed before this one could be latched. */
        static u64 sPrevCb;
        u64 now = gettime();

        if (sPrevCb != 0) {
            u32 us = (u32) ticks_to_microsecs(diff_ticks(sPrevCb, now));

            if (us > gGcAiCbMaxUs) {
                gGcAiCbMaxUs = us;
            }
            if (us > (DMA_FRAMES * 1000000u / OUTPUT_RATE_HZ) + 1500u) {
                gGcAiCbLate++;
            }
        }
        sPrevCb = now;
    }

    /*
     * How close to empty the ring actually runs, and whether the silence comes
     * in one lump or in scattered single frames.
     *
     * `under` alone said 130 to 265 silent frames per heartbeat -- 0.3 % of the
     * output -- while `ring` said 2040 frames were buffered. Both cannot be
     * describing the same instant, and a total cannot tell a burst from a
     * sprinkle: 256 isolated zero frames are 256 clicks a second, one run of
     * 256 is a single 5 ms dropout, and they need different fixes. So count the
     * events, keep the longest run, and record the depth at the moment the
     * consumer looks -- `ring` is sampled by the *producer*, after it has just
     * filled the ring, which is the one moment it is guaranteed to look full.
     */
    if (usedAtEntry < gGcAiRingMin) {
        gGcAiRingMin = usedAtEntry;
    }
    if (++sCbSincePush > gGcAiPushGapMax) {
        gGcAiPushGapMax = sCbSincePush;
    }
    }
#endif

    /* And only now the work: the block after the one just latched. */
    fill_block(sDmaBuf[sDmaIndex ^ 1]);
}

static void ai_start(void) {
    if (sStarted) {
        return;
    }
    sStarted = TRUE;

#if GC_AUDIOTEST
    /* On a thread, before the first interrupt: see the note on test_tone. */
    test_tone_init();
#endif

    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(dma_callback);

    memset(sDmaBuf, 0, sizeof(sDmaBuf));
    DCFlushRange(sDmaBuf, sizeof(sDmaBuf));

    AUDIO_InitDMA((u32) sDmaBuf[0], DMA_BYTES);
    AUDIO_StartDMA();
    /* The index names the block the AI is playing, and the *other* one is
     * always the one already filled and ready to be latched. Both are silence
     * to begin with, which is the right first block anyway. */
    sDmaIndex = 0;
}

/*
 * Set the rate the game's own sample buffers run at, and return it.
 *
 * The return value is not a status code, and returning 0 here silently broke
 * the whole audio path for days. On the N64 this returns the rate the DAC
 * could actually be programmed to, which is never exactly what was asked for
 * because the divider is integral -- and the audio manager takes that answer
 * as gospel:
 *
 *     fsize = (f32) c->outputRate * 2 / (f32) gVideoRefreshRate;   audiomgr.c
 *
 * so `outputRate` sizes every audio frame the game will ever build. With 0
 * coming back, `frameSize` was 0 and `minFrameSize` was 0 - 16 as unsigned,
 * i.e. 0xFFFFFFF0; the per-frame budget then computed as -16, alAudioFrame was
 * handed a negative outLen, its command-building loop never ran once, and the
 * mixer downstream was faithfully executing an empty list sixty times a
 * second. Measured: `am frameSamples -16 (left 1881, size 0, min 4294967280)`.
 *
 * This port resamples to the DAC's 48 kHz itself (see the note above), so the
 * rate the game should be told about is the one its buffers are actually in --
 * exactly what it asked for, and no divider to round against.
 */
s32 osAiSetFrequency(u32 frequency) {
    {
        static BOOL sSaid;

        if (!sSaid) {
            sSaid = TRUE;
            gc_logfile_mark("init: first osAiSetFrequency\n");
        }
    }
    if (frequency != 0) {
        sGameRate = frequency;
    }
    ai_start();
    return (s32) sGameRate;
}

/*
 * Queues a frame of game audio.
 *
 * `len` is in bytes of stereo 16-bit samples at the game's rate. The samples
 * are resampled into the ring; if the ring is full the tail is dropped, which
 * only happens if the game has run far ahead of the DAC and is preferable to
 * blocking the audio thread.
 */
/* See the note in platform/gc/audio/audio_mixer.c. */
extern const int gGcAudioMixerImplemented;

/* An upper bound on one frame of game audio, in bytes.
 *
 * audiomgr sizes its buffers as maxFrameSize = frameSize + EXTRA_SAMPLES + 16
 * stereo samples, and frameSize is two video frames' worth of the game's rate:
 * 848 samples, 3392 bytes, at 22050 Hz and 60 Hz. This is comfortably above
 * that and still small enough to catch a length that is not a length. */
#define AI_MAX_BUFFER_BYTES 16384

s32 osAiSetNextBuffer(void *buf, u32 len) {
    const Frame *in = (const Frame *) buf;
    u32 inFrames;
    u32 step;
    u32 pos;

    /*
     * Refuse a length the game cannot have meant, and refuse it the way
     * libultra does -- with -1, not by trying.
     *
     * audiomgr passes `lastInfo->frameSamples << 2`, and frameSamples is a
     * *signed* per-frame budget computed by subtracting osAiGetLength's report
     * from a fixed frame size. When that budget goes negative the shift arrives
     * here as a huge unsigned value: measured, -1040 samples became a request
     * for 1073740784 frames. Without this test the loop below happily filled
     * the whole ring from whatever lay past the game's buffer -- which is what
     * the "beeps" were -- and left the ring pinned full, so osAiGetLength kept
     * reporting a full backlog, which kept the budget negative. The port fed
     * its own failure back into the game's accounting and neither side could
     * get out.
     *
     * Rejecting instead lets the ring drain, which drops the reported backlog,
     * which makes the next budget positive again: the game recovers by itself.
     */
    if ((s32) len <= 0 || len > AI_MAX_BUFFER_BYTES) {
#ifdef GC_DEBUG
        gGcAiRejected++;
#endif
        return -1;
    }

    inFrames = len / 4;
    if (inFrames == 0) {
        return 0;
    }
    ai_start();

    if (inFrames > sLastBufferGameSamples) {
        sLastBufferGameSamples = inFrames;
    }

#ifdef GC_DEBUG
    gGcAiOffers++;
    gGcAiOfferedFrames += inFrames;
    gGcAiGameRate = sGameRate;
#endif

    /*
     * All of the buffer, or none of it -- a guard against truncation, not a
     * regulator.
     *
     * The loop below stops when the ring fills, which means a push that does
     * not fit is silently cut short: the first part of the buffer plays and the
     * rest is dropped, so the waveform jumps to the start of the next buffer.
     * That is what crackles. Measured with a 4096-frame ring and no guard: the
     * ring settles where only ~995 of the 1568 frames fit, and 6.8 % of DAC
     * frames come out silent.
     *
     * With the audio manager running at its intended cadence this never
     * actually fires (`ref+0` in the heartbeat, every beat). It is here so that
     * a future change which does upset the balance fails as a whole dropped
     * buffer -- which is how the N64's two-deep AI queue failed -- rather than
     * as a chopped one.
     */
    {
        u32 needed = (u32) (((u64) inFrames * OUTPUT_RATE_HZ + sGameRate - 1) / sGameRate);

        if (ring_free() < needed) {
#ifdef GC_DEBUG
            gGcAiRefusedFull++;
#endif
            return -1;
        }
    }

    /*
     * The resampling step, corrected for the drift between the two clocks.
     *
     * This is the crackle, and the tenth session measured it exactly. On the
     * user's PAL console the game emits one audio frame every two retraces --
     * 25 a second -- of `frameSamples` samples each, and `frameSamples` sits at
     * **880**. That is 22 000 samples a second. The resampler was converting
     * them as if they were 22 050, because that is what `osAiSetFrequency` was
     * told, so it produced 47 891 output frames a second against a DAC that
     * consumes exactly 48 000.
     *
     * A 0.23 % shortfall, which is 109 frames a second. The log shows the ring
     * draining at 136:
     *
     *     ringMin 2299 -> 1525 -> 1907 -> 878 -> 1510 -> ... -> 342 -> 247 -> 124
     *
     * and then sticking there, at which point every callback comes up a
     * fraction of a block short. The signature is unmistakable once the
     * instrument reports the shape: `ringMin 247, longest 9` and `ringMin 124,
     * longest 132` -- both exactly `256 - longest`, one partial DMA block, ten
     * to sixteen times a second. Ten clicks a second is what "ça craque"
     * sounds like.
     *
     * On the N64 this cannot happen, and that is why the game gets away with
     * it: there is no fixed-rate consumer. The AI plays whatever buffer the
     * game hands it, so an undersupply of 0.23 % simply means the music runs
     * 0.23 % slow and nobody can hear it. Here libogc's DAC is a real clock.
     *
     * Fixing `sGameRate` to 22 000 would work on this console and be wrong on
     * the next one: `frameSamples` is derived from the video rate and the
     * game's own rounding (`& ~0xf`), so it differs between PAL and NTSC and
     * moves within a session. What is right is to stop trusting the nominal
     * rate at all and steer on the one quantity that states the truth -- how
     * deep the ring actually is.
     *
     * So: a slow proportional loop on the ring depth, smoothed over about
     * sixty pushes so that it tracks the drift and not the jitter, clamped to
     * +/- 1.5 % so that a bug here can never become a pitch bend. 0.23 % is a
     * pitch error of four cents, which is inaudible; a dropout ten times a
     * second is not.
     */
    {
        s32 base = (s32) (((u64) sGameRate << 16) / OUTPUT_RATE_HZ);
        s32 used = (s32) ring_used();
        s32 err, corr, maxCorr;

        if (sDepthAvgQ8 == 0) {
            sDepthAvgQ8 = used << 8; /* no ramp on the first push */
        }
        sDepthAvgQ8 += ((used << 8) - sDepthAvgQ8) >> 6;

        err = (sDepthAvgQ8 >> 8) - RING_TARGET;
        corr = err / 8;
        maxCorr = base / 64;
        if (corr > maxCorr) {
            corr = maxCorr;
        } else if (corr < -maxCorr) {
            corr = -maxCorr;
        }
        step = (u32) (base + corr);
#ifdef GC_DEBUG
        gGcAiStep = step;
        gGcAiDepthAvg = (u32) (sDepthAvgQ8 >> 8);
#endif
    }
    pos = sResamplePhase;

    /*
     * The work buffer: the tail of the previous push, then this one.
     *
     * A symmetric filter needs samples on both sides of the point it is
     * interpolating, so the resampler runs RS_HALF input samples behind the
     * newest one and keeps the last RS_TAPS frames to reach backwards into.
     * That is 0.7 ms of extra delay at 22 kHz, which is nothing against the
     * ring's 64.
     */
    resample_bank_build();
    memcpy(sWork, sHist, sizeof(sHist));
    if (inFrames > RS_WORK_FRAMES - RS_TAPS) {
        inFrames = RS_WORK_FRAMES - RS_TAPS;
    }
    memcpy(&sWork[RS_TAPS], in, inFrames * sizeof(Frame));

    while ((pos >> 16) + RS_HALF < RS_TAPS + inFrames && ring_free() > 0) {
        u32 i = pos >> 16;
        u32 ph = ((pos & 0xFFFF) * RS_PHASES) >> 16;
        const f32 *h = sBank[ph];
        const Frame *s = &sWork[i - RS_HALF + 1];
        Frame *dst = &sRing[sRingWrite];

#if GC_AUDIOTEST == 1
        /* The tone takes the resampler's place, one frame per iteration, so
         * the ring fills and drains at exactly the rate it normally would. */
        test_tone(dst, 1);
        (void) h;
        (void) s;
#else
        if (gGcAudioMixerImplemented) {
            f32 l = 0.0f;
            f32 r = 0.0f;
            u32 k;

            for (k = 0; k < RS_TAPS; k++) {
                l += h[k] * (f32) s[k].l;
                r += h[k] * (f32) s[k].r;
            }
            dst->l = clamp_s16(l);
            dst->r = clamp_s16(r);
        } else {
            dst->l = 0;
            dst->r = 0;
        }
#endif

        sRingWrite = (sRingWrite + 1) % RING_FRAMES;
        pos += step;
#ifdef GC_DEBUG
        gGcAiPushed++;
#endif
    }

    /* Keep the last RS_TAPS frames for the next push to reach back into. */
    memcpy(sHist, &sWork[inFrames], sizeof(sHist));

#ifdef GC_DEBUG
    gGcAiRingUsed = ring_used();
    sCbSincePush = 0;
#endif

    /*
     * Carry the position into the next buffer.
     *
     * The work buffer shifts by inFrames between pushes -- what was
     * sWork[RS_TAPS + inFrames] becomes sWork[RS_TAPS] -- so the position in
     * work coordinates moves back by exactly that. It settles around RS_HALF,
     * which is the filter's built-in delay, and never at zero.
     */
    sResamplePhase = pos - (inFrames << 16);
    if ((s32) sResamplePhase < (s32) ((RS_HALF - 1) << 16)) {
        sResamplePhase = (RS_HALF - 1) << 16;
    }
    return 0;
}

/*
 * How much audio is still queued, in samples at the game's rate.
 *
 * The audio manager subtracts this from its target buffer depth to decide how
 * much to generate, so under-reporting makes it generate too much and drift
 * ahead; over-reporting starves the DAC. Reporting the ring depth exactly is
 * what keeps the two clocks locked.
 */
u32 osAiGetLength(void) {
    u32 backlog = frames_to_game(ring_used());

    /*
     * Report at most one buffer's worth.
     *
     * The N64 returned the remaining length of the DMA in progress, so this
     * could never exceed a single buffer, and audiomgr's budget arithmetic is
     * built on that ceiling: it goes negative once the backlog passes 848
     * samples at this rate, and nothing in the game catches it because the
     * clamp below the subtraction tests (u32).
     *
     * Capping costs nothing the game acts on -- its clamp to minFrameSize
     * already binds for any backlog above 128 samples, so across the whole
     * capped range the budget it computes is the constant 720 either way. What
     * the cap removes is only the negative excursion, and removing it was
     * measured: without the cap, `left 748` with 3 buffers a second rejected
     * for a bad length and the synthesiser dropping to 0.89 x real time.
     */
    if (backlog > sLastBufferGameSamples) {
        backlog = sLastBufferGameSamples;
    }

    /* The caller divides by four to get frames, so answer in bytes. */
    return backlog * 4;
}

u32 osAiGetStatus(void) {
    return 0;
}

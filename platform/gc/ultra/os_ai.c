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
 * out. Linear interpolation is enough here: the source is already a mix of
 * 16 kHz-ish ADPCM samples, and the ratio is close to a small rational, so the
 * artefacts sit well below the material.
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

#include <string.h>

#include "gc_ultra.h"

#define OUTPUT_RATE_HZ 48000

/* One DMA block. The AI wants a multiple of 32 bytes; at 48 kHz stereo 16-bit
 * this is a little over 5 ms, short enough to keep latency invisible and long
 * enough that the callback overhead is irrelevant. */
#define DMA_FRAMES 256
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
static u32 sResamplePhase;

/* The rate loop's smoothed view of the ring depth, in 24.8 output frames. */
static s32 sDepthAvgQ8;

/* The largest buffer the game has handed over, in its own samples.
 * osAiGetLength reports no more backlog than this -- see the note there. */
static u32 sLastBufferGameSamples = 1;

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
u32 gGcAiRingMin = 0xFFFFFFFFu;
u32 gGcAiPushGapMax;
/* The rate loop's two state variables, so the heartbeat can show it working:
 * the corrected 16.16 step and the smoothed ring depth it is steering. */
u32 gGcAiStep;
u32 gGcAiDepthAvg;
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

static void dma_callback(void) {
    u8 *buf = sDmaBuf[sDmaIndex];
    Frame *out = (Frame *) buf;
    u32 i;
#ifdef GC_DEBUG
    u32 runHere = 0;
    u32 usedAtEntry = ring_used();
#endif

#ifdef GC_DEBUG
    gGcAiCallbacks++;

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
#endif

    for (i = 0; i < DMA_FRAMES; i++) {
        if (sRingRead != sRingWrite) {
            out[i] = sRing[sRingRead];
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
    AUDIO_InitDMA((u32) buf, DMA_BYTES);
    AUDIO_StartDMA();
    sDmaIndex ^= 1;
}

static void ai_start(void) {
    if (sStarted) {
        return;
    }
    sStarted = TRUE;

    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(dma_callback);

    memset(sDmaBuf, 0, sizeof(sDmaBuf));
    DCFlushRange(sDmaBuf, sizeof(sDmaBuf));

    AUDIO_InitDMA((u32) sDmaBuf[0], DMA_BYTES);
    AUDIO_StartDMA();
    sDmaIndex = 1;
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

    while ((pos >> 16) < inFrames && ring_free() > 0) {
        u32 i = pos >> 16;
        u32 frac = pos & 0xFFFF;
        u32 j = (i + 1 < inFrames) ? i + 1 : i;
        Frame *dst = &sRing[sRingWrite];

        if (gGcAudioMixerImplemented) {
            dst->l = (s16) (in[i].l + (((s32) (in[j].l - in[i].l) * (s32) frac) >> 16));
            dst->r = (s16) (in[i].r + (((s32) (in[j].r - in[i].r) * (s32) frac) >> 16));
        } else {
            dst->l = 0;
            dst->r = 0;
        }

        sRingWrite = (sRingWrite + 1) % RING_FRAMES;
        pos += step;
#ifdef GC_DEBUG
        gGcAiPushed++;
#endif
    }

#ifdef GC_DEBUG
    gGcAiRingUsed = ring_used();
    sCbSincePush = 0;
#endif

    /* Carry the leftover fraction into the next buffer. */
    sResamplePhase = pos - (inFrames << 16);
    if ((s32) sResamplePhase < 0) {
        sResamplePhase = 0;
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

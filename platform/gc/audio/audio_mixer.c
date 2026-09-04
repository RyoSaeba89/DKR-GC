/*
 * The audio task: executing an N64 audio command list on the CPU.
 *
 * The game's sound engine is unchanged by the port. libultra's synthesiser
 * still runs -- alAudioFrame still walks the sequence and sound players once
 * per retrace and still emits a list of Acmd words. What is gone is the RSP
 * that used to execute that list, so this file executes it instead.
 *
 * The command set is the audio microcode ABI in include/PR/abi.h: a small
 * stack machine over a scratch area of 16-bit samples, with commands to load
 * and store buffers, decode ADPCM, resample, apply an envelope, mix with a
 * gain, and finally interleave a stereo pair out to DRAM. Fifteen opcodes
 * carry all the work, and all fifteen are implemented here -- that is not an
 * estimate, it is a census of the aXxx macros the synthesiser under
 * libultra/src/audio actually expands:
 *
 *     aSetBuffer 33  aMix 12  aLoadBuffer 10  aClearBuffer 10  aSaveBuffer 7
 *     aSetVolume 5   aDMEMMove 5   aResample 3  aLoadADPCM 3   aSegment 2
 *     aPoleFilter 2  aEnvMixer 2   aSetLoop 1   aInterleave 1  aADPCMdec 1
 *
 * Two properties make this tractable. The buffers are plain arrays rather than
 * anything hardware-shaped, so "DMEM" is just a static array here. And both
 * machines are big-endian, so sample data lifted straight out of the asset
 * image needs no byte swapping -- which is the single largest reason a port
 * like this is easier from N64 to GameCube than to a little-endian target.
 *
 * The arithmetic below is sm64-port's src/pc/mixer.c, scalar path, transcribed
 * opcode by opcode: the microcode is shared between the two games, so the
 * semantics carry across exactly. The one exception is A_POLEF, which
 * sm64-port does not implement and which is reconstructed here from the
 * coefficients its caller builds; its comment carries the derivation. What is
 * *not* shared, and is the port's own
 * work, is everything around it -- the command walker (sm64-port has none: it
 * redefines the aXxx macros to call the implementations directly and never
 * builds a list at all), the address normalisation below, and the instrument.
 *
 * DKR cannot take sm64-port's shortcut. audiomgr.c genuinely builds a list
 * into a double-buffered ACMDList, reads back the length alAudioFrame returns,
 * and hands the buffer to the scheduler as an OS_TASK; bypassing that would
 * mean rewriting the audio manager. Decoding w0/w1 keeps the game's
 * architecture exactly as the ROM has it.
 */

#include <ultra64.h>

#include <gctypes.h>

#include <string.h>

#include "../gc_ultra.h"

/* ---- DMEM ---------------------------------------------------------------- */

/*
 * The microcode's scratch memory. The command list's addresses are offsets
 * into it, and libultra fixes the layout in synthInternals.h from
 * AL_MAX_RSP_SAMPLES = 160: the highest buffer is AL_AUX_R_OUT at 2048, one
 * more 320-byte buffer long, so 2368 bytes are ever addressed. 0x1000 leaves
 * the rounding-up every opcode does (to 8, 16 or 32 bytes) room to spill into
 * without a bounds test in the inner loops.
 */
#define AUDIO_DMEM_SIZE 0x1000

/*
 * A guard on either side, and the reason for it.
 *
 * The resampler writes up to eight samples *below* its input pointer to
 * reinstate the tail of the previous call, and reads four below that. With the
 * resampler's input at AL_DECODER_OUT (320) that never reaches address zero,
 * so the guard is not load-bearing today -- it is there so a future buffer at
 * offset 0 cannot corrupt whatever the linker happened to place in front of
 * this array. sm64-port relies on the equivalent writes landing in the struct
 * fields declared before its buffer, which is not a property worth inheriting.
 */
#define AUDIO_DMEM_GUARD 64

static u8 sDmem[AUDIO_DMEM_GUARD + AUDIO_DMEM_SIZE + AUDIO_DMEM_GUARD]
    __attribute__((aligned(32)));

#define DMEM_U8(a)  (sDmem + AUDIO_DMEM_GUARD + (a))
#define DMEM_S16(a) ((s16 *) (sDmem + AUDIO_DMEM_GUARD + ((a) & ~1u)))

#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v)  (((v) + 7) & ~7)

/* ---- DRAM addresses ------------------------------------------------------ */

/*
 * Normalising an address the command list carries.
 *
 * The list holds two different flavours of DRAM pointer, and they do not
 * arrive the same way:
 *
 *   - most sites pass osVirtualToPhysical(p), which this port defines as the
 *     identity precisely so the mixer receives something it can dereference
 *     (see the note in ultra/os_system.c);
 *   - load.c passes K0_TO_PHYS(p) for the ADPCM decoder state and loop state,
 *     and that macro is a literal `& 0x1FFFFFFF`. It is not a call the port
 *     can intercept, and on the GameCube it strips the 0x80000000 that names
 *     MEM1.
 *
 * So the walker puts the bit back on anything that has lost it, exactly as
 * segmented_to_virtual does for display-list pointers in gfx/gfx_gx.c. All of
 * MEM1 lives at 0x80000000 and nothing the audio path touches lives below it,
 * so the test is unambiguous.
 */
static void *dram(u32 addr) {
    if (addr < 0x80000000u) {
        addr |= 0x80000000u;
    }
    return (void *) addr;
}

/* ---- microcode state ----------------------------------------------------- */

static struct {
    u16 in;
    u16 out;
    u16 nbytes;

    s16 vol[2];

    u16 dry_right;
    u16 wet_left;
    u16 wet_right;

    s16 target[2];
    s32 rate[2];

    s16 vol_dry;
    s16 vol_wet;

    s16 *adpcm_loop_state;

    s16 adpcm_table[8][2][8];
} sRspa;

static const s16 sResampleTable[64][4] = {
    { 0x0c39,  0x66ad,  0x0d46,    -33 }, { 0x0b39,  0x6696,  0x0e5f,    -40 },
    { 0x0a44,  0x6669,  0x0f83,    -48 }, { 0x095a,  0x6626,  0x10b4,    -56 },
    { 0x087d,  0x65cd,  0x11f0,    -65 }, { 0x07ab,  0x655e,  0x1338,    -74 },
    { 0x06e4,  0x64d9,  0x148c,    -84 }, { 0x0628,  0x643f,  0x15eb,    -95 },
    { 0x0577,  0x638f,  0x1756,   -106 }, { 0x04d1,  0x62cb,  0x18cb,   -118 },
    { 0x0435,  0x61f3,  0x1a4c,   -130 }, { 0x03a4,  0x6106,  0x1bd7,   -143 },
    { 0x031c,  0x6007,  0x1d6c,   -156 }, { 0x029f,  0x5ef5,  0x1f0b,   -170 },
    { 0x022a,  0x5dd0,  0x20b3,   -184 }, { 0x01be,  0x5c9a,  0x2264,   -198 },
    { 0x015b,  0x5b53,  0x241e,   -212 }, { 0x0101,  0x59fc,  0x25e0,   -226 },
    { 0x00ae,  0x5896,  0x27a9,   -240 }, { 0x0063,  0x5720,  0x297a,   -254 },
    { 0x001f,  0x559d,  0x2b50,   -268 }, {    -30,  0x540d,  0x2d2c,   -280 },
    {    -84,  0x5270,  0x2f0d,   -293 }, {   -132,  0x50c7,  0x30f3,   -304 },
    {   -173,  0x4f14,  0x32dc,   -314 }, {   -210,  0x4d57,  0x34c8,   -323 },
    {   -241,  0x4b91,  0x36b6,   -330 }, {   -267,  0x49c2,  0x38a5,   -336 },
    {   -289,  0x47ed,  0x3a95,   -340 }, {   -306,  0x4611,  0x3c85,   -341 },
    {   -320,  0x4430,  0x3e74,   -340 }, {   -330,  0x424a,  0x4060,   -337 },
    {   -337,  0x4060,  0x424a,   -330 }, {   -340,  0x3e74,  0x4430,   -320 },
    {   -341,  0x3c85,  0x4611,   -306 }, {   -340,  0x3a95,  0x47ed,   -289 },
    {   -336,  0x38a5,  0x49c2,   -267 }, {   -330,  0x36b6,  0x4b91,   -241 },
    {   -323,  0x34c8,  0x4d57,   -210 }, {   -314,  0x32dc,  0x4f14,   -173 },
    {   -304,  0x30f3,  0x50c7,   -132 }, {   -293,  0x2f0d,  0x5270,    -84 },
    {   -280,  0x2d2c,  0x540d,    -30 }, {   -268,  0x2b50,  0x559d, 0x001f },
    {   -254,  0x297a,  0x5720, 0x0063 }, {   -240,  0x27a9,  0x5896, 0x00ae },
    {   -226,  0x25e0,  0x59fc, 0x0101 }, {   -212,  0x241e,  0x5b53, 0x015b },
    {   -198,  0x2264,  0x5c9a, 0x01be }, {   -184,  0x20b3,  0x5dd0, 0x022a },
    {   -170,  0x1f0b,  0x5ef5, 0x029f }, {   -156,  0x1d6c,  0x6007, 0x031c },
    {   -143,  0x1bd7,  0x6106, 0x03a4 }, {   -130,  0x1a4c,  0x61f3, 0x0435 },
    {   -118,  0x18cb,  0x62cb, 0x04d1 }, {   -106,  0x1756,  0x638f, 0x0577 },
    {    -95,  0x15eb,  0x643f, 0x0628 }, {    -84,  0x148c,  0x64d9, 0x06e4 },
    {    -74,  0x1338,  0x655e, 0x07ab }, {    -65,  0x11f0,  0x65cd, 0x087d },
    {    -56,  0x10b4,  0x6626, 0x095a }, {    -48,  0x0f83,  0x6669, 0x0a44 },
    {    -40,  0x0e5f,  0x6696, 0x0b39 }, {    -33,  0x0d46,  0x66ad, 0x0c39 }
};

static inline s16 clamp16(s32 v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (s16) v;
}

static inline s32 clamp32(s64 v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (s32) v;
}

/* ---- the opcodes --------------------------------------------------------- */

static void a_clear_buffer(u16 addr, int nbytes) {
    memset(DMEM_U8(addr), 0, (size_t) ROUND_UP_16(nbytes));
}

static void a_load_buffer(const void *src) {
    memcpy(DMEM_U8(sRspa.in), src, (size_t) ROUND_UP_8(sRspa.nbytes));
}

static void a_save_buffer(s16 *dst) {
    memcpy(dst, DMEM_S16(sRspa.out), (size_t) ROUND_UP_8(sRspa.nbytes));
}

static void a_load_adpcm(int num_entries_times_16, const void *book) {
    memcpy(sRspa.adpcm_table, book, (size_t) num_entries_times_16);
}

static void a_set_buffer(u8 flags, u16 in, u16 out, u16 nbytes) {
    if (flags & A_AUX) {
        sRspa.dry_right = in;
        sRspa.wet_left = out;
        sRspa.wet_right = nbytes;
        return;
    }
    sRspa.in = in;
    sRspa.out = out;
    sRspa.nbytes = nbytes;
}

static void a_set_volume(u8 flags, s16 v, s16 t, s16 r) {
    if (flags & A_AUX) {
        sRspa.vol_dry = v;
        sRspa.vol_wet = r;
    } else if (flags & A_VOL) {
        sRspa.vol[(flags & A_LEFT) ? 0 : 1] = v;
    } else {
        int c = (flags & A_LEFT) ? 0 : 1;

        sRspa.target[c] = v;
        sRspa.rate[c] = (s32) ((u32) (u16) t << 16 | (u32) (u16) r);
    }
}

/*
 * Interleave two mono buffers into one stereo buffer.
 *
 * `nbytes` is the size of ONE channel, so the command produces `nbytes/2`
 * stereo pairs -- twice `nbytes` bytes of output. That factor is the whole
 * point of this comment, because getting it wrong is not silent:
 *
 * This port originally wrote `ROUND_UP_16(nbytes) / sizeof(s16) / 2` pairs,
 * i.e. half of them, and left the upper half of the output buffer untouched.
 * save.c then saves `outCount << 2` bytes from DMEM 0 regardless, so every
 * audio frame carried 160 real stereo samples followed by 160 samples of
 * whatever was last at AL_TEMP_0 -- which is AL_RESAMPLER_OUT, the raw
 * un-enveloped voice output, at full amplitude. That is what pinned the
 * `peak` counter to 32767 on nearly every beat, and it is what the user heard
 * as "inaudible, ca craque dans tous les sens": half of every frame was noise.
 *
 * ref-sm64gc's aInterleaveImpl does `ROUND_UP_16(nbytes) / sizeof(s16) / 8`
 * iterations of eight pairs, which is the same count written differently:
 * nbytes/2 pairs. Unrolling by eight buys nothing here, so it is one pair per
 * iteration with the count spelt out.
 */
static void a_interleave(u16 left, u16 right) {
    int count = ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16);
    s16 *l = DMEM_S16(left);
    s16 *r = DMEM_S16(right);
    s16 *d = DMEM_S16(sRspa.out);

    while (count > 0) {
        *d++ = *l++;
        *d++ = *r++;
        --count;
    }
}

static void a_dmem_move(u16 in_addr, u16 out_addr, int nbytes) {
    memmove(DMEM_U8(out_addr), DMEM_U8(in_addr), (size_t) ROUND_UP_16(nbytes));
}

static void a_set_loop(s16 *loop_state) {
    sRspa.adpcm_loop_state = loop_state;
}

static void a_adpcm_dec(u8 flags, s16 *state) {
    u8 *in = DMEM_U8(sRspa.in);
    s16 *out = DMEM_S16(sRspa.out);
    int nbytes = ROUND_UP_32(sRspa.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if (flags & A_LOOP) {
        memcpy(out, sRspa.adpcm_loop_state, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
    out += 16;

    while (nbytes > 0) {
        int shift = *in >> 4;          /* 0..12 */
        int table_index = *in++ & 0xf; /* 0..7  */
        s16 (*tbl)[8] = sRspa.adpcm_table[table_index];
        int i;

        for (i = 0; i < 2; i++) {
            s16 ins[8];
            s16 prev1 = out[-1];
            s16 prev2 = out[-2];
            int j, k;

            for (j = 0; j < 4; j++) {
                ins[j * 2] = (s16) ((((*in >> 4) << 28) >> 28) << shift);
                ins[j * 2 + 1] = (s16) ((((*in++ & 0xf) << 28) >> 28) << shift);
            }
            for (j = 0; j < 8; j++) {
                s32 acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);

                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
        nbytes -= 16 * (int) sizeof(s16);
    }
    memcpy(state, out - 16, 16 * sizeof(s16));
}

static void a_resample(u8 flags, u16 pitch, s16 *state) {
    s16 tmp[16];
    s16 *in_initial = DMEM_S16(sRspa.in);
    s16 *in = in_initial;
    s16 *out = DMEM_S16(sRspa.out);
    int nbytes = ROUND_UP_16(sRspa.nbytes);
    u32 pitch_accumulator;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(s16));
    } else {
        memcpy(tmp, state, 16 * sizeof(s16));
    }
    /* Bit 1 of the resampler's flags is not one of the named A_* constants --
     * abi.h gives that bit three different names for three different opcodes.
     * It is the "continue from the saved tail" bit, kept numeric here rather
     * than given a name this port has not verified. */
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / (int) sizeof(s16);
    }
    in -= 4;
    pitch_accumulator = (u16) tmp[4];
    memcpy(in, tmp, 4 * sizeof(s16));

    do {
        for (i = 0; i < 8; i++) {
            const s16 *tbl = sResampleTable[pitch_accumulator * 64 >> 16];
            s32 sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                         ((in[1] * tbl[1] + 0x4000) >> 15) +
                         ((in[2] * tbl[2] + 0x4000) >> 15) +
                         ((in[3] * tbl[3] + 0x4000) >> 15);

            *out++ = clamp16(sample);

            pitch_accumulator += (u32) pitch << 1;
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * (int) sizeof(s16);
    } while (nbytes > 0);

    state[4] = (s16) pitch_accumulator;
    memcpy(state, in, 4 * sizeof(s16));
    i = (int) (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = (s16) i;
    memcpy(state + 8, in, 8 * sizeof(s16));
}

static void a_env_mixer(u8 flags, s16 *state) {
    s16 *in = DMEM_S16(sRspa.in);
    s16 *dry[2] = { DMEM_S16(sRspa.out), DMEM_S16(sRspa.dry_right) };
    s16 *wet[2] = { DMEM_S16(sRspa.wet_left), DMEM_S16(sRspa.wet_right) };
    int nbytes = ROUND_UP_16(sRspa.nbytes);
    s16 target[2];
    s32 rate[2];
    s16 vol_dry, vol_wet;
    s32 step_diff[2];
    s32 vols[2][8];
    int c, i;

    if (flags & A_INIT) {
        target[0] = sRspa.target[0];
        target[1] = sRspa.target[1];
        rate[0] = sRspa.rate[0];
        rate[1] = sRspa.rate[1];
        vol_dry = sRspa.vol_dry;
        vol_wet = sRspa.vol_wet;

        /*
         * Both slopes are derived from vol[0], and that is not a typo being
         * copied blindly -- it is the reference being trusted over a tidier
         * reading.
         *
         * This port first "corrected" the second line to vol[1], on the
         * argument that pairing a left-derived slope with a right-derived base
         * could not be what the microcode does. That argument was reasoning,
         * not measurement, against an implementation that has been checked
         * against real hardware output for years. The rule for this file is
         * that it transcribes sm64-port's scalar path; where it wants to
         * disagree, it needs a measurement first, and there was none.
         */
        step_diff[0] = sRspa.vol[0] * (rate[0] - 0x10000) / 8;
        step_diff[1] = sRspa.vol[0] * (rate[1] - 0x10000) / 8;

        for (i = 0; i < 8; i++) {
            vols[0][i] = clamp32((s64) ((s32) sRspa.vol[0] << 16) +
                                 (s64) step_diff[0] * (i + 1));
            vols[1][i] = clamp32((s64) ((s32) sRspa.vol[1] << 16) +
                                 (s64) step_diff[1] * (i + 1));
        }
    } else {
        memcpy(vols[0], state, 32);
        memcpy(vols[1], state + 16, 32);
        target[0] = state[32];
        target[1] = state[35];
        rate[0] = ((s32) state[33] << 16) | (u16) state[34];
        rate[1] = ((s32) state[36] << 16) | (u16) state[37];
        vol_dry = state[38];
        vol_wet = state[39];
    }

    do {
        for (c = 0; c < 2; c++) {
            for (i = 0; i < 8; i++) {
                if ((rate[c] >> 16) > 0) {
                    /* Rising towards the target: never overshoot it. */
                    if ((vols[c][i] >> 16) > target[c]) {
                        vols[c][i] = (s32) target[c] << 16;
                    }
                } else {
                    /* Falling towards it: same, from the other side. */
                    if ((vols[c][i] >> 16) < target[c]) {
                        vols[c][i] = (s32) target[c] << 16;
                    }
                }
                dry[c][i] = clamp16((dry[c][i] * 0x7fff +
                                     in[i] * (((vols[c][i] >> 16) * vol_dry + 0x4000) >> 15) +
                                     0x4000) >> 15);
                if (flags & A_AUX) {
                    wet[c][i] = clamp16((wet[c][i] * 0x7fff +
                                         in[i] * (((vols[c][i] >> 16) * vol_wet + 0x4000) >> 15) +
                                         0x4000) >> 15);
                }
                vols[c][i] = clamp32((s64) vols[c][i] * rate[c] >> 16);
            }
            dry[c] += 8;
            if (flags & A_AUX) {
                wet[c] += 8;
            }
        }
        nbytes -= 16;
        in += 8;
    } while (nbytes > 0);

    memcpy(state, vols[0], 32);
    memcpy(state + 16, vols[1], 32);
    state[32] = target[0];
    state[35] = target[1];
    state[33] = (s16) (rate[0] >> 16);
    state[34] = (s16) rate[0];
    state[36] = (s16) (rate[1] >> 16);
    state[37] = (s16) rate[1];
    state[38] = vol_dry;
    state[39] = vol_wet;
}

/*
 * A_POLEF: the reverb's damping filter.
 *
 * This is the one opcode ref-sm64gc does not implement, so unlike everything
 * above it is reconstructed rather than transcribed. The reconstruction is not
 * guesswork -- the caller pins the arithmetic down completely.
 *
 * `_filterBuffer` (libultra/src/audio/mips1/reverb.c:429) emits exactly three
 * commands:
 *
 *     aSetBuffer (0, buff, buff, count << 1);          // in place
 *     aLoadADPCM (32, lp->fcvec.fccoef);               // 16 s16 coefficients
 *     aPoleFilter(lp->first, lp->fgain, lp->fstate);
 *
 * and `_init_lpfilter` (drvrnew.c) fills those sixteen coefficients:
 *
 *     fc            = (lp->fc * 16384) >> 15
 *     lp->fgain     = 16384 - fc
 *     fccoef[0..7]  = 0
 *     fccoef[8+k]   = 16384 * (fc/16384)^(k+1),  k = 0..7
 *
 * Three things follow, and together they leave no freedom.
 *
 * 1. Loading the coefficients through aLoadADPCM puts them in the ADPCM book,
 *    which the decoder reads as book[0][0..7] (the two-samples-back predictor)
 *    and book[1][0..7] (the one-sample-back predictor). Here the first half is
 *    all zeros: the second pole is switched off, and what the reverb actually
 *    asks for is a one-pole filter. The eight powers in the second half are the
 *    RSP computing eight outputs per vector step; a scalar CPU needs only the
 *    first, because y[n-1] is available at every sample.
 *
 * 2. The fixed-point base is 16384, not the decoder's 2048 -- SCALE in
 *    drvrnew.c, and the reason `fgain = SCALE - fc` is written that way.
 *
 * 3. fgain + fc == 16384 exactly, so the filter has unity gain at DC. That is
 *    the property that makes it safe inside the delay loop, and the check that
 *    the shift is right: any other shift would make the reverb either die or
 *    run away.
 *
 * So per sample:  y = (fcoef[0] * y[-2] + fcoef[8] * y[-1] + gain * x) >> 14.
 *
 * The two-samples-back term is kept even though this game always loads zero
 * there, because it costs one multiply and it is what makes this a *pole*
 * filter rather than a special case that happens to work.
 *
 * State is POLEF_STATE, `short[4]` (include/PR/abi.h:250). Two slots hold the
 * output history; A_INIT means start from silence, which is what `lp->first`
 * requests on the first frame after the reverb is built.
 *
 * Why this matters beyond the tail's colour: `_saveBuffer(out_ptr, buff2)`
 * writes the *filtered* buffer back into the delay line, so the filter sits
 * inside the feedback path. Skipping it leaves an undamped accumulator, which
 * is why GC_AUDIO_FX had to force the reverb off while this was missing.
 */
static void a_pole_filter(u8 flags, s16 gain, s16 *state) {
    const s16 *coef2 = sRspa.adpcm_table[0][0]; /* fccoef[0..7]: y[n-2] */
    const s16 *coef1 = sRspa.adpcm_table[0][1]; /* fccoef[8..15]: y[n-1] */
    s16 *in = DMEM_S16(sRspa.in);
    s16 *out = DMEM_S16(sRspa.out);
    int count = ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16);
    s32 prev1;
    s32 prev2;
    int i;

    if (flags & A_INIT) {
        prev2 = 0;
        prev1 = 0;
    } else {
        prev2 = state[0];
        prev1 = state[1];
    }

    for (i = 0; i < count; i++) {
        s32 acc = coef2[0] * prev2 + coef1[0] * prev1 + gain * (s32) in[i];
        s16 y = clamp16(acc >> 14);

        out[i] = y;
        prev2 = prev1;
        prev1 = y;
    }

    state[0] = (s16) prev2;
    state[1] = (s16) prev1;
}

static void a_mix(s16 gain, u16 in_addr, u16 out_addr) {
    int nbytes = ROUND_UP_32(sRspa.nbytes);
    s16 *in = DMEM_S16(in_addr);
    s16 *out = DMEM_S16(out_addr);
    int i;

    if (gain == -0x8000) {
        /* -1.0 is the one gain the fixed-point multiply below cannot express,
         * so the microcode special-cases it to a plain subtraction. */
        while (nbytes > 0) {
            for (i = 0; i < 16; i++) {
                *out = clamp16(*out - *in++);
                out++;
            }
            nbytes -= 16 * (int) sizeof(s16);
        }
        return;
    }

    while (nbytes > 0) {
        for (i = 0; i < 16; i++) {
            s32 sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;

            *out++ = clamp16(sample);
        }
        nbytes -= 16 * (int) sizeof(s16);
    }
}

/* ---- instrumentation ----------------------------------------------------- */

/*
 * The same shape as the renderer's `ignored:` line, for the same reason.
 *
 * Verification here is not visual: "the mixer is silent" and "the mixer is
 * running" differ by one integer, so the integer gets printed rather than
 * argued about. Three numbers do it -- what the walker executed, what it
 * dropped, and the peak absolute sample that actually left DMEM for DRAM.
 *
 * The peak is sampled at aSaveBuffer because that is the only opcode that
 * writes back out of DMEM, so it measures what the audio manager will hand to
 * osAiSetNextBuffer rather than anything internal to the mix.
 */
#ifdef GC_DEBUG
u32 gGcAudioOpcodes[16];
u32 gGcAudioIgnored[256];
u32 gGcAudioCmds;
u32 gGcAudioSaves;
u32 gGcAudioPeak;
u32 gGcAudioSamples;
u32 gGcAudioClipped;

#define GC_AUDIO_IGNORE(op) (gGcAudioIgnored[op]++)

/*
 * `peak` alone turned out to be too blunt an instrument, and it cost a day.
 *
 * It is a maximum over roughly half a million samples a second, so one clipped
 * sample pins it at 32767 and it reads exactly the same as a mix that is
 * railing continuously. Before the interleave fix it really was railing; after
 * it, peak still touched full scale in a quarter of the beats -- and that
 * number cannot distinguish "a loud drum hit clipped one sample" from "a
 * quarter of the output is square".
 *
 * So the clipped samples are counted as well. The ratio is what matters:
 * a handful in half a million is a loud game, several per cent is distortion.
 */
static void note_saved(const s16 *samples, int nbytes) {
    int n = nbytes / (int) sizeof(s16);
    int i;

    gGcAudioSaves++;
    gGcAudioSamples += (u32) n;
    for (i = 0; i < n; i++) {
        s32 v = samples[i];

        if (v < 0) {
            v = -v;
        }
        if (v >= 32767) {
            gGcAudioClipped++;
        }
        if ((u32) v > gGcAudioPeak) {
            gGcAudioPeak = (u32) v;
        }
    }
}
#else
#define GC_AUDIO_IGNORE(op)          ((void) 0)
#define note_saved(samples, nbytes)  ((void) 0)
#endif

/* ---- the walker ---------------------------------------------------------- */

/*
 * Whether this file renders the audio command list.
 *
 * It does now, and the flag stays because the output stage still has to know:
 * an unfilled buffer is not silence, it is whatever the allocator left there,
 * and played back sixty times a second that is a continuous tone. os_ai.c asks
 * this before it copies anything into the DAC ring.
 */
const int gGcAudioMixerImplemented = 1;

/*
 * Entry point from the scheduler.
 *
 * The list is executed to completion before returning, because the audio
 * manager blocks on the completion message and then immediately queues the
 * buffer it expects to have been filled.
 */
void gc_audio_run_cmds(const void *cmdList, unsigned int sizeBytes) {
    const u32 *cmd = (const u32 *) cmdList;
    const u32 *end = cmd + (sizeBytes / sizeof(u32));

#ifdef GC_DEBUG
    memset(gGcAudioOpcodes, 0, sizeof(gGcAudioOpcodes));
    memset(gGcAudioIgnored, 0, sizeof(gGcAudioIgnored));
    gGcAudioCmds = 0;
    gGcAudioSaves = 0;
    gGcAudioPeak = 0;
    gGcAudioSamples = 0;
    gGcAudioClipped = 0;
#endif

    while (cmd + 1 < end) {
        u32 w0 = cmd[0];
        u32 w1 = cmd[1];
        u32 op = w0 >> 24;

        cmd += 2;

#ifdef GC_DEBUG
        gGcAudioCmds++;
        if (op < 16) {
            gGcAudioOpcodes[op]++;
        }
#endif

        switch (op) {
            case A_SPNOOP:
                break;

            case A_ADPCM:
                a_adpcm_dec((u8) (w0 >> 16), (s16 *) dram(w1));
                break;

            case A_CLEARBUFF:
                a_clear_buffer((u16) w0, (int) w1);
                break;

            case A_ENVMIXER:
                a_env_mixer((u8) (w0 >> 16), (s16 *) dram(w1));
                break;

            case A_LOADBUFF:
                a_load_buffer(dram(w1));
                break;

            case A_RESAMPLE:
                a_resample((u8) (w0 >> 16), (u16) w0, (s16 *) dram(w1));
                break;

            case A_SAVEBUFF: {
                s16 *dst = (s16 *) dram(w1);

                a_save_buffer(dst);
                note_saved(dst, ROUND_UP_8(sRspa.nbytes));
                break;
            }

            /*
             * Segment addressing. The synthesiser emits exactly one of these
             * per frame -- aSegment(cmdPtr++, 0, 0) in synthesizer.c: segment
             * zero, base zero, which is the identity and nothing to do. A
             * non-zero base would mean audio pointers need resolving through a
             * table, so it is counted rather than assumed away.
             */
            case A_SEGMENT:
                if (w1 != 0) {
                    GC_AUDIO_IGNORE(op);
                }
                break;

            case A_SETBUFF:
                a_set_buffer((u8) (w0 >> 16), (u16) w0, (u16) (w1 >> 16), (u16) w1);
                break;

            case A_SETVOL:
                a_set_volume((u8) (w0 >> 16), (s16) w0, (s16) (w1 >> 16), (s16) w1);
                break;

            case A_DMEMMOVE:
                a_dmem_move((u16) w0, (u16) (w1 >> 16), (int) (u16) w1);
                break;

            case A_LOADADPCM:
                a_load_adpcm((int) (w0 & 0xFFFFFF), dram(w1));
                break;

            case A_MIXER:
                a_mix((s16) w0, (u16) (w1 >> 16), (u16) w1);
                break;

            case A_INTERLEAVE:
                a_interleave((u16) (w1 >> 16), (u16) w1);
                break;

            /*
             * A_POLEF is only ever emitted by the reverb, so the first one is
             * the moment the effect starts running -- a path that was refused
             * outright until 2026-09-04 and has never executed on hardware.
             * Marked once, so a crash near here has something to point at.
             */
            case A_POLEF: {
                static BOOL sSaid;

                if (!sSaid) {
                    sSaid = TRUE;
                    gc_logfile_mark("aud: first A_POLEF, reverb is running\n");
                }
                a_pole_filter((u8) (w0 >> 16), (s16) w0, (s16 *) dram(w1));
                break;
            }

            case A_SETLOOP:
                a_set_loop((s16 *) dram(w1));
                break;

            default:
                GC_AUDIO_IGNORE(op);
                break;
        }
    }
}

/*
 * Entry point, replacing src/main.c.
 *
 * The N64 boot path was three threads deep: IPL3 jumped to mainproc, which
 * started an idle thread, which created the game thread and then dropped its
 * own priority to zero and spun forever. That shape existed because libultra
 * needed a thread at priority zero to fall back on when everything else was
 * blocked.
 *
 * libogc already has an idle thread of its own, so reproducing that would give
 * us two -- and the game's version is a busy loop, which on a preemptive
 * scheduler would burn every spare cycle rather than yielding them. So the
 * boot collapses to what it was actually for: bring the hardware up, then run
 * thread3_main.
 *
 * Order matters here. The video hardware has to be alive before any game code
 * runs, because the game's own video_init programs a VI it assumes is already
 * configured. The asset image has to be in ARAM before the game thread starts,
 * because the very first thing thread3_main does is DMA the asset lookup table
 * out of it.
 */

#include <ultra64.h>

#include <ogc/system.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/video.h>
#include <ogc/consol.h>
#include <ogc/exi.h>
#include <ogc/pad.h>

#include <fat.h>
#include <stdarg.h>
#include <stdio.h>

#include "gc_ultra.h"
#include "gfx/gfx_gx.h"

/* Declared rather than included: thread3_main.h reaches enums.h, which reaches
 * the generated asset_enums.h, and this file has no other reason to depend on
 * the asset build. */
void thread3_main(void *arg);

/*
 * Where the asset image is looked for, in order.
 *
 * The first is where `make -f Makefile.gc dist` puts it, next to the
 * executable on an SD card. The others cover a card reader in slot A or B,
 * which Swiss maps separately. All are tried because a user copying a folder
 * across has no reason to know which one their setup presents.
 *
 * None of them can resolve under Dolphin: its GameCube EXI device list has no
 * SD card entry at all, so fatInitDefault finds nothing to mount. That is what
 * the linked-in image is for -- see platform/gc/assets_blob.S -- and why it is
 * tried first.
 */
static const char *const kAssetPaths[] = {
    "sd:/dkr/dkr.assets",
    "carda:/dkr/dkr.assets",
    "cardb:/dkr/dkr.assets",
};

/*
 * A private, cheap console for after GX takes the screen. 320x32 at two bytes a
 * pixel is 20 KB, so libogc's scroll copies 20 KB rather than the 686 KB a
 * full PAL framebuffer costs -- and it copies into this array instead of into
 * the picture. Aligned because the console writes it as pixels.
 */
#define CONSOLE_SCRATCH_W 320
#define CONSOLE_SCRATCH_H 32
static u8 sConsoleScratch[CONSOLE_SCRATCH_W * CONSOLE_SCRATCH_H * VI_DISPLAY_PIX_SZ]
    __attribute__((aligned(32)));

/* Whether anything is listening to printf once GX owns the screen. See the
 * comment on gc_console_set in ultra/os_system.c: the framebuffer console costs
 * a 737 KB uncached memcpy per scrolled line and nobody can read it after the
 * first frame, so without a Gecko the port stops printing entirely. */
static BOOL sGeckoOn;

static OSThread sGameThread;
static u64 sGameThreadStack[GC_GAME_STACK_SIZE / sizeof(u64)];

static void open_assets(void) {
    u32 i;

    if (gc_assets_open_embedded()) {
        gc_assets_verify();
        return;
    }

    for (i = 0; i < sizeof(kAssetPaths) / sizeof(kAssetPaths[0]); i++) {
        if (gc_assets_open(kAssetPaths[i])) {
            gc_assets_verify();
            return;
        }
    }

    gc_fatal("could not find dkr.assets.\n"
             "Rebuild with GC_EMBED_ASSETS=1 to link it into the executable,\n"
             "or copy build/gc/dkr.assets to sd:/dkr/ .");
}

/*
 * The heartbeat.
 *
 * A renderer that draws nothing and a game that has hung look identical on
 * screen, so once a second the boot thread says what the machine is actually
 * doing: what the scheduler dispatched, whether the retrace message survived
 * every hop, whether each call that hands control to GX came back, what the
 * last display list contained, and what it drew. Compiled out entirely unless
 * GC_DEBUG -- the whole block, not just the printing, because the counters it
 * reads do not exist otherwise.
 */
#ifdef GC_DEBUG
static void gc_heartbeat(u32 ticks) {
    int gameW, gameH;
    u32 op;
    static u64 sLastBeat;
    u64 now = gettime();
    u32 elapsedMs = (u32) (sLastBeat != 0 ? ticks_to_millisecs(diff_ticks(sLastBeat, now)) : 0);

    sLastBeat = now;

    gc_video_game_resolution(&gameW, &gameH);

    /*
     * Real elapsed milliseconds between heartbeats, from the Gekko time base.
     *
     * The heartbeat fires every 60 VSyncs, and for a long time every rate in
     * this log was implicitly divided by "one second" on that assumption. It is
     * not one second: under Dolphin the VI retrace and VSync counts diverge by
     * about ten per cent, and two separate audio measurements were misread
     * because of it. Every per-second figure derived from this log has to be
     * scaled by 1000/elapsedMs before it means anything.
     */
    gc_log("           clock: %u ms since last beat (60 VSyncs)\n", (unsigned) elapsedMs);

    gc_log("dkr-gc: %u retr | task gfx %u aud %u | vi %u/%u sched %u/%u/%u"
           " | dl %u/%u swap %u/%u copy %u/%u\n",
           (unsigned) ticks, (unsigned) gGcGfxTasks, (unsigned) gGcAudioTasks,
           (unsigned) gGcViMesgs, (unsigned) gGcViDrops, (unsigned) gGcSchedMsgs,
           (unsigned) gGcSchedRetraces, (unsigned) gGcClientSends, (unsigned) gGcDlIn,
           (unsigned) gGcDlOut, (unsigned) gGcSwapIn, (unsigned) gGcSwapOut,
           (unsigned) gGcCopyIn, (unsigned) gGcCopyOut);
    gc_log("           fb_update %u/%u, %u retrace mesgs received\n", (unsigned) gGcFbIn,
           (unsigned) gGcFbOut, (unsigned) gGcFbMesgs);
    gc_log("           last list: %u commands, %u dma sublists, depth %u\n",
           (unsigned) gGcDlCommands, (unsigned) gGcDlDmaLists, (unsigned) gGcDlMaxDepth);

    gc_log("           opcodes:");
    for (op = 0; op < 256; op++) {
        if (gGcDlOpcodes[op] != 0) {
            gc_log(" %02x:%u", (unsigned) op, (unsigned) gGcDlOpcodes[op]);
        }
    }
    gc_log("\n");

    /* What the walker dropped. An empty line here means every command the
     * frame sent was acted on; anything listed is a feature the picture is
     * missing, with the frequency that says how much it costs. */
    gc_log("           ignored:");
    for (op = 0; op < 256; op++) {
        if (gGcDlIgnored[op] != 0) {
            gc_log(" %02x:%u", (unsigned) op, (unsigned) gGcDlIgnored[op]);
        }
    }
    gc_log("\n");

    /* The dynamic-ambient lighting branch (calc_dynamic_lighting_for_object_2,
     * src/hasm/obj_shade_fast.c). `obj` counts the objects that reached it in
     * the last second and `nosh` those that turned back for want of a
     * ShadeProperties; `grey` is the span of the value it wrote. A grey pinned
     * at 0 or at 255 is a different bug from a branch that never runs, and a
     * branch that never runs is a different bug again from one that runs and
     * writes a sensible range -- which is why all three are on one line. */
    gc_log("           dynlit2: obj %u (nosh %u), verts %u, grey %u..%u\n",
           (unsigned) gGcDynLit2Objects, (unsigned) gGcDynLit2NoShading,
           (unsigned) gGcDynLit2Verts,
           (unsigned) (gGcDynLit2Verts != 0 ? gGcDynLit2Min : 0),
           (unsigned) gGcDynLit2Max);
    gGcDynLit2Objects = 0;
    gGcDynLit2NoShading = 0;
    gGcDynLit2Verts = 0;
    gGcDynLit2Min = 0xFFFFFFFF;
    gGcDynLit2Max = 0;

    /* The audio task, in the same shape. `aud` is what the walker executed, by
     * ABI opcode; `aud-ign` is what it dropped; `peak` is the loudest sample
     * that reached DRAM this task, out of 32767. A peak of zero with a
     * non-empty `aud` line means the list ran and produced silence, which is a
     * different bug from the list not arriving at all. */
    gc_log("           aud %u cmds, %u saves, peak %u, clipped %u/%u |",
           (unsigned) gGcAudioCmds, (unsigned) gGcAudioSaves, (unsigned) gGcAudioPeak,
           (unsigned) gGcAudioClipped, (unsigned) gGcAudioSamples);
    for (op = 0; op < 16; op++) {
        if (gGcAudioOpcodes[op] != 0) {
            gc_log(" %u:%u", (unsigned) op, (unsigned) gGcAudioOpcodes[op]);
        }
    }
    gc_log("\n           ai cb %u, under %u, pushed %u, ring %u, rejected %u, refused %u",
           (unsigned) gGcAiCallbacks, (unsigned) gGcAiUnderruns,
           (unsigned) gGcAiPushed, (unsigned) gGcAiRingUsed, (unsigned) gGcAiRejected,
           (unsigned) gGcAiRefusedFull);
    /* The shape of the silence, not just its total: how many separate dropouts,
     * the longest run of silent frames in one DMA block, how empty the ring got
     * when the consumer looked, and the longest the producer left it unfed.
     * `ringMin` and `ring` disagreeing is the whole point -- `ring` is sampled
     * by the producer right after it fills. Reset each beat except ringMin's
     * floor, so a beat's numbers describe that beat. */
    gc_log("\n           ai drops %u events, longest %u frames, ringMin %u, gap %u cb",
           (unsigned) gGcAiUnderEvents, (unsigned) gGcAiUnderMax,
           (unsigned) (gGcAiRingMin == 0xFFFFFFFFu ? 0 : gGcAiRingMin),
           (unsigned) gGcAiPushGapMax);
    /* The rate loop, so that "the ring drains" is visible as it is corrected:
     * the step it settles on against its nominal value, and the depth it holds.
     * A step that sits at its clamp means the drift is larger than the loop can
     * absorb, which would be a different defect. */
    gc_log(" | rate step %u depth %u", (unsigned) gGcAiStep,
           (unsigned) gGcAiDepthAvg);
    gGcAiUnderEvents = 0;
    gGcAiUnderMax = 0;
    gGcAiRingMin = 0xFFFFFFFFu;
    gGcAiPushGapMax = 0;
    gc_log("\n           n64 io: %u reads, %u writes, %u unknown (last %08x) | asserts %u",
           (unsigned) gGcIoReads, (unsigned) gGcIoWrites, (unsigned) gGcIoUnknown,
           (unsigned) gGcIoLastUnknown, (unsigned) gGcAsserts);
    gc_log("\n           aram reads %u, slow %u, contended %u",
           (unsigned) gGcAssetReads, (unsigned) gGcAssetSlow,
           (unsigned) gGcAssetContended);
    gc_log("\n           ai offers %u, offeredFrames %u, gameRate %u",
           (unsigned) gGcAiOffers, (unsigned) gGcAiOfferedFrames,
           (unsigned) gGcAiGameRate);
    gc_log("\n           am frameSamples %d (left %u, size %u, min %u)",
           (int) gGcAmFrameSamples, (unsigned) gGcAmSamplesLeft,
           (unsigned) gGcAmFrameSize, (unsigned) gGcAmMinFrameSize);
    gc_log("\n           syn frames %u (nohead %u), players %u, lastcmds %u",
           (unsigned) gGcSynFrames, (unsigned) gGcSynNoHead,
           (unsigned) gGcSynPlayers, (unsigned) gGcSynCmds);
    gc_log("\n           syn curSamples %u", (unsigned) gGcSynCurSamples);
    gc_log("\n           aud-ign:");
    for (op = 0; op < 256; op++) {
        if (gGcAudioIgnored[op] != 0) {
            gc_log(" %u:%u", (unsigned) op, (unsigned) gGcAudioIgnored[op]);
        }
    }
    gc_log("\n");

    gc_log("           geo set %08x clear %08x | xfb %08x\n", (unsigned) gGcGeoSet,
           (unsigned) gGcGeoClear, (unsigned) gc_video_xfb());
    for (op = 0; op < gGcCimgCount; op++) {
        gc_log("           cimg%u addr %08x w0 %08x (fmt%u siz%u width%u)\n", (unsigned) op,
               (unsigned) gGcCimg[op][0], (unsigned) gGcCimg[op][1],
               (unsigned) ((gGcCimg[op][1] >> 21) & 7), (unsigned) ((gGcCimg[op][1] >> 19) & 3),
               (unsigned) ((gGcCimg[op][1] & 0xFFF) + 1));
    }
    for (op = 0; op < gGcZimgCount; op++) {
        gc_log("           zimg%u addr %08x\n", (unsigned) op, (unsigned) gGcZimg[op]);
    }

    gc_log("           fills %u (%u blended, %u vers le Z), fog %u lots/%u sommets, last %06x at (%u,%u)-(%u,%u), game %dx%d\n",
           (unsigned) gGcFills, (unsigned) gGcFillsBlend, (unsigned) gGcFillsOffscreen,
           (unsigned) gGcFogBatches, (unsigned) gGcFogVerts, (unsigned) gGcFillColor, (unsigned) gGcFillRect[0],
           (unsigned) gGcFillRect[1], (unsigned) gGcFillRect[2], (unsigned) gGcFillRect[3],
           gameW, gameH);
    gc_log("           texrects %u, timg fmts %08x, tile fmts %08x\n", (unsigned) gGcTexRects,
           (unsigned) gGcTexFormats, (unsigned) gGcTileFormats);
    gc_log("           tile modes (cmt<<2|cms) %08x\n", (unsigned) gGcTileModes);
    /* Where the rectangles go. `drawn` equal to `texrects` with nothing on the
     * screen means the loss is in the blend or the combiner; `offscreen` equal
     * to it means the coordinates. `first` is there because a plausible number
     * is not evidence. */
    gc_log("           texrect funnel: %u submitted = %u drawn (%u offscreen) + %u zero-area"
           " + %u no-image + %u no-tex | first (%d,%d)-(%d,%d)\n",
           (unsigned) gGcTexRects, (unsigned) gGcTrDrawn, (unsigned) gGcTrOffScreen,
           (unsigned) gGcTrZeroArea, (unsigned) gGcTrNoImage, (unsigned) gGcTrNoTex,
           (int) gGcTrFirstBox[0], (int) gGcTrFirstBox[1], (int) gGcTrFirstBox[2],
           (int) gGcTrFirstBox[3]);
    /* Sixteen consecutive frames of emitted triangles. An alternation here says
     * the strobe is upstream of the renderer; a flat row says it is inside it. */
    /*
     * The game's heap and its decompressions, both of which were silent.
     *
     * mempool reports "No more slots available" through stubbed_printf, which
     * is defined as nothing; object_model_init and texture_load use plain
     * mempool_alloc and hand back NULL without a word. So an exhausted pool
     * looks exactly like "the menu text is missing" and says nothing at all.
     * `slots` is a cliff rather than a slope -- at 1600 the allocator refuses
     * everything -- and `largest` separates out of space from fragmented.
     */
    {
        u32 slotsUsed, slotsMax, freeBytes, largestFree, usedBytes;

        gc_pool_report(&slotsUsed, &slotsMax, &freeBytes, &largestFree, &usedBytes);
        gc_log("           pool: %u/%u slots, used %u KB, free %u KB, largest free %u KB"
               " | gzip %u ok, %u failed\n",
               (unsigned) slotsUsed, (unsigned) slotsMax, (unsigned) (usedBytes >> 10),
               (unsigned) (freeBytes >> 10), (unsigned) (largestFree >> 10),
               (unsigned) gGcGzipOk, (unsigned) gGcGzipFail);
    }
    /* The rectangles themselves. A glyph-sized box whose u span is far past
     * 1000 is the whole font smeared across it, which is what the photographs
     * of the menu show. */
    for (op = 0; op < (int) gGcTrDbgCount; op++) {
        const s32 *e = gGcTrDbg[op];

        gc_log("           tr%d (%d,%d)-(%d,%d) tex %08x %dx%d fmtsiz %02x"
               " u %d..%d v %d..%d | omH %08x omL %08x zcmp%d zupd%d ac%d\n",
               op, (int) e[0], (int) e[1], (int) e[2], (int) e[3], (unsigned) e[4],
               (int) e[5], (int) e[6], (unsigned) e[7], (int) e[8], (int) e[9],
               (int) e[10], (int) e[11], (unsigned) e[12], (unsigned) e[13],
               (int) ((e[13] >> 4) & 1),  /* Z_CMP  */
               (int) ((e[13] >> 5) & 1),  /* Z_UPD  */
               (int) (e[13] & 3));        /* alpha compare mode */
    }
    gc_log("           tris out, last 16 frames:");
    for (op = 0; op < 16; op++) {
        gc_log(" %u", (unsigned) gGcTrisOutHist[(gGcTrisHistPos + op) % 16]);
    }
    gc_log("\n");
    for (op = 0; op < gGcStateDbgCount; op++) {
        gc_log("           st%u omH %08x omL %08x cc %08x %08x | zcmp%u zupd%u forcebl%u decal%u"
               " cvgxa%u cyc%u\n",
               (unsigned) op, (unsigned) gGcStateDbg[op][0], (unsigned) gGcStateDbg[op][1],
               (unsigned) gGcStateDbg[op][2], (unsigned) gGcStateDbg[op][3],
               (unsigned) (gGcStateDbg[op][4] & 1), (unsigned) ((gGcStateDbg[op][4] >> 1) & 1),
               (unsigned) ((gGcStateDbg[op][4] >> 2) & 1), (unsigned) ((gGcStateDbg[op][4] >> 3) & 1),
               (unsigned) ((gGcStateDbg[op][4] >> 4) & 1),
               (unsigned) ((gGcStateDbg[op][0] >> 20) & 3));
    }
    if (gGcTexRawValid) {
        u32 r, c;

        gc_log("           RAW %ux%u stride%u fmt%u addr %08x\n", (unsigned) gGcTexRawW,
               (unsigned) gGcTexRawH, (unsigned) gGcTexRawStride, (unsigned) gGcTexRawFmt,
               (unsigned) gGcTexRawAddr);
        for (r = 0; r < gGcTexRawH; r++) {
            gc_log("           RAW%02u", (unsigned) r);
            for (c = 0; c < gGcTexRawStride; c++) {
                gc_log("%02x", gGcTexRaw[r * gGcTexRawStride + c]);
            }
            gc_log("\n");
        }
    }
    for (op = 0; op < gGcTexDbgCount; op++) {
        const GcTexDbg *e = &gGcTexDbg[op];

        gc_log("           tex%u ok%u fmt%u siz%u line%u tmem%u pal%u"
               " uls%u ult%u lrs%u lrt%u -> %ux%u stride%u swap%u\n",
               (unsigned) op, (unsigned) e->ok, (unsigned) e->fmt, (unsigned) e->siz,
               (unsigned) e->line, (unsigned) e->tmem, (unsigned) e->palette, (unsigned) e->uls,
               (unsigned) e->ult, (unsigned) e->lrs, (unsigned) e->lrt, (unsigned) e->width,
               (unsigned) e->height, (unsigned) e->stride, (unsigned) e->swapOdd);
        gc_log("                cms%u cmt%u masks%u maskt%u shifts%u shiftt%u"
               " addr %08x head %08x %08x %08x %08x\n",
               (unsigned) e->cms, (unsigned) e->cmt, (unsigned) e->masks, (unsigned) e->maskt,
               (unsigned) e->shifts, (unsigned) e->shiftt, (unsigned) e->addr,
               (unsigned) e->head[0], (unsigned) e->head[1], (unsigned) e->head[2],
               (unsigned) e->head[3]);
    }
    gc_log("           trin %u cmds, tris in %u -> badidx %u clip %u (behind %u, near %u) cull %u out %u"
           " | vtx %u loaded, %u max\n",
           (unsigned) gGcTrinCmds, (unsigned) gGcTrisIn, (unsigned) gGcTrisBadIdx,
           (unsigned) gGcTrisClipped, (unsigned) gGcTrisBehind, (unsigned) gGcTrisNearOnly,
           (unsigned) gGcTrisCulled, (unsigned) gGcTrisOut,
           (unsigned) gGcVtxLoaded, (unsigned) gGcVtxMaxCount);
    gc_log("           tex asks %u = hits %u + converts %u | NULL dim %u addr %u alloc %u"
           " | held %u KB\n",
           (unsigned) gGcTexAsks, (unsigned) gGcTexHits, (unsigned) gGcTexConverts,
           (unsigned) gGcTexNullDim, (unsigned) gGcTexNullAddr, (unsigned) gGcTexNullAlloc,
           (unsigned) (gGcTexBytes / 1024));
    gc_log("           cover kind%u area %u/1000 (%d,%d)-(%d,%d) seq %u/%u"
           " | cc %08x %08x omH %08x omL %08x | tex %08x fmtsiz %02x"
           " | col %08x prim %08x env %08x\n",
           (unsigned) gGcCoverKind, (unsigned) gGcCoverArea, (int) gGcCoverBox[0],
           (int) gGcCoverBox[1], (int) gGcCoverBox[2], (int) gGcCoverBox[3],
           (unsigned) gGcCoverSeq, (unsigned) gGcCoverTotal, (unsigned) gGcCoverCc[0],
           (unsigned) gGcCoverCc[1], (unsigned) gGcCoverOmh, (unsigned) gGcCoverOml,
           (unsigned) gGcCoverTex, (unsigned) gGcCoverFmtSiz, (unsigned) gGcCoverCol,
           (unsigned) gGcCoverPrim, (unsigned) gGcCoverEnv);
    gc_log("           tex magenta: unhandled %u (last fmtsiz %02x), no tlut %u\n",
           (unsigned) gGcTexUnhandled, (unsigned) gGcTexUnhandledFmtSiz,
           (unsigned) gGcTexNoTlut);
    gc_log("           cpu box (%d,%d)-(%d,%d) slot%u bb%u over %u tris | anchor %d %d %d %d\n",
           (int) gGcCpuBox[0], (int) gGcCpuBox[1], (int) gGcCpuBox[2], (int) gGcCpuBox[3],
           (unsigned) gGcCpuBoxMtx, (unsigned) gGcCpuBoxBb, (unsigned) gGcCpuBoxTris,
           (int) gGcCpuBoxAnchor[0], (int) gGcCpuBoxAnchor[1], (int) gGcCpuBoxAnchor[2],
           (int) gGcCpuBoxAnchor[3]);
    gc_log("           trin proj: hw %u, cpu fallback %u (no-w %u, not-affine %u)\n",
           (unsigned) gGcTrinHw, (unsigned) gGcTrinCpu,
           (unsigned) gGcProjNoW, (unsigned) gGcProjNotAffine);
    gc_log("           bb seen %u n=%u app=%u base=%u cnt=%u\n",
           (unsigned) gGcBbSeen, (unsigned) gGcBbBatch[0], (unsigned) gGcBbBatch[1],
           (unsigned) gGcBbBatch[2], (unsigned) gGcBbBatch[3]);
    gc_log("           bb anchor %d %d %d %d | pre %d %d %d %d | post %d %d %d %d\n",
           (int) gGcBbAnchor[0], (int) gGcBbAnchor[1], (int) gGcBbAnchor[2], (int) gGcBbAnchor[3],
           (int) gGcBbPre[0], (int) gGcBbPre[1], (int) gGcBbPre[2], (int) gGcBbPre[3],
           (int) gGcBbPost[0], (int) gGcBbPost[1], (int) gGcBbPost[2], (int) gGcBbPost[3]);
    {
        s32 mrow;
        for (mrow = 0; mrow < 4; mrow++) {
            gc_log("           mtx x1000 slot1 %d %d %d %d | bbslot %d %d %d %d\n",
                   (int) gGcBbMtxScene[mrow * 4 + 0], (int) gGcBbMtxScene[mrow * 4 + 1],
                   (int) gGcBbMtxScene[mrow * 4 + 2], (int) gGcBbMtxScene[mrow * 4 + 3],
                   (int) gGcBbMtxBb[mrow * 4 + 0], (int) gGcBbMtxBb[mrow * 4 + 1],
                   (int) gGcBbMtxBb[mrow * 4 + 2], (int) gGcBbMtxBb[mrow * 4 + 3]);
        }
    }
    gc_log("           mtx slot0 %u vtx/%u behind (%u ld,%u sel)  slot1 %u/%u (%u,%u)"
           "  slot2 %u/%u (%u,%u)\n",
           (unsigned) gGcVtxByMtx[0], (unsigned) gGcVtxBehindByMtx[0],
           (unsigned) gGcMtxLoads[0], (unsigned) gGcMtxSelects[0],
           (unsigned) gGcVtxByMtx[1], (unsigned) gGcVtxBehindByMtx[1],
           (unsigned) gGcMtxLoads[1], (unsigned) gGcMtxSelects[1],
           (unsigned) gGcVtxByMtx[2], (unsigned) gGcVtxBehindByMtx[2],
           (unsigned) gGcMtxLoads[2], (unsigned) gGcMtxSelects[2]);
    gc_log("           settile %08x %08x  tilesize %08x %08x\n", (unsigned) gGcLastTile[0],
           (unsigned) gGcLastTile[1], (unsigned) gGcLastTileSize[0],
           (unsigned) gGcLastTileSize[1]);
    gc_log("           texrect %08x %08x / %08x %08x / %08x %08x\n",
           (unsigned) gGcLastTexRect[0], (unsigned) gGcLastTexRect[1],
           (unsigned) gGcLastTexRect[2], (unsigned) gGcLastTexRect[3],
           (unsigned) gGcLastTexRect[4], (unsigned) gGcLastTexRect[5]);
    gc_log("           game thread: blocked at %08x on queue %08x (%u waits)\n",
           (unsigned) gGcBlockPc, (unsigned) gGcBlockQ, (unsigned) gGcBlockSeq);
    if (gc_stack_overflowed() >= 0) {
        gc_log("           STACK OVERFLOW on thread %d\n", (int) gc_stack_overflowed());
    }

    /*
     * Push the beat to the card. This is the one place in the port where a
     * few milliseconds of FAT write is affordable: the boot thread has just
     * finished printing and has 59 retraces of nothing to do before the next
     * beat, and the game runs on its own thread. Flushing per beat also means
     * that whatever the user pulls the power on, the log stops at most one
     * second short of it.
     */
    gc_logfile_flush();
}
#else
/*
 * Without GC_DEBUG there are no counters to print, but the log is still open
 * and still holds the boot trace and anything gc_fatal wrote. Flushing on the
 * same cadence keeps it current without pulling in the heartbeat.
 */
#define gc_heartbeat(ticks) (gc_logfile_flush(), (void) (ticks))
#endif


/*
 * One line of the boot trace, to both channels.
 *
 * printf reaches the framebuffer console and the USB Gecko; the log reaches
 * the card. Neither is available in every situation the port has to be
 * diagnosed in, which is why both are written.
 */
static void boot_step(const char *fmt, ...) {
    char line[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("dkr-gc: %s\n", line);
    gc_logfile_printf("boot: %s\n", line);
}

int main(void) {
    GXRModeObj *rmode;

    /* A console on the framebuffer, so that a failure before the game has
     * drawn anything is visible rather than a black screen. gfx takes the
     * display over on its first frame. */
    gc_video_init();
    rmode = gc_video_mode();
    /*
     * The console window has to fit inside the framebuffer, and the canonical
     * libogc arguments do not: passing `rmode->fbWidth, rmode->xfbHeight` with
     * a start of (20, 20) describes a region that runs twenty rows off the
     * bottom. __console_write's scroll then reads `stride * con_yres` bytes
     * from `destbuffer + stride*16`, which is twenty rows past the end of the
     * buffer -- 25 KB of out-of-bounds read per scrolled line. Subtracting the
     * margins costs nothing and makes it impossible.
     */
    CON_Init(gc_video_xfb(), 20, 20, rmode->fbWidth - 40, rmode->xfbHeight - 40,
             rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    /*
     * The card comes up first, and the Gecko console only afterwards -- because
     * whether the Gecko console may be enabled at all depends on what the card
     * turned out to be.
     *
     * It used to be mounted lazily, inside gc_assets_open, which meant an
     * embedded-assets build -- the one that runs under Dolphin, and the one the
     * dist target ships -- never mounted a filesystem at all. The save file had
     * nowhere to go, and so would the log.
     */
    gc_fat_mount();
    gc_logfile_init();
    gc_crash_init();

    /*
     * Mirror the console onto a USB Gecko -- unless libfat is already using
     * that slot.
     *
     * This was `CON_EnableGecko(EXI_CHANNEL_1, FALSE)`, unconditionally, from
     * the first day of the port, and on the user's console it is actively
     * destructive. EXI channel 1 is memory card slot B, and their SD reader is
     * an SD Gecko in slot B -- the log itself resolves to `cardb:`. A USB Gecko
     * and an SD Gecko are both EXI device 0 on their channel, so every printf
     * selects the SD adapter and clocks USB-Gecko protocol bytes into it. With
     * `safe` false libogc does not even check that a Gecko is there first; it
     * just writes.
     *
     * Under Dolphin that is harmless and it is how every diagnosis so far was
     * made, because Dolphin's slot B really is a Gecko. On hardware it puts
     * stray traffic on the bus the game boots from, and it went unnoticed for
     * as long as the port only read the card at boot. The moment the log and
     * the save started using the card *while the game runs*, it stopped being
     * theoretical -- the first two hardware runs both ended with a card that
     * could no longer be written, which is why the crash report never arrived.
     *
     * So: never on a slot libfat holds, and `safe` true, which makes libogc
     * verify a Gecko is actually present before writing to it.
     */
    sGeckoOn = !(gc_fat_slots() & (1u << EXI_CHANNEL_1));
    if (sGeckoOn) {
        CON_EnableGecko(EXI_CHANNEL_1, TRUE);
    }

    /* Which EXI slots libfat took. CARD_* must stay off those, the Gecko
     * console above must stay off them, and on the user's console the log
     * itself lives on one of them. Through boot_step so it is visible on
     * whichever channel this machine actually has. */
    boot_step("fat slots 0x%x (bit0 carda/slotA, bit1 cardb/slotB), gecko %s",
              (unsigned) gc_fat_slots(),
              (gc_fat_slots() & (1u << EXI_CHANNEL_1)) ? "off (slot B is libfat)" : "on");

    /* A boot trace. Until the renderer draws a frame, this console is the only
     * thing that can tell a hang in one subsystem apart from a hang in another,
     * and each of these steps talks to hardware that can stall. Each step goes
     * to the SD log too, unconditionally -- not under GC_DEBUG -- because a
     * boot that dies before the first frame is exactly the failure a user on
     * hardware cannot otherwise report. */
    boot_step("video ok (%dx%d)", rmode->fbWidth, rmode->xfbHeight);

    /*
     * The whole geometry of the picture, in one line.
     *
     * "Every texture in the game is far too smooth" has two candidate causes
     * and they are not distinguishable on a television: the EFB copy's
     * deflicker filter, which blurs vertically across three scanlines by
     * design and which libogc turns on for every interlaced mode; and the fact
     * that the game's 320x264 coordinate space is being magnified onto a
     * 640x528 EFB, so textures authored for one texel per pixel are sampled
     * bilinearly at 2x. Both are plausible, and the port had never printed
     * either. `vfilter` non-flat and `aa 1` name the first; a scale of 2.00
     * names the second.
     */
    boot_step("video: efb %dx%d xfb %dx%d vi %d, aa %d, vfilter %d %d %d %d %d %d %d",
              rmode->fbWidth, rmode->efbHeight, rmode->fbWidth, rmode->xfbHeight,
              rmode->viHeight, (int) rmode->aa,
              (int) rmode->vfilter[0], (int) rmode->vfilter[1], (int) rmode->vfilter[2],
              (int) rmode->vfilter[3], (int) rmode->vfilter[4], (int) rmode->vfilter[5],
              (int) rmode->vfilter[6]);

    osInitialize();
    boot_step("ultra ok");

    gc_gfx_init();
    boot_step("gx ok");

    /*
     * GX owns the screen from here, so the framebuffer console is write-only
     * noise -- and expensive noise. Without a Gecko to read it, stop printing;
     * the SD log is a separate path and keeps everything.
     */
    if (!sGeckoOn) {
        gc_console_set(FALSE);
        gc_logfile_printf("console: off (no USB Gecko; GX owns the screen)\n");
    }

    /*
     * And move libogc's console off the framebuffer whatever happens.
     *
     * gc_console_set only silences the port's own printf. It cannot silence
     * newlib -- an assert, a stray fprintf from the game, anything that reaches
     * stdout or stderr still lands in __console_write, and that scrolls by
     * copying `stride * con_yres - 16` bytes: 686 KB per line on this PAL
     * framebuffer, into the buffer GX is copying into. The sixth hardware run
     * crashed on the audio thread inside exactly that memcpy.
     *
     * Pointing the console at a small private buffer makes every one of those
     * writers cost twenty kilobytes instead of seven hundred, and takes the
     * console out of the framebuffer entirely. A USB Gecko, when there is one,
     * still receives the full text: libogc emits it from __console_write
     * regardless of what the buffer looks like.
     */
    CON_Init(sConsoleScratch, 0, 0, CONSOLE_SCRATCH_W, CONSOLE_SCRATCH_H,
             CONSOLE_SCRATCH_W * VI_DISPLAY_PIX_SZ);

    PAD_Init();
    boot_step("pad ok");

    open_assets();
    boot_step("assets ok");

    /* thread3_main is the game: it sets up the scheduler, the subsystems and
     * then never returns. It is given a thread of its own rather than being
     * called directly because the scheduler it creates has to be able to
     * preempt it. */
    osCreateThread(&sGameThread, 3, thread3_main, NULL,
                   &sGameThreadStack[GC_GAME_STACK_SIZE / sizeof(u64)], 10);
    osStartThread(&sGameThread);
    boot_step("game running");
    gc_logfile_flush();

    /*
     * Nothing left for the boot thread to do but prove the machine is still
     * alive. Sleeping on the retrace rather than spinning leaves the CPU to
     * the game.
     *
     * The first beats come early, and that is not tidiness. The first hardware
     * run crashed before tick 60 and the log therefore held nothing but the
     * boot trace -- six lines, none of them about the game. Beats at 5, 15 and
     * 30 retraces cost three extra card writes per boot and mean that a crash
     * inside the first second still leaves the opcode census, the task counts
     * and the audio state behind it. After tick 60 the cadence is the usual
     * one.
     *
     * And the count is retraces, not seconds. The first hardware log opened
     * with `video ok (640x576)` -- a PAL console, 50 retraces a second -- so
     * "60 retraces" is 1.2 s there against 1.0 s on NTSC. That is why the beat
     * prints its own elapsed milliseconds: every rate in this log has to be
     * scaled by 1000/elapsedMs, and the denominator is not a constant.
     */
    for (;;) {
        static u32 ticks;

        VIDEO_WaitVSync();
#if GC_CRASHTEST
        /*
         * Fault on purpose, once, a couple of seconds in.
         *
         * The crash handler is the one piece of this port that only runs when
         * everything else has already failed, and it was wrong for four
         * hardware runs without anyone being able to tell: it took a second
         * exception inside vsnprintf and destroyed the evidence it existed to
         * collect. A path like that has to be exercised deliberately, on the
         * emulator, where a run costs nothing.
         *
         * A trap instruction, not a bad pointer, and that took two attempts to
         * get right. Address 4 is ordinary OS-globals memory on this machine --
         * MEM1 starts at 0x80000000 and the BATs cover it, so a NULL-ish write
         * is perfectly legal and produced no fault at all. An address outside
         * every BAT (0xDEADBEE0) did not fault under Dolphin either, which does
         * not emulate the MMU for homebrew. `__builtin_trap` emits `twi 31,0,0`
         * and raises a Program exception on the emulator and on hardware alike,
         * which is what a test of the *handler* needs.
         */
        if (ticks == 120) {
            gc_logfile_mark("crashtest: about to fault deliberately\n");
            __builtin_trap();
        }
#endif
        /* If the game thread has faulted and the exception handler could not
         * write its report from inside the exception, this thread does it. */
        gc_crash_poll();
        /* The only thread that writes the log to the card. Free when there is
         * nothing buffered, which is every retrace but about one a second. */
        gc_logfile_flush();
        ticks++;
        if (ticks == 5 || ticks == 15 || ticks == 30 || ticks % 60 == 0) {
            gc_heartbeat(ticks);
        }
    }
}

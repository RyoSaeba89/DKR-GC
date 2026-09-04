#ifndef GC_ULTRA_H
#define GC_ULTRA_H

/*
 * Shared surface of the GameCube platform layer.
 *
 * Everything the replaced libultra pieces need from each other lives here.
 * Game code does not include this file: it keeps talking to the os* API
 * declared in include/PR, and the implementations behind that API live in
 * platform/gc/ultra.
 */

#include <ultra64.h>

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- diagnostics --------------------------------------------------------- */

/* Prints to whatever console the build has (USB Gecko, then the framebuffer
 * console) and halts. Used for conditions the port cannot continue past, such
 * as a libogc primitive failing to allocate. */
void gc_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* ---- the log file on the SD card ----------------------------------------- *
 *
 * Everything gc_log and gc_fatal print is also written to sd:/dkr/dkr.log, so
 * that a session on real hardware -- where there is no USB Gecko and no second
 * machine -- leaves the same evidence a Dolphin session does. See
 * gc_logfile.c; all of it degrades to nothing when no card is mounted.
 */

/* Mount the SD card if it has not been mounted, and say whether there is one.
 * Cached: the underlying fatInitDefault runs at most once per boot. Defined in
 * gc_assets.c, which is where the card was first needed. */
BOOL gc_fat_mount(void);

/* Which EXI slots libfat took, as a bitmask (bit 0 = slot A / carda:, bit 1 =
 * slot B / cardb:). A slot libfat holds must never be handed to CARD_*. */
u32 gc_fat_slots(void);

/* One lock around every EXI card access -- libfat and CARD_* alike. Three
 * threads now reach a card (log flush, save, breadcrumbs) where one used to,
 * and they share the EXI channels. Never taken by the crash handler. */
void gc_fs_lock(void);
void gc_fs_unlock(void);

/* ---- persistent storage --------------------------------------------------- *
 *
 * One named, fixed-size blob, on a GameCube memory card if there is one and on
 * the SD card otherwise. Both save paths -- the cartridge EEPROM in
 * ultra/os_eeprom.c and the Controller Pak in ultra/os_pfs.c -- go through it,
 * because on this machine they are the same thing. Defined in gc_storage.c.
 *
 * A card write costs tens of milliseconds and stops the EXI bus, so these are
 * for the game's own save points, never for anything per-frame.
 */
BOOL gc_storage_read(const char *name, void *buf, u32 size);
BOOL gc_storage_write(const char *name, const void *buf, u32 size);

/* Whether there is anywhere at all to save. osPfsIsPlug answers the game's
 * "is a Controller Pak plugged in" with this. */
BOOL gc_storage_present(void);

/* ---- the crash handler --------------------------------------------------- */

/* Read back a crash record left by the previous run, if there is one, and
 * delete it from the card. Call at boot, after gc_logfile_init. What it finds
 * is what get_lockup_status reports and what render_epc_lock_up_display draws.
 * Defined in gc_crash.c. */
void gc_crash_init(void);

/* The crash report's second chance. Call from the retrace loop: if the
 * exception handler could not get its report onto the card from inside the
 * exception, this writes it from an ordinary thread context. Does nothing
 * when there is no pending report, which is every call but at most one. */
void gc_crash_poll(void);

/* Truncate the log and note which path took it. Call once, after the FAT
 * volume is mounted and before anything worth logging happens. */
void gc_logfile_init(void);

/* Append to the log buffer. Neither writes to the card by itself unless the
 * buffer is nearly full. */
void gc_logfile_write(const char *text, u32 len);
void gc_logfile_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gc_logfile_vprintf(const char *fmt, va_list ap);

/* One line, written AND flushed. For once-per-boot breadcrumbs that have to
 * survive a crash a millisecond later -- the first entry into a newly written
 * subsystem, above all. Costs an open/append/close, so not for anything that
 * repeats. */
void gc_logfile_mark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Push the buffer to the card, open/append/close, so that what has been logged
 * survives the power switch. Call at a point where a few milliseconds of FAT
 * write does not matter -- the end of a heartbeat, or a fatal. */
void gc_logfile_flush(void);

/* The same, without taking the lock. Only for an exception handler, which
 * cannot afford to block on a mutex the faulting thread may still hold. */
void gc_logfile_flush_unlocked(void);

/* Stop taking the log's lock at all, for the rest of the run. The crash
 * handler calls this before writing its report: it is the only writer left by
 * then, and blocking on a mutex held by the thread that just faulted would
 * turn a crash report into a freeze. Never cleared. */
void gc_logfile_set_crash_mode(void);

/* Whether a card was found, and where the log is going. */
BOOL gc_logfile_active(void);
const char *gc_logfile_path(void);

/* Whether the port should still write to libogc's framebuffer console.
 * Turned off once GX owns the screen and no USB Gecko is listening: the
 * console scrolls by a 737 KB uncached memcpy per line on a PAL framebuffer,
 * into the buffer GX is copying into, and nobody can read it. Measured at
 * 1659 ms per 60 retraces on the user's console, against 1200 ms expected. */
void gc_console_set(BOOL on);
BOOL gc_console_on(void);

/* Non-fatal trace, compiled out unless GC_DEBUG. */
#ifdef GC_DEBUG
void gc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* What the scheduler has dispatched since boot. These answer the one question
 * a black screen cannot: whether the game thread is still producing work, or
 * whether it stopped and the renderer is merely drawing nothing. */
extern u32 gGcGfxTasks;
extern u32 gGcAudioTasks;
extern u32 gGcSwaps;

/* The retrace message chain, counted at each hop: the VI interrupt posting to
 * the scheduler's interrupt queue, the posts it had to drop because that queue
 * was full, what the scheduler woke up for, and what it forwarded to its
 * clients. The game's frame pacing blocks on the last of these, so a chain
 * that stops partway is the difference between a stalled game and a renderer
 * that draws nothing. */
extern u32 gGcViMesgs;
extern u32 gGcViDrops;
extern u32 gGcSchedMsgs;
extern u32 gGcSchedRetraces;
extern u32 gGcClientSends;

/* Entered/left pairs around the two places the port hands control to GX. A
 * count that goes in and does not come out names the call that hung. */
extern u32 gGcDlIn, gGcDlOut;

/* The shape of the most recently walked display list: commands executed,
 * counted G_DMADL sublists entered, and the deepest nesting reached. */
extern u32 gGcDlCommands, gGcDlDmaLists, gGcDlMaxDepth;

/* Per-opcode counts for that same list, indexed by the command's top byte. */
extern u32 gGcDlOpcodes[256];

/* The last frame's fill rectangles: count, the last colour as 0xRRGGBB, and
 * its corners in game space. */
extern u32 gGcFills, gGcFillColor, gGcFillRect[4];

/* Texture formats the last frame asked for, as a bitmask over
 * (fmt << 2) | siz; how many textured rectangles it drew; and the last tile's
 * size in texels. */
extern u32 gGcTexFormats, gGcTileFormats, gGcTexRects, gGcTexW, gGcTexH;

/* Where the frame's textured rectangles went: submitted, actually drawn, drawn
 * entirely outside the game's screen, and the three ways one is dropped before
 * it reaches GX. Plus the box of the first one drawn, in game pixels. */
extern u32 gGcTrZeroArea, gGcTrNoImage, gGcTrNoTex, gGcTrOffScreen, gGcTrDrawn;
extern s32 gGcTrFirstBox[4];

/* Emitted triangles for each of the last sixteen frames, oldest at
 * gGcTrisHistPos. A per-frame alternation is visible here and in no total. */
extern u32 gGcTrisOutHist[16];
extern u32 gGcTrisHistPos;

/* The game's heap, which is otherwise unobservable: mempool reports its own
 * failures through stubbed_printf, which is defined as nothing, and the two
 * callers that matter (object_model_init, texture_load) use plain mempool_alloc
 * and return NULL silently. Slots are a cliff -- once curNumSlots + 1 reaches
 * 1600 the allocator refuses everything -- and `largest` separates "out of
 * space" from "fragmented". Defined in gc_mainpool.c. */
void gc_pool_report(u32 *slotsUsed, u32 *slotsMax, u32 *freeBytes, u32 *largestFree,
                    u32 *usedBytes);

/* Decompressions attempted and how many came back the wrong length. `ok`
 * climbing is the positive statement the log never made: every compressed asset
 * in the ROM was verified offline against this exact container, so a runtime
 * failure here is a buffer or a pointer, never the format. Defined in
 * gc_gzip.c. */
extern u32 gGcGzipOk, gGcGzipFail;
extern u32 gGcTileModes;

/* What a textured batch actually resolved to: see texdbg_note in gfx_gx.c.
 * "The textures are wrong" has a dozen causes that look identical on screen and
 * differ only in a number, so the numbers get printed. */
#define GC_TEXDBG_MAX 8

typedef struct {
    u32 fmt, siz, line, tmem, palette;
    u32 uls, ult, lrs, lrt;
    u32 cms, cmt, masks, maskt, shifts, shiftt;
    u32 addr, width, height, stride, swapOdd, ok;
    u32 head[4];
} GcTexDbg;

extern u32 gGcTexDbgCount;
extern GcTexDbg gGcTexDbg[GC_TEXDBG_MAX];

#define GC_STATEDBG_MAX 10
extern u32 gGcTexRawValid, gGcTexRawW, gGcTexRawH, gGcTexRawStride, gGcTexRawFmt, gGcTexRawAddr;
extern u8 gGcTexRaw[2048];

extern u32 gGcStateDbgCount;
extern u32 gGcStateDbg[GC_STATEDBG_MAX][5];
/* The G_TRIN funnel: how many triangles were asked for and where the ones
 * that never reached GX were lost. See the block comment in gfx_gx.c. */
extern u32 gGcDlIgnored[256];
extern u32 gGcCimg[6][2], gGcCimgCount;
extern u32 gGcZimg[6], gGcZimgCount;
extern u32 gGcGeoSet, gGcGeoClear;
extern u32 gGcFillsBlend, gGcFillsOffscreen, gGcFogBatches, gGcFogVerts;
extern u32 gGcCoverKind, gGcCoverArea, gGcCoverSeq, gGcCoverTotal;
extern s32 gGcCoverBox[4];

/* The audio task's coverage, the same measurement as the renderer's but for a
 * subsystem with nothing to look at. Per-opcode executed counts over the ABI's
 * sixteen commands, what the walker dropped, and the peak absolute sample that
 * actually reached DRAM -- that last one is what separates "the mixer runs"
 * from "the mixer is silent" without anyone having to listen. */
extern u32 gGcAudioOpcodes[16];
extern u32 gGcAudioIgnored[256];
/* N64 memory-mapped register accesses the port answered instead of letting
 * them fault (gc_n64io.c). `unknown` is the one that matters: it counts the
 * addresses this port does not model, and `lastUnknown` names the newest. */
extern u32 gGcIoReads, gGcIoWrites, gGcIoUnknown, gGcIoLastUnknown;

/* Assertions the decompilation left in and that actually fire. Three exist,
 * all in libultra/src/audio/env.c, all on the audio thread. Non-zero here
 * means the envmixer is being asked for something it says is impossible --
 * see gc_assert.c, and the log line naming which one. */
extern u32 gGcAsserts;

extern u32 gGcAudioCmds, gGcAudioSaves, gGcAudioPeak;
/* Samples that reached DRAM, and how many of them were at the clamp. The
 * ratio is the instrument; peak on its own cannot tell one clipped drum hit
 * from a mix that is railing, and for a day it did not. */
extern u32 gGcAudioSamples, gGcAudioClipped;

/* One level up from the mixer: libultra's own synthesiser, in
 * libultra/src/audio. `syn` counts alAudioFrame calls, how many turned back
 * because no player was ever registered, how many registrations happened at
 * all, and how many words the last frame emitted. This is what tells apart "the
 * mixer is broken" from "the mixer is handed an empty list". */
extern u32 gGcSynFrames, gGcSynNoHead, gGcSynPlayers, gGcSynCmds;
extern u32 gGcSynCurSamples;

/* And one level above that: the audio manager's per-frame sample budget, from
 * src/audiomgr.c. `frameSamples` is what alAudioFrame receives as outLen, and
 * it is computed by subtracting osAiGetLength's report from a fixed frame size
 * -- so it is where an over-deep output ring in this port turns into a
 * negative budget and a silent synthesiser. */
extern u32 gGcAmSamplesLeft, gGcAmFrameSize, gGcAmMinFrameSize;

/* The DAC side, from ultra/os_ai.c: DMA callbacks served, frames of silence
 * emitted for want of data, frames pushed into the ring, and the ring depth. */
extern u32 gGcAiCallbacks, gGcAiUnderruns, gGcAiPushed, gGcAiRingUsed;
/* The shape of the dropouts: separate events, longest run of silent frames in
 * one DMA block, how empty the ring is when the consumer looks (not when the
 * producer does), and the longest run of callbacks with no push at all. */
extern u32 gGcAiUnderEvents, gGcAiUnderMax, gGcAiRingMin, gGcAiPushGapMax;
/* The rate loop that holds the ring at RING_TARGET against the drift between
 * the game's supply and the DAC's 48 kHz: the corrected 16.16 resampling step
 * and the smoothed depth it is steering. */
extern u32 gGcAiStep, gGcAiDepthAvg;
extern u32 gGcAiRejected;
extern u32 gGcAiRefusedFull;
extern u32 gGcAiOffers, gGcAiOfferedFrames, gGcAiGameRate;

/* The ARAM read path (gc_assets.c): total reads, how many needed the shared
 * bounce buffer, and how many found its lock already held. The last one
 * measures the game-thread/audio-thread race directly. */
extern u32 gGcAssetReads, gGcAssetSlow, gGcAssetContended;
extern s32 gGcAmFrameSamples;

/* The dynamic-ambient lighting branch, defined in src/hasm/obj_shade_fast.c.
 * Game-side rather than port-side, but it is the port that translated the
 * function, so its measurement belongs on the port's heartbeat. */
extern u32 gGcDynLit2Objects, gGcDynLit2NoShading, gGcDynLit2Verts;
extern u32 gGcDynLit2Min, gGcDynLit2Max;
extern u32 gGcCoverCc[2];
extern u32 gGcCoverOml, gGcCoverOmh, gGcCoverTex, gGcCoverFmtSiz;
extern u32 gGcCoverCol, gGcCoverPrim, gGcCoverEnv;
extern u32 gGcTexUnhandled, gGcTexNoTlut, gGcTexUnhandledFmtSiz;
extern u32 gGcTrisIn, gGcTrisBadIdx, gGcTrisClipped, gGcTrisCulled, gGcTrisOut;
extern u32 gGcTrisBehind, gGcTrisNearOnly;
extern u32 gGcVtxByMtx[3], gGcVtxBehindByMtx[3], gGcMtxLoads[3], gGcMtxSelects[3];
extern u32 gGcTrinCmds, gGcVtxLoaded, gGcVtxMaxCount;
extern u32 gGcTrinHw, gGcTrinCpu;
extern u32 gGcProjNoW, gGcProjNotAffine;
extern u32 gGcTexNullDim, gGcTexNullAddr, gGcTexNullAlloc;
extern u32 gGcTexHits, gGcTexConverts, gGcTexBytes, gGcTexAsks;
extern s32 gGcCpuBox[4];
extern u32 gGcCpuBoxMtx, gGcCpuBoxTris;
extern u32 gGcCpuBoxBb;
extern s32 gGcCpuBoxAnchor[4];
extern u32 gGcBbBatch[4], gGcBbSeen;
extern float gGcBbAnchor[4], gGcBbPre[4], gGcBbPost[4];
extern s32 gGcBbMtxScene[16], gGcBbMtxBb[16];
extern u32 gGcLastTile[2], gGcLastTileSize[2], gGcLastTexRect[6];
extern u32 gGcSwapIn, gGcSwapOut;
extern u32 gGcCopyIn, gGcCopyOut;

/* fb_update, the last thing the game's frame does. Entered/left, plus the
 * count of retrace messages it actually pulled off the video queue: the
 * scheduler posting them and the game receiving them are different claims. */
extern u32 gGcFbIn, gGcFbOut, gGcFbMesgs;

/* Where the game thread last went to sleep. `pc` is the return address of
 * whoever called a blocking osRecvMesg and is zero while the thread is
 * runnable; `seq` counts how many times it has blocked. A pc that stays put
 * while seq stops moving is the game waiting for a message that is never
 * coming -- resolve it with powerpc-eabi-addr2line against build/gc/dkr.elf.
 * Both staying at zero while the game makes no progress means it is spinning
 * rather than waiting. */
extern volatile u32 gGcBlockPc;
extern volatile u32 gGcBlockQ;
extern volatile u32 gGcBlockSeq;
#else
#define gc_log(...) ((void) 0)
#endif

/* ---- events -------------------------------------------------------------- */

/* Delivers the message registered by osSetEventMesg for `event`, if any.
 * Safe to call from an interrupt handler. */
void gc_event_fire(OSEvent event);

/* ---- threads ------------------------------------------------------------- */

/* The OSThread the calling code is running on, or NULL for a thread the game
 * did not create (the boot thread, libogc's own). */
OSThread *gc_current_thread(void);

/* The libultra id of the first thread that has overflowed its stack, or -1 if
 * none has. Cheap enough to poll; see the canary note in ultra/os_thread.c. */
s32 gc_stack_overflowed(void);

/* Stack size handed to libogc for threads the game creates. The game's own
 * stack arrays are sized for MIPS frames and are not used as the actual stack
 * (see ultra/os_thread.c), so this is the number that matters. */
#ifndef GC_THREAD_STACK_SIZE
#define GC_THREAD_STACK_SIZE (64 * 1024)
#endif

/* The game thread runs the whole simulation and the display-list build, and
 * recurses through the object update tree, so it gets considerably more. */
#ifndef GC_GAME_STACK_SIZE
#define GC_GAME_STACK_SIZE (256 * 1024)
#endif

/* ---- asset image --------------------------------------------------------- */

/* The port keeps the N64 asset layout intact and serves it through the PI DMA
 * entry points, so asset_loading.c and memory.c work unmodified. This opens
 * the image; see ultra/os_pi.c for how reads are served. */
BOOL gc_assets_open(const char *path);

/* The same image, linked into the executable rather than read from a card.
 * Returns FALSE if this build embedded none. */
BOOL gc_assets_open_embedded(void);

void gc_assets_close(void);

/* Reads `len` bytes at `romOffset` in the asset image into `dst`. */
void gc_assets_read(u32 romOffset, void *dst, u32 len);

/* Reads the image back out of ARAM and compares it with what was uploaded.
 * One line in the log; call once, at boot, after the image is open. */
void gc_assets_verify(void);

/* Turns a RAM address back into the read that filled it: the newest recorded
 * asset read whose destination covers `addr`. FALSE means no read in the ring
 * ever wrote there, which is the most informative answer of the three. */
BOOL gc_assets_find_read(u32 addr, u32 *rom, u32 *dst, u32 *len, u32 *seq, u32 *slow);

/* How many asset reads have been served since boot. */
u32 gc_assets_read_seq(void);

/* ---- task execution ------------------------------------------------------ */

/* The two entry points the reimplemented scheduler dispatches to in place of
 * kicking off an RSP task. Both run synchronously on the calling thread and
 * must have finished all their work when they return, because the scheduler
 * sends the task's completion message the moment they do.
 *
 * They take plain pointers rather than OSTask_t because the graphics side
 * cannot see the PR headers that define it; see platform/gc/gfx/gfx_gx.h. */
void gc_gfx_run_dl(const void *dl);
void gc_audio_run_cmds(const void *cmdList, u32 sizeBytes);

/* ---- video --------------------------------------------------------------- */

/* Brings up VI, the framebuffers and GX. Called once, before the game thread
 * starts. */
void gc_video_init(void);

/* Called from the retrace interrupt; raises OS_EVENT_VI and advances the
 * frame counter the scheduler runs off. */
void gc_video_retrace(u32 retraceCount);

/* Presents the framebuffer a finished graphics task drew into. Called by the
 * scheduler when the task carried OS_SC_SWAPBUFFER. */
void gc_video_swap(void *framebuffer);

/* The resolution the game is building its display list in, which is not the
 * resolution GX rasterises at. */
void gc_video_game_resolution(int *width, int *height);

/* The render mode chosen from the console's TV standard. gfx sizes the
 * viewport, the scissor and the copy-out from it. */
struct _gx_rmodeobj;
void *gc_video_xfb(void);

struct _gx_rmodeobj *gc_video_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* GC_ULTRA_H */

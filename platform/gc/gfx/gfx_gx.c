/*
 * The graphics task: walking an F3DDKR display list and driving GX with it.
 *
 * This is the replacement for the RSP. On the N64 a graphics task was a buffer
 * of 64-bit commands that the RSP fetched, transformed and fed to the RDP; the
 * scheduler kicked it off and waited for an interrupt. Here the scheduler
 * calls gc_gfx_run_task instead, and this file interprets the same buffer on
 * the CPU, translating as it goes into GX state and vertex submission.
 *
 * A few things about the command stream are worth knowing before reading the
 * dispatch loop.
 *
 * Segmented addressing. Almost every pointer in a display list is a segment
 * number in the top byte and an offset in the rest, resolved against a table
 * the list itself populates. The game uses this to relocate assets that were
 * DMA'd to arbitrary addresses, and it means no pointer in the stream can be
 * dereferenced without going through segmented_to_virtual first.
 *
 * F3DDKR is not stock F3DEX. Rare modified the microcode, and the differences
 * are not cosmetic:
 *
 *   - G_TRIN (5) submits a whole batch of triangles from a pointer, rather
 *     than one triangle encoded in the command word. Each entry carries its
 *     own texture coordinates, so a batch is self-contained.
 *   - G_DMADL (7) runs a fixed number of commands from somewhere else and then
 *     comes back. It is not a call: the count is in the command word and the
 *     block it names has no G_ENDDL to find (see dDialogueBoxDrawModes in
 *     src/font.c, which is a bare Gfx[2]). The RSP DMA'd exactly that many
 *     words into DMEM, ran them, and resumed.
 *   - G_VTX packs its vertex count and destination index differently, and has
 *     an append flag that decides whether the loaded vertices go to the start
 *     of the internal array or after what is already there.
 *   - G_MOVEWORD gains a billboard toggle, which makes subsequent vertices
 *     offsets from vertex 0 rather than positions in their own right.
 *
 * Those are described in include/f3ddkr.h, which is the authority.
 *
 * State of this file: the GX bring-up, the command walker, the structural
 * opcodes, the 2D path (scissor, fill, textured rectangles), the geometry path
 * (matrices, vertices, batched triangles, viewport, depth, near-plane
 * clipping, back-face culling) and the render state (the colour combiner as
 * TEV stages, the render mode as blend, depth and alpha-test state, the
 * primitive and environment colours) all work. What is still ignored,
 * deliberately and silently, is G_TEXTURE's S and T scale, the tile shifts and
 * origins, and the second texture a two-texture combiner asks for. Each of
 * those degrades the picture; none of them stops a frame.
 */

#include <ogc/gx.h>
#include <ogc/gu.h>
#include <ogc/gx_struct.h>
#include <ogc/cache.h>
#include <ogc/system.h>

#include <malloc.h>
#include <string.h>

#include "gfx_gx.h"

/* Declared here rather than pulled in from gc_ultra.h, which includes the PR
 * headers this file cannot see (see gfx_gx.h). */
void gc_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
GXRModeObj *gc_video_mode(void);
void *gc_video_xfb(void);
void gc_video_game_resolution(int *width, int *height);

#ifdef GC_DEBUG
void gc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
#define gc_log(...) ((void) 0)
#endif

/*
 * PR/gbi.h cannot be included here. It declares Vtx and Mtx, and so does
 * <ogc/gx.h> and <ogc/gu.h>, with incompatible definitions -- the same
 * collision aluzed hit in sm64-port-gc. The opcode values the walker needs are
 * therefore repeated below rather than included. They are stable: these are
 * the numbers burned into every display list the asset build produces.
 */
#define G_NOOP        0xC0
#define G_MTX         0x01
#define G_MOVEMEM     0x03
#define G_VTX         0x04
#define G_TRIN        0x05 /* F3DDKR: batched triangles */
#define G_DL          0x06
#define G_DMADL       0x07 /* F3DDKR: a counted run of commands, not a call */

/* G_DL's flag byte. gbi.h cannot be included here (see above); these are its
 * G_DL_PUSH / G_DL_NOPUSH. */
#define G_DL_PUSH     0x00
#define G_TRI1        0xBF
#define G_CULLDL      0xBE
#define G_MOVEWORD    0xBC
#define G_TEXTURE     0xBB
#define G_SETOTHERMODE_H 0xBA
#define G_SETOTHERMODE_L 0xB9
#define G_ENDDL       0xB8
#define G_RDPFULLSYNC 0xE9
#define G_SETCIMG 0xFF
#define G_SETZIMG 0xFE
#define G_SETGEOMETRYMODE 0xB7
#define G_CLEARGEOMETRYMODE 0xB6
#define G_SETFOGCOLOR 0xF8

/* include/PR/gbi.h:267. The only geometry-mode bit this port acts on, and the
 * measurement says why: over 84 frames of intro, menus and a demo race the
 * game's SET mask is 00010205 and its CLEAR mask 001f3205 -- G_CULL_FRONT and
 * G_CULL_BACK appear only in the clear, never in the set, so geometry-mode
 * culling never turns on and the per-triangle BACKFACE_DRAW flag the port
 * already honours is the whole story. G_FOG is genuinely toggled both ways. */
#define G_FOG 0x00010000

/* gbi.h:1087. gSPFogPosition packs the RSP's two fog coefficients into one
 * moveword: (128000/(max-min)) << 16 | (500-min)*256/(max-min). */
#define G_MW_FOG 0x08

/* Blender colour and alpha muxes, gbi.h:563. Cycle 1 of DKR's 3D render modes
 * is G_RM_FOG_SHADE_A, which is P = CLR_FOG with A = A_SHADE; the measured
 * words agree (omL c8112078: bits 31..30 = 11, bits 27..26 = 10). */
#define BL_P_CLR_FOG 3
#define BL_A_SHADE 2
#define G_RDPLOADSYNC 0xE6
#define G_RDPPIPESYNC 0xE7
#define G_SETTILE     0xF5
#define G_SETPRIMCOLOR 0xFA
#define G_SETENVCOLOR 0xFB
#define G_SETCOMBINE  0xFC
#define G_SETTIMG     0xFD
#define G_TEXRECT     0xE4
/* The one command that writes both other-mode words at once. DKR's draw
 * tables are built out of it (gsDPSetOtherMode, include/f3ddkr.h), so the
 * render mode and the cycle type arrive here and not through
 * G_SETOTHERMODE_H/_L -- which is why nothing was reading them. */
#define G_RDPSETOTHERMODE 0xEF
/* G_IMMFIRST is 0xBF, and gbi.h defines these as G_IMMFIRST-12 and -13. */
#define G_RDPHALF_1   0xB3
#define G_RDPHALF_2   0xB2
#define G_SETTILESIZE 0xF2
#define G_LOADBLOCK   0xF3
#define G_LOADTILE    0xF4

/* Moveword indices. gImmp21 puts the index in bits 0..7 and the offset in bits
 * 8..23, which is the opposite of what it looks like: see gMoveWd in gbi.h.
 * G_MW_BILLBOARD and G_MW_MVPMATRIX are Rare's, and reuse index values stock
 * F3DEX gives to other things. */
#define G_MW_SEGMENT   0x06
#define G_MW_BILLBOARD 0x02
#define G_MW_MVPMATRIX 0x0A

/* G_MOVEMEM's parameter byte for a viewport, from gSPViewport. */
#define G_MV_VIEWPORT 0x80

/* The Triangle flag byte, from include/structs.h. */
#define BACKFACE_DRAW 0x40
#define G_SETSCISSOR  0xED
#define G_FILLRECT    0xF6
#define G_SETFILLCOLOR 0xF7
#define G_SETBLENDCOLOR 0xF9
/* G_IMMFIRST-11, so 0xB4. */
#define G_PERSPNORMALIZE 0xB4

/* The FIFO GX reads its commands from. 256 KB is the size libogc's own
 * examples use and is comfortable for a scene of this complexity; it only has
 * to absorb one frame's worth of commands before the GP catches up. */
#define GX_FIFO_SIZE (256 * 1024)

/* Display lists nest through G_DL. The microcode's own stack was ten deep. */
#define DL_STACK_DEPTH 16

/*
 * A ceiling on how far one display list may be walked.
 *
 * The RSP could not run away: it fetched from a fixed DMEM window and the
 * microcode was trusted. Walking the same stream on the CPU has no such floor,
 * and while this renderer is incomplete it will meet lists it does not fully
 * understand -- a jump through a segment the list has not set yet resolves to
 * low memory, which is zeroes, which decode as G_NOOP forever. That is a hang
 * inside the scheduler thread, so it takes the whole machine down: no more
 * retraces are forwarded, the game thread never gets its task reply, and from
 * outside it is indistinguishable from a renderer that draws nothing.
 *
 * A budget turns that into a dropped frame and a line of diagnostics. A real
 * DKR frame is a few thousand commands, so this is two orders of magnitude of
 * headroom rather than a tuning parameter.
 */
#define DL_MAX_COMMANDS 200000

#define NUM_SEGMENTS 16

typedef union {
    struct {
        u32 w0;
        u32 w1;
    };
    u64 force_align;
} GfxCmd;

static void *sFifo;
static BOOL sGxReady;

/*
 * The screen space the display list is written in, and what one of its units
 * is worth in EFB pixels.
 *
 * The list gives every coordinate in the game's own 320x240 (or 320x284 on
 * PAL) space. Rather than scale each one, the projection is set up in that
 * space and GX's viewport does the stretching to the 640x480 EFB. The scale
 * factors are still needed for the scissor, which GX takes in EFB pixels.
 */
static f32 sGameWidth = 320.0f;
static f32 sGameHeight = 240.0f;
static f32 sScaleX = 2.0f;
static f32 sScaleY = 2.0f;

/*
 * The texture pipeline, as much of it as a rectangle needs.
 *
 * The RDP loaded texels into a 4 KB TMEM and then read them back through a
 * tile descriptor. Emulating that byte for byte is not needed, but the shape
 * of it is: a load names a destination in TMEM, and a tile names where it
 * reads from, and only the TMEM address ties the two together. DKR's own load
 * idiom makes that concrete -- gDPLoadTextureBlock describes the load through
 * tile 7 and the drawing through tile 0, and a colour-index texture is
 * followed by a second load, its palette, into the top of TMEM. So each load
 * is recorded against its TMEM address (see TmemLoad below) and a tile looks
 * its texels up by its own; G_SETTILESIZE says how big the picture is and
 * `line` says how wide a row of it is in memory.
 */
typedef struct {
    u32 fmt;
    u32 siz;
    u32 uls, ult, lrs, lrt; /* 10.2 fixed point, from G_SETTILESIZE */
    /*
     * How the tile is addressed outside its own bounds. Only fmt and siz used
     * to be kept, so every texture was clamped and every repeating surface --
     * road, water, walls -- showed one smeared edge column instead of tiling.
     * Field positions are gDPSetTile's own (include/PR/gbi.h): in w1, cmt at
     * 18, maskt at 14, shiftt at 10, cms at 8, masks at 4, shifts at 0.
     */
    u32 cms, cmt;       /* G_TX_MIRROR = 1, G_TX_CLAMP = 2 */
    u32 masks, maskt;   /* wrap period is 2^mask texels */
    u32 shifts, shiftt;
    /*
     * Where the tile sits in TMEM and how wide one of its rows is there.
     *
     * `line` is in 64-bit words and is what gives a row its real stride:
     * gDPLoadTextureBlock computes it as ((width * LINE_BYTES) + 7) >> 3, so a
     * row is padded to a multiple of eight bytes and a packed width * bytes
     * assumption shears every four-bit texture whose width is not a multiple
     * of sixteen. `tmem` is what identifies which load filled this tile.
     */
    u32 line, tmem;
    u32 palette;        /* which 16-entry bank a CI4 tile reads */
} TileDesc;

#define NUM_TILES 8

static TileDesc sTiles[NUM_TILES];

/*
 * The RDP's high other-mode word, tracked because the texture filter lives in
 * it and DKR does ask for a filter: gsDPSetTextureFilter(G_TF_BILERP) appears
 * in rcp_dkr.c, menu.c, printf.c, weather.c and fade_transition.c. The port
 * used to sample every texture GX_NEAR regardless.
 *
 * gSPSetOtherMode puts the shift at bits 8..15 and the length at 0..7 of w0,
 * with w1 already shifted into place, so the register is updated by masking
 * that field out and OR-ing w1 back in.
 */
static u32 sOtherModeH;
static u32 sTimgAddr; /* the image G_SETTIMG named */

/*
 * What has been loaded into TMEM, at the granularity that matters.
 *
 * Emulating TMEM byte for byte is not needed, but a single "the texture is
 * whatever the last load consumed" global is not enough either: the standard
 * load idiom describes the load through tile 7 and draws through tile 0, and a
 * colour-index texture is followed by a second load -- its palette -- that
 * would otherwise overwrite the answer. What ties the two ends together is the
 * TMEM address: the load tile and the render tile name the same one.
 *
 * So each load records where it went and where it came from, and a tile finds
 * its texels by looking up its own tmem. That also picks the palette out for
 * free, because in DKR a palette is nothing but a load to a high TMEM address
 * (see tile_image).
 */
#define NUM_TMEM_LOADS 8

/* TMEM's upper half, in 64-bit words, which is where every palette lives. */
#define TMEM_TLUT_BASE 256

typedef struct {
    u32 tmem;
    u32 addr;
    BOOL swapOdd; /* the load left the odd rows exchanged: see read_texel */
    BOOL valid;
} TmemLoad;

static TmemLoad sLoads[NUM_TMEM_LOADS];
static u32 sLoadNext;

/* G_TEXTURE's S and T scale. One is the identity and is what DKR asks for,
 * but reading it costs nothing and a list that scales its coordinates would
 * otherwise draw the wrong part of every texture. */
static f32 sTexScaleS = 1.0f;
static f32 sTexScaleT = 1.0f;

static const TmemLoad *tmem_find(u32 tmem) {
    u32 i;

    for (i = 0; i < NUM_TMEM_LOADS; i++) {
        if (sLoads[i].valid && sLoads[i].tmem == tmem) {
            return &sLoads[i];
        }
    }
    return NULL;
}

/*
 * G_LOADBLOCK and G_LOADTILE. `dxt` is the block load's texel-to-line
 * increment; `block` says which of the two this was.
 *
 * dxt is the whole point. A block load with a real dxt exchanges the two
 * 32-bit halves of every 64-bit word on odd rows as it writes, which cancels
 * the exchange the texture unit performs when it reads them back, so the image
 * in DRAM is plain. With dxt = 0 it writes verbatim, the read's exchange
 * stands, and the image in DRAM must already be exchanged -- which is exactly
 * what DKR's line-swapped textures are (src/textures_sprites.c:1505 picks the
 * gDPLoadTextureBlockS macros, whose only difference is dxt = 0). A tile load
 * walks rows and exchanges them, so it never needs this.
 */
static void tmem_note_load(u32 tile, u32 dxt, BOOL block) {
    u32 tmem = sTiles[tile & 7].tmem;
    u32 i;
    TmemLoad *e = NULL;

    for (i = 0; i < NUM_TMEM_LOADS; i++) {
        if (sLoads[i].valid && sLoads[i].tmem == tmem) {
            e = &sLoads[i];
            break;
        }
    }
    if (e == NULL) {
        e = &sLoads[sLoadNext];
        sLoadNext = (sLoadNext + 1) % NUM_TMEM_LOADS;
    }

    e->tmem = tmem;
    e->addr = sTimgAddr;
    e->swapOdd = block && dxt == 0;
    e->valid = TRUE;
}

/* ---- render state --------------------------------------------------------
 *
 * The colour combiner, the render mode and the two constant colours describe
 * one stage of the RDP's pipeline between them, and they were wrong together
 * for as long as all of them were ignored: with the TEV pinned to modulate and
 * no render mode read, a decal drawn with alpha came out as an opaque quad.
 */

/*
 * The RDP's low other-mode word: alpha compare in bits 0..1, the depth source
 * in bit 2, and the render mode -- coverage, depth and the blender -- in the
 * rest. These are gbi.h's own values, repeated rather than included for the
 * reason given at the top of this file.
 */
/* othermode_L bits 0..1, G_MDSFT_ALPHACOMPARE = 0. G_AC_THRESHOLD compares the
 * pixel's alpha against the blend colour's alpha rather than against a fixed
 * value, which is the only thing in this game that reads G_SETBLENDCOLOR. */
#define RM_ALPHACOMPARE 0x00000003
#define RM_AC_THRESHOLD 0x00000001

#define RM_Z_CMP        0x00000010
#define RM_Z_UPD        0x00000020
#define RM_ZMODE_MASK   0x00000C00
#define RM_ZMODE_DEC    0x00000C00
#define RM_CVG_X_ALPHA  0x00001000
#define RM_FORCE_BL     0x00004000

/*
 * The blender's four muxes. It computes p * a + m * b, so p and m name
 * colours and a and b name the factors they are scaled by; the two pairs sit
 * at bits 30/26/22/18 for the first cycle and 28/24/20/16 for the second.
 */
#define BL_P_CLR_IN     0
#define BL_M_CLR_MEM    1
#define BL_A_IN         0
#define BL_A_ZERO       3
#define BL_B_1MA        0
#define BL_B_A_MEM      1
#define BL_B_ONE        2
#define BL_B_ZERO       3

/*
 * The default is the state this file used to pin the pipeline to: depth
 * tested and written, ordinary source-alpha blending. So anything that draws
 * before the game's first render mode arrives behaves exactly as it did.
 */
#define OTHERMODE_L_DEFAULT                                                         (RM_Z_CMP | RM_Z_UPD | RM_FORCE_BL | ((u32) BL_P_CLR_IN << 30) |                 ((u32) BL_A_IN << 26) | ((u32) BL_M_CLR_MEM << 22) | ((u32) BL_B_1MA << 18))

static u32 sOtherModeL = OTHERMODE_L_DEFAULT;

/*
 * Set to 0 to go back to the pinned pipeline -- source-alpha blending, depth
 * always tested and written, no alpha test -- while leaving the combiner on.
 * "The picture changed" has two possible causes once both are implemented,
 * and this separates them without a second build of everything.
 */
#ifndef GC_RENDERMODE
#define GC_RENDERMODE 1
#endif

/*
 * Set to 0 to pin the TEV back to modulate-by-shade (pass the vertex colour
 * when there is no texture), leaving the render mode alone. The counterpart of
 * GC_RENDERMODE: between them, a change in the picture can be attributed to
 * the combiner or to the render mode without rebuilding anything else.
 */
#ifndef GC_COMBINER
#define GC_COMBINER 1
#endif

/*
 * ZMODE_DEC, as a depth bias in normalised device coordinates.
 *
 * Large enough to clear the rounding between a decal and the surface it lies
 * on, small enough not to let a shadow show through a thin floor. The same
 * constant, for the same reason, as ref-sm64gc's GFX_GX_DECAL_BIAS.
 */
#ifndef GC_DECAL_BIAS
#define GC_DECAL_BIAS 0.0001f
#endif

static f32 sDecalBias;

/*
 * G_SETCOMBINE, decoded.
 *
 * Each cycle of the RDP's combiner computes (a - b) * c + d, once for colour
 * and once for alpha, from four muxes. The muxes are not one enumeration:
 * each of the four slots reads a field of a different width over a different
 * set of sources, and the same number means different things in different
 * slots -- 6 is "1" in the a slot, the chroma-key centre in the b slot, the
 * key scale in the c slot and "1" again in the d slot. So a raw field value
 * means nothing until it has been through the table for its own slot, which
 * is what gfx_set_combine does; everything downstream works in the operand
 * names below.
 */
typedef enum {
    CC_COMBINED,
    CC_TEXEL0,
    CC_TEXEL1,
    CC_PRIM,
    CC_SHADE,
    CC_ENV,
    CC_COMBINED_A,
    CC_TEXEL0_A,
    CC_TEXEL1_A,
    CC_PRIM_A,
    CC_SHADE_A,
    CC_ENV_A,
    CC_ONE,
    CC_ZERO
} CombineOperand;

typedef struct {
    u8 col[2][4]; /* a, b, c, d -- for cycle 0 and cycle 1 */
    u8 alp[2][4];
} CombineDesc;

/* TEXEL0 * SHADE for both channels: G_CC_MODULATEIA, which is what the
 * hardcoded GX_MODULATE this replaces was computing. */
static const CombineDesc kCombineDefault = {
    { { CC_TEXEL0, CC_ZERO, CC_SHADE, CC_ZERO },
      { CC_TEXEL0, CC_ZERO, CC_SHADE, CC_ZERO } },
    { { CC_TEXEL0_A, CC_ZERO, CC_SHADE_A, CC_ZERO },
      { CC_TEXEL0_A, CC_ZERO, CC_SHADE_A, CC_ZERO } }
};

static CombineDesc sCombine = {
    { { CC_TEXEL0, CC_ZERO, CC_SHADE, CC_ZERO },
      { CC_TEXEL0, CC_ZERO, CC_SHADE, CC_ZERO } },
    { { CC_TEXEL0_A, CC_ZERO, CC_SHADE_A, CC_ZERO },
      { CC_TEXEL0_A, CC_ZERO, CC_SHADE_A, CC_ZERO } }
};

/* G_SETENVCOLOR. DKR tints with it constantly -- every dialogue box and every
 * piece of text picks its colour this way (font.c), and object shading feeds
 * the level's light colour through it (objects.c:3358). */
static GXColor sEnvColor = { 0xFF, 0xFF, 0xFF, 0xFF };

/*
 * G_SETBLENDCOLOR. The RDP keeps one blend colour and uses it in two places:
 * as the blender's CLR_BL input, and as the reference the alpha test compares
 * against under G_AC_THRESHOLD.
 *
 * DKR emits it exactly once per rendered frame -- gDPSetBlendColor(0, 0, 0,
 * 100) at src/tracks.c:351, before the track is drawn -- and never selects
 * CLR_BL in a blender mux, so the alpha reference is the whole of its effect
 * here. 100/255 is the cutoff the game wants for its threshold surfaces, and
 * apply_render_mode uses it below.
 */
static GXColor sBlendColor = { 0x00, 0x00, 0x00, 0x64 };

/*
 * The three matrix slots, and which one is current.
 *
 * Each is a full model-view-projection: camera.c composes it as
 * `mtxf_mul(model, viewProj, out)` at line 1418, and `mtxf_mul` is
 * `out[i][j] = sum_k a[i][k] * b[k][j]` (src/hasm/math_util.c), so with the
 * N64's row vectors a vertex is transformed as `clip = v * M` -- column j of
 * the matrix produces clip component j.
 */
#define NUM_MATRICES 3

static f32 sMatrix[NUM_MATRICES][4][4];
static u32 sCurMatrix;

/*
 * The viewport, which is what maps normalised device coordinates to pixels.
 *
 * Reading it rather than assuming a mapping matters: DKR moves and resizes it
 * per camera (viewport_rsp_set in camera.c), and splitscreen depends on that.
 * Both arrays carry two bits of fraction (Vp_t in gbi.h).
 */
static f32 sVpScaleX = 160.0f, sVpScaleY = 120.0f;
static f32 sVpTransX = 160.0f, sVpTransY = 120.0f;

/*
 * The vertex buffer the display list fills and then indexes.
 *
 * G_VTX carries its count in five bits, so a single command loads at most 32;
 * G_VTX_APPEND accumulates across commands, and the repository does not say
 * how deep the microcode's own buffer was. Sixty-four is twice the largest
 * single load and every index is bounds-checked, so an overrun drops triangles
 * instead of reading past the array.
 */
#define MAX_VERTICES 64

/*
 * A transformed vertex, kept in clip space.
 *
 * The perspective divide happens at emit time rather than here because
 * clipping has to run before it: a vertex behind the camera has a negative w,
 * and dividing by it turns a triangle that crosses the near plane inside out
 * instead of shortening it.
 */
typedef struct {
    f32 x, y, z, w;
    u8 r, g, b, a;
} XVertex;

static XVertex sVerts[MAX_VERTICES];
static u32 sVertCount;

/*
 * Where an appending G_VTX starts.
 *
 * f3ddkr.h is precise about this and the distinction matters: a load with the
 * flag clear "written to the beginning of RSP's internal vertex array, and
 * their count is stored", and a load with G_VTX_APPEND is "appended after
 * those written with flag 0" -- after the last non-appending load's count, not
 * after the running total.
 *
 * Using the running total is right for the first append and wrong for every
 * one after it. The sprite builder appends in groups of five quads and resets
 * its own index to zero between them (textures_sprites.c:1399 and the
 * `curVertIndex = 0` a few lines below), so every group is meant to land back
 * at index 1, on top of the last. Accumulating instead put the second group at
 * 21 while its triangles still referenced 1..20 -- drawing the previous group's
 * corners again -- and pushed later groups past MAX_VERTICES entirely. Any
 * sprite made of more than five tiles came out wrong.
 *
 * The other appending site, the head in objects.c:4304, wants the same rule:
 * it loads offsetStartVertex vertices through slot 1 with the flag clear, then
 * appends the rest through slot 2, and its triangles index the whole model.
 */
static u32 sVertBase;

/*
 * G_MW_BILLBOARD. While this is on, a vertex is an offset from vertex 0 rather
 * than a position of its own.
 *
 * The header describes the whole mechanism (include/f3ddkr.h, top of file):
 * push one anchor vertex through the ordinary MVP, put a billboard matrix --
 * identity in the simplest case, with no camera or projection in it -- in slot
 * 2, enable billboarding, then push the sprite's corners in sprite space.
 * The RSP adds those to the anchor's coordinates *after* the matrix transform
 * and *before* the perspective divide, so the sprite ends up at the anchor's
 * depth and is scaled by the anchor's w -- which is what makes distant sprites
 * small.
 *
 * The header's own worked example keeps the anchor's w: a corner of a 32-wide
 * sprite becomes (x+32, y, z, w), not (x+32, y, z, w+1). So x, y and z are
 * summed and w is taken from the anchor.
 *
 * Leaving this unimplemented is not cosmetic. The sprite corners are small
 * integers in sprite space; treated as clip coordinates against a w near 1
 * they cover the whole screen.
 *
 * The anchor is vertex 0, and that is read off the game rather than inferred:
 * both emission sites (camera.c:1157 and camera.c:1240) push it with
 * gSPVertexDKR(..., 1, 0) -- one vertex, G_VTX_APPEND clear, so index 0 --
 * before loading slot 2 and before enabling billboarding, and the decompiler's
 * own comment beside the vehicle-part branch says "sprite vertices are
 * hardcoded to start from index 1". The measured batch agrees: n=4, append=1,
 * base=1.
 *
 * This was written once, appeared to make the picture much worse, and was
 * turned off. That was a misreading: load_matrix was decoding every negative
 * matrix coefficient as a huge positive one, so the anchor's clip coordinates
 * were nonsense and adding them to anything made a mess. With that fixed the
 * arithmetic below behaves, so it is on by default. GC_BILLBOARD=0 still
 * disables it for A/B.
 */
#ifndef GC_BILLBOARD
#define GC_BILLBOARD 1
#endif

static BOOL sBillboard;

/* G_SETPRIMCOLOR. One of the combiner's two constant colours, and what tints
 * most of DKR's interface -- the flashing menu text is one texture drawn
 * repeatedly in different colours. It reaches the TEV as register 0. */
static GXColor sPrimColor = { 0xFF, 0xFF, 0xFF, 0xFF };

/* G_SETFILLCOLOR, as the RDP stores it: two RGBA5551 pixels packed into one
 * word, because a fill covers two pixels of a 16-bit framebuffer per cycle.
 * Both halves are the same colour in everything DKR emits, so the low one is
 * taken. */
static GXColor sFillColor = { 0, 0, 0, 0xFF };

/* G_SETFOGCOLOR. apply_fog (src/tracks.c:4331) emits it beside gSPFogPosition
 * every frame, from the level header's fogR/fogG/fogB. */
static GXColor sFogColor = { 0xFF, 0xFF, 0xFF, 0xFF };

/* The two coefficients gSPFogPosition packs, as the RSP reads them back. */
static s16 sFogMul;
static s16 sFogOff;

/* The geometry mode, as G_SETGEOMETRYMODE and G_CLEARGEOMETRYMODE leave it. */
static u32 sGeoMode;

/*
 * The RDP's current colour and depth images.
 *
 * DKR names exactly two targets per frame -- measured, 84 frames running:
 * cimg 81000000 and cimg 82000000, both fmt0 siz2 width320, with
 * zimg 82000000. The second colour target *is* the depth buffer, which is the
 * ordinary N64 idiom for clearing depth: point the colour image at the Z
 * buffer, fill it, point it back. There is no off-screen colour target in this
 * game, so retargeting is not what these commands need -- what they need is
 * for the port to stop painting that depth clear into the visible framebuffer.
 *
 * Both reset per list so that an unknown target fails open: a frame whose
 * first primitive precedes any G_SETCIMG still draws.
 */
static u32 sCimg;
static u32 sZimg;

/*
 * Is the RDP currently pointed at the colour buffer?
 *
 * Only a positively identified depth target says no. Zero means "not stated
 * yet", which has to draw.
 */
static BOOL drawing_to_color(void) {
    return !(sZimg != 0 && sCimg == sZimg);
}

/* The corner colour for a rectangle whose colour comes from the combiner. */
static const GXColor kWhite = { 0xFF, 0xFF, 0xFF, 0xFF };

#ifdef GC_DEBUG
/* Which N64 texture formats the frame actually asks for, as a bitmask indexed
 * by (fmt << 2) | siz -- fmt is RGBA/YUV/CI/IA/I, siz is 4/8/16/32 bits. Only
 * the converters for formats that appear are worth writing, and there is no
 * way to know which those are without looking. The last tile's dimensions come
 * along because a texture cache is keyed on them. */
/* Which (cmt, cms) pairs the frame's tiles actually ask for, indexed
 * (cmt << 2) | cms. Everything used to be clamped regardless; this says how
 * much of the picture that was costing. */
u32 gGcTileModes;
u32 gGcCombineW0, gGcCombineW1;
u32 gGcTexFormats;   /* from G_SETTIMG: how the block is fetched */
u32 gGcTileFormats;  /* from G_SETTILE: how the texels are actually read */
u32 gGcTexRects;
u32 gGcTexW, gGcTexH;
/* The raw words of the last G_SETTILE and G_SETTILESIZE, and of the last
 * G_TEXRECT with its two G_RDPHALF followers. Decoding these by hand is what
 * the texrect implementation needs, and a bitmask is too lossy for that. */
u32 gGcLastTile[2];
u32 gGcLastTileSize[2];
u32 gGcLastTexRect[6];
static u32 sTexRectWord;
static u32 sTexRectArea;

/* What the last frame's fill rectangles actually were: how many were emitted,
 * the colour of the last one and its game-space corners. A black screen with a
 * non-zero count and a black colour is the game clearing to black; a zero count
 * is the renderer dropping the primitive. */
/* Every opcode the walker did not act on, by opcode.
 *
 * The census beside it counts what the game *sends*; this counts what the
 * port *drops*, which is a different and more useful number. Without it the
 * walker's `default: break` is silent by construction: an unimplemented
 * command degrades the picture and says nothing, so "there are still a lot of
 * graphical bugs" cannot be turned into a list. With it, one run names every
 * missing command and how badly it is wanted. */
u32 gGcDlIgnored[256];

/* Where the game says to draw.
 *
 * G_SETCIMG and G_SETZIMG are ignored by the walker, eight commands a frame,
 * so everything lands in the main framebuffer whatever target was asked for.
 * Before writing any retargeting, the question is what those targets actually
 * are: the framebuffer the port is already drawing into (nothing to do), the
 * depth buffer (a Z clear idiom), or somewhere else entirely. Distinct
 * addresses per frame, with the format word, answer it in one run. */
#define GC_IMGDBG_MAX 6
u32 gGcCimg[GC_IMGDBG_MAX][2]; /* addr, w0 */
u32 gGcCimgCount;
u32 gGcZimg[GC_IMGDBG_MAX];
u32 gGcZimgCount;

/* Every geometry-mode bit the frame sets and clears, OR'd. The repository says
 * which flags the source writes; this says which ones actually arrive, which
 * is the number that decides what has to be implemented. */
u32 gGcGeoSet;
u32 gGcGeoClear;

u32 gGcFills;
u32 gGcFillsBlend;
u32 gGcFillsOffscreen;
u32 gGcFogBatches;
u32 gGcFogVerts;
u32 gGcFillColor;
u32 gGcFillRect[4];

/* The fate of every triangle the last frame's G_TRIN commands asked for.
 * "Nothing appeared" has four quite different causes and they are not
 * distinguishable from the picture: the command never ran, its vertex indices
 * point past what G_VTX loaded, the near-plane clip threw the triangle away,
 * or the backface test did. Each has its own counter so one run says which,
 * instead of a build per hypothesis.
 *
 * Read them as a funnel: in = badidx + clipped + culled + out. A model that is
 * missing while `culled` accounts for it is the winding convention being
 * backwards; one where `in` itself is zero is not a renderer problem at all. */
/* What covers the screen.
 *
 * A frame that comes out uniformly one colour while a thousand triangles are
 * emitted is not a frame that failed to draw; it is a frame something drew
 * over. This keeps the widest primitive of the frame -- in per-mille of the
 * screen, so triangles, texture rectangles and fills are comparable -- and
 * enough state beside it to name what drew it. Ties go to the later
 * primitive, because the thing on top is the thing drawn last. */
u32 gGcCoverKind; /* 0 nothing, 1 hw triangle, 2 cpu triangle, 3 texrect, 4 fill */
u32 gGcCoverArea;
s32 gGcCoverBox[4];
u32 gGcCoverSeq;
u32 gGcCoverTotal;
u32 gGcCoverCc[2];
u32 gGcCoverOml;
u32 gGcCoverOmh;
u32 gGcCoverTex;
u32 gGcCoverFmtSiz;
u32 gGcCoverCol;
u32 gGcCoverPrim;
u32 gGcCoverEnv;
static u32 sCoverTexAddr;
static u32 sCoverFmtSiz;

/* Formats the texture converter has no case for, and colour-index textures
 * whose palette never arrived. Both paint magenta, so a magenta screen is
 * either one of these or something else entirely -- this says which. */
u32 gGcTexUnhandled;
u32 gGcTexNoTlut;
u32 gGcTexUnhandledFmtSiz;

u32 gGcTrisIn;
u32 gGcTrisBadIdx;
u32 gGcTrisClipped;
/* Of the triangles the near plane rejected, how many were genuinely behind the
 * eye (every corner at w <= 0) and how many merely fell inside NEAR_W while
 * still being in front of it. The second number is the one that indicts the
 * threshold: geometry behind the camera has to go, geometry a metre in front
 * of it does not. */
u32 gGcTrisBehind;
u32 gGcTrisNearOnly;
u32 gGcTrisCulled;
u32 gGcTrisOut;
u32 gGcTrinCmds;
/* How many G_TRIN batches got the hardware projection and how many fell back
 * to dividing on the CPU. A fallback is not an error, but it is affine and
 * unclipped, so if the wedges and smears persist this is the first number to
 * read: it says whether the new path is being taken at all. */
u32 gGcTrinHw;
u32 gGcTrinCpu;
/* And why a fallback happened: the matrix has no w variation at all (an
 * orthographic or billboard matrix), or it has one but clip.z is not an affine
 * function of clip.w. The two want completely different work, so counting them
 * apart is the difference between knowing and guessing. */
u32 gGcProjNoW;
/* Why a texture failed to reach the screen. A texture that simply does not
 * display leaves no other trace: the batch draws untextured and everything
 * upstream of it looks perfectly healthy. */
u32 gGcTexNullDim, gGcTexNullAddr, gGcTexNullAlloc;
u32 gGcTexHits, gGcTexConverts, gGcTexBytes, gGcTexAsks;
/* The screen-space extent of the widest CPU-fallback batch in a frame. A
 * fallback batch that covers most of the screen is the wedge. */
s32 gGcCpuBox[4];
u32 gGcCpuBoxMtx, gGcCpuBoxTris;
/* Whether billboarding was on for the widest fallback batch, and the anchor's
 * clip coordinates at the time, so a sprite's on-screen size can be checked
 * against the header's own formula: width_px = (sprite_units / w) * 160. */
u32 gGcCpuBoxBb;
s32 gGcCpuBoxAnchor[4];
u32 gGcProjNotAffine;

/* The first vertex batch of the frame pushed while billboarding is on: how it
 * was addressed and what the arithmetic actually produced. Reading this is the
 * difference between knowing why the sprites are wrong and guessing again.
 *   [0] count  [1] append flag  [2] base index  [3] sVertCount on entry */
u32 gGcBbBatch[4];
/* Anchor clip coords, the first sprite corner before the anchor is added, and
 * after. x, y, z, w each. */
f32 gGcBbAnchor[4];
f32 gGcBbPre[4];
f32 gGcBbPost[4];
u32 gGcBbSeen;
/* The matrix that transformed the anchor, and the billboard matrix, as they
 * were actually decoded. Scaled by 1000 and printed as integers because gc_log
 * has no float conversion. */
s32 gGcBbMtxScene[16];
s32 gGcBbMtxBb[16];
u32 gGcVtxLoaded;
u32 gGcVtxMaxCount;
/* Per matrix slot: vertices transformed through it, how many of those landed
 * behind the eye, how many matrices were loaded into it and how many times it
 * was selected. DKR keeps three slots and uses them for quite different
 * things, so a slot whose every vertex comes out behind the camera is a matrix
 * the port is reading wrong -- not a camera that happens to face away. */
u32 gGcVtxByMtx[NUM_MATRICES];
u32 gGcVtxBehindByMtx[NUM_MATRICES];
u32 gGcMtxLoads[NUM_MATRICES];
u32 gGcMtxSelects[NUM_MATRICES];
#endif

/* The segment table, populated by the display list through G_MOVEWORD. */
static u32 sSegments[NUM_SEGMENTS];

/*
 * Resolves an address out of a display list.
 *
 * DKR does not use segmented addressing for its assets, and says so:
 * src/set_rsp_segment.h notes that the custom microcode caps the segment table
 * at eight entries "a DMEM saving measure, since they don't use segments for
 * assets". The five segments it names are SEGMENT_MAIN, the framebuffer, the
 * z-buffer and two unused ones, and thread3_main.c:258 sets SEGMENT_MAIN --
 * segment 0 -- to `0 + K0BASE`. Every pointer in a DKR display list is
 * OS_K0_TO_PHYSICAL of a real pointer, which is exactly `p - 0x80000000`, and
 * segment 0 puts the 0x80000000 back. The framebuffer and z-buffer segments
 * are consumed by G_SETCIMG and G_SETZIMG, which this port does not implement
 * because it renders into the EFB.
 *
 * So the whole address is the offset, and splitting the top byte off is wrong
 * here even though it is what the RSP did.
 *
 * On the N64 that split is lossless: RDRAM is 4 MB, so every physical address
 * fits in the 24 bits the offset field has, and no pointer ever reaches the
 * segment field. It is not lossless on the GameCube. This DOL carries the
 * game's assets in .rodata, twelve megabytes of it, so gMainMemoryPool is
 * linked at 0x80d878c0 and its four megabytes run to 0x811878c0 -- physical
 * 0x011878c0, which needs twenty-five bits. Everything the game allocated
 * above 0x81000000 therefore had a top byte of 0x01: the old code read it as
 * segment 1, added the framebuffer's base, and dropped bit 24 with the mask.
 * Textures, vertices and triangle batches in the top 1.5 MB of the pool all
 * resolved to an address in the framebuffer instead of to themselves, which is
 * why some of them came back as noise and others were dropped by the bounds
 * checks downstream. Nothing about it was visible on the N64.
 */
static void *segmented_to_virtual(u32 addr) {
    /*
     * A pointer the game left absolute still resolves: once segment 0 carries
     * K0BASE, 0x80000000 + 0x80d878c0 wraps to 0x00d878c0 in thirty-two bits
     * and the physical-to-virtual step below puts the top bit back. That is
     * what the old code's special case for a high top byte was reaching for.
     */
    u32 resolved = sSegments[0] + addr;

    /*
     * Physical to cached virtual. The RSP read RDRAM by physical address, so
     * the pointers the game hands the microcode -- a G_DMADL target, a vertex
     * batch -- have had the 0x80000000 taken off them. The CPU cannot use
     * those directly on either machine; libultra's own osPhysicalToVirtual put
     * it back, and so does this. MEM1 sits at the same base as RDRAM did,
     * which is why nothing more than an OR is needed.
     */
    if (resolved < 0x80000000) {
        resolved |= 0x80000000;
    }

    return (void *) resolved;
}

/*
 * Whether a resolved display-list pointer can be walked.
 *
 * MEM1 is 24 MB at 0x80000000, and commands are eight bytes and aligned. The
 * check exists to catch a jump through an unset segment, which is the common
 * shape of the failure and which otherwise walks off into zeroed memory.
 */
static BOOL dl_addr_ok(const void *p) {
    u32 a = (u32) p;

    return a >= 0x80003000 && a < 0x81800000 && (a & 7) == 0;
}

void gc_gfx_set_segment(unsigned int segment, unsigned int base) {
    if (segment < NUM_SEGMENTS) {
        sSegments[segment] = base;
    }
}

/* ---- texture conversion and cache ---------------------------------------- *
 *
 * Everything is converted to GX_TF_RGBA8. The N64 formats in play would each
 * map to a narrower GX format, and one day should, but a single 32-bit target
 * means one tiler to be right about instead of eight. GX stores RGBA8 in 4x4
 * blocks of 64 bytes: sixteen AR pairs, then sixteen GB pairs, which is why
 * the copy below cannot be a memcpy however matched the endianness is.
 */

/*
 * The cache has to be big enough that no entry is evicted while the frame that
 * used it is still being drawn.
 *
 * That is not a performance concern, it is the correctness one. The CPU runs
 * ahead of the GP through the FIFO, so a batch is submitted long before its
 * texels are read. Round-robin eviction with sixty-four entries against the
 * ninety-odd distinct textures a frame asks for meant slots were reused twice
 * within a single frame: the buffer was freed, reallocated and overwritten
 * with a different texture while the GP was still drawing the batch that
 * pointed at it. Measured on the character select screen, which reported
 * "tex asks 243 = hits 153 + converts 90" -- ninety conversions into sixty-four
 * slots. The result is textures that simply do not appear, or appear wearing
 * another surface's texels, with every stage upstream of them correct.
 *
 * Two hundred and fifty-six covers the busiest frame measured with room to
 * spare, at roughly a megabyte of converted texels.
 */
#define TEX_CACHE_SIZE 256
#define TEX_MAX_DIM 512

typedef struct {
    u32 addr;
    u32 fmt, siz;
    u32 width, height;
    u32 stride;
    BOOL swapOdd;
    u32 tlutAddr;
    u32 hash; /* of the source texels: the pool reuses addresses */
    /* Sampler state, kept because GX bakes it into the texture object: the
     * same texels can legitimately be drawn clamped in one place and repeated
     * in another. */
    u8 wrapS, wrapT, filt;
    u32 bytes;    /* what the current texture occupies */
    u32 capacity; /* what the buffer can hold, so it need not be reallocated */
    void *texels;
    GXTexObj obj;
    BOOL valid;
} TexCacheEntry;

static TexCacheEntry sTexCache[TEX_CACHE_SIZE];
static u32 sTexCacheNext;

/* Expands a 5-bit channel so that full scale comes out full scale. */
static u8 expand5(u32 v) {
    return (u8) ((v << 3) | (v >> 2));
}

static u8 expand4(u32 v) {
    return (u8) ((v << 4) | v);
}

/* One RGBA5551 word, the format both 16-bit colour images and the palette use. */
static void read_rgba16(const u8 *p, u8 *out) {
    u32 c = ((u32) p[0] << 8) | p[1];

    out[0] = expand5((c >> 11) & 0x1F);
    out[1] = expand5((c >> 6) & 0x1F);
    out[2] = expand5((c >> 1) & 0x1F);
    out[3] = (c & 1) ? 0xFF : 0x00;
}

/*
 * One source texel as straight RGBA8, whatever the N64 called it.
 *
 * `row` points at the start of the texel's row and `x` is its column, rather
 * than a flat index, because the byte offset inside the row is not a pure
 * function of x: see `swap` below.
 *
 * swap is the RDP's odd-line word exchange, and it is the whole reason this
 * takes a row rather than an index. The texture unit reads TMEM with bit 2 of
 * the address inverted on odd rows -- the two 32-bit halves of every 64-bit
 * word are exchanged. gDPLoadBlock normally cancels that by performing the
 * same exchange as it writes, which is what its dxt parameter drives; with
 * dxt = 0 it writes verbatim and the read's exchange stands, so the image in
 * DRAM has to be stored already exchanged. That is precisely what DKR's
 * "interlaced" textures are: material_init (src/textures_sprites.c:1505)
 * picks the gDPLoadTextureBlockS macros, whose only difference is dxt = 0,
 * for every texture whose header carries RENDER_LINE_SWAP -- which the asset
 * spec says is every non-power-of-two texture that is not on the HUD.
 *
 * Reading such an image linearly gives every odd row with its 4-byte groups
 * transposed, which is a fine hatch across the texture. Applying the exchange
 * here is what the RDP's own read does.
 */
static void read_texel(const u8 *row, u32 fmt, u32 siz, u32 x, BOOL swap, const u8 *tlut,
                       u32 tlutFmt, u8 *out) {
    u32 off;
    u32 nib = 0;

    /* The byte the texel starts at, before the exchange. Four-bit formats
     * carry their nibble separately because two of them share a byte. */
    switch (siz) {
        case 0:
            off = x >> 1;
            nib = x & 1;
            break;
        case 1:
            off = x;
            break;
        case 2:
            off = x * 2;
            break;
        default:
            off = x * 4;
            break;
    }
    if (swap) {
        off ^= 4;
    }

    switch ((fmt << 2) | siz) {
        case (0 << 2) | 2: /* RGBA5551 */
            read_rgba16(row + off, out);
            break;

        case (0 << 2) | 3: /* RGBA8888 */
            out[0] = row[off + 0];
            out[1] = row[off + 1];
            out[2] = row[off + 2];
            out[3] = row[off + 3];
            break;

        case (2 << 2) | 0:   /* CI4 */
        case (2 << 2) | 1: { /* CI8 */
            u32 index;

            if (tlut == NULL) {
                /* A colour-index texture whose palette never arrived. Magenta
                 * rather than whatever the indices happen to look like as
                 * intensities, so it is obvious that the palette is the thing
                 * that went missing. */
                out[0] = 0xFF;
                out[1] = 0x00;
                out[2] = 0xFF;
                out[3] = 0xFF;
                break;
            }
            if (siz == 0) {
                index = nib ? (row[off] & 0xF) : (row[off] >> 4);
            } else {
                index = row[off];
            }
            if (tlutFmt == 3) { /* G_TT_IA16 */
                out[0] = out[1] = out[2] = tlut[index * 2];
                out[3] = tlut[index * 2 + 1];
            } else {
                read_rgba16(tlut + index * 2, out);
            }
            break;
        }

        case (3 << 2) | 2: /* IA16: eight bits of intensity, eight of alpha */
            out[0] = out[1] = out[2] = row[off];
            out[3] = row[off + 1];
            break;

        case (3 << 2) | 1: { /* IA8: four and four */
            u8 b = row[off];

            out[0] = out[1] = out[2] = expand4(b >> 4);
            out[3] = expand4(b & 0xF);
            break;
        }

        case (3 << 2) | 0: { /* IA4: three bits of intensity, one of alpha */
            u8 b = nib ? (row[off] & 0xF) : (row[off] >> 4);
            u8 i = (u8) ((b >> 1) & 7);

            out[0] = out[1] = out[2] = (u8) ((i << 5) | (i << 2) | (i >> 1));
            out[3] = (b & 1) ? 0xFF : 0x00;
            break;
        }

        case (4 << 2) | 1: /* I8 */
            out[0] = out[1] = out[2] = out[3] = row[off];
            break;

        case (4 << 2) | 0: { /* I4 */
            u8 b = nib ? (row[off] & 0xF) : (row[off] >> 4);

            out[0] = out[1] = out[2] = out[3] = expand4(b);
            break;
        }

        default:
            /* An unhandled format draws as opaque magenta rather than as
             * whatever was in the buffer, so it is obvious which one to write
             * next instead of being a subtle discolouration. */
            out[0] = 0xFF;
            out[1] = 0x00;
            out[2] = 0xFF;
            out[3] = 0xFF;
            break;
    }
}

/* How many bytes one row of a packed image of this width occupies. Only the
 * fallback for a tile that carries no line: the real stride is the tile's. */
static u32 row_bytes(u32 siz, u32 width) {
    switch (siz) {
        case 0:
            return (width + 1) / 2;
        case 1:
            return width;
        case 2:
            return width * 2;
        default:
            return width * 4;
    }
}

/* A cheap fingerprint of the source texels. The game decompresses textures
 * into pool memory it reuses, so an address alone does not identify one. */
static u32 texel_hash(const u8 *src, u32 bytes) {
    u32 h = 2166136261u;
    u32 i;
    u32 step = (bytes > 256) ? (bytes / 64) : 1;

    for (i = 0; i < bytes; i += step) {
        h = (h ^ src[i]) * 16777619u;
    }
    return h ^ bytes;
}

/*
 * Bring-up mire, behind GC_TEXTEST.
 *
 * Replaces every texture with a pattern that encodes its own coordinates:
 * red rises left to right, green rises top to bottom, and the blue channel
 * carries a four-texel checker so the 4x4 tiling is visible. Alpha is opaque.
 *
 * It separates the three things that look identical on screen. A clean
 * two-axis gradient means the conversion, the upload and the coordinates are
 * all correct and the fault is in the texels being fed in. Blocky or repeated
 * gradients mean the 4x4 tiling or the stride is wrong. A flat colour means
 * the wrong texture object is bound. A gradient running the wrong way means
 * the coordinates are transposed or mirrored.
 */
#ifndef GC_TEXTEST
#define GC_TEXTEST 0
#endif

/*
 * Paints every triangle that took the CPU-divide fallback solid magenta, and
 * every hardware-projected one solid green. ref-sm64gc carries the same knob
 * as GFX_GX_DEBUG_PROJ_TINT and its legend is the reason: the two paths fail
 * in different ways and produce defects that look alike on screen, so the only
 * cheap way to attribute a wrong-looking area is to colour it by the path that
 * drew it.
 */
#ifndef GC_PROJ_TINT
#define GC_PROJ_TINT 0
#endif

static void convert_test_pattern(u32 width, u32 height, u8 *dst) {
    u32 blocksW = (width + 3) / 4;
    u32 by, bx, y, x;

    for (by = 0; by * 4 < height; by++) {
        for (bx = 0; bx < blocksW; bx++) {
            u8 *block = dst + ((by * blocksW) + bx) * 64;

            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u32 sx = bx * 4 + x;
                    u32 sy = by * 4 + y;
                    u32 i = y * 4 + x;
                    u8 r = (u8) (width > 1 ? (sx * 255) / (width - 1) : 0);
                    u8 g = (u8) (height > 1 ? (sy * 255) / (height - 1) : 0);
                    u8 b = (u8) ((((sx >> 2) ^ (sy >> 2)) & 1) ? 0xC0 : 0x20);

                    block[i * 2 + 0] = 0xFF;
                    block[i * 2 + 1] = r;
                    block[32 + i * 2 + 0] = g;
                    block[32 + i * 2 + 1] = b;
                }
            }
        }
    }
}

static void convert_to_rgba8(const u8 *src, u32 fmt, u32 siz, u32 width, u32 height, u32 stride,
                             BOOL swapOdd, const u8 *tlut, u32 tlutFmt, u8 *dst) {
    u32 blocksW = (width + 3) / 4;
    u32 by, bx, y, x;

#ifdef GC_DEBUG
    /* The two ways read_texel paints magenta, counted once per texture rather
     * than once per texel: an (fmt, siz) pair it has no case for, and a
     * colour-index texture with no palette. */
    switch ((fmt << 2) | siz) {
        case (0 << 2) | 2:
        case (0 << 2) | 3:
        case (3 << 2) | 0:
        case (3 << 2) | 1:
        case (3 << 2) | 2:
        case (4 << 2) | 0:
        case (4 << 2) | 1:
            break;
        case (2 << 2) | 0:
        case (2 << 2) | 1:
            if (tlut == NULL) {
                gGcTexNoTlut++;
            }
            break;
        default:
            gGcTexUnhandled++;
            gGcTexUnhandledFmtSiz = (fmt << 2) | siz;
            break;
    }
#endif

    for (by = 0; by * 4 < height; by++) {
        for (bx = 0; bx < blocksW; bx++) {
            u8 *block = dst + ((by * blocksW) + bx) * 64;

            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u32 sx = bx * 4 + x;
                    u32 sy = by * 4 + y;
                    u32 i = y * 4 + x;
                    u8 rgba[4] = { 0, 0, 0, 0 };

                    /*
                     * GX stores 4x4 blocks, so a texture whose size is not a
                     * multiple of four has texels past its edge. Repeat the
                     * edge rather than leaving them zero: zero is transparent
                     * black, and bilinear filtering pulls it into the last row
                     * and column as a dark fringe. (ref-sm64gc's swizzle
                     * carries the same note.)
                     */
                    if (sx >= width) {
                        sx = width - 1;
                    }
                    if (sy >= height) {
                        sy = height - 1;
                    }
                    read_texel(src + sy * stride, fmt, siz, sx, swapOdd && (sy & 1) != 0, tlut,
                               tlutFmt, rgba);

                    /* AR in the first half of the block, GB in the second. */
                    block[i * 2 + 0] = rgba[3];
                    block[i * 2 + 1] = rgba[0];
                    block[32 + i * 2 + 0] = rgba[1];
                    block[32 + i * 2 + 1] = rgba[2];
                }
            }
        }
    }
}

/*
 * The texture filter the list asked for.
 *
 * G_MDSFT_TEXTFILT is bit 12 of the high other-mode word, two bits wide:
 * G_TF_POINT is 0, G_TF_BILERP is 2 and G_TF_AVERAGE is 3 (include/PR/gbi.h).
 * DKR does ask -- gsDPSetTextureFilter(G_TF_BILERP) is in rcp_dkr.c, menu.c,
 * printf.c, weather.c and fade_transition.c -- and everything here used to be
 * sampled GX_NEAR anyway.
 */
static u8 tex_filter(void) {
    return ((sOtherModeH >> 12) & 3) != 0 ? GX_LINEAR : GX_NEAR;
}

/* G_MDSFT_TEXTLUT, bit 14, two bits: G_TT_NONE 0, G_TT_RGBA16 2, G_TT_IA16 3. */
static u32 tlut_fmt(void) {
    return (sOtherModeH >> 14) & 3;
}

/*
 * A tile's shift, as a multiplier on a texture coordinate.
 *
 * G_SETTILE's shifts and shiftt are a signed exponent in four bits: 0 to 10
 * shift the coordinate right by that many bits, 11 to 15 shift it left by
 * sixteen minus that. The RDP applies the shift before subtracting the tile's
 * origin, which is the order tex_coord below uses.
 */
static f32 tile_shift_scale(u32 shift) {
    if (shift == 0) {
        return 1.0f;
    }
    if (shift <= 10) {
        return 1.0f / (f32) (1u << shift);
    }
    return (f32) (1u << (16 - shift));
}

/*
 * N64 clamp/mirror/wrap flags to GX. Clamp wins over mirror, which is the
 * precedence gfx_opengl.c and ref-sm64gc's gfx_gx_cm_to_gx both use.
 */
static u8 cm_to_gx(u32 cm) {
    if (cm & 2) { /* G_TX_CLAMP */
        return GX_CLAMP;
    }
    return (cm & 1) ? GX_MIRROR : GX_REPEAT; /* G_TX_MIRROR */
}

static GXTexObj *texture_get(u32 addr, u32 fmt, u32 siz, u32 width, u32 height, u32 stride,
                             BOOL swapOdd, u32 tlutAddr, u8 wrapS, u8 wrapT, u8 filt) {
    const u8 *src = (const u8 *) addr;
    const u8 *tlut = NULL;
    u32 srcBytes = stride * height;
    u32 tlutBytes = (siz == 0) ? 16 * 2 : 256 * 2;
    u32 hash;
    u32 i;
    TexCacheEntry *e;
    u32 padW, padH, bytes;

#ifdef GC_DEBUG
    gGcTexAsks++;
#endif
    if (width == 0 || height == 0 || width > TEX_MAX_DIM || height > TEX_MAX_DIM) {
#ifdef GC_DEBUG
        gGcTexNullDim++;
#endif
        return NULL;
    }
    if (addr < 0x80003000 || addr + srcBytes >= 0x81800000) {
#ifdef GC_DEBUG
        gGcTexNullAddr++;
#endif
        return NULL;
    }
    if (fmt == 2) { /* G_IM_FMT_CI */
        if (tlutAddr < 0x80003000 || tlutAddr + tlutBytes >= 0x81800000) {
            tlutAddr = 0;
        } else {
            tlut = (const u8 *) tlutAddr;
        }
    } else {
        tlutAddr = 0;
    }

    hash = texel_hash(src, srcBytes);
    if (tlut != NULL) {
        hash ^= texel_hash(tlut, tlutBytes) * 31u;
    }

    for (i = 0; i < TEX_CACHE_SIZE; i++) {
        e = &sTexCache[i];
        if (e->valid && e->addr == addr && e->hash == hash && e->fmt == fmt && e->siz == siz &&
            e->width == width && e->height == height && e->stride == stride &&
            e->swapOdd == swapOdd && e->tlutAddr == tlutAddr) {
            /* The texels match; only the sampler state might not. GX bakes
             * wrap and filter into the object, so re-describe it in place
             * rather than converting the same texels a second time. */
            if (e->wrapS != wrapS || e->wrapT != wrapT || e->filt != filt) {
                GX_InitTexObj(&e->obj, e->texels, (u16) width, (u16) height, GX_TF_RGBA8, wrapS,
                              wrapT, GX_FALSE);
                GX_InitTexObjFilterMode(&e->obj, filt, filt);
                e->wrapS = wrapS;
                e->wrapT = wrapT;
                e->filt = filt;
            }
#ifdef GC_DEBUG
            gGcTexHits++;
#endif
            return &e->obj;
        }
    }

    /* Miss. Round-robin eviction: a frame's textures are drawn once each, so
     * recency carries no information worth the bookkeeping. */
    e = &sTexCache[sTexCacheNext];
    sTexCacheNext = (sTexCacheNext + 1) % TEX_CACHE_SIZE;

    padW = (width + 3) & ~3u;
    padH = (height + 3) & ~3u;
    bytes = padW * padH * 4;

    /*
     * Reuse the buffer when it is already large enough. Freeing and
     * reallocating a different size on every miss fragments libogc's arena for
     * no gain -- the entry is about to be overwritten either way.
     */
    if (e->texels != NULL && e->capacity < bytes) {
        free(e->texels);
        e->texels = NULL;
        e->capacity = 0;
    }
    if (e->texels == NULL) {
        e->texels = memalign(32, bytes);
        e->capacity = (e->texels != NULL) ? bytes : 0;
    }
    if (e->texels == NULL) {
#ifdef GC_DEBUG
        gGcTexNullAlloc++;
        gGcTexBytes -= e->valid ? e->bytes : 0;
        e->bytes = 0;
#endif
        e->valid = FALSE;
        return NULL;
    }
#ifdef GC_DEBUG
    gGcTexConverts++;
    gGcTexBytes += bytes - (e->valid ? e->bytes : 0);
    e->bytes = bytes;
#endif

    if (GC_TEXTEST) {
        convert_test_pattern(width, height, (u8 *) e->texels);
    } else {
        convert_to_rgba8(src, fmt, siz, width, height, stride, swapOdd, tlut, tlut_fmt(),
                         (u8 *) e->texels);
    }
    /*
     * Two caches sit between these bytes and the texture unit, and both have
     * to be told.
     *
     * DCFlushRange pushes the CPU's copy out to main memory, which is the
     * obvious half. The other is the GP's own texture cache, and it is the one
     * that was missing: this pool recycles its buffers round-robin every
     * frame, so a converted texture very often lands at an address the GP has
     * already cached from a previous texture. Nothing in GX notices that the
     * memory changed, so the GP goes on serving whatever it cached -- the
     * right geometry, the right combiner, the right coordinates, and the
     * texels of something else. It is invisible to every check upstream,
     * because everything upstream is correct.
     *
     * ref-sm64gc calls GX_InvalidateTexAll once per frame in
     * gfx_gx_start_frame for exactly this. Here it goes with the write that
     * makes the cache stale.
     */
    DCFlushRange(e->texels, bytes);
    GX_InvalidateTexAll();

    GX_InitTexObj(&e->obj, e->texels, (u16) width, (u16) height, GX_TF_RGBA8, wrapS, wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&e->obj, filt, filt);

    e->addr = addr;
    e->fmt = fmt;
    e->siz = siz;
    e->width = width;
    e->height = height;
    e->stride = stride;
    e->swapOdd = swapOdd;
    e->tlutAddr = tlutAddr;
    e->hash = hash;
    e->wrapS = wrapS;
    e->wrapT = wrapT;
    e->filt = filt;
    e->valid = TRUE;
    return &e->obj;
}

/*
 * Everything a tile needs before it can be turned into a GX texture object:
 * where its texels are, how wide a row of them is, whether the odd rows are
 * exchanged, and which palette a colour-index tile reads.
 *
 * Returns FALSE when nothing has been loaded into the tile's corner of TMEM,
 * which is the honest answer for a list that draws before it loads.
 */
typedef struct {
    u32 addr;
    u32 width, height;
    u32 stride;
    BOOL swapOdd;
    u32 tlutAddr;
} TileImage;

static BOOL tile_image(const TileDesc *t, TileImage *out) {
    const TmemLoad *ld = tmem_find(t->tmem);

    if (ld == NULL) {
        return FALSE;
    }

    out->addr = ld->addr;
    out->width = ((t->lrs - t->uls) >> 2) + 1;
    out->height = ((t->lrt - t->ult) >> 2) + 1;

    /*
     * The row stride is the tile's, not the width's.
     *
     * gDPLoadTextureBlock sets line to ((width * LINE_BYTES) + 7) >> 3, in
     * 64-bit words, and the block load copies DRAM into TMEM verbatim -- so
     * the rows in DRAM are padded to that same multiple of eight bytes.
     * Assuming a packed width * bytes-per-texel row shears every four-bit
     * texture whose width is not a multiple of sixteen.
     *
     * Thirty-two-bit textures are the exception: their LINE_BYTES is 2 rather
     * than 4, because the RDP splits them across the two halves of TMEM, so
     * the tile's line describes half a row. In DRAM the row is width * 4.
     */
    if (t->siz == 3) {
        out->stride = out->width * 4;
    } else if (t->line != 0) {
        out->stride = t->line * 8;
    } else {
        out->stride = row_bytes(t->siz, out->width);
    }

    /*
     * The odd-row exchange, skipped for 32-bit textures: their block load
     * runs over half-rows in each half of TMEM, so the exchange does not
     * reduce to inverting bit 2 of a DRAM offset the way it does for every
     * other size, and inventing an answer would be worse than leaving the
     * few that exist alone.
     *
     * It also needs the tile's line: inverting bit 2 only stays inside the row
     * when the row is a whole number of 64-bit words, which is what line
     * guarantees and the packed fallback does not.
     */
    out->swapOdd = ld->swapOdd && t->siz != 3 && t->line != 0;

    /*
     * The palette, for a colour-index tile.
     *
     * It does not arrive as G_LOADTLUT. gDPLoadTLUT_pal16 is defined twice in
     * gbi.h and the later definition wins (include/PR/gbi.h:3360): it is a
     * plain _gDPLoadTextureBlock into TMEM word 256 + pal * 16. So the palette
     * is found the same way as any other image -- by asking which load landed
     * at that TMEM address. A 256-entry palette (gDPLoadTLUT_pal256) lands at
     * 256 with the tile's palette field unused.
     */
    out->tlutAddr = 0;
    if (t->fmt == 2) {
        const TmemLoad *pal =
            tmem_find(t->siz == 0 ? TMEM_TLUT_BASE + t->palette * 16 : TMEM_TLUT_BASE);

        if (pal == NULL) {
            pal = tmem_find(TMEM_TLUT_BASE);
        }
        if (pal != NULL) {
            out->tlutAddr = pal->addr;
        }
    }
    return TRUE;
}

#ifdef GC_DEBUG
/*
 * What a textured batch actually resolved to, one entry per distinct tile
 * configuration in a frame.
 *
 * "The textures are wrong" has a dozen possible causes that all look the same
 * on screen -- the wrong address, the wrong stride, the wrong format, the
 * wrong size, the odd-row exchange applied when it should not be. Every one of
 * them is a number, and this prints the numbers so the answer stops being a
 * matter of opinion.
 */
#define GC_TEXDBG_MAX 8

typedef struct {
    u32 fmt, siz, line, tmem, palette;
    u32 uls, ult, lrs, lrt;
    u32 cms, cmt, masks, maskt, shifts, shiftt;
    u32 addr, width, height, stride, swapOdd, ok;
    u32 head[4]; /* the first sixteen source bytes */
} GcTexDbg;

u32 gGcTexDbgCount;
GcTexDbg gGcTexDbg[GC_TEXDBG_MAX];

/* Distinct render states a textured or untextured batch was drawn with. The
 * combiner and the render mode are the two stages this file gained today and
 * the two it has no other way to check. */
#define GC_STATEDBG_MAX 10

u32 gGcTexRawValid, gGcTexRawW, gGcTexRawH, gGcTexRawStride, gGcTexRawFmt, gGcTexRawAddr;
u8 gGcTexRaw[2048];

u32 gGcStateDbgCount;
u32 gGcStateDbg[GC_STATEDBG_MAX][5]; /* omH, omL, cc0, cc1, zcmp|zupd<<1|blend<<2 */

/*
 * Record one emitted primitive's screen coverage.
 *
 * Coordinates are per-mille of the screen: 0 is left or top, 1000 is right or
 * bottom, whatever space the primitive itself was in. Area is the product over
 * a thousand, so a full-screen quad reads 1000.
 */
static void cover_note(u32 kind, s32 x0, s32 y0, s32 x1, s32 y1, u32 col) {
    s32 area;

    gGcCoverTotal++;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > 1000) { x1 = 1000; }
    if (y1 > 1000) { y1 = 1000; }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    area = ((x1 - x0) * (y1 - y0)) / 1000;
    if ((u32) area < gGcCoverArea) {
        return;
    }
    gGcCoverArea = (u32) area;
    gGcCoverKind = kind;
    gGcCoverBox[0] = x0;
    gGcCoverBox[1] = y0;
    gGcCoverBox[2] = x1;
    gGcCoverBox[3] = y1;
    gGcCoverSeq = gGcCoverTotal;
    gGcCoverCc[0] = gGcCombineW0;
    gGcCoverCc[1] = gGcCombineW1;
    gGcCoverOml = sOtherModeL;
    gGcCoverOmh = sOtherModeH;
    gGcCoverTex = sCoverTexAddr;
    gGcCoverFmtSiz = sCoverFmtSiz;
    gGcCoverCol = col;
    gGcCoverPrim = ((u32) sPrimColor.r << 24) | ((u32) sPrimColor.g << 16) |
                   ((u32) sPrimColor.b << 8) | sPrimColor.a;
    gGcCoverEnv = ((u32) sEnvColor.r << 24) | ((u32) sEnvColor.g << 16) |
                  ((u32) sEnvColor.b << 8) | sEnvColor.a;
}

static void statedbg_note(u32 cc0, u32 cc1, u32 flags) {
    u32 i;

    for (i = 0; i < gGcStateDbgCount; i++) {
        if (gGcStateDbg[i][0] == sOtherModeH && gGcStateDbg[i][1] == sOtherModeL &&
            gGcStateDbg[i][2] == cc0 && gGcStateDbg[i][3] == cc1) {
            return;
        }
    }
    if (gGcStateDbgCount >= GC_STATEDBG_MAX) {
        return;
    }
    gGcStateDbg[gGcStateDbgCount][0] = sOtherModeH;
    gGcStateDbg[gGcStateDbgCount][1] = sOtherModeL;
    gGcStateDbg[gGcStateDbgCount][2] = cc0;
    gGcStateDbg[gGcStateDbgCount][3] = cc1;
    gGcStateDbg[gGcStateDbgCount][4] = flags;
    gGcStateDbgCount++;
}

static void texdbg_note(const TileDesc *t, const TileImage *img, BOOL ok) {
    u32 i;
    GcTexDbg *e;

    for (i = 0; i < gGcTexDbgCount; i++) {
        e = &gGcTexDbg[i];
        if (e->fmt == t->fmt && e->siz == t->siz && e->line == t->line &&
            e->addr == (ok ? img->addr : 0) && e->lrs == t->lrs && e->lrt == t->lrt) {
            return;
        }
    }
    if (gGcTexDbgCount >= GC_TEXDBG_MAX) {
        return;
    }
    e = &gGcTexDbg[gGcTexDbgCount++];
    e->fmt = t->fmt;
    e->siz = t->siz;
    e->line = t->line;
    e->tmem = t->tmem;
    e->palette = t->palette;
    e->uls = t->uls;
    e->ult = t->ult;
    e->lrs = t->lrs;
    e->lrt = t->lrt;
    e->cms = t->cms;
    e->cmt = t->cmt;
    e->masks = t->masks;
    e->maskt = t->maskt;
    e->shifts = t->shifts;
    e->shiftt = t->shiftt;
    e->ok = ok;
    e->addr = ok ? img->addr : 0;
    e->width = ok ? img->width : 0;
    e->height = ok ? img->height : 0;
    e->stride = ok ? img->stride : 0;
    e->swapOdd = ok ? (u32) img->swapOdd : 0;
    e->head[0] = e->head[1] = e->head[2] = e->head[3] = 0;
    /* The first texture small enough to print whole gets dumped verbatim, so
     * the conversion can be checked off the machine instead of by eye. */
    if (ok && !gGcTexRawValid && img->width * img->height * 2 <= sizeof(gGcTexRaw) &&
        img->stride * img->height <= sizeof(gGcTexRaw) && t->siz == 2 && img->width <= 32 &&
        img->height <= 32 && img->addr >= 0x80003000 &&
        img->addr + img->stride * img->height < 0x81800000) {
        gGcTexRawW = img->width;
        gGcTexRawH = img->height;
        gGcTexRawStride = img->stride;
        gGcTexRawFmt = (t->fmt << 2) | t->siz;
        gGcTexRawAddr = img->addr;
        memcpy(gGcTexRaw, (const void *) img->addr, img->stride * img->height);
        gGcTexRawValid = 1;
    }
    if (ok && img->addr >= 0x80003000 && img->addr + 16 < 0x81800000) {
        const u32 *w = (const u32 *) img->addr;

        e->head[0] = w[0];
        e->head[1] = w[1];
        e->head[2] = w[2];
        e->head[3] = w[3];
    }
}
#endif

/*
 * The render state every 2D primitive shares.
 *
 * Flat colour straight from the vertex, no texture, no lighting, no depth.
 * The RDP's fill and copy cycles do not test or write Z either, so this
 * matches rather than approximates. It is re-applied per list because the
 * textured path, when it arrives, will change all of it.
 */
static void gfx_set_2d_state(void) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE,
                   GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    /* Both of these can have been left on by apply_render_mode, and a fill
     * rectangle is not drawn through the combiner at all -- the RDP's fill
     * cycle bypasses it, and so does this path. */
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
}

/*
 * An orthographic projection, written out rather than taken from libogc's gu.
 *
 * Calling guOrtho would pull gu.o into the link, and gu.o also defines
 * guPerspective -- which libultra's own gu/perspective.c already provides for
 * the game, with different semantics (an N64 fixed-point Mtx and a perspNorm
 * out-parameter). The two cannot coexist, and the game's is the one that has
 * to win, so the four lines that matter are here instead.
 *
 * `t` above `b` is deliberate: it flips the y axis so that y grows downwards,
 * which is the direction every coordinate in a display list is written in.
 */
static void gfx_ortho(Mtx44 mt, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    memset(mt, 0, sizeof(Mtx44));

    mt[0][0] = 2.0f / (r - l);
    mt[0][3] = -(r + l) / (r - l);
    mt[1][1] = 2.0f / (t - b);
    mt[1][3] = -(t + b) / (t - b);
    mt[2][2] = -1.0f / (f - n);
    mt[2][3] = -f / (f - n);
    mt[3][3] = 1.0f;
}

/*
 * Points GX at the game's screen space.
 *
 * The projection is set up in the game's own units and the viewport covers the
 * whole EFB, so a 320-wide list fills a 640-wide frame without any coordinate
 * being scaled on the way through.
 */
/*
 * Which projection GX is holding.
 *
 * Interface rectangles and transformed geometry need different ones, and a
 * display list interleaves them freely, so the projection is switched lazily
 * per primitive rather than once per list. Only the projection and viewport
 * move; the scissor belongs to the list (G_SETSCISSOR) and must not be
 * disturbed, which is why this is separate from gfx_set_2d_projection.
 */
typedef enum { PROJ_NONE, PROJ_2D, PROJ_3D } ProjMode;

static ProjMode sProjMode = PROJ_NONE;

static void load_2d_projection(void) {
    GXRModeObj *rmode = gc_video_mode();
    Mtx44 proj;

    if (sProjMode == PROJ_2D) {
        return;
    }

    /* Game space straight through: x 0..320, y 0..240 downwards, and a depth
     * range wide enough that the flat interface path is never clipped. */
    gfx_ortho(proj, 0.0f, sGameHeight, 0.0f, sGameWidth, -1.0f, 0.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_SetViewport(0.0f, 0.0f, (f32) rmode->fbWidth, (f32) rmode->efbHeight, 0.0f, 1.0f);
    sProjMode = PROJ_2D;
}

/*
 * The hardware projection for transformed geometry.
 *
 * This is the whole reason the geometry path stopped dividing on the CPU. GX
 * can only be handed three position components, so the fourth -- w -- has to
 * come out of the projection matrix, and GX's perspective form is not a
 * general 4x4: the hardware keeps six coefficients, so clip.z can only be an
 * affine function of clip.w.
 *
 * For DKR's matrices it is one, exactly, and that is provable rather than
 * fitted. guPerspectiveF (libultra/src/gu/perspective.c) writes
 *
 *     mf[2][2] = (n+f)/(n-f)    mf[2][3] = -1
 *     mf[3][2] = 2nf/(n-f)      mf[3][3] = 0
 *
 * With row vectors (clip = v * M) column j of the matrix produces clip
 * component j, and the modelview premultiplied onto it is affine -- its fourth
 * column is (0,0,0,1). Working the product out, column 2 of the MVP is
 * -(n+f)/(n-f) times column 3 for the first three rows, and the same multiple
 * plus 2nf/(n-f) on the fourth. So
 *
 *     clip.z = alpha * clip.w + beta
 *     alpha  = M[i][2] / M[i][3]   for any i in 0..2
 *     beta   = M[3][2] - alpha * M[3][3]
 *
 * and the two are read straight off the matrix the game handed us. This is the
 * step aluzed/sm64-port-gc has to do by least-squares fitting per batch
 * (gfx_gx_setup_perspective), because gfx_pc gives it coordinates that are
 * already projected and no matrix to read. We have the matrix.
 *
 * Depth conventions: substituting z = -n and z = -f above gives -1 and +1, so
 * the game's normalised z is the OpenGL range. GX's is -1 at the near plane
 * and 0 at the far one, so z_gx = (z_gl - 1) / 2, and since GX computes
 * clip.z' = P22 * z_view + P23 with z_view = -w,
 *
 *     P22 = (1 - alpha) / 2        P23 = beta / 2
 *
 * Returns FALSE when the relation does not hold -- an orthographic or
 * otherwise non-perspective matrix -- and the caller falls back to dividing on
 * the CPU, which is still correct, just affine.
 */
static BOOL load_3d_projection(void) {
    const f32 (*m)[4] = sMatrix[sCurMatrix];
    GXRModeObj *rmode = gc_video_mode();
    Mtx44 proj;
    f32 alpha = 0.0f, beta;
    f32 best = 0.0f;
    f32 x0, y0, w, h;
    s32 i;

    /* Pick the row with the largest |w| coefficient: any of the first three
     * gives the same alpha, but dividing by the largest is the one that does
     * not amplify rounding. */
    for (i = 0; i < 3; i++) {
        f32 d = m[i][3] < 0.0f ? -m[i][3] : m[i][3];

        if (d > best) {
            best = d;
            alpha = m[i][2] / m[i][3];
        }
    }
    if (best < 1e-9f) {
#ifdef GC_DEBUG
        gGcProjNoW++;
#endif
        return FALSE; /* w does not vary with position: not a perspective matrix */
    }

    /* The relation has to hold for all three rows, not just the one it was
     * taken from. If it does not, this is not a matrix of the expected shape
     * and guessing would be worse than the CPU path. */
    for (i = 0; i < 3; i++) {
        f32 want = alpha * m[i][3];
        f32 diff = m[i][2] - want;
        f32 mag = (want < 0.0f ? -want : want) + 1.0f;

        if (diff > 1e-3f * mag || diff < -1e-3f * mag) {
#ifdef GC_DEBUG
            gGcProjNotAffine++;
#endif
            return FALSE;
        }
    }
    beta = m[3][2] - alpha * m[3][3];

    memset(proj, 0, sizeof(Mtx44));
    proj[0][0] = 1.0f;  /* clip.x = the x we submit */
    proj[1][1] = 1.0f;  /* clip.y = the y we submit */
    /*
     * The decal bias rides in P22. GX computes clip.z = P22 * z_view + P23
     * with z_view = -w, so the normalised depth is -P22 + P23/w: adding to P22
     * subtracts from the depth, and here that is towards the viewer. Doing it
     * in the projection rather than per vertex keeps the hardware path and the
     * CPU fallback writing depths on the same scale -- the same reasoning as
     * ref-sm64gc's gfx_gx_load_persp(1 - p + bias, q).
     */
    proj[2][2] = (1.0f - alpha) * 0.5f + sDecalBias;
    proj[2][3] = beta * 0.5f;
    proj[3][2] = -1.0f; /* clip.w = -(the z we submit), so we submit -w */
    GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);

    /*
     * The viewport is now GX's job too, because the divide is. It comes from
     * the display list (G_MOVEMEM), in game-space units with the centre in
     * vtrans and the half-size in vscale, and is scaled here to EFB pixels.
     * Both GX and the N64 put normalised y = +1 at the top of the viewport, so
     * no flip is needed -- the software path's explicit negation was doing the
     * same job by hand.
     */
    x0 = (sVpTransX - sVpScaleX) * sScaleX;
    y0 = (sVpTransY - sVpScaleY) * sScaleY;
    w = 2.0f * sVpScaleX * sScaleX;
    h = 2.0f * sVpScaleY * sScaleY;
    if (w < 0.0f) {
        x0 += w;
        w = -w;
    }
    if (h < 0.0f) {
        y0 += h;
        h = -h;
    }
    GX_SetViewport(x0, y0, w, h, 0.0f, 1.0f);

    sProjMode = PROJ_3D;
    return TRUE;
}

static void gfx_set_2d_projection(void) {
    GXRModeObj *rmode = gc_video_mode();
    Mtx44 proj;
    Mtx identity;
    int w, h;

    gc_video_game_resolution(&w, &h);
    sGameWidth = (f32) w;
    sGameHeight = (f32) h;
    sScaleX = (f32) rmode->fbWidth / sGameWidth;
    sScaleY = (f32) rmode->efbHeight / sGameHeight;

    /*
     * Near and far are -1 and 0 rather than 0 and 1, which looks odd until you
     * work the formula: it makes the z row `clip.z = -z_in`, so a depth handed
     * in as 0..1 comes out as 0..-1. That is the span this same function's own
     * arithmetic produces for an ordinary orthographic box, so whichever end
     * of it GX treats as the near plane, the values land inside the range
     * instead of being clipped away. The alternative was to assume which
     * entries of a projection matrix GX reads and which way its normalised z
     * points, and the header documents neither.
     */
    memset(identity, 0, sizeof(Mtx));
    identity[0][0] = 1.0f;
    identity[1][1] = 1.0f;
    identity[2][2] = 1.0f;
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    sProjMode = PROJ_NONE;
    load_2d_projection();
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    (void) proj;
}

/* ---- the combiner and the render mode, as GX state ------------------------ */

/*
 * The N64 combiner computes  out = (a - b) * c + d.
 * A TEV stage computes       out = d (+/-) ((1 - c) * a + c * b),
 * which is a linear interpolation, not the same shape. Four cases cover it:
 *
 *   c is zero, or a == b   out = d              TEV(0, 0, 0, d)      1 stage
 *   d == b                 out = lerp(b, a, c)  TEV(b, a, c, 0)      1 stage
 *   b is zero              out = a*c + d        TEV(0, a, c, d)      1 stage
 *   otherwise              out = (a-b)*c + d    TEV(0, a, c, d)      2 stages
 *                                               TEV(0, b, c, prev) subtracting
 *
 * Every combiner DKR uses -- the seventeen G_CC_* modes at the top of
 * include/f3ddkr.h plus the stock ones its draw tables pull in -- lands in one
 * of the first three. The general case must leave its first stage unclamped:
 * d + c*a can legitimately exceed 1 before the second stage subtracts c*b, and
 * a TEV register is wide enough to carry the overshoot.
 *
 * This is ref-sm64gc's gfx_gx_plan_form with one case added. That backend gets
 * its formulas pre-classified by gfx_cc and never sees a bare mux word; here
 * the classification has to come from the decoded operands, which is why "b is
 * zero" is worth naming separately -- G_CC_MODULATEIA_PRIM and
 * G_CC_MODULATEIDECALA, the two modes most of DKR draws through, are exactly
 * that shape and would otherwise cost two stages each.
 */
typedef struct {
    u8 a[2], b[2], c[2], d[2];
    u8 prevD[2]; /* this stage's d operand is the previous stage's result */
    u8 op[2];
    u8 clamp[2];
    u32 count;
} TevForm;

static void tev_plan(const u8 v[4], TevForm *f) {
    u8 A = v[0], B = v[1], C = v[2], D = v[3];

    f->op[0] = f->op[1] = GX_TEV_ADD;
    f->clamp[0] = f->clamp[1] = GX_TRUE;
    f->prevD[0] = f->prevD[1] = FALSE;

    if (C == CC_ZERO || A == B) {
        f->a[0] = CC_ZERO; f->b[0] = CC_ZERO; f->c[0] = CC_ZERO; f->d[0] = D;
        f->count = 1;
    } else if (D == B) {
        f->a[0] = B; f->b[0] = A; f->c[0] = C; f->d[0] = CC_ZERO;
        f->count = 1;
    } else if (B == CC_ZERO) {
        f->a[0] = CC_ZERO; f->b[0] = A; f->c[0] = C; f->d[0] = D;
        f->count = 1;
    } else {
        f->a[0] = CC_ZERO; f->b[0] = A; f->c[0] = C; f->d[0] = D;
        f->clamp[0] = GX_FALSE;
        f->a[1] = CC_ZERO; f->b[1] = B; f->c[1] = C; f->d[1] = CC_ZERO;
        f->prevD[1] = TRUE;
        f->op[1] = GX_TEV_SUB;
        f->count = 2;
    }
}

/*
 * An operand as a GX colour input.
 *
 * `textured` matters because a G_TRIN batch can have its texture bit clear
 * while the combiner still names TEXEL0. The RDP would read whatever was left
 * in TMEM; there is no texture bound here at all, so the neutral value is one,
 * which turns a modulate into a pass-through of the other operand rather than
 * into black.
 *
 * TEXEL1 reads the same texture as TEXEL0. Two-texture combiners do exist in
 * DKR -- the water in G_CC_BLENDTEX_MODULATEA_1_PRIM, the blinking lights in
 * G_CC_BLENDTEX_PRIM -- but only one tile is ever converted and bound, so this
 * is an approximation, and a knowingly visible one.
 */
static u8 cc_colour(u8 item, BOOL textured, BOOL combinedInReg) {
    switch (item) {
        case CC_COMBINED:   return combinedInReg ? GX_CC_C2 : GX_CC_CPREV;
        case CC_COMBINED_A: return combinedInReg ? GX_CC_A2 : GX_CC_APREV;
        case CC_TEXEL0:
        case CC_TEXEL1:     return textured ? GX_CC_TEXC : GX_CC_ONE;
        case CC_TEXEL0_A:
        case CC_TEXEL1_A:   return textured ? GX_CC_TEXA : GX_CC_ONE;
        case CC_PRIM:       return GX_CC_C0;
        case CC_PRIM_A:     return GX_CC_A0;
        case CC_SHADE:      return GX_CC_RASC;
        case CC_SHADE_A:    return GX_CC_RASA;
        case CC_ENV:        return GX_CC_C1;
        case CC_ENV_A:      return GX_CC_A1;
        case CC_ONE:        return GX_CC_ONE;
        default:            return GX_CC_ZERO;
    }
}

/* The same operand as a GX alpha input. GX has no constant one here, so it is
 * borrowed from the konst selector, which tev_emit_cycle pins to 1.0. */
static u8 cc_alpha(u8 item, BOOL textured, BOOL combinedInReg) {
    switch (item) {
        case CC_COMBINED:
        case CC_COMBINED_A: return combinedInReg ? GX_CA_A2 : GX_CA_APREV;
        case CC_TEXEL0:
        case CC_TEXEL1:
        case CC_TEXEL0_A:
        case CC_TEXEL1_A:   return textured ? GX_CA_TEXA : GX_CA_KONST;
        case CC_PRIM:
        case CC_PRIM_A:     return GX_CA_A0;
        case CC_SHADE:
        case CC_SHADE_A:    return GX_CA_RASA;
        case CC_ENV:
        case CC_ENV_A:      return GX_CA_A1;
        case CC_ONE:        return GX_CA_KONST;
        default:            return GX_CA_ZERO;
    }
}

/*
 * One combiner cycle, as one or two TEV stages.
 *
 * Colour and alpha share stages, so the shorter of the two formulas is padded
 * with a pass-through of the previous result. Only the last stage of a cycle
 * writes `outReg`; the ones before it write TEVPREV, which is where the
 * general case's second stage looks for its first.
 *
 * Returns the next free stage number.
 */
static u32 tev_emit_cycle(u32 stage, const u8 col[4], const u8 alp[4], BOOL textured,
                          BOOL combinedInReg, u8 outReg) {
    TevForm cf, af;
    u32 n, i;

    tev_plan(col, &cf);
    tev_plan(alp, &af);
    n = cf.count > af.count ? cf.count : af.count;

    for (i = 0; i < n; i++) {
        u8 st = (u8) (GX_TEVSTAGE0 + stage + i);
        u8 out = (i + 1 == n) ? outReg : GX_TEVPREV;

        /* A stage that reads no texture must be given the null coordinate and
         * the null map, or it samples whatever was last bound. */
        GX_SetTevOrder(st, textured ? GX_TEXCOORD0 : GX_TEXCOORDNULL,
                       textured ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLOR0A0);

        if (i < cf.count) {
            GX_SetTevColorIn(st, cc_colour(cf.a[i], textured, combinedInReg),
                             cc_colour(cf.b[i], textured, combinedInReg),
                             cc_colour(cf.c[i], textured, combinedInReg),
                             cf.prevD[i] ? GX_CC_CPREV
                                         : cc_colour(cf.d[i], textured, combinedInReg));
            GX_SetTevColorOp(st, cf.op[i], GX_TB_ZERO, GX_CS_SCALE_1, cf.clamp[i], out);
        } else {
            GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
            GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, out);
        }

        if (i < af.count) {
            GX_SetTevAlphaIn(st, cc_alpha(af.a[i], textured, combinedInReg),
                             cc_alpha(af.b[i], textured, combinedInReg),
                             cc_alpha(af.c[i], textured, combinedInReg),
                             af.prevD[i] ? GX_CA_APREV
                                         : cc_alpha(af.d[i], textured, combinedInReg));
            GX_SetTevAlphaOp(st, af.op[i], GX_TB_ZERO, GX_CS_SCALE_1, af.clamp[i], out);
        } else {
            GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
            GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, out);
        }

        /* Pinned on every stage rather than only where cc_alpha asked for it:
         * it costs one register write and removes a way to get it wrong. */
        GX_SetTevKAlphaSel(st, GX_TEV_KASEL_1);
    }
    return stage + n;
}

/*
 * The whole TEV plan for the combiner currently set, plus the texture
 * coordinate generator and the rasterised colour channel that feed it.
 */
/*
 * Does this batch want the fog blend?
 *
 * Three conditions, all read off the state rather than assumed. G_FOG has to
 * be on, because that is what makes the RSP write a fog factor into the vertex
 * alpha in the first place. The cycle type has to be two, because in one-cycle
 * mode the single blender cycle is the surface blend and there is nowhere for
 * fog to go. And cycle 1's muxes have to be the fog pair -- DKR's draw tables
 * put G_RM_FOG_SHADE_A there and the real mode in cycle 2, which is why
 * apply_render_mode deliberately reads cycle 2 and would otherwise throw the
 * fog away entirely.
 */
static BOOL fog_active(void) {
    u32 rm = sOtherModeL;

    if ((sGeoMode & G_FOG) == 0) {
        return FALSE;
    }
    if (((sOtherModeH >> 20) & 3) != 1) { /* G_CYC_2CYCLE */
        return FALSE;
    }
    return ((rm >> 30) & 3) == BL_P_CLR_FOG && ((rm >> 26) & 3) == BL_A_SHADE;
}

/*
 * The fog blend, as one extra TEV stage after the combiner.
 *
 * The N64 does this in the blender, not the combiner: G_RM_FOG_SHADE_A is
 * "fog colour over the pixel, by shade alpha". GX has a fog unit, but its
 * curve is its own and would not reproduce the RSP's linear factor, so the
 * factor rides in the vertex alpha exactly as the RSP leaves it and this stage
 * performs the lerp.
 *
 *   out = d + a*(1-c) + b*c,  with a = CPREV, b = KONST(fog), c = RASA
 *
 * The konst register is free -- PRIM and ENV are in TEVREG0/1 and a two-cycle
 * combiner parks its first cycle in TEVREG2 -- so the fog colour goes there
 * rather than competing for a register.
 */
static void tev_emit_fog(u32 stage) {
    GX_SetTevKColor(GX_KCOLOR0, sFogColor);
    GX_SetTevKColorSel((u8) stage, GX_TEV_KCSEL_K0);
    GX_SetTevOrder((u8) stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevColorIn((u8) stage, GX_CC_CPREV, GX_CC_KONST, GX_CC_RASA, GX_CC_ZERO);
    GX_SetTevColorOp((u8) stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    /* Alpha passes the combiner's through untouched: it is what the second
     * blender cycle uses to compose the surface against memory. */
    GX_SetTevAlphaIn((u8) stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp((u8) stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void apply_combiner(BOOL textured) {
    BOOL twoCycle = ((sOtherModeH >> 20) & 3) == 1; /* G_MDSFT_CYCLETYPE */
    u32 stage;

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL,
                   GX_DF_NONE, GX_AF_NONE);
    if (textured) {
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    } else {
        GX_SetNumTexGens(0);
    }

    /* PRIMITIVE and ENVIRONMENT are constant for the whole batch. They go into
     * TEV registers rather than into the konst registers because a stage can
     * read several registers but only one konst, and G_CC_BLENDPE_A_PRIM wants
     * both of them in the same stage. */
    GX_SetTevColor(GX_TEVREG0, sPrimColor);
    GX_SetTevColor(GX_TEVREG1, sEnvColor);

    if (twoCycle) {
        /* The second cycle reads the first's result as COMBINED. Parking that
         * in register 2 instead of leaving it in TEVPREV is what makes the
         * two-stage general case safe: a stage of the second cycle overwrites
         * TEVPREV, and an operand still reading COMBINED from there would pick
         * up the half-finished value. */
        stage = tev_emit_cycle(0, sCombine.col[0], sCombine.alp[0], textured, FALSE,
                               GX_TEVREG2);
        stage = tev_emit_cycle(stage, sCombine.col[1], sCombine.alp[1], textured, TRUE,
                               GX_TEVPREV);
    } else {
        stage = tev_emit_cycle(0, sCombine.col[0], sCombine.alp[0], textured, FALSE,
                               GX_TEVPREV);
    }

    if (stage == 0) {
        stage = 1;
    }
    if (fog_active()) {
        tev_emit_fog(stage);
        stage++;
#ifdef GC_DEBUG
        gGcFogBatches++;
#endif
    }
    GX_SetNumTevStages((u8) stage);
}

/* The pinned pipeline this replaced, kept behind GC_COMBINER for the A/B. */
static void apply_combiner_pinned(BOOL textured) {
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL,
                   GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTevStages(1);
    if (textured) {
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    } else {
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }
}

static void apply_combiner_current(BOOL textured) {
    if (GC_COMBINER) {
        apply_combiner(textured);
    } else {
        apply_combiner_pinned(textured);
    }
}

/*
 * The render mode, as GX blend, depth and alpha-test state.
 *
 * The RDP's blender computes p * a + m * b from four muxes, and whether it
 * runs at all is FORCE_BL. Without that bit the blend happens only on
 * partially covered pixels -- an edge treatment for anti-aliasing, not
 * transparency -- so those modes write the pixel they computed, which is what
 * GX_BM_NONE does.
 */
static void apply_render_mode(void) {
    u32 rm = sOtherModeL;
    BOOL twoCycle = ((sOtherModeH >> 20) & 3) == 1;
    u32 p, a, m, b;
    BOOL zcmp, zupd;

    if (!GC_RENDERMODE) {
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
        sDecalBias = 0.0f;
        return;
    }

    /* In two-cycle mode it is the second cycle's blender that writes to
     * memory, and DKR pairs a fog blend in the first with the real one in the
     * second all through its draw tables (G_RM_FOG_SHADE_A | G_RM_XLU_SURF2,
     * src/textures_sprites.c). Reading the first cycle's muxes there would
     * read the fog and blend against the fog colour. */
    if (twoCycle) {
        p = (rm >> 28) & 3;
        a = (rm >> 24) & 3;
        m = (rm >> 20) & 3;
        b = (rm >> 16) & 3;
    } else {
        p = (rm >> 30) & 3;
        a = (rm >> 26) & 3;
        m = (rm >> 22) & 3;
        b = (rm >> 18) & 3;
    }

    if ((rm & RM_FORCE_BL) != 0 && p == BL_P_CLR_IN && m == BL_M_CLR_MEM) {
        u8 src = (a == BL_A_ZERO) ? GX_BL_ZERO : GX_BL_SRCALPHA;
        u8 dst;

        switch (b) {
            case BL_B_ONE:
                dst = GX_BL_ONE;
                break;
            case BL_B_ZERO:
                dst = GX_BL_ZERO;
                break;
            case BL_B_A_MEM:
                /* Framebuffer alpha, which this pixel format has none of:
                 * GX_BL_DSTALPHA needs GX_PF_RGBA6_Z24 and gc_gfx_init asks
                 * for RGB8_Z24. One minus the source alpha is what the same
                 * surface gets from the ordinary transparent mode. */
                dst = GX_BL_INVSRCALPHA;
                break;
            default: /* BL_B_1MA */
                dst = GX_BL_INVSRCALPHA;
                break;
        }
        GX_SetBlendMode(GX_BM_BLEND, src, dst, GX_LO_CLEAR);
    } else {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }

    /*
     * CVG_X_ALPHA is how the RDP writes a cutout: coverage is multiplied by
     * alpha, so a fully transparent texel disappears instead of blending. On
     * GX that is the alpha test -- and the depth test has to move behind it,
     * because in the default order a rejected texel has already written its
     * depth and goes on occluding whatever is behind it.
     *
     * G_AC_THRESHOLD is the RDP's other alpha test, and it compares against
     * the blend colour's alpha instead of against a fixed value. DKR_OML_COMMON
     * is G_AC_NONE and the draw tables OR onto it, so it is the rarer of the
     * two here; it is honoured rather than folded into the CVG_X_ALPHA case
     * because the two use different references and only one of them is 128.
     */
    if ((rm & RM_CVG_X_ALPHA) != 0 || (rm & RM_ALPHACOMPARE) == RM_AC_THRESHOLD) {
        u8 ref = ((rm & RM_ALPHACOMPARE) == RM_AC_THRESHOLD) ? sBlendColor.a : 128;

        GX_SetAlphaCompare(GX_GREATER, ref, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_FALSE);
    } else {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
    }

    zcmp = (rm & RM_Z_CMP) != 0;
    zupd = (rm & RM_Z_UPD) != 0;

    if ((rm & RM_ZMODE_MASK) == RM_ZMODE_DEC) {
        /*
         * A decal -- a shadow, a skid mark, a painted sign -- is coplanar with
         * the surface it lies on, so it must test against it and never write.
         * Not writing is only half of it: the two interpolate to depths that
         * differ by rounding, GX_LEQUAL then passes on some pixels and fails
         * on others, and a shadow on a large flat floor comes out shimmering
         * and half missing. The bias that separates them goes into the
         * projection, in load_3d_projection.
         */
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
        sDecalBias = GC_DECAL_BIAS;
    } else {
        /*
         * The update is gated on the test because GX and the N64 disagree
         * about what "no depth test" means: GX honours update_enable whatever
         * the comparison says, so a mode carrying Z_UPD without Z_CMP would
         * stamp depth across everything it covers. ref-sm64gc hit exactly this
         * with SM64's full-screen rectangles, and wrote down why.
         */
        GX_SetZMode(zcmp ? GX_TRUE : GX_FALSE, zcmp ? GX_LEQUAL : GX_ALWAYS,
                    (zcmp && zupd) ? GX_TRUE : GX_FALSE);
        sDecalBias = 0.0f;
    }
}

/*
 * Swaps the pipeline over to textured rectangles.
 *
 * The combiner and the render mode apply here exactly as they do to geometry:
 * a G_TEXRECT goes through the same RDP stages, and DKR leans on that --
 * font.c draws every character with G_CC_ENV_DECALA, which takes the glyph's
 * shape from the texture's alpha and its colour from ENVIRONMENT.
 */
static void gfx_set_textured_state(GXTexObj *tex) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GX_LoadTexObj(tex, GX_TEXMAP0);
    apply_combiner_current(TRUE);
    apply_render_mode();
}

/*
 * The state for transformed geometry.
 *
 * Two things separate it from the 2D state: positions carry a z, and the depth
 * buffer is on. Everything the game draws in three dimensions goes through
 * here; interface rectangles keep the flat path, which is correct rather than
 * merely cheaper -- the RDP's fill and copy cycles do not test or write z
 * either.
 *
 * GX_LEQUAL with the depth handed in as 0 at the near plane is the pairing
 * that has to match GX's own sense of the depth range. If solid geometry
 * appears inside out -- far surfaces drawn over near ones -- this comparison
 * is the thing to flip, not the projection.
 */
static void gfx_set_3d_state(GXTexObj *tex) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    if (tex != NULL) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_LoadTexObj(tex, GX_TEXMAP0);
    }

    apply_combiner_current(tex != NULL);
    apply_render_mode();

    /* Back faces are decided per triangle from the Triangle flag byte, so GX's
     * own culling stays out of it. */
    GX_SetCullMode(GX_CULL_NONE);
}

/* One axis-aligned rectangle in game-space coordinates, flat colour. */
static void gfx_draw_rect(f32 x0, f32 y0, f32 x1, f32 y1, GXColor c) {
    load_2d_projection();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(x0, y0);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_Position2f32(x1, y0);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_Position2f32(x1, y1);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_Position2f32(x0, y1);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_End();
}

/*
 * Brings GX up.
 *
 * Called once, after gc_video_init has chosen the render mode, because
 * everything here is sized from it.
 */
void gc_gfx_init(void) {
    GXRModeObj *rmode = gc_video_mode();
    GXColor background = { 0, 0, 0, 0xFF };
    f32 yscale;
    u32 xfbHeight;

    sFifo = memalign(32, GX_FIFO_SIZE);
    if (sFifo == NULL) {
        gc_fatal("could not allocate a %d byte GX FIFO", GX_FIFO_SIZE);
    }
    memset(sFifo, 0, GX_FIFO_SIZE);

    GX_Init(sFifo, GX_FIFO_SIZE);
    GX_SetCopyClear(background, GX_MAX_Z24);

    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

    GX_SetPixelFmt((rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24), GX_ZC_LINEAR);

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    gfx_set_2d_state();
    gfx_set_2d_projection();
    /* Flush the EFB once so the first real frame starts from a known state.
     * The destination must be a genuine external framebuffer: GX_CopyDisp does
     * a GPU-side write of the whole buffer, so a NULL destination does not mean
     * "nowhere", it means address 0 -- straight over the GameCube's OS globals
     * at the bottom of MEM1. That corrupts the low memory the SI and interrupt
     * tables live in, and the next PAD_Init hangs waiting on hardware whose
     * bookkeeping has been overwritten. */
    GX_CopyDisp(gc_video_xfb(), GX_TRUE);

    sGxReady = TRUE;
}

/* ---- per-command handlers ------------------------------------------------ */
/*
 * Each of these takes the two command words already split out. They are
 * separate functions rather than inline cases so that the dispatch loop stays
 * readable and so each can be filled in independently; the file header lists
 * which are still ignored.
 */

/*
 * G_TEXTURE, which is gSPTexture: a scale for S and for T in the two halves of
 * w1, as 0.16 fractions, plus a level, a tile and an enable in w0.
 *
 * 0xFFFF is the conventional spelling of 1.0 rather than 65535/65536, and a
 * scale of zero means the microcode was asked not to scale at all; both are
 * taken as the identity. The enable is not read: whether a G_TRIN is textured
 * is carried in the command itself, and a rectangle says so by existing.
 */
static void gfx_texture(u32 w0, u32 w1) {
    u32 sc = (w1 >> 16) & 0xFFFF;
    u32 tc = w1 & 0xFFFF;

    sTexScaleS = (sc == 0xFFFF || sc == 0) ? 1.0f : (f32) sc / 65536.0f;
    sTexScaleT = (tc == 0xFFFF || tc == 0) ? 1.0f : (f32) tc / 65536.0f;
    (void) w0;
}

static void gfx_set_timg(u32 w0, u32 w1) {
#ifdef GC_DEBUG
    gGcTexFormats |= 1u << ((((w0 >> 21) & 7) << 2) | ((w0 >> 19) & 3));
#endif
    /* Only the address is kept. The format and width in this command describe
     * how the RDP was to fetch the block into TMEM; how the texels are read
     * back out is the tile's business, and the two disagree on purpose in the
     * usual load idiom. */
    sTimgAddr = (u32) segmented_to_virtual(w1);
    (void) w0;
    /* TODO: the source image for the next load. Combined with G_SETTILE this
     * identifies a texture; the cache keyed on that pair is what turns an N64
     * TMEM load into a GX texture object. */
}

/*
 * G_SETCOMBINE.
 *
 * Sixteen mux fields across the two words, packed by gDPSetCombineLERP
 * (include/PR/gbi.h) as
 *
 *   w0   a0 20..23   c0 15..19   Aa0 12..14  Ac0 9..11  a1 5..8   c1 0..4
 *   w1   b0 28..31   b1 24..27   Aa1 21..23  Ac1 18..20 d0 15..17
 *        Ab0 12..14  Ad0 9..11   d1 6..8     Ab1 3..5   Ad1 0..2
 *
 * The field widths differ per slot -- four bits for a and b, five for c, three
 * for d and for every alpha slot -- and each slot decodes a different set of
 * sources, so every field goes through its own table. A value a table does not
 * name reads as zero, which is what the RDP does with the unused encodings.
 */
static void gfx_set_combine(u32 w0, u32 w1) {
    static const u8 aTbl[16] = {
        CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM, CC_SHADE, CC_ENV, CC_ONE,
        CC_ZERO /* noise */, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO,
        CC_ZERO, CC_ZERO
    };
    static const u8 bTbl[16] = {
        CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM, CC_SHADE, CC_ENV,
        CC_ZERO /* key centre */, CC_ZERO /* K4 */, CC_ZERO, CC_ZERO, CC_ZERO,
        CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO
    };
    static const u8 cTbl[32] = {
        CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM, CC_SHADE, CC_ENV,
        /* The key scale, which G_CC_DECAL_SCALE names. It only appears in the
         * copy cycle, where the RDP does not run the combiner at all, and one
         * is the value that turns that mode into the plain blit it is. */
        CC_ONE,
        CC_COMBINED_A, CC_TEXEL0_A, CC_TEXEL1_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A,
        CC_ZERO /* LOD fraction */, CC_ZERO /* prim LOD fraction */, CC_ZERO /* K5 */,
        CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO,
        CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO
    };
    static const u8 dTbl[8] = {
        CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM, CC_SHADE, CC_ENV, CC_ONE, CC_ZERO
    };
    /* The alpha a, b and d slots share one mux; alpha c has its own, with the
     * two LOD fractions where combined and one would be. */
    static const u8 aAlphaTbl[8] = {
        CC_COMBINED_A, CC_TEXEL0_A, CC_TEXEL1_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A,
        CC_ONE, CC_ZERO
    };
    static const u8 cAlphaTbl[8] = {
        CC_ZERO /* LOD fraction */, CC_TEXEL0_A, CC_TEXEL1_A, CC_PRIM_A, CC_SHADE_A,
        CC_ENV_A, CC_ZERO /* prim LOD fraction */, CC_ZERO
    };

    sCombine.col[0][0] = aTbl[(w0 >> 20) & 0xF];
    sCombine.col[0][1] = bTbl[(w1 >> 28) & 0xF];
    sCombine.col[0][2] = cTbl[(w0 >> 15) & 0x1F];
    sCombine.col[0][3] = dTbl[(w1 >> 15) & 7];

    sCombine.alp[0][0] = aAlphaTbl[(w0 >> 12) & 7];
    sCombine.alp[0][1] = aAlphaTbl[(w1 >> 12) & 7];
    sCombine.alp[0][2] = cAlphaTbl[(w0 >> 9) & 7];
    sCombine.alp[0][3] = aAlphaTbl[(w1 >> 9) & 7];

    sCombine.col[1][0] = aTbl[(w0 >> 5) & 0xF];
    sCombine.col[1][1] = bTbl[(w1 >> 24) & 0xF];
    sCombine.col[1][2] = cTbl[w0 & 0x1F];
    sCombine.col[1][3] = dTbl[(w1 >> 6) & 7];

    sCombine.alp[1][0] = aAlphaTbl[(w1 >> 21) & 7];
    sCombine.alp[1][1] = aAlphaTbl[(w1 >> 3) & 7];
    sCombine.alp[1][2] = cAlphaTbl[(w1 >> 18) & 7];
    sCombine.alp[1][3] = aAlphaTbl[w1 & 7];
#ifdef GC_DEBUG
    gGcCombineW0 = w0;
    gGcCombineW1 = w1;
#endif
}

/*
 * G_SETSCISSOR. Coordinates are 12-bit 10.2 fixed point in game space; GX
 * wants whole EFB pixels, so they are scaled and rounded outwards -- a scissor
 * that is a pixel too generous costs nothing, one that is a pixel short clips
 * the edge of the picture.
 */
static void gfx_set_scissor(u32 w0, u32 w1) {
    GXRModeObj *rmode = gc_video_mode();
    f32 ulx = (f32) ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 uly = (f32) (w0 & 0xFFF) / 4.0f;
    f32 lrx = (f32) ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 lry = (f32) (w1 & 0xFFF) / 4.0f;
    s32 x0 = (s32) (ulx * sScaleX);
    s32 y0 = (s32) (uly * sScaleY);
    s32 x1 = (s32) (lrx * sScaleX + 0.5f);
    s32 y1 = (s32) (lry * sScaleY + 0.5f);

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > rmode->fbWidth) {
        x1 = rmode->fbWidth;
    }
    if (y1 > rmode->efbHeight) {
        y1 = rmode->efbHeight;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    GX_SetScissor((u32) x0, (u32) y0, (u32) (x1 - x0), (u32) (y1 - y0));
}

/* G_SETFILLCOLOR. RGBA5551, expanded so that 0x1F becomes 0xFF rather than
 * 0xF8 -- replicating the high bits down is what keeps white white. */
static void gfx_set_fill_color(u32 w1) {
    u32 c = w1 & 0xFFFF;
    u32 r = (c >> 11) & 0x1F;
    u32 g = (c >> 6) & 0x1F;
    u32 b = (c >> 1) & 0x1F;

    sFillColor.r = (u8) ((r << 3) | (r >> 2));
    sFillColor.g = (u8) ((g << 3) | (g >> 2));
    sFillColor.b = (u8) ((b << 3) | (b >> 2));
    sFillColor.a = (u8) ((c & 1) ? 0xFF : 0x00);
}

/*
 * G_FILLRECT. The coordinates are whole pixels held in 10.2 fields, and the
 * RDP's rectangle includes its lower-right pixel, so the quad runs one past
 * it.
 *
 * What it *draws*, though, depends on the cycle type, and getting that wrong
 * is what painted the game black.
 *
 * In G_CYC_FILL the RDP bypasses the combiner and the blender and writes the
 * fill colour straight into the framebuffer -- the clear at the top of every
 * frame. In G_CYC_1CYCLE or G_CYC_2CYCLE it does no such thing: the rectangle
 * is an ordinary primitive that goes through the combiner and the blender,
 * and the fill colour is not consulted at all. DKR uses that second form for
 * its fades, and says so in as many words -- transition_render_fullscreen in
 * src/fade_transition.c is
 *
 *     gSPDisplayList(dTransitionFadeSettings)   // G_CYC_1CYCLE, G_RM_CLD_SURF
 *     gDPSetPrimColor(..., gCurFadeRed, ..., gCurFadeAlpha)
 *     gDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE)
 *     gDPFillRectangle(0, 0, w, h)
 *
 * so the rectangle's colour is PRIMITIVE and its alpha is the fade's. Outside
 * a transition that alpha is zero and the rectangle is invisible.
 *
 * Drawing it as an opaque fill instead covered the whole screen in opaque
 * black on the last primitive of every gameplay frame. Measured: the widest
 * primitive of the frame was `kind4 area 1000/1000 (0,0)-(1000,1000)
 * seq 1725/1725 | cc fcffffff fffdf6fb omH 00802c0f omL 00504340 |
 * col 000000ff prim 00000000` -- a full-screen fill, last in the list, in
 * one-cycle mode (omH bits 20..21 = 0), with the cloud-surface render mode
 * and a primitive colour whose alpha was zero. Seventeen hundred triangles
 * were drawn correctly underneath it every frame.
 */
static void gfx_fill_rect(u32 w0, u32 w1) {
    f32 lrx = (f32) ((w0 >> 14) & 0x3FF);
    f32 lry = (f32) ((w0 >> 2) & 0x3FF);
    f32 ulx = (f32) ((w1 >> 14) & 0x3FF);
    f32 uly = (f32) ((w1 >> 2) & 0x3FF);
    BOOL fillCycle = ((sOtherModeH >> 20) & 3) == 3; /* G_CYC_FILL */

    if (lrx < ulx || lry < uly) {
        return;
    }

#ifdef GC_DEBUG
    gGcFills++;
    gGcFillColor = ((u32) sFillColor.r << 16) | ((u32) sFillColor.g << 8) | sFillColor.b;
    gGcFillRect[0] = (u32) ulx;
    gGcFillRect[1] = (u32) uly;
    gGcFillRect[2] = (u32) lrx;
    gGcFillRect[3] = (u32) lry;
#endif

#ifdef GC_DEBUG
    sCoverTexAddr = 0;
    sCoverFmtSiz = 0xFF;
    cover_note(fillCycle ? 4 : 5, (s32) (ulx * 1000.0f / sGameWidth),
               (s32) (uly * 1000.0f / sGameHeight), (s32) ((lrx + 1.0f) * 1000.0f / sGameWidth),
               (s32) ((lry + 1.0f) * 1000.0f / sGameHeight),
               fillCycle ? (((u32) sFillColor.r << 24) | ((u32) sFillColor.g << 16) |
                            ((u32) sFillColor.b << 8) | sFillColor.a)
                         : (((u32) sPrimColor.r << 24) | ((u32) sPrimColor.g << 16) |
                            ((u32) sPrimColor.b << 8) | sPrimColor.a));
#endif

    if (!drawing_to_color()) {
        /* The colour image is pointed at the depth buffer: this is the N64's
         * depth clear, and on GX the depth buffer is cleared by the EFB copy
         * (gc_gfx_copy_display forces the write state so GX_CopyDisp's clear
         * argument actually reaches it). Painting it would put a full-screen
         * rectangle of the clear pattern into the visible framebuffer. */
#ifdef GC_DEBUG
        gGcFillsOffscreen++;
#endif
        return;
    }

    if (!fillCycle) {
        /* The combiner supplies the colour, so the corners carry white: the
         * RDP rasterises no shade for a rectangle, and the modes DKR fills
         * with read PRIMITIVE. Same reasoning as gfx_tex_rect's corners. */
#ifdef GC_DEBUG
        gGcFillsBlend++;
#endif
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        apply_combiner_current(FALSE);
        apply_render_mode();
        gfx_draw_rect(ulx, uly, lrx + 1.0f, lry + 1.0f, kWhite);
        /* Leave the pipeline as the rest of the interface path expects it. */
        gfx_set_2d_state();
        return;
    }

    /* The fill cycle writes straight to memory: no combiner, no blend, no
     * depth. gfx_set_2d_state is exactly that state, and a preceding batch
     * can have left the blender on. */
    gfx_set_2d_state();
    gfx_draw_rect(ulx, uly, lrx + 1.0f, lry + 1.0f, sFillColor);
}

#ifdef GC_DEBUG
/* G_SETTILESIZE: uls/ult/lrs/lrt in 10.2 fixed point. The tile covers the
 * inclusive rectangle, so its size is the difference plus one texel. */
static void gfx_note_tile_size(u32 w0, u32 w1) {
    u32 uls = (w0 >> 12) & 0xFFF;
    u32 ult = w0 & 0xFFF;
    u32 lrs = (w1 >> 12) & 0xFFF;
    u32 lrt = w1 & 0xFFF;

    gGcTexW = ((lrs - uls) >> 2) + 1;
    gGcTexH = ((lrt - ult) >> 2) + 1;
}
#endif

/* ---- geometry ------------------------------------------------------------ */

/*
 * Reads one N64 fixed-point matrix.
 *
 * The layout is not a guess: src/hasm/math_util.c's `mtxf_to_mtx` writes the
 * integer halves of all sixteen elements into the first eight words and the
 * fractional halves into the last eight, element (i,j) landing at 16-bit index
 * i*4+j in each block. Reading it back as u16 pairs inverts exactly that.
 */
static void load_matrix(f32 dst[4][4], const u16 *src) {
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s32 whole = (s16) src[i * 4 + j];
            u32 frac = src[16 + i * 4 + j];
            /*
             * The cast back to s32 is the whole of this function.
             *
             * `frac` is unsigned, so `(whole << 16) | frac` is evaluated at
             * type unsigned int -- C converts both operands to the common
             * type, and unsigned wins. Converting that to float then reads a
             * negative element as 2^32 plus itself: -2.45 comes back as
             * 65533.55, and every negative coefficient in the matrix becomes
             * an enormous positive one.
             *
             * It hid well. The billboard matrix from mtxf_billboard happens to
             * have no negative entries in the common case, so it decoded
             * perfectly while every camera matrix beside it did not, which
             * made the fault look like a billboard problem. What it actually
             * produced was geometry flung far off screen -- the large flat
             * wedges of colour -- and depths where z/w sat at the far plane
             * for everything.
             */
            s32 fixed = (s32) (((u32) whole << 16) | frac);

            dst[i][j] = (f32) fixed / 65536.0f;
        }
    }
}

/*
 * One vertex through the current matrix and the viewport.
 *
 * `clip = v * M` with v a row vector, then the perspective divide, then the
 * viewport's scale and translate. guPerspectiveF (libultra/src/gu/perspective.c)
 * puts -z in the w column, so w is positive in front of the camera and a
 * vertex with w <= 0 is behind it.
 */
static void transform_vertex(const s16 *pos, XVertex *out) {
    const f32 (*m)[4] = sMatrix[sCurMatrix];
    f32 x = (f32) pos[0];
    f32 y = (f32) pos[1];
    f32 z = (f32) pos[2];

    out->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    out->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
    out->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
    out->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
}

/* ---- clipping and projection --------------------------------------------- *
 *
 * One triangle corner on its way to GX: clip-space position, the texture
 * coordinates that belong to this corner of this triangle (they come from the
 * Triangle record, not from the vertex), and the vertex colour. Clipping has
 * to interpolate all of it.
 */
typedef struct {
    f32 x, y, z, w;
    f32 u, v;
    f32 r, g, b, a;
} Corner;

/* Everything in front of this is drawable. Not zero: a vertex exactly on the
 * eye plane divides by zero. */
#define NEAR_W 1.0f

/* Set by Makefile.gc's GC_NO_CULL knob. Set to 1 to draw both windings. The repository states which flag value means
 * "draw the back face" but never which winding is the front, so the only way
 * to tell a culling bug from a geometry bug is to turn culling off and see
 * whether the missing surface was being culled or was never submitted. */
#ifndef GC_NO_CULL
#define GC_NO_CULL 0
#endif

static void corner_lerp(const Corner *a, const Corner *b, f32 t, Corner *out) {
    out->x = a->x + (b->x - a->x) * t;
    out->y = a->y + (b->y - a->y) * t;
    out->z = a->z + (b->z - a->z) * t;
    out->w = a->w + (b->w - a->w) * t;
    out->u = a->u + (b->u - a->u) * t;
    out->v = a->v + (b->v - a->v) * t;
    out->r = a->r + (b->r - a->r) * t;
    out->g = a->g + (b->g - a->g) * t;
    out->b = a->b + (b->b - a->b) * t;
    out->a = a->a + (b->a - a->a) * t;
}

/*
 * Sutherland-Hodgman against the single plane w = NEAR_W.
 *
 * Only the near plane needs doing: the sides and top are handled by GX's own
 * scissor once the triangle is on screen, and the far plane by the depth
 * range. The near plane is the one that cannot be left to the rasteriser,
 * because a vertex behind the eye has no meaningful screen position at all.
 *
 * Three vertices against one plane yield at most four.
 */
static u32 clip_near(const Corner *in, u32 count, Corner *out) {
    u32 n = 0;
    u32 i;

    for (i = 0; i < count; i++) {
        const Corner *cur = &in[i];
        const Corner *next = &in[(i + 1) % count];
        BOOL curIn = cur->w >= NEAR_W;
        BOOL nextIn = next->w >= NEAR_W;

        if (curIn) {
            out[n++] = *cur;
        }
        if (curIn != nextIn) {
            f32 t = (NEAR_W - cur->w) / (next->w - cur->w);

            corner_lerp(cur, next, t, &out[n++]);
        }
    }
    return n;
}

/*
 * Clip space to screen.
 *
 * The x and y mapping is the viewport's, read from the display list.
 *
 * The depth has to come out on the same convention as the hardware path, and
 * it did not. That path puts the near plane at -1 and the far plane at 0,
 * which is GX's own range and is what load_3d_projection derives; this one
 * used to send `ndc * 0.5 + 0.5`, and gfx_ortho with n = -1, f = 0 makes
 * mt[2][2] = -1, so the composed result was `clip.z = -(ndc * 0.5 + 0.5)` --
 * zero at the near plane and -1 at the far one. The same numeric range, read
 * backwards.
 *
 * Under GX_LEQUAL that gives a distant sprite the most-near depth there is, so
 * it wins against everything and draws in front. Every sprite in the game goes
 * through here, because the slot 2 billboard matrix has no w variation and so
 * always takes this path -- which is why sprites, and only sprites, came out
 * in front of the character.
 *
 * `0.5 - ndc * 0.5` inverts it: 1 at the near plane, 0 at the far one, and the
 * ortho's negation turns that into -1 near and 0 far. The decal bias follows
 * the same flip -- nearer is now a larger sz, so a decal is biased by adding.
 */
static void project_corner(const Corner *c, f32 *sx, f32 *sy, f32 *sz) {
    f32 inv = 1.0f / c->w;

    /* y is negated because the projection's normalised y points up while
     * screen y counts down. The repository documents the viewport's fields but
     * not this sign, so it was settled by looking at the picture. */
    *sx = (c->x * inv) * sVpScaleX + sVpTransX;
    *sy = -(c->y * inv) * sVpScaleY + sVpTransY;
    *sz = 0.5f - (c->z * inv) * 0.5f + sDecalBias;
}

/* G_VTX. `struct Vertex` in include/structs.h: three s16 and four u8, ten
 * bytes, read byte-wise because the array has no alignment guarantee. */
static void gfx_vertex(u32 w0, u32 w1) {
    u32 p = (w0 >> 16) & 0xFF;
    u32 count = (p >> 3) + 1;
    BOOL append = (p & 1) != 0;
    const u8 *src = (const u8 *) segmented_to_virtual(w1);
    u32 base = append ? sVertBase : 0;
    u32 i;

    if ((u32) src < 0x80003000 || (u32) src >= 0x81800000) {
        return;
    }

    for (i = 0; i < count && base + i < MAX_VERTICES; i++) {
        const u8 *v = src + i * 10;
        s16 pos[3];

        pos[0] = (s16) ((v[0] << 8) | v[1]);
        pos[1] = (s16) ((v[2] << 8) | v[3]);
        pos[2] = (s16) ((v[4] << 8) | v[5]);

        transform_vertex(pos, &sVerts[base + i]);
#ifdef GC_DEBUG
        if (sBillboard && gGcBbSeen == 0 && i == 0) {
            gGcBbSeen = 1;
            gGcBbBatch[0] = count;
            gGcBbBatch[1] = append;
            gGcBbBatch[2] = base;
            gGcBbBatch[3] = sVertCount;
            {
                s32 r, c;

                for (r = 0; r < 4; r++) {
                    for (c = 0; c < 4; c++) {
                        gGcBbMtxBb[r * 4 + c] = (s32) (sMatrix[sCurMatrix][r][c] * 1000.0f);
                        gGcBbMtxScene[r * 4 + c] = (s32) (sMatrix[1][r][c] * 1000.0f);
                    }
                }
            }
            gGcBbAnchor[0] = sVerts[0].x;
            gGcBbAnchor[1] = sVerts[0].y;
            gGcBbAnchor[2] = sVerts[0].z;
            gGcBbAnchor[3] = sVerts[0].w;
            gGcBbPre[0] = sVerts[base].x;
            gGcBbPre[1] = sVerts[base].y;
            gGcBbPre[2] = sVerts[base].z;
            gGcBbPre[3] = sVerts[base].w;
        }
#endif
        if (GC_BILLBOARD && sBillboard && base + i != 0) {
            /* Vertex 0 is the anchor, already in clip space. */
            sVerts[base + i].x += sVerts[0].x;
            sVerts[base + i].y += sVerts[0].y;
            sVerts[base + i].z += sVerts[0].z;
            sVerts[base + i].w = sVerts[0].w;
        }
#ifdef GC_DEBUG
        if (gGcBbSeen == 1 && i == 0) {
            gGcBbSeen = 2;
            gGcBbPost[0] = sVerts[base].x;
            gGcBbPost[1] = sVerts[base].y;
            gGcBbPost[2] = sVerts[base].z;
            gGcBbPost[3] = sVerts[base].w;
        }
#endif
#ifdef GC_DEBUG
        gGcVtxByMtx[sCurMatrix]++;
        if (sVerts[base + i].w <= 0.0f) {
            gGcVtxBehindByMtx[sCurMatrix]++;
        }
#endif
        sVerts[base + i].r = v[6];
        sVerts[base + i].g = v[7];
        sVerts[base + i].b = v[8];
        sVerts[base + i].a = v[9];

        /*
         * With G_FOG on, the RSP overwrites the vertex alpha with the fog
         * factor -- the vertex's own alpha is simply lost, which is why this
         * assignment comes after the one above rather than instead of it. The
         * factor is the clip-space depth run through gSPFogPosition's two
         * coefficients: alpha = z/w * mul + offset, clamped to a byte.
         *
         * A vertex at or behind the eye has no meaningful z/w; the far end of
         * the range is the honest answer there, and it is what sm64-port's
         * gfx_pc reaches for as well.
         */
        if ((sGeoMode & G_FOG) != 0) {
            f32 w = sVerts[base + i].w;
            f32 f;

            if (w < 0.001f) {
                f = 32767.0f;
            } else {
                f = sVerts[base + i].z / w;
            }
            f = f * (f32) sFogMul + (f32) sFogOff;
            if (f < 0.0f) {
                f = 0.0f;
            } else if (f > 255.0f) {
                f = 255.0f;
            }
            sVerts[base + i].a = (u8) f;
#ifdef GC_DEBUG
            gGcFogVerts++;
#endif
        }
    }

    sVertCount = base + i;
    if (!append) {
        sVertBase = sVertCount;
    }
#ifdef GC_DEBUG
    gGcVtxLoaded += i;
    if (sVertCount > gGcVtxMaxCount) {
        gGcVtxMaxCount = sVertCount;
    }
#endif
}

/*
 * G_TRIN, F3DDKR's batched triangle command.
 *
 * gSPPolygon (include/f3ddkr.h) packs the count minus one into bits 20..23 and
 * a texture-enable flag into bits 16..19, with the batch's byte length in the
 * low sixteen. Each entry is a `struct Triangle`: a flag byte, three vertex
 * indices, and three pairs of texture coordinates.
 */
static void gfx_triangles(u32 w0, u32 w1) {
    u32 numTris = ((w0 >> 20) & 0xF) + 1;
    BOOL textured = ((w0 >> 16) & 1) != 0;
    const u8 *src = (const u8 *) segmented_to_virtual(w1);
    GXTexObj *tex = NULL;
    f32 texW = 1.0f, texH = 1.0f;
    f32 uScale = 1.0f, vScale = 1.0f;
    f32 uOff = 0.0f, vOff = 0.0f;
    BOOL hw;
    u32 i;

#ifdef GC_DEBUG
    gGcTrinCmds++;
#endif

    if ((u32) src < 0x80003000 || (u32) src >= 0x81800000) {
        return;
    }

#ifdef GC_DEBUG
    /* Counted after the address check, so a batch that never ran shows up as a
     * command with no triangles rather than as triangles that vanished. */
    gGcTrisIn += numTris;
#endif

    if (textured) {
        /* Tile 0 is G_TX_RENDERTILE, the one every load idiom in the game
         * describes last and draws through. */
        const TileDesc *t = &sTiles[0];
        TileImage img;

        BOOL haveImage = tile_image(t, &img);

#ifdef GC_DEBUG
        texdbg_note(t, &img, haveImage);
#endif
        if (haveImage) {
            tex = texture_get(img.addr, t->fmt, t->siz, img.width, img.height, img.stride,
                              img.swapOdd, img.tlutAddr, cm_to_gx(t->cms), cm_to_gx(t->cmt),
                              tex_filter());
            if (tex != NULL) {
#ifdef GC_DEBUG
                sCoverTexAddr = img.addr;
                sCoverFmtSiz = (t->fmt << 2) | t->siz;
#endif
                texW = (f32) img.width;
                texH = (f32) img.height;
                uScale = sTexScaleS * tile_shift_scale(t->shifts);
                vScale = sTexScaleT * tile_shift_scale(t->shiftt);
                uOff = (f32) t->uls / 4.0f;
                vOff = (f32) t->ult / 4.0f;
            }
        }
    }

#ifdef GC_DEBUG
    if (tex == NULL) {
        sCoverTexAddr = 0;
        sCoverFmtSiz = 0xFF;
    }
#endif
    gfx_set_3d_state(tex);
#ifdef GC_DEBUG
    statedbg_note(gGcCombineW0, gGcCombineW1,
                  ((sOtherModeL & RM_Z_CMP) ? 1u : 0u) | ((sOtherModeL & RM_Z_UPD) ? 2u : 0u) |
                      ((sOtherModeL & RM_FORCE_BL) ? 4u : 0u) |
                      (((sOtherModeL & RM_ZMODE_MASK) == RM_ZMODE_DEC) ? 8u : 0u) |
                      ((sOtherModeL & RM_CVG_X_ALPHA) ? 16u : 0u));
#endif

    /*
     * Hand the perspective divide to the GP if the matrix allows it.
     *
     * Dividing on the CPU and submitting screen coordinates works, and is what
     * this file used to do, but it costs two things that show up plainly on
     * screen. Texture coordinates then interpolate affinely -- linearly in
     * screen space rather than in depth -- so large surfaces smear, which is
     * the warped sky and ground of the intro flyby. And a vertex behind the
     * eye has to be clipped by hand; anything the hand-rolled clip does not
     * catch is mirrored through the origin by the divide and flung off screen,
     * dragging its triangle out as a large flat wedge.
     *
     * Both are named and described in aluzed/sm64-port-gc, which fought the
     * same fight: its GFX_GX_DEBUG_PROJ_TINT legend calls the CPU-divide path
     * "no near-plane clipping, affine interpolation -- produces both the wedge
     * and the smear". Its fix is this one, reached by fitting the projection
     * coefficients per batch because gfx_pc hands it pre-projected vertices.
     * DKR hands us the matrix itself, so load_3d_projection reads the same two
     * coefficients off it exactly.
     *
     * The CPU path stays as the fallback for matrices that are not of that
     * shape.
     */
    hw = load_3d_projection();
    if (!hw) {
        /*
         * The fallback path divides on the CPU and submits screen coordinates
         * -- game-space pixels with the viewport already baked in by
         * project_corner -- so it needs the flat projection and the whole-EFB
         * viewport, exactly as the 2D path does.
         *
         * load_3d_projection returns without loading anything when the matrix
         * is not of the affine shape it needs, which left the perspective
         * matrix and the game's 3D viewport from the previous batch in place.
         * Screen coordinates run through a perspective projection come out as
         * large flat wedges of colour, which is the defect this port has been
         * chasing under that name. It is not rare: the slot 2 billboard matrix
         * has no w variation at all, so every sprite in the frame takes this
         * path -- sixteen batches on the character select screen.
         */
        load_2d_projection();
    }
#ifdef GC_DEBUG
    if (hw) {
        gGcTrinHw++;
    } else {
        gGcTrinCpu++;
    }
#endif

    for (i = 0; i < numTris; i++) {
        const u8 *t = src + i * 16;
        Corner tri[3];
        Corner clipped[8];
        f32 sx[8], sy[8], sz[8];
        u32 n, k;
        BOOL bad = FALSE;

        for (k = 0; k < 3; k++) {
            u32 idx = t[1 + k];
            const XVertex *v;
            s16 tu, tv;

            if (idx >= sVertCount) {
                bad = TRUE;
                break;
            }
            v = &sVerts[idx];
            tu = (s16) ((t[4 + k * 4] << 8) | t[5 + k * 4]);
            tv = (s16) ((t[6 + k * 4] << 8) | t[7 + k * 4]);

            tri[k].x = v->x;
            tri[k].y = v->y;
            tri[k].z = v->z;
            tri[k].w = v->w;
            /* The coordinate in the record is in sixteenths of a texel with
             * five fractional bits; the tile's shift scales it, its origin
             * moves it, and GX wants the result normalised. */
            tri[k].u = (((f32) tu / 32.0f) * uScale - uOff) / texW;
            tri[k].v = (((f32) tv / 32.0f) * vScale - vOff) / texH;
            tri[k].r = (f32) v->r;
            tri[k].g = (f32) v->g;
            tri[k].b = (f32) v->b;
            tri[k].a = (f32) v->a;
        }
        if (bad) {
#ifdef GC_DEBUG
            gGcTrisBadIdx++;
#endif
            continue;
        }

        if (hw) {
            /*
             * Back faces, in normalised device coordinates.
             *
             * The two paths agree with each other and were both inverted. The
             * software path measures the area in screen space, where y counts
             * downwards, and this one measures it in normalised coordinates,
             * where y counts up; a reflection negates a signed area, so the
             * opposite comparisons below are the same rule written twice.
             *
             * Which winding is the front is not stated anywhere in the
             * repository, so it was a coin toss, and it came down wrong.
             * Measured on the intro beach: with culling off the sky gains its
             * clouds, the horizon gains its treeline and the terrain stops
             * being a flat wedge -- all of it geometry the old sign was
             * throwing away, while the back faces it kept were drawing over
             * what remained.
             *
             * Skipped when any corner is at or behind the eye: the divide is
             * meaningless there, and the GP is about to clip the triangle
             * properly anyway.
             */
            if (!GC_NO_CULL && (t[0] & BACKFACE_DRAW) == 0 && tri[0].w > 1e-6f &&
                tri[1].w > 1e-6f && tri[2].w > 1e-6f) {
                f32 ax = tri[0].x / tri[0].w, ay = tri[0].y / tri[0].w;
                f32 bx = tri[1].x / tri[1].w, by = tri[1].y / tri[1].w;
                f32 cx = tri[2].x / tri[2].w, cy = tri[2].y / tri[2].w;
                f32 area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);

                if (area <= 0.0f) {
#ifdef GC_DEBUG
                    gGcTrisCulled++;
#endif
                    continue;
                }
            }

#ifdef GC_DEBUG
            if (tri[0].w > 1e-6f && tri[1].w > 1e-6f && tri[2].w > 1e-6f) {
                f32 px[3], py[3];
                f32 lox, loy, hix, hiy;

                for (k = 0; k < 3; k++) {
                    px[k] = ((tri[k].x / tri[k].w) * 0.5f + 0.5f) * 1000.0f;
                    py[k] = (0.5f - (tri[k].y / tri[k].w) * 0.5f) * 1000.0f;
                }
                lox = hix = px[0];
                loy = hiy = py[0];
                for (k = 1; k < 3; k++) {
                    if (px[k] < lox) { lox = px[k]; }
                    if (px[k] > hix) { hix = px[k]; }
                    if (py[k] < loy) { loy = py[k]; }
                    if (py[k] > hiy) { hiy = py[k]; }
                }
                cover_note(1, (s32) lox, (s32) loy, (s32) hix, (s32) hiy,
                           ((u32) (u8) tri[0].r << 24) | ((u32) (u8) tri[0].g << 16) |
                               ((u32) (u8) tri[0].b << 8) | (u8) tri[0].a);
            } else {
                gGcCoverTotal++;
            }
#endif
            GX_Begin(GX_TRIANGLES, tex != NULL ? GX_VTXFMT3 : GX_VTXFMT2, 3);
            for (k = 0; k < 3; k++) {
                /* The projection turns the z we submit back into w, so submit
                 * -w and let the GP recover clip space, divide it, and clip
                 * it. */
                GX_Position3f32(tri[k].x, tri[k].y, -tri[k].w);
                if (GC_PROJ_TINT) {
                    GX_Color4u8(0x00, 0xFF, 0x00, 0xFF);
                } else {
                    GX_Color4u8((u8) tri[k].r, (u8) tri[k].g, (u8) tri[k].b, (u8) tri[k].a);
                }
                if (tex != NULL) {
                    GX_TexCoord2f32(tri[k].u, tri[k].v);
                }
            }
            GX_End();
#ifdef GC_DEBUG
            gGcTrisOut++;
#endif
            continue;
        }

        n = clip_near(tri, 3, clipped);
        if (n < 3) {
#ifdef GC_DEBUG
            gGcTrisClipped++;
            if (tri[0].w <= 0.0f && tri[1].w <= 0.0f && tri[2].w <= 0.0f) {
                gGcTrisBehind++;
            } else {
                gGcTrisNearOnly++;
            }
#endif
            continue;
        }

        for (k = 0; k < n; k++) {
            project_corner(&clipped[k], &sx[k], &sy[k], &sz[k]);
        }
#ifdef GC_DEBUG
        {
            s32 x0 = (s32) sx[0], x1 = (s32) sx[0], y0 = (s32) sy[0], y1 = (s32) sy[0];
            u32 q;

            for (q = 1; q < n; q++) {
                if ((s32) sx[q] < x0) { x0 = (s32) sx[q]; }
                if ((s32) sx[q] > x1) { x1 = (s32) sx[q]; }
                if ((s32) sy[q] < y0) { y0 = (s32) sy[q]; }
                if ((s32) sy[q] > y1) { y1 = (s32) sy[q]; }
            }
            if ((x1 - x0) + (y1 - y0) >
                (gGcCpuBox[2] - gGcCpuBox[0]) + (gGcCpuBox[3] - gGcCpuBox[1])) {
                gGcCpuBox[0] = x0;
                gGcCpuBox[1] = y0;
                gGcCpuBox[2] = x1;
                gGcCpuBox[3] = y1;
                gGcCpuBoxMtx = sCurMatrix;
                gGcCpuBoxBb = sBillboard ? 1u : 0u;
                gGcCpuBoxAnchor[0] = (s32) sVerts[0].x;
                gGcCpuBoxAnchor[1] = (s32) sVerts[0].y;
                gGcCpuBoxAnchor[2] = (s32) sVerts[0].z;
                gGcCpuBoxAnchor[3] = (s32) sVerts[0].w;
            }
            gGcCpuBoxTris++;
        }
#endif

        /*
         * Back faces.
         *
         * include/structs.h names the flag byte's meanings outright:
         * BACKFACE_CULL is 0x00 and BACKFACE_DRAW is 0x40. Which winding
         * counts as the front is not stated anywhere; the sign below is the
         * mirror of the hardware path's and was corrected with it.
         */
        if (!GC_NO_CULL && (t[0] & BACKFACE_DRAW) == 0) {
            f32 area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);

            if (area >= 0.0f) {
#ifdef GC_DEBUG
                gGcTrisCulled++;
#endif
                continue;
            }
        }

#ifdef GC_DEBUG
        {
            f32 lox = sx[0], hix = sx[0], loy = sy[0], hiy = sy[0];
            u32 q;

            for (q = 1; q < n; q++) {
                if (sx[q] < lox) { lox = sx[q]; }
                if (sx[q] > hix) { hix = sx[q]; }
                if (sy[q] < loy) { loy = sy[q]; }
                if (sy[q] > hiy) { hiy = sy[q]; }
            }
            cover_note(2, (s32) (lox * 1000.0f / sGameWidth), (s32) (loy * 1000.0f / sGameHeight),
                       (s32) (hix * 1000.0f / sGameWidth), (s32) (hiy * 1000.0f / sGameHeight),
                       ((u32) (u8) clipped[0].r << 24) | ((u32) (u8) clipped[0].g << 16) |
                           ((u32) (u8) clipped[0].b << 8) | (u8) clipped[0].a);
        }
#endif
        /* Clipping can turn a triangle into a quad or a pentagon; a fan from
         * the first vertex covers any of them. */
        GX_Begin(GX_TRIANGLES, tex != NULL ? GX_VTXFMT3 : GX_VTXFMT2, (u16) ((n - 2) * 3));
        for (k = 1; k + 1 < n; k++) {
            u32 order[3];
            u32 j;

            order[0] = 0;
            order[1] = k;
            order[2] = k + 1;
            for (j = 0; j < 3; j++) {
                u32 c = order[j];

                GX_Position3f32(sx[c], sy[c], sz[c]);
                if (GC_PROJ_TINT) {
                    GX_Color4u8(0xFF, 0x00, 0xFF, 0xFF);
                } else {
                    GX_Color4u8((u8) clipped[c].r, (u8) clipped[c].g, (u8) clipped[c].b,
                                (u8) clipped[c].a);
                }
                if (tex != NULL) {
                    GX_TexCoord2f32(clipped[c].u, clipped[c].v);
                }
            }
        }
        GX_End();
#ifdef GC_DEBUG
        gGcTrisOut++;
#endif
    }

    /* Leave the pipeline as the interface path expects to find it. */
    gfx_set_2d_state();
}

/*
 * G_TEXRECT.
 *
 * The command carries only the screen rectangle and which tile to read; the
 * texture coordinates arrive in the two words that follow it, which is why the
 * walker hands them in rather than the handler reading ahead.
 *
 *   s, t       start coordinates, 10.5 fixed point
 *   dsdx, dtdy texel step per pixel, 5.10 fixed point
 *
 * The rectangle includes its lower-right pixel, so the quad runs one past it,
 * and the texture coordinate at that far corner follows from the step rather
 * than from a second pair of coordinates.
 */
static void gfx_tex_rect(u32 w0, u32 w1, u32 stWord, u32 dWord) {
    f32 lrx = (f32) ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 lry = (f32) (w0 & 0xFFF) / 4.0f;
    f32 ulx = (f32) ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 uly = (f32) (w1 & 0xFFF) / 4.0f;
    u32 tile = (w1 >> 24) & 7;
    const TileDesc *t = &sTiles[tile];
    f32 s = (f32) (s16) (stWord >> 16) / 32.0f;
    f32 tt = (f32) (s16) (stWord & 0xFFFF) / 32.0f;
    f32 dsdx = (f32) (s16) (dWord >> 16) / 1024.0f;
    f32 dtdy = (f32) (s16) (dWord & 0xFFFF) / 1024.0f;
    TileImage img;
    GXTexObj *tex;
    f32 w, h, u0, v0, u1, v1, uScale, vScale, uOff, vOff;

    if (lrx < ulx || lry < uly) {
        return; /* DKR emits zero-area rectangles for hidden interface pieces */
    }
    if (!tile_image(t, &img)) {
        return;
    }

    tex = texture_get(img.addr, t->fmt, t->siz, img.width, img.height, img.stride, img.swapOdd,
                      img.tlutAddr, cm_to_gx(t->cms), cm_to_gx(t->cmt), tex_filter());
    if (tex == NULL) {
        return;
    }

    w = lrx - ulx + 1.0f;
    h = lry - uly + 1.0f;

    uScale = sTexScaleS * tile_shift_scale(t->shifts);
    vScale = sTexScaleT * tile_shift_scale(t->shiftt);
    uOff = (f32) t->uls / 4.0f;
    vOff = (f32) t->ult / 4.0f;

    /* GX texture coordinates are normalised; the RDP's are in texels, before
     * the tile's shift and origin. */
    u0 = (s * uScale - uOff) / (f32) img.width;
    v0 = (tt * vScale - vOff) / (f32) img.height;
    u1 = ((s + w * dsdx) * uScale - uOff) / (f32) img.width;
    v1 = ((tt + h * dtdy) * vScale - vOff) / (f32) img.height;

#ifdef GC_DEBUG
    sCoverTexAddr = img.addr;
    sCoverFmtSiz = (t->fmt << 2) | t->siz;
    cover_note(3, (s32) (ulx * 1000.0f / sGameWidth), (s32) (uly * 1000.0f / sGameHeight),
               (s32) ((lrx + 1.0f) * 1000.0f / sGameWidth),
               (s32) ((lry + 1.0f) * 1000.0f / sGameHeight), 0xFFFFFFFFu);
#endif
    gfx_set_textured_state(tex);
    load_2d_projection();

    /*
     * White, not the primitive colour. A rectangle has no shade -- the RDP
     * rasterises no colour for one -- and the combiners DKR draws rectangles
     * with take their colour from PRIMITIVE or ENVIRONMENT, which now reach
     * the TEV as registers. Feeding the primitive colour in here as well was
     * the stand-in for a combiner and would double the tint.
     */
    GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
    GX_Position2f32(ulx, uly);
    GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
    GX_TexCoord2f32(u0, v0);
    GX_Position2f32(ulx + w, uly);
    GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
    GX_TexCoord2f32(u1, v0);
    GX_Position2f32(ulx + w, uly + h);
    GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
    GX_TexCoord2f32(u1, v1);
    GX_Position2f32(ulx, uly + h);
    GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
    GX_TexCoord2f32(u0, v1);
    GX_End();

    /* Leave the pipeline as the fill path expects to find it. */
    gfx_set_2d_state();
}

/*
 * G_MOVEWORD.
 *
 * The index is in bits 0..7 and the offset in bits 8..23 -- gMoveWd is
 * gImmp21(pkt, G_MOVEWORD, offset, index, data), and gImmp21 puts p0 at bit 8
 * and p1 at bit 0 (gbi.h). Reading the index from bits 16..23, which is where
 * a Dma1p command keeps its parameter byte, matches nothing: G_MW_SEGMENT
 * never fired, and the segment table stayed empty. It went unnoticed because
 * DKR's own rsp_segment passes `base + K0BASE`, so the addresses in the list
 * are absolute anyway and segment resolution had nothing to do.
 */
static void gfx_moveword(u32 w0, u32 w1) {
    u32 index = w0 & 0xFF;
    u32 offset = (w0 >> 8) & 0xFFFF;

    switch (index) {
        case G_MW_SEGMENT:
            /* gSPSegment passes the segment number times four. */
            gc_gfx_set_segment(offset / 4, w1);
            break;

        case G_MW_BILLBOARD:
            sBillboard = w1 != 0;
            break;

        case G_MW_FOG:
            /* gSPFogPosition packs both coefficients into the one word, high
             * half first. Signed: the offset is (500-min)*256/(max-min), which
             * goes negative as soon as the fog starts beyond 500. */
            sFogMul = (s16) (w1 >> 16);
            sFogOff = (s16) w1;
            break;

        case G_MW_MVPMATRIX:
            /* gSPSelectMatrixDKR passes the slot shifted up by six. */
            if ((w1 >> 6) < NUM_MATRICES) {
                sCurMatrix = w1 >> 6;
#ifdef GC_DEBUG
                gGcMtxSelects[sCurMatrix]++;
#endif
            }
            break;

        default:
            break;
    }
}

/*
 * G_MTX. gSPMatrixDKR is gSPMatrix with the slot shifted up by six, and
 * gSPMatrix is a Dma1p whose parameter byte sits at bits 16..23 -- so the slot
 * lands at bits 22..23.
 *
 * Loading a matrix also makes that slot current. The macro's name says "load"
 * and there is a separate gSPSelectMatrixDKR, so treating the two as
 * independent is the obvious reading -- and it is wrong. Three call sites in
 * camera.c only make sense the other way:
 *
 *   - mtx_cam_push (1429) uploads each object's own MVP into slot 1 and never
 *     selects it. Without the implicit select the object would be drawn with
 *     whatever matrix the scenery left behind, which is what happened here:
 *     every character model transformed to w <= 0 and was clipped away as
 *     "behind the camera".
 *   - mtx_pop (1552) restores the parent transform by re-uploading it into
 *     slot 1; if uploading did not select, popping would do nothing at all.
 *     Its other branch, for an empty stack, does use an explicit select.
 *   - mtx_head_push (1520) uploads the head matrix into slot 2 and then
 *     selects slot 1 back. That select is only needed because the upload just
 *     made slot 2 current.
 *
 * So gSPSelectMatrixDKR is for returning to a slot already uploaded, not for
 * arming an upload.
 */
static void gfx_matrix(u32 w0, u32 w1) {
    u32 slot = (w0 >> 22) & 3;
    const u16 *src = (const u16 *) segmented_to_virtual(w1);

    if (slot >= NUM_MATRICES) {
        return;
    }
    if ((u32) src < 0x80003000 || (u32) src + 64 >= 0x81800000) {
        return;
    }

    load_matrix(sMatrix[slot], src);
    sCurMatrix = slot;
#ifdef GC_DEBUG
    gGcMtxLoads[slot]++;
#endif
}

/*
 * G_MOVEMEM. The only one that matters here is the viewport, which is what
 * turns normalised coordinates into pixels; DKR rewrites it per camera, so
 * splitscreen depends on reading it rather than assuming a full screen.
 */
static void gfx_movemem(u32 w0, u32 w1) {
    const u8 *src;
    s16 scale[2], trans[2];

    if (((w0 >> 16) & 0xFF) != G_MV_VIEWPORT) {
        return;
    }

    src = (const u8 *) segmented_to_virtual(w1);
    if ((u32) src < 0x80003000 || (u32) src + 16 >= 0x81800000) {
        return;
    }

    scale[0] = (s16) ((src[0] << 8) | src[1]);
    scale[1] = (s16) ((src[2] << 8) | src[3]);
    trans[0] = (s16) ((src[8] << 8) | src[9]);
    trans[1] = (s16) ((src[10] << 8) | src[11]);

    /* Two bits of fraction, per Vp_t in gbi.h. */
    sVpScaleX = (f32) scale[0] / 4.0f;
    sVpScaleY = (f32) scale[1] / 4.0f;
    sVpTransX = (f32) trans[0] / 4.0f;
    sVpTransY = (f32) trans[1] / 4.0f;
}

/* ---- the walker ---------------------------------------------------------- */

/* Set once a list has been abandoned, so a frame that fails every frame says
 * so once rather than sixty times a second. */
static BOOL sDlComplained;

#ifdef GC_DEBUG
/* How many times each opcode appeared in the last walked list. The renderer is
 * being filled in one opcode at a time, and this says which one is worth
 * writing next -- guessing from what the intro screen looks like is how you
 * spend a day on a command the frame never issues. */
u32 gGcDlOpcodes[256];


/* What the last walked list contained. Reset per list, so these describe one
 * frame rather than the whole run: how many commands it took, how many of them
 * were the counted G_DMADL sublists, and how deep the nesting went. Without
 * them "no list was abandoned" cannot be told apart from "that path was never
 * reached". */
u32 gGcDlCommands;
u32 gGcDlDmaLists;
u32 gGcDlMaxDepth;
#endif

static void dl_abandon(const char *why, u32 w0, u32 w1) {
    if (!sDlComplained) {
        sDlComplained = TRUE;
        gc_log("dkr-gc: display list abandoned (%s) at %08x %08x\n", why, (unsigned) w0,
               (unsigned) w1);
    }
}

/*
 * One suspended list.
 *
 * `end` is what makes G_DMADL work: a list entered through it finishes after a
 * known number of commands rather than at a G_ENDDL, so the walker has to carry
 * that limit per level and restore the enclosing one on the way out.
 */
typedef struct {
    const GfxCmd *ret;
    const GfxCmd *end; /* NULL when the list is terminated by G_ENDDL */
} DlFrame;

static void run_dl(const GfxCmd *dl) {
    DlFrame stack[DL_STACK_DEPTH];
    /* Unsigned so the pops below are provably in range: the depth checks
     * return before they can underflow, but a signed index leaves the
     * compiler unable to see that. */
    u32 sp = 0;
    const GfxCmd *end = NULL; /* the task's own list ends with G_ENDDL */
    u32 budget = DL_MAX_COMMANDS;

    /* Fail open: until a list states its target, drawing goes to the screen. */
    sCimg = 0;
    sZimg = 0;

#ifdef GC_DEBUG
    gGcDlCommands = 0;
    gGcDlDmaLists = 0;
    gGcDlMaxDepth = 0;
    gGcFills = 0;
    gGcFillsBlend = 0;
    gGcFillsOffscreen = 0;
    gGcFogBatches = 0;
    gGcFogVerts = 0;
    memset(gGcDlOpcodes, 0, sizeof(gGcDlOpcodes));
    memset(gGcDlIgnored, 0, sizeof(gGcDlIgnored));
    gGcCimgCount = 0;
    gGcZimgCount = 0;
    gGcGeoSet = 0;
    gGcGeoClear = 0;
    gGcTexFormats = 0;
    gGcTileFormats = 0;
    gGcTexDbgCount = 0;
    gGcStateDbgCount = 0;
    gGcTexRawValid = 0;
    gGcTileModes = 0;
    gGcTexRects = 0;
    sTexRectWord = 6;
    sTexRectArea = 0;
    gGcCoverKind = 0;
    gGcCoverArea = 0;
    gGcCoverSeq = 0;
    gGcCoverTotal = 0;
    gGcCoverBox[0] = gGcCoverBox[1] = gGcCoverBox[2] = gGcCoverBox[3] = 0;
    gGcCoverCol = 0;
    gGcTexUnhandled = 0;
    gGcTexNoTlut = 0;
    gGcTrisIn = 0;
    gGcTrisBadIdx = 0;
    gGcTrisClipped = 0;
    gGcTrisBehind = 0;
    gGcTrisNearOnly = 0;
    gGcTrisCulled = 0;
    gGcTrisOut = 0;
    gGcTrinCmds = 0;
    gGcTrinHw = 0;
    gGcTrinCpu = 0;
    gGcProjNoW = 0;
    gGcTexNullDim = 0;
    gGcTexNullAddr = 0;
    gGcTexNullAlloc = 0;
    gGcTexHits = 0;
    gGcTexConverts = 0;
    gGcTexAsks = 0;
    gGcCpuBox[0] = gGcCpuBox[1] = gGcCpuBox[2] = gGcCpuBox[3] = 0;
    gGcCpuBoxMtx = 0;
    gGcCpuBoxTris = 0;
    gGcCpuBoxBb = 0;
    gGcCpuBoxAnchor[0] = gGcCpuBoxAnchor[1] = gGcCpuBoxAnchor[2] = gGcCpuBoxAnchor[3] = 0;
    gGcProjNotAffine = 0;
    gGcBbSeen = 0;
    gGcVtxLoaded = 0;
    gGcVtxMaxCount = 0;
    memset(gGcVtxByMtx, 0, sizeof(gGcVtxByMtx));
    memset(gGcVtxBehindByMtx, 0, sizeof(gGcVtxBehindByMtx));
    memset(gGcMtxLoads, 0, sizeof(gGcMtxLoads));
    memset(gGcMtxSelects, 0, sizeof(gGcMtxSelects));
#endif

    if (!dl_addr_ok(dl)) {
        dl_abandon("bad list address", (u32) dl, 0);
        return;
    }

    for (;;) {
        u32 w0, w1, op;

        /* A counted list that has run its commands. There is no G_ENDDL
         * coming, so the return has to happen here. */
        if (end != NULL && dl >= end) {
            if (sp == 0) {
                return;
            }
            sp--;
            dl = stack[sp].ret;
            end = stack[sp].end;
            continue;
        }

        if (budget-- == 0) {
            dl_abandon("ran past its command budget", (u32) dl, 0);
            return;
        }

        w0 = dl->w0;
        w1 = dl->w1;
        op = w0 >> 24;

#ifdef GC_DEBUG
        gGcDlCommands++;
        gGcDlOpcodes[op]++;
        if (sp > gGcDlMaxDepth) {
            gGcDlMaxDepth = sp;
        }
#endif

        dl++;

        switch (op) {
            case G_NOOP:
            case G_RDPFULLSYNC:
                break;

            case G_MTX:
                gfx_matrix(w0, w1);
                break;

            case G_VTX:
                gfx_vertex(w0, w1);
                break;

            case G_TRIN:
                gfx_triangles(w0, w1);
                break;

            case G_DL:
                /* A call, unless the flag byte says G_DL_NOPUSH, which makes it
                 * a jump. Either way the list it names ends with G_ENDDL. */
                {
                    const GfxCmd *target = (const GfxCmd *) segmented_to_virtual(w1);

                    if (!dl_addr_ok(target)) {
                        dl_abandon("G_DL through an unset segment", w0, w1);
                        return;
                    }
                    if (((w0 >> 16) & 0xFF) == G_DL_PUSH) {
                        if (sp >= DL_STACK_DEPTH) {
                            dl_abandon("call stack overflow", w0, w1);
                            return;
                        }
                        stack[sp].ret = dl;
                        stack[sp].end = end;
                        sp++;
                    }
                    dl = target;
                    end = NULL;
                }
                break;

            case G_DMADL:
                /*
                 * Run `count` commands from elsewhere, then carry on here.
                 *
                 * gDkrDmaDisplayList (include/f3ddkr.h) puts the count in bits
                 * 16..23 and the same count in bytes in bits 0..15; the two are
                 * checked against each other because a mismatch means the
                 * walker is reading something that is not a command, and saying
                 * so is worth more than rendering a wrong frame.
                 *
                 * Reading those bits as G_DL's push flag -- which is what they
                 * are for G_DL -- made every G_DMADL with a count other than
                 * zero look like a jump. The walker then never came back to the
                 * parent list and eventually ran into data.
                 */
                {
                    u32 count = (w0 >> 16) & 0xFF;
                    u32 bytes = w0 & 0xFFFF;
                    const GfxCmd *target = (const GfxCmd *) segmented_to_virtual(w1);

                    if (bytes != count * sizeof(GfxCmd)) {
                        dl_abandon("G_DMADL length does not match its count", w0, w1);
                        return;
                    }
                    if (count == 0) {
                        break;
                    }
                    if (!dl_addr_ok(target) || !dl_addr_ok(target + count - 1)) {
                        dl_abandon("G_DMADL through an unset segment", w0, w1);
                        return;
                    }
                    if (sp >= DL_STACK_DEPTH) {
                        dl_abandon("call stack overflow", w0, w1);
                        return;
                    }
                    stack[sp].ret = dl;
                    stack[sp].end = end;
                    sp++;
                    dl = target;
                    end = target + count;
#ifdef GC_DEBUG
                    gGcDlDmaLists++;
#endif
                }
                break;

            case G_ENDDL:
                if (sp == 0) {
                    return;
                }
                sp--;
                dl = stack[sp].ret;
                end = stack[sp].end;
                break;

            case G_RDPHALF_1:
            case G_RDPHALF_2:
#ifdef GC_DEBUG
                if (sTexRectWord < 6) {
                    gGcLastTexRect[sTexRectWord++] = w0;
                    gGcLastTexRect[sTexRectWord++] = w1;
                }
#endif
                break;

            case G_MOVEWORD:
                gfx_moveword(w0, w1);
                break;

            case G_TEXTURE:
                gfx_texture(w0, w1);
                break;

            case G_SETTIMG:
                gfx_set_timg(w0, w1);
                break;

            case G_SETCIMG: {
                u32 addr = (u32) segmented_to_virtual(w1);
#ifdef GC_DEBUG
                u32 k;

                for (k = 0; k < gGcCimgCount; k++) {
                    if (gGcCimg[k][0] == addr && gGcCimg[k][1] == w0) {
                        break;
                    }
                }
                if (k == gGcCimgCount && gGcCimgCount < GC_IMGDBG_MAX) {
                    gGcCimg[gGcCimgCount][0] = addr;
                    gGcCimg[gGcCimgCount][1] = w0;
                    gGcCimgCount++;
                }
#endif
                sCimg = addr;
                break;
            }

            case G_SETZIMG: {
                u32 addr = (u32) segmented_to_virtual(w1);
#ifdef GC_DEBUG
                u32 k;

                for (k = 0; k < gGcZimgCount; k++) {
                    if (gGcZimg[k] == addr) {
                        break;
                    }
                }
                if (k == gGcZimgCount && gGcZimgCount < GC_IMGDBG_MAX) {
                    gGcZimg[gGcZimgCount++] = addr;
                }
#endif
                sZimg = addr;
                break;
            }

            case G_SETGEOMETRYMODE:
#ifdef GC_DEBUG
                gGcGeoSet |= w1;
#endif
                sGeoMode |= w1;
                break;

            case G_CLEARGEOMETRYMODE:
#ifdef GC_DEBUG
                gGcGeoClear |= w1;
#endif
                sGeoMode &= ~w1;
                break;

            case G_SETFOGCOLOR:
                sFogColor.r = (u8) (w1 >> 24);
                sFogColor.g = (u8) (w1 >> 16);
                sFogColor.b = (u8) (w1 >> 8);
                sFogColor.a = (u8) w1;
                break;

            case G_SETCOMBINE:
                gfx_set_combine(w0, w1);
                break;

            case G_TEXRECT:
                /*
                 * The only three-word command in the list. Its texture
                 * coordinates live in the two G_RDPHALF commands that follow,
                 * so they are consumed here rather than dispatched; leaving
                 * them to the switch would draw the rectangle untextured and
                 * then execute two no-ops.
                 */
                if (end != NULL && dl + 2 > end) {
                    dl_abandon("G_TEXRECT truncated at the end of a sublist", w0, w1);
                    return;
                }
#ifdef GC_DEBUG
                gGcTexRects++;
                {
                    u32 area = (((w0 >> 12) & 0xFFF) - ((w1 >> 12) & 0xFFF)) *
                               ((w0 & 0xFFF) - (w1 & 0xFFF));

                    if (area > sTexRectArea) {
                        sTexRectArea = area;
                        gGcLastTexRect[0] = w0;
                        gGcLastTexRect[1] = w1;
                        gGcLastTexRect[2] = dl[0].w1;
                        gGcLastTexRect[3] = dl[1].w1;
                    }
                }
#endif
                gfx_tex_rect(w0, w1, dl[0].w1, dl[1].w1);
                dl += 2;
                break;

            case G_SETTILESIZE:
                {
                    u32 tile = (w1 >> 24) & 7;

                    sTiles[tile].uls = (w0 >> 12) & 0xFFF;
                    sTiles[tile].ult = w0 & 0xFFF;
                    sTiles[tile].lrs = (w1 >> 12) & 0xFFF;
                    sTiles[tile].lrt = w1 & 0xFFF;
                }
#ifdef GC_DEBUG
                gfx_note_tile_size(w0, w1);
                if (gGcTexRects == 0) {
                    gGcLastTileSize[0] = w0;
                    gGcLastTileSize[1] = w1;
                }
#endif
                break;

            case G_LOADBLOCK:
                /* The load is what commits an image address to a corner of
                 * TMEM. Reading it here rather than at G_SETTIMG matters: the
                 * list sets the image, describes a load tile, loads, and only
                 * then describes the tile it will draw through -- and the two
                 * tiles are different. w1 carries the tile in bits 24..26, the
                 * texel count in 12..23 and dxt in 0..11 (gDPLoadBlock). */
                tmem_note_load((w1 >> 24) & 7, w1 & 0xFFF, TRUE);
                break;

            case G_LOADTILE:
                /* A tile load walks rows and performs the odd-row exchange
                 * itself, so it never leaves the image needing one. */
                tmem_note_load((w1 >> 24) & 7, 0, FALSE);
                break;

            case G_SETSCISSOR:
                gfx_set_scissor(w0, w1);
                break;

            case G_SETFILLCOLOR:
                gfx_set_fill_color(w1);
                break;

            case G_FILLRECT:
                gfx_fill_rect(w0, w1);
                break;

            case G_SETTILE:
                {
                    u32 tile = (w1 >> 24) & 7;

                    sTiles[tile].fmt = (w0 >> 21) & 7;
                    sTiles[tile].siz = (w0 >> 19) & 3;
                    sTiles[tile].line = (w0 >> 9) & 0x1FF;
                    sTiles[tile].tmem = w0 & 0x1FF;
                    sTiles[tile].palette = (w1 >> 20) & 0xF;
                    sTiles[tile].cmt = (w1 >> 18) & 3;
                    sTiles[tile].maskt = (w1 >> 14) & 0xF;
                    sTiles[tile].shiftt = (w1 >> 10) & 0xF;
                    sTiles[tile].cms = (w1 >> 8) & 3;
                    sTiles[tile].masks = (w1 >> 4) & 0xF;
                    sTiles[tile].shifts = w1 & 0xF;
#ifdef GC_DEBUG
                    gGcTileModes |= 1u << (((w1 >> 18) & 3) << 2 | ((w1 >> 8) & 3));
#endif
                }
#ifdef GC_DEBUG
                gGcTileFormats |= 1u << (((w0 >> 21) & 7) << 2 | ((w0 >> 19) & 3));
                if (gGcTexRects == 0) {
                    gGcLastTile[0] = w0;
                    gGcLastTile[1] = w1;
                }
#endif
                break;

            case G_MOVEMEM:
                gfx_movemem(w0, w1);
                break;

            case G_SETPRIMCOLOR:
                /* w1 is RGBA8888; the two bytes of w0 are the LOD fraction and
                 * minimum level, which nothing here uses yet. */
                sPrimColor.r = (u8) (w1 >> 24);
                sPrimColor.g = (u8) (w1 >> 16);
                sPrimColor.b = (u8) (w1 >> 8);
                sPrimColor.a = (u8) w1;
                break;

            case G_SETOTHERMODE_H:
                {
                    /* w0 carries the shift at 8..15 and the length at 0..7;
                     * w1 is already shifted into place (gSPSetOtherMode). */
                    u32 sft = (w0 >> 8) & 0xFF;
                    u32 len = w0 & 0xFF;
                    u32 mask = len >= 32 ? 0xFFFFFFFFu : (((1u << len) - 1u) << sft);

                    sOtherModeH = (sOtherModeH & ~mask) | (w1 & mask);
                }
                break;

            case G_SETOTHERMODE_L:
                /* Same encoding as the high word: the shift at w0 bits 8..15,
                 * the length at 0..7, and w1 already shifted into place. */
                {
                    u32 sft = (w0 >> 8) & 0xFF;
                    u32 len = w0 & 0xFF;
                    u32 mask = len >= 32 ? 0xFFFFFFFFu : (((1u << len) - 1u) << sft);

                    sOtherModeL = (sOtherModeL & ~mask) | (w1 & mask);
                }
                break;

            case G_RDPSETOTHERMODE:
                /* Both words at once, and this -- not the two partial
                 * commands above -- is how DKR sets its render state: every
                 * entry of every draw table is a gsDPSetCombineLERP followed
                 * by a gsDPSetOtherMode (src/textures_sprites.h). The high
                 * word is the low 24 bits of w0; its bottom four bits are set
                 * by the microcode on the way past and mean nothing here
                 * (G_DKR_BLENDMASK, include/f3ddkr.h). */
                sOtherModeH = w0 & 0x00FFFFFF;
                sOtherModeL = w1;
                break;

            case G_SETBLENDCOLOR:
                sBlendColor.r = (u8) (w1 >> 24);
                sBlendColor.g = (u8) (w1 >> 16);
                sBlendColor.b = (u8) (w1 >> 8);
                sBlendColor.a = (u8) w1;
                /* The alpha test reads it, so the state has to be re-applied
                 * rather than waiting for the next render-mode change. */
                apply_render_mode();
                break;

            /*
             * G_PERSPNORMALIZE. On the N64 this hands the RSP a 16-bit scale
             * it multiplies w by before the perspective divide, purely to keep
             * the divide inside 16-bit range -- guPerspectiveF computes it as
             * an output parameter beside the matrix (src/camera.c:155), and it
             * is not part of the projection.
             *
             * The GP does the divide here, in floating point, from the matrix
             * alone. There is nothing to scale and nothing to lose, so this is
             * a genuine no-op rather than an unimplemented feature -- and it is
             * a case rather than a default so that the `ignored:` census keeps
             * meaning "not handled".
             */
            case G_PERSPNORMALIZE:
                break;

            case G_SETENVCOLOR:
                /* RGBA8888, as gDPSetEnvColor packs it. */
                sEnvColor.r = (u8) (w1 >> 24);
                sEnvColor.g = (u8) (w1 >> 16);
                sEnvColor.b = (u8) (w1 >> 8);
                sEnvColor.a = (u8) w1;
                break;

            case G_RDPLOADSYNC:
            case G_RDPPIPESYNC:
                /* Genuinely nothing to do: they order the RDP's own pipeline
                 * against itself, and there is no RDP here. Named rather than
                 * left to the default so they do not show up as work. */
                break;

            case G_CULLDL:
            case G_TRI1:
            default:
                /* Ignored rather than treated as an error: an unhandled
                 * command should degrade the picture, not stop the frame. But
                 * it is counted, so the heartbeat can say what is missing. */
#ifdef GC_DEBUG
                gGcDlIgnored[op]++;
#endif
                break;
        }
    }
}

/*
 * Entry point from the scheduler.
 *
 * `dl` is the graphics task's data_ptr, exactly as it would have been handed
 * to the RSP. The work is synchronous: when this returns, the scheduler
 * immediately tells the game its frame is done.
 */
#ifdef GC_DEBUG
u32 gGcDlIn, gGcDlOut;
u32 gGcCopyIn, gGcCopyOut;
#define MARK(x) ((x)++)
#else
#define MARK(x) ((void) 0)
#endif

void gc_gfx_run_dl(const void *dl) {
    if (!sGxReady) {
        return;
    }

    MARK(gGcDlIn);
    memset(sSegments, 0, sizeof(sSegments));

    /* The caches the GP keeps do not survive a frame's worth of pool
     * recycling; the pipeline state it holds does. Same pairing as
     * ref-sm64gc's gfx_gx_start_frame. */
    GX_InvVtxCache();
    GX_InvalidateTexAll();

    /* The list assumes a fresh pipeline: it sets its own scissor, colours and
     * modes, but nothing in it establishes the vertex format or the
     * projection. Those are re-applied here rather than once at init because
     * the game can change resolution between frames. */
    /* Texture state does not survive a list either: the addresses it refers to
     * are the game's pool, which it reuses between frames. */
    memset(sTiles, 0, sizeof(sTiles));
    memset(sLoads, 0, sizeof(sLoads));
    sLoadNext = 0;
    sOtherModeH = 0;
    sOtherModeL = OTHERMODE_L_DEFAULT;
    sTimgAddr = 0;
    sTexScaleS = 1.0f;
    sTexScaleT = 1.0f;
    sDecalBias = 0.0f;
    sCombine = kCombineDefault;
    sPrimColor.r = sPrimColor.g = sPrimColor.b = sPrimColor.a = 0xFF;
    sEnvColor.r = sEnvColor.g = sEnvColor.b = sEnvColor.a = 0xFF;
    sBlendColor.r = sBlendColor.g = sBlendColor.b = 0x00;
    sBlendColor.a = 0x64;
    sVertCount = 0;
    sVertBase = 0;
    sCurMatrix = 0;
    sBillboard = FALSE;
    memset(sMatrix, 0, sizeof(sMatrix));

    gfx_set_2d_state();
    gfx_set_2d_projection();

    run_dl((const GfxCmd *) dl);
    MARK(gGcDlOut);
}

void gc_gfx_copy_display(void *xfb) {
    if (!sGxReady) {
        return;
    }

    MARK(gGcCopyIn);

    /* Wait for the GP to finish the frame before queueing the copy, so this
     * call is where the CPU blocks rather than somewhere inside the copy. */
    GX_DrawDone();

    /*
     * GX_CopyDisp's clear argument only clears what the current write state
     * lets it write. With depth updates off it clears colour and leaves the
     * depth buffer untouched -- and every display list here ends in
     * gfx_set_2d_state, which turns depth off, so the depth buffer was never
     * cleared at all.
     *
     * The effect is not a black screen but something far more confusing: depth
     * accumulates across frames, so 3D geometry -- which tests GX_LEQUAL --
     * loses against depths written by earlier frames from a different camera
     * position and vanishes, while the 2D path, which does not test depth,
     * keeps drawing. Backgrounds and text stay; models and scenery come and
     * go.
     *
     * Forcing both updates on for the duration of the copy is the fix, and it
     * is the one aluzed/sm64-port-gc arrived at as well
     * (gfx_ogc_copy_to_xfb in src/pc/gfx/gfx_ogc.c). The next list re-applies
     * its own state, so nothing needs restoring here.
     */
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(xfb, GX_TRUE);
    GX_DrawDone();
    MARK(gGcCopyOut);
}

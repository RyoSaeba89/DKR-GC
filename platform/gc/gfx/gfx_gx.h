#ifndef GFX_GX_H
#define GFX_GX_H

/*
 * Deliberately free of PR headers.
 *
 * <ogc/gx.h> and <ogc/gu.h> define Mtx and Vtx, and so do PR/gbi.h and
 * PR/gu.h, with different meanings -- an N64 Mtx is a union of fixed-point
 * words, a GameCube Mtx is a 3x4 array of floats. A translation unit cannot
 * hold both. The renderer therefore lives entirely on the libogc side of that
 * line, and this header keeps the boundary in plain types so that the game
 * side, which lives on the PR side, can call across it.
 */

/* Brings GX up. Must run after gc_video_init, which chooses the render mode
 * everything here is sized from. */
void gc_gfx_init(void);

/* Interprets one F3DDKR display list and draws it. `dl` is the task's
 * data_ptr; the list is self-terminating, so no length is needed. */
void gc_gfx_run_dl(const void *dl);

/* Sets one entry of the display list's segment table. */
void gc_gfx_set_segment(unsigned int segment, unsigned int base);

/* Copies the finished embedded framebuffer out to `xfb`. Called by the video
 * layer at flip time; it lives here because only this file owns GX. */
void gc_gfx_copy_display(void *xfb);

#endif /* GFX_GX_H */

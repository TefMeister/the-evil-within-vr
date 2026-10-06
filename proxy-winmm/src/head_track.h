#pragma once

#include "head_k.h"

/*
 * Head tracking at run time (2026-10-06, /pd). Glue between three threads; the maths is in head_k.c.
 *
 *   draw threads (any):   head_track_sample(mvp)     measure the lens from a few perspective draws per frame
 *   game thread, Present: head_track_on_present()    settle this frame's lens (median of the samples)
 *   headset thread:       head_track_publish_pose(q) the newest head orientation (OpenXR quaternion, LOCAL space)
 *
 * stereo_afr.c reads the newest pose and the settled lens at every Present to build that frame's K, and keeps the
 * pose it used so the headset can be told which way the head pointed when the picture was drawn.
 *
 * Knob: HEADTRACK = 1 (needs OPENXR = 1; tew_xr.c then submits per-eye projection views).
 */

typedef struct { float x, y, z, w; } HeadQuat;

/* Reads HEADTRACK once. */
void head_track_init(void);
int head_track_on(void);

/* Any thread, per draw: offers a perspective MVP to the lens estimate. Cheap, never blocks, samples a few per frame.
 * Square lenses are not sampled: those are shadow-map and cube-map cameras, never the player's view. */
void head_track_sample(const Mat4 *mvp);

/* Any thread, per draw: 1 when this MVP was made with a DIFFERENT camera than the main one (a shadow map, a
 * reflection), judged by its lens shape. Such draws must not be turned by the head or shifted per eye. 0 when it
 * matches, when it cannot be judged (uneven model scale), or before the main lens is known. */
int head_track_other_camera(const Mat4 *mvp);

/* Game thread, at Present: settles the lens from this frame's samples. Returns 1 once a lens has ever been measured. */
int head_track_on_present(void);

/* The settled lens (any thread; copied under a lock). 0 until one has been measured. */
int head_track_lens(HeadLens *out);

/* Headset thread: the newest head orientation. Game thread: read it (identity until the first one arrives). */
void head_track_publish_pose(HeadQuat q);
HeadQuat head_track_latest_pose(void);

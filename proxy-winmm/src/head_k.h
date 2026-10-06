#pragma once

#include "camera.h"

/*
 * HEAD ROTATION IN CLIP SPACE (2026-10-06, /pd, dev PC). Pure maths, no Windows, no state: tools/head_k_test.c
 * links this file as shipped.
 *
 * The patch only ever sees each draw's full MVP = P * V * Mmodel, never P or V alone (dossier §6). Turning the head
 * by R (in the game's view space) must give P * R^T * V * Mmodel, which is
 *     K_head * MVP     with     K_head = P * R4^T * P^-1
 * one matrix for every draw, IF the game's lens P is known. It is measured, not guessed: every perspective MVP
 * carries it. Its rows are
 *     row0 = sx*v0 + cx*v2,  row1 = sy*v1 + cy*v2,  row2 = A*v2 (+ B in .w),  row3 = v2
 * where v0, v1, v2 are the camera's right/up/forward axes in model space, all of one length when the model has a
 * uniform scale. So, from xyz parts alone:
 *     cx = row0.row3 / |row3|^2      sx = |row0 - cx*row3| / |row3|      (likewise cy, sy from row1)
 *     A  = row2.row3 / |row3|^2      (and row2 - A*row3 must be ~0)
 * The uniform scale cancels. A draw whose model is scaled unevenly fails the checks (axes not square, row2 not
 * parallel to row3) and is not used. B never matters for a rotation: it cancels in P * R4^T * P^-1.
 *
 * VIEW FRAME. "View space" here is the frame the clip axes define: x right on screen, y up, z forward (= clip w).
 * That is left-handed, so an OpenXR quaternion (right-handed, -z forward) converts as (x, y, z, w) -> (-x, -y, z, w),
 * as in Alan Wake (headtrack.h there).
 */

typedef struct {
    float sx, sy;   /* lens scale: ndc per unit of x/z and y/z (sx = 1/tan(half horizontal fov) when centred) */
    float cx, cy;   /* off-centre terms: ndc of a point straight ahead (0 for a centred lens) */
    float a;        /* depth term A in clip z = A*z + B (about 1 for a far plane far away) */
} HeadLens;

typedef float HeadMat3[3][3];

/* Measure the lens from one perspective MVP. Returns 1 and fills *out when the draw passes the checks (uniform
 * model scale, sane values), else 0. */
int head_lens_from_mvp(const Mat4 *mvp, HeadLens *out);

/* Rotation (game view space) from an OpenXR orientation quaternion. Normalises; identity for a zero quaternion. */
void head_rot_from_xr_quat(float qx, float qy, float qz, float qw, HeadMat3 out);

/* K_head = P * R4^T * P^-1 for the measured lens and head rotation r. Identity rotation gives the identity. */
Mat4 head_k_build(const HeadLens *lens, const HeadMat3 r);

/* The lens as OpenXR fov tangents (left/down negative): ndc = +-1 at x/z = (+-1 - cx)/sx. Returns 0 if degenerate. */
int head_lens_tangents(const HeadLens *lens, float *l, float *r, float *u, float *d);

/* Where the 3D picture sits in a w x h back buffer: the lens covers ndc -1..1 of the VIEWPORT, and The Evil Within
 * draws a letterboxed viewport (black bars). Assumes the viewport is centred and fills the width (letterbox) or the
 * height (pillarbox), with its aspect sy/sx. Writes x, y, width, height in pixels. */
void head_lens_image_rect(const HeadLens *lens, unsigned w, unsigned h, int rect[4]);

#pragma once

/*
 * Generic 4x4 matrix helpers - Task 6 repurposing (per the rewritten Task 6
 * brief): this file WAS meant to hold a camera_set_override()/view-proj
 * design targeting a single shared view/projection upload path. That design
 * is dead - Task 4/5 discovery proved this engine has no such path (every
 * draw carries its own per-object MVP in vertex-shader cb0, see
 * mvptable.c/mvp_patch.c) - so this file now holds only plain, engine-
 * agnostic 4x4 matrix math with no state and no D3D11 dependency at all.
 *
 * Storage/multiply convention (matches how the engine's own mvpmatrixx/y/z/w
 * constant-buffer rows work, per Task 4's shader-disassembly finding):
 * row-major, Mat4.m[row][col], transforming a COLUMN vector as
 * pos' = M * pos, i.e. pos'.row_i = dot(M.m[row_i], pos). A shader's 4
 * mvpmatrix{x,y,z,w} float4 constants ARE, in order, rows 0..3 of M here.
 * mat4_mul(a, b) computes ordinary matrix multiplication a*b (apply b
 * first, then a) - so to left-multiply a draw's existing mvp by a test/eye
 * matrix K (mvp' = K * mvp), call mat4_mul(K, mvp).
 *
 * mat4_mul() and mat4_translation_local() are declared for Task 7/8 to
 * reuse (per-eye K_eye construction); mvp_patch.c (Task 6) itself only
 * uses mat4_identity(), mat4_mul(), and mat4_rotation_y() - Task 6's test
 * rotation needs no translation.
 */

typedef struct {
    float m[4][4];
} Mat4;

/* Returns the 4x4 identity matrix. */
Mat4 mat4_identity(void);

/* Ordinary 4x4 matrix multiplication, result = a * b (row-major, per the
 * convention above - apply b to a vector first, then a). */
Mat4 mat4_mul(Mat4 a, Mat4 b);

/* Builds a rotation-about-Y (yaw) matrix for `degrees` degrees, in the same
 * row-major/column-vector convention as every other function here. Task 6
 * uses this directly as its test matrix K (TEWVR_TEST_YAW); a real
 * per-eye K_eye (Task 7/8) is expected to layer translation/projection
 * around a matrix built the same way, not necessarily this exact function. */
Mat4 mat4_rotation_y(float degrees);

/* Builds a translation matrix that translates by `dx` along `view`'s own
 * local +X axis (read as `view`'s row 0 xyz - the convention a row-major
 * world-to-view matrix uses for its "right" basis vector - normalized, then
 * scaled by `dx` and placed in a plain 4x4 translation matrix's column 3).
 * NOT used by Task 6 itself (K here is a pure yaw rotation with no
 * translation) - provided only to satisfy this file's Task 6 brief
 * interface contract for Task 7/8's per-eye IPD offset to reuse once a
 * real view matrix (as opposed to a full per-draw MVP) is available to
 * pass in. Task 7/8 should re-verify this axis-extraction convention
 * against whatever view matrix they actually obtain before relying on it -
 * it is unexercised and unverified by any test in Task 6. */
Mat4 mat4_translation_local(const Mat4 *view, float dx);

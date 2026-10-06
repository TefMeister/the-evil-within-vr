#include "head_k.h"

#include <math.h>

/* Checks a draw must pass before its lens is believed (see head_k.h). */
#define HEAD_LENS_PERP_TOL   1e-3f   /* |e0.e1| / (|e0||e1|): camera right and up axes must be square */
#define HEAD_LENS_PAR_TOL    1e-3f   /* |row2 - A*row3| / |row2|: the depth row must be the forward axis */
#define HEAD_LENS_MIN_S      0.2f    /* lens scale range: about a 157 to 6 degree field of view */
#define HEAD_LENS_MAX_S      20.0f
#define HEAD_LENS_MAX_C      0.9f    /* off-centre terms beyond this are not a game camera */
#define HEAD_LENS_MIN_A      0.5f
#define HEAD_LENS_MAX_A      2.0f

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

int head_lens_from_mvp(const Mat4 *mvp, HeadLens *out) {
    const float *r0 = mvp->m[0], *r1 = mvp->m[1], *r2 = mvp->m[2], *r3 = mvp->m[3];
    float ll = dot3(r3, r3), e0[3], e1[3], d2[3], n0, n1, cx, cy, a, sx, sy, l;
    int i;

    if (!(ll > 0.0f) || !isfinite(ll)) return 0;
    cx = dot3(r0, r3) / ll;
    cy = dot3(r1, r3) / ll;
    a = dot3(r2, r3) / ll;
    for (i = 0; i < 3; ++i) {
        e0[i] = r0[i] - cx * r3[i];
        e1[i] = r1[i] - cy * r3[i];
        d2[i] = r2[i] - a * r3[i];
    }
    n0 = sqrtf(dot3(e0, e0));
    n1 = sqrtf(dot3(e1, e1));
    l = sqrtf(ll);
    if (!(n0 > 0.0f) || !(n1 > 0.0f)) return 0;
    if (fabsf(dot3(e0, e1)) > HEAD_LENS_PERP_TOL * n0 * n1) return 0;
    if (sqrtf(dot3(d2, d2)) > HEAD_LENS_PAR_TOL * sqrtf(dot3(r2, r2))) return 0;
    sx = n0 / l;
    sy = n1 / l;
    if (!(sx >= HEAD_LENS_MIN_S && sx <= HEAD_LENS_MAX_S && sy >= HEAD_LENS_MIN_S && sy <= HEAD_LENS_MAX_S)) return 0;
    if (fabsf(cx) > HEAD_LENS_MAX_C || fabsf(cy) > HEAD_LENS_MAX_C) return 0;
    if (!(a >= HEAD_LENS_MIN_A && a <= HEAD_LENS_MAX_A)) return 0;
    out->sx = sx; out->sy = sy; out->cx = cx; out->cy = cy; out->a = a;
    return 1;
}

void head_rot_from_xr_quat(float qx, float qy, float qz, float qw, HeadMat3 o) {
    float n = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw), x, y, z, w;
    if (!(n > 0.0f)) {
        int i, j;
        for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) o[i][j] = i == j ? 1.0f : 0.0f;
        return;
    }
    x = -qx / n; y = -qy / n; z = qz / n; w = qw / n;   /* OpenXR (right-handed, -z forward) -> game view frame */
    o[0][0] = 1 - 2 * (y * y + z * z); o[0][1] = 2 * (x * y - z * w);     o[0][2] = 2 * (x * z + y * w);
    o[1][0] = 2 * (x * y + z * w);     o[1][1] = 1 - 2 * (x * x + z * z); o[1][2] = 2 * (y * z - x * w);
    o[2][0] = 2 * (x * z - y * w);     o[2][1] = 2 * (y * z + x * w);     o[2][2] = 1 - 2 * (x * x + y * y);
}

Mat4 head_k_build(const HeadLens *L, const HeadMat3 r) {
    /* P with B = 1 (B cancels for a rotation) and its exact inverse. */
    Mat4 p = mat4_identity(), pinv = mat4_identity(), rt = mat4_identity();
    int i, j;
    p.m[0][0] = L->sx; p.m[0][2] = L->cx;
    p.m[1][1] = L->sy; p.m[1][2] = L->cy;
    p.m[2][2] = L->a;  p.m[2][3] = 1.0f;
    p.m[3][2] = 1.0f;  p.m[3][3] = 0.0f;
    pinv.m[0][0] = 1.0f / L->sx; pinv.m[0][3] = -L->cx / L->sx;
    pinv.m[1][1] = 1.0f / L->sy; pinv.m[1][3] = -L->cy / L->sy;
    pinv.m[2][2] = 0.0f;         pinv.m[2][3] = 1.0f;          /* view z = clip w */
    pinv.m[3][2] = 1.0f;         pinv.m[3][3] = -L->a;         /* the homogeneous 1 = clip z - A * clip w */
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) rt.m[i][j] = r[j][i];   /* R^T */
    return mat4_mul(p, mat4_mul(rt, pinv));
}

int head_lens_tangents(const HeadLens *L, float *l, float *r, float *u, float *d) {
    if (!(L->sx > 0.0f) || !(L->sy > 0.0f)) return 0;
    *l = (-1.0f - L->cx) / L->sx;
    *r = (1.0f - L->cx) / L->sx;
    *d = (-1.0f - L->cy) / L->sy;
    *u = (1.0f - L->cy) / L->sy;
    return 1;
}

void head_lens_image_rect(const HeadLens *L, unsigned w, unsigned h, int rect[4]) {
    float vp = (L->sx > 0.0f) ? L->sy / L->sx : 0.0f;   /* viewport width / height */
    float bb = h ? (float)w / (float)h : 0.0f;
    rect[0] = 0; rect[1] = 0; rect[2] = (int)w; rect[3] = (int)h;
    if (!(vp > 0.0f) || !(bb > 0.0f)) return;
    if (vp > bb * 1.01f) {                 /* wider than the buffer: bars top and bottom */
        int vh = (int)((float)w / vp + 0.5f);
        rect[1] = ((int)h - vh) / 2;
        rect[3] = vh;
    } else if (vp < bb * 0.99f) {          /* narrower: bars left and right */
        int vw = (int)((float)h * vp + 0.5f);
        rect[0] = ((int)w - vw) / 2;
        rect[2] = vw;
    }
}

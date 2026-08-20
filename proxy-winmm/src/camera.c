#include "camera.h"

#include <math.h>
#include <stddef.h>

Mat4 mat4_identity(void) {
    Mat4 r;
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r.m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    return r;
}

Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++) {
                sum += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = sum;
        }
    }
    return r;
}

Mat4 mat4_rotation_y(float degrees) {
    Mat4 r = mat4_identity();
    double rad = (double)degrees * 3.14159265358979323846 / 180.0;
    float c = (float)cos(rad);
    float s = (float)sin(rad);

    /* Standard right-handed rotation about the Y (up) axis, row-major,
     * column-vector convention (see camera.h's file comment): rotates the
     * X/Z plane, leaves Y and W untouched. This is a pure test rotation
     * (TEWVR_TEST_YAW) applied as a LEFT-multiply directly onto each
     * draw's already-projected per-object MVP (mvp' = K * mvp) - not a
     * physically-correct view-space camera yaw (that would need to be
     * applied before projection, which Task 7/8's K_eye construction
     * handles via P_eye * T_eye * inverse(P), per the discovery ledger).
     * Applied post-projection like this, it still visibly rotates the
     * rendered world for a qualitative go/no-go check (Task 6 Step 3) -
     * the exact visual character (which direction, any perspective skew)
     * is for the human-witnessed test to confirm, not asserted here. */
    r.m[0][0] = c;
    r.m[0][2] = s;
    r.m[2][0] = -s;
    r.m[2][2] = c;

    return r;
}

Mat4 mat4_translation_local(const Mat4 *view, float dx) {
    Mat4 r = mat4_identity();
    float x, y, z, len;

    if (view == NULL) {
        return r;
    }

    /* "Local +X axis" = row 0 of `view`, per this file's row-major
     * world-to-view convention (see camera.h - unverified/unused by Task 6,
     * see that header's warning). */
    x = view->m[0][0];
    y = view->m[0][1];
    z = view->m[0][2];
    len = (float)sqrt((double)(x * x + y * y + z * z));
    if (len > 0.0001f) {
        x /= len;
        y /= len;
        z /= len;
    } else {
        x = 1.0f;
        y = 0.0f;
        z = 0.0f;
    }

    r.m[0][3] = x * dx;
    r.m[1][3] = y * dx;
    r.m[2][3] = z * dx;

    return r;
}

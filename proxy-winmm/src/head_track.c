#include "head_track.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "config.h"
#include "log.h"

#define HT_SAMPLE_EVERY     16u     /* offer one perspective draw in this many to the lens estimate */
#define HT_SAMPLES_MAX      32      /* samples kept per frame */
#define HT_SAMPLES_MIN      5       /* a frame with fewer keeps the previous lens */
#define HT_LOG_CHANGE       0.01f   /* log the lens again when sx or sy moves by more than this fraction */
#define HT_RAD_TO_DEG       57.29578f
#define HT_SQUARE_TOL       0.03f   /* |sy/sx - 1| below this is a square lens: shadow or cube map, not sampled */
#define HT_OTHER_TOL        0.03f   /* a draw whose sy/sx differs from the main lens by more is another camera */

static int g_on;
static volatile LONG g_draws;
static volatile LONG g_busy;                  /* try-lock over g_samples / g_nsamples */
static HeadLens g_samples[HT_SAMPLES_MAX];
static int g_nsamples;
static CRITICAL_SECTION g_cs;                 /* the settled lens and the newest pose */
static int g_have_lens;
static HeadLens g_lens, g_logged;
static HeadQuat g_pose = { 0.0f, 0.0f, 0.0f, 1.0f };
static volatile float g_main_aspect;         /* sy/sx of the settled lens, 0 until known (read lock-free per draw) */
static volatile LONG g_other_draws;
static volatile LONG g_world_since;           /* draws the main lens matched since the last report: turned + shifted */
static volatile LONG g_other_since;           /* draws left alone as other cameras since the last report */
static volatile float g_other_aspect_lo, g_other_aspect_hi;   /* their shapes (racy min/max, for the log only) */
static int g_group_size;

void head_track_init(void) {
    char buf[16];
    DWORD n = tewvr_getenv("TEWVR_HEADTRACK", buf, sizeof buf);
    InitializeCriticalSection(&g_cs);
    g_on = n > 0 && n < sizeof buf && atoi(buf) != 0;
    log_msg("head_track: %s", g_on ? "ON - the head turns the picture (needs OPENXR = 1)" : "HEADTRACK unset/0 -> off");
}

int head_track_on(void) { return g_on; }

void head_track_sample(const Mat4 *mvp) {
    HeadLens l;
    if (!g_on || ((ULONG)InterlockedIncrement(&g_draws) % HT_SAMPLE_EVERY) != 0) return;
    if (!head_lens_from_mvp(mvp, &l)) return;
    if (fabsf(l.sy / l.sx - 1.0f) < HT_SQUARE_TOL) return;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;   /* another thread is in: skip, never wait */
    if (g_nsamples < HT_SAMPLES_MAX) g_samples[g_nsamples++] = l;
    InterlockedExchange(&g_busy, 0);
}

int head_track_on_present(void) {
    HeadLens s[HT_SAMPLES_MAX], l;
    int n, have;
    if (!g_on) return 0;
    while (InterlockedCompareExchange(&g_busy, 1, 0) != 0) YieldProcessor();
    n = g_nsamples;
    memcpy(s, g_samples, (size_t)n * sizeof s[0]);
    g_nsamples = 0;
    InterlockedExchange(&g_busy, 0);
    if (n >= HT_SAMPLES_MIN && (g_group_size = head_lens_pick_main(s, n, HT_OTHER_TOL, &l)) >= HT_SAMPLES_MIN) {
        EnterCriticalSection(&g_cs);
        g_lens = l;
        g_have_lens = 1;
        LeaveCriticalSection(&g_cs);
        g_main_aspect = l.sy / l.sx;
        if (fabsf(l.sx - g_logged.sx) > HT_LOG_CHANGE * l.sx || fabsf(l.sy - g_logged.sy) > HT_LOG_CHANGE * l.sy) {
            g_logged = l;
            log_msg("head_track: lens sx=%.4f sy=%.4f cx=%.4f cy=%.4f A=%.5f (median of the %d draws in the biggest same-shape group) = %.1f x %.1f deg, "
                    "picture aspect %.3f", (double)l.sx, (double)l.sy, (double)l.cx, (double)l.cy, (double)l.a, g_group_size,
                    2.0 * atan(1.0 / l.sx) * HT_RAD_TO_DEG, 2.0 * atan(1.0 / l.sy) * HT_RAD_TO_DEG,
                    (double)(l.sy / l.sx));
            log_msg("head_track: %ld draw(s) from other cameras (shadow maps etc.) left alone so far", (long)g_other_draws);
        }
    }
    EnterCriticalSection(&g_cs);
    have = g_have_lens;
    LeaveCriticalSection(&g_cs);
    return have;
}

int head_track_other_camera(const Mat4 *mvp) {
    HeadLens l;
    float main = g_main_aspect;
    if (!(main > 0.0f) || !head_lens_from_mvp(mvp, &l)) return 0;
    if (fabsf(l.sy / l.sx - main) <= HT_OTHER_TOL * main) {
        InterlockedIncrement(&g_world_since);
        return 0;
    }
    InterlockedIncrement(&g_other_draws);
    InterlockedIncrement(&g_other_since);
    {
        float asp = l.sy / l.sx;
        if (g_other_aspect_lo == 0.0f || asp < g_other_aspect_lo) g_other_aspect_lo = asp;
        if (asp > g_other_aspect_hi) g_other_aspect_hi = asp;
    }
    return 1;
}

void head_track_report(void) {
    LONG world, other;
    float lo = g_other_aspect_lo, hi = g_other_aspect_hi;
    if (!g_on) return;
    world = InterlockedExchange(&g_world_since, 0);
    other = InterlockedExchange(&g_other_since, 0);
    g_other_aspect_lo = g_other_aspect_hi = 0.0f;
    log_msg("head_track: since last line %ld draw(s) TURNED (main lens shape %.3f), %ld left alone as other cameras "
            "(shapes %.3f..%.3f); in gameplay a big left-alone share means the world is not being turned",
            (long)world, (double)g_main_aspect, (long)other, (double)lo, (double)hi);
}

int head_track_lens(HeadLens *out) {
    int have;
    if (!g_on) return 0;
    EnterCriticalSection(&g_cs);
    have = g_have_lens;
    if (have) *out = g_lens;
    LeaveCriticalSection(&g_cs);
    return have;
}

void head_track_publish_pose(HeadQuat q) {
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    g_pose = q;
    LeaveCriticalSection(&g_cs);
}

HeadQuat head_track_latest_pose(void) {
    HeadQuat q;
    EnterCriticalSection(&g_cs);
    q = g_pose;
    LeaveCriticalSection(&g_cs);
    return q;
}

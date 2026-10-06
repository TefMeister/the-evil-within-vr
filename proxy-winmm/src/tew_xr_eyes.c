/* tew_xr_eyes.c - hands each eye's picture from the game's device to the headset thread's device (2026-10-06,
 * /pd reader, "own device" build). See tew_xr_internal.h for the handoff rules.
 *
 * Why: the first build ran the whole OpenXR frame loop on the game's render thread, on the game's own device. Live,
 * the game stalled after the first headset frame inside the runtime's swapchain calls (the runtime used the game's
 * immediate context too). Now the game thread only copies its back buffer into a shared texture and never calls
 * OpenXR or waits for the headset thread; Alan Wake's working shape (own device, own thread) does the rest. */
#include "tew_xr_internal.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

#define EYE_MAX_DIM      8192u   /* refuse absurd sizes rather than allocate them */
#define EYE_SYNC_KEY     0ull    /* both devices use key 0: the keyed mutex acts as a plain lock */
#define EYE_NO_WAIT_MS   0u      /* neither side ever waits for the lock */
#define EYE_LOG_EVERY    600ul   /* one "eye texture busy" line per this many skips */

enum { FAM_NONE = 0, FAM_RGBA8, FAM_BGRA8, FAM_BGRX8, FAM_RGB10A2, FAM_RGBA16F, FAM_R11G11B10 };

typedef struct { DXGI_FORMAT f; int fam; int srgb; } FmtInfo;

static const FmtInfo g_fmts[] = {
    { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, FAM_RGBA8, 1 },   { DXGI_FORMAT_R8G8B8A8_UNORM, FAM_RGBA8, 0 },
    { DXGI_FORMAT_R8G8B8A8_TYPELESS, FAM_RGBA8, 0 },
    { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, FAM_BGRA8, 1 },   { DXGI_FORMAT_B8G8R8A8_UNORM, FAM_BGRA8, 0 },
    { DXGI_FORMAT_B8G8R8A8_TYPELESS, FAM_BGRA8, 0 },
    { DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, FAM_BGRX8, 1 },   { DXGI_FORMAT_B8G8R8X8_UNORM, FAM_BGRX8, 0 },
    { DXGI_FORMAT_B8G8R8X8_TYPELESS, FAM_BGRX8, 0 },
    { DXGI_FORMAT_R10G10B10A2_UNORM, FAM_RGB10A2, 0 },   { DXGI_FORMAT_R10G10B10A2_TYPELESS, FAM_RGB10A2, 0 },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, FAM_RGBA16F, 0 },  { DXGI_FORMAT_R16G16B16A16_TYPELESS, FAM_RGBA16F, 0 },
    { DXGI_FORMAT_R11G11B10_FLOAT, FAM_R11G11B10, 0 },
};

static int fmt_family(int64_t f) {
    size_t i;
    for (i = 0; i < sizeof g_fmts / sizeof g_fmts[0]; ++i)
        if ((int64_t)g_fmts[i].f == f) return g_fmts[i].fam;
    return FAM_NONE;
}

int64_t tew_eyes_pick_format(const int64_t *fmts, uint32_t n, DXGI_FORMAT bb) {
    int fam = fmt_family(bb), pass;
    uint32_t i;
    size_t k;
    if (fam == FAM_NONE) return 0;
    for (pass = 0; pass < 2; ++pass)              /* _SRGB member first: the game's output is already gamma-encoded */
        for (i = 0; i < n; ++i)
            for (k = 0; k < sizeof g_fmts / sizeof g_fmts[0]; ++k)
                if ((int64_t)g_fmts[k].f == fmts[i] && g_fmts[k].fam == fam && g_fmts[k].srgb == (pass == 0))
                    return fmts[i];
    return 0;
}

/* ---- Published set: written by the game thread, read by the headset thread, under g_pub_cs ------------------- */
static CRITICAL_SECTION g_pub_cs;
static volatile LONG g_pub_cs_ready;
static struct { LONG gen; HANDLE h[2]; UINT w, h_; DXGI_FORMAT fmt; } g_pub;
static volatile LONG g_serial[2];          /* bumped by the game after each finished copy into eye i */
static volatile unsigned long g_st_game_copies, g_st_xr_copies, g_st_xr_busy, g_st_open_fail;
static volatile long g_st_open_hr;

void tew_share_init(void) {
    if (InterlockedCompareExchange(&g_pub_cs_ready, 1, 0) == 0) InitializeCriticalSection(&g_pub_cs);
}

/* ---- Game thread ------------------------------------------------------------------------------------------- */
typedef struct { ID3D11Texture2D *tex[2]; IDXGIKeyedMutex *km[2]; HANDLE h[2]; } GameSet;

static GameSet g_cur, g_old;               /* g_old: the previous set, kept until g_cur is published */
static UINT g_gw, g_gh;
static DXGI_FORMAT g_gfmt;
static LONG g_ggen;
static int g_unpublished;                  /* g_cur has not been handed to the headset thread yet */
static unsigned long g_busy_skips;

static void set_release(GameSet *s) {
    int i;
    for (i = 0; i < 2; ++i) {
        if (s->km[i]) IDXGIKeyedMutex_Release(s->km[i]);
        if (s->tex[i]) ID3D11Texture2D_Release(s->tex[i]);
    }
    memset(s, 0, sizeof *s);
}

static int game_make(const D3D11_TEXTURE2D_DESC *bd) {
    D3D11_TEXTURE2D_DESC d;
    GameSet n;
    int i;
    memset(&n, 0, sizeof n);
    if (bd->Width == 0 || bd->Height == 0 || bd->Width > EYE_MAX_DIM || bd->Height > EYE_MAX_DIM) return 0;
    memset(&d, 0, sizeof d);
    d.Width = bd->Width;
    d.Height = bd->Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = bd->Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (i = 0; i < 2; ++i) {
        IDXGIResource *r = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D(g_d3d.dev, &d, NULL, &n.tex[i])) ||
            FAILED(ID3D11Texture2D_QueryInterface(n.tex[i], &IID_IDXGIKeyedMutex, (void **)&n.km[i])) ||
            FAILED(ID3D11Texture2D_QueryInterface(n.tex[i], &IID_IDXGIResource, (void **)&r)) ||
            FAILED(IDXGIResource_GetSharedHandle(r, &n.h[i]))) {
            if (r) IDXGIResource_Release(r);
            log_msg("tew_xr: could not make the shared eye texture (%ux%u format %d)", d.Width, d.Height, (int)d.Format);
            set_release(&n);
            return 0;
        }
        IDXGIResource_Release(r);
    }
    if (g_unpublished) set_release(&g_cur);      /* the headset thread never saw it */
    else { set_release(&g_old); g_old = g_cur; }  /* it may have it open; keep ours until the new set is published */
    g_cur = n;
    g_gw = d.Width;
    g_gh = d.Height;
    g_gfmt = d.Format;
    g_ggen++;
    g_unpublished = 1;
    log_msg("tew_xr: sharing %ux%u eye images (format %d) with the headset thread", d.Width, d.Height, (int)d.Format);
    return 1;
}

static void game_try_publish(void) {
    if (!g_unpublished || !g_pub_cs_ready || !TryEnterCriticalSection(&g_pub_cs)) return;   /* never wait */
    g_pub.gen = g_ggen;
    g_pub.h[0] = g_cur.h[0];
    g_pub.h[1] = g_cur.h[1];
    g_pub.w = g_gw;
    g_pub.h_ = g_gh;
    g_pub.fmt = g_gfmt;
    LeaveCriticalSection(&g_pub_cs);
    g_unpublished = 0;
    set_release(&g_old);
}

/* Head pose each eye's picture was drawn with (OpenXR quaternion). Written and read only while that eye's keyed
 * mutex is held, so a picture and its pose always travel together. */
static float g_eye_pose[2][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };

int tew_share_capture(IDXGISwapChain *sc, int eye, const float pose[4]) {
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC bd;
    int i, wrote = 0, first = eye < 0 ? 0 : eye, last = eye < 0 ? 1 : eye;
    if (!sc || FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)) || !bb) return 0;
    ID3D11Texture2D_GetDesc(bb, &bd);
    if ((bd.Width != g_gw || bd.Height != g_gh || bd.Format != g_gfmt || !g_cur.tex[0]) && !game_make(&bd)) {
        ID3D11Texture2D_Release(bb);
        return 0;
    }
    game_try_publish();
    for (i = first; i <= last; ++i) {
        if (IDXGIKeyedMutex_AcquireSync(g_cur.km[i], EYE_SYNC_KEY, EYE_NO_WAIT_MS) != S_OK) {   /* WAIT_TIMEOUT: busy */
            if ((g_busy_skips++ % EYE_LOG_EVERY) == 0)
                log_msg("tew_xr: eye texture busy, frame not shared (%lu time(s)) - the game did not wait", g_busy_skips);
            continue;
        }
        if (bd.SampleDesc.Count > 1)
            ID3D11DeviceContext_ResolveSubresource(g_d3d.ctx, (ID3D11Resource *)g_cur.tex[i], 0, (ID3D11Resource *)bb,
                                                   0, bd.Format);
        else
            ID3D11DeviceContext_CopyResource(g_d3d.ctx, (ID3D11Resource *)g_cur.tex[i], (ID3D11Resource *)bb);
        if (pose) memcpy(g_eye_pose[i], pose, sizeof g_eye_pose[i]);
        IDXGIKeyedMutex_ReleaseSync(g_cur.km[i], EYE_SYNC_KEY);
        InterlockedIncrement(&g_serial[i]);
        g_st_game_copies++;
        wrote = 1;
    }
    ID3D11Texture2D_Release(bb);
    return wrote;
}

/* ---- Headset thread ---------------------------------------------------------------------------------------- */
void tew_share_xr_release(TewXrEyes *x) {
    int i;
    for (i = 0; i < 2; ++i) {
        if (x->km[i]) IDXGIKeyedMutex_Release(x->km[i]);
        if (x->shared[i]) ID3D11Texture2D_Release(x->shared[i]);
        if (x->held[i]) ID3D11Texture2D_Release(x->held[i]);
    }
    memset(x, 0, sizeof *x);
}

int tew_share_open(TewXrEyes *x, ID3D11Device *dev) {
    D3D11_TEXTURE2D_DESC d;
    int i, ok = 1;
    LONG gen;
    if (!g_pub_cs_ready) return -1;
    EnterCriticalSection(&g_pub_cs);   /* the game only ever TRY-enters, so holding it never stalls the game */
    gen = g_pub.gen;
    if (gen == 0 || gen == x->gen) {
        LeaveCriticalSection(&g_pub_cs);
        return (gen == 0 || !x->held[0]) ? -1 : 0;
    }
    tew_share_xr_release(x);
    x->w = g_pub.w;
    x->h = g_pub.h_;
    x->fmt = g_pub.fmt;
    for (i = 0; i < 2 && ok; ++i) {    /* opened while the game still holds this set alive (it is the published one) */
        HRESULT hr = ID3D11Device_OpenSharedResource(dev, g_pub.h[i], &IID_ID3D11Texture2D, (void **)&x->shared[i]);
        g_st_open_hr = (long)hr;
        log_msg("tew_xr: OpenSharedResource(eye %d, handle %p) on the headset device -> hr=0x%08lX", i,
                (void *)g_pub.h[i], (unsigned long)hr);
        ok = SUCCEEDED(hr) &&
             SUCCEEDED(ID3D11Texture2D_QueryInterface(x->shared[i], &IID_IDXGIKeyedMutex, (void **)&x->km[i]));
    }
    LeaveCriticalSection(&g_pub_cs);
    if (ok) {
        ID3D11Texture2D_GetDesc(x->shared[0], &d);
        d.MiscFlags = 0;
        for (i = 0; i < 2 && ok; ++i) ok = SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &x->held[i]));
    }
    if (!ok) {
        g_st_open_fail++;
        log_msg("tew_xr: could not open the game's eye texture on the headset device (a runtime on another graphics "
                "card is not supported in this build)");
        tew_share_xr_release(x);
    }
    x->gen = gen;                      /* set even on failure, so a bad set is not retried every frame */
    return ok ? 1 : -1;
}

void tew_share_pull(TewXrEyes *x, ID3D11DeviceContext *ctx) {
    int i;
    for (i = 0; i < 2; ++i) {
        LONG s = g_serial[i];
        if (!x->km[i] || s == x->seen[i]) continue;
        if (IDXGIKeyedMutex_AcquireSync(x->km[i], EYE_SYNC_KEY, EYE_NO_WAIT_MS) != S_OK) { g_st_xr_busy++; continue; }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)x->held[i], (ID3D11Resource *)x->shared[i]);
        memcpy(x->pose[i], g_eye_pose[i], sizeof x->pose[i]);
        IDXGIKeyedMutex_ReleaseSync(x->km[i], EYE_SYNC_KEY);
        x->seen[i] = s;
        x->have[i] = 1;
        g_st_xr_copies++;
    }
}

void tew_share_stats(TewShareStats *o) {
    o->game_copies = g_st_game_copies;
    o->game_busy = g_busy_skips;
    o->xr_copies = g_st_xr_copies;
    o->xr_busy = g_st_xr_busy;
    o->open_fail = g_st_open_fail;
    o->last_open_hr = g_st_open_hr;
    o->serial[0] = g_serial[0];
    o->serial[1] = g_serial[1];
}

/* tew_xr.c - shows the two alternate-frame eyes in an OpenXR headset (2026-10-06, /pd reader, "own device" build).
 *
 * Ported from Alan Wake's aw_xr.c. By default TWO QUAD LAYERS, one per eye (eyeVisibility LEFT / RIGHT), world-locked
 * in front of where the head was when the session started, so each eye sees only the frames rendered for it
 * (stereo_afr.c flips the eye at every Present). OPENXR_LAYERS = projection submits projection views instead.
 *
 * THREADING (changed 2026-10-06 after a live stall). The first build made the session on the GAME's device and ran
 * the frame loop on the game's render thread; live, the game stopped after the first headset frame, inside the
 * runtime's swapchain calls on the game's immediate context. Now it is Alan Wake's shape, which works with the same
 * simulator:
 *   HEADSET THREAD (ours): loads the runtime, makes its OWN D3D11 device on the runtime's adapter, creates the session
 *     and swapchains on it, and runs xrWaitFrame / xrBeginFrame / xrEndFrame. It is the only thread that calls xr*.
 *   GAME THREAD, at Present: tew_share_capture() copies the back buffer into that eye's shared texture (keyed mutex,
 *     zero timeout, skipped if busy; tew_xr_eyes.c). It never calls OpenXR and never waits for the headset thread.
 * The game is no longer paced to the headset; each headset frame shows the newest finished picture of each eye.
 *
 * OpenXR headers: Khronos, Apache-2.0 OR MIT (third_party/openxr). The loader is loaded at run time from the game
 * folder, so a machine without it, or with OPENXR off, loads nothing new. */
#include "tew_xr.h"
#include "tew_xr_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "config.h"
#include "head_track.h"
#include "log.h"
#include "stereo_afr.h"

/* ---- Settings ------------------------------------------------------------------------------------- */
#define XR_SCREEN_DIST_M      2.0f       /* floating screen distance in front of the start pose */
#define XR_SCREEN_WIDTH_M     3.2f       /* floating screen width; height follows the frame aspect */
#define XR_MAX_FORMATS        64u
#define XR_LOG_EVERY_FRAMES   900ul      /* one progress line per this many submitted headset frames */
#define XR_IMAGE_WAIT_NS      100000000  /* 100 ms: longest the headset thread waits for a swapchain image */
#define XR_IDLE_SLEEP_MS      10         /* headset thread nap while the session is not running / nothing shared */
#define XR_DIAG_EVERY         600ul      /* one handoff diagnostic line per this many headset frames (v2) */
#define XR_TP_W               1280u      /* test-pattern swapchain size while nothing is shared yet (v2) */
#define XR_TP_H               720u
#define XR_TP_FMT             DXGI_FORMAT_R8G8B8A8_UNORM
#define XR_KNOB_LEN           MAX_PATH
#define XR_LOADER_NAME        "openxr_loader.dll"
#define XR_APP_NAME           "the-evil-within-vr"

enum { XR_STAGE_OFF = 0, XR_STAGE_LOADING, XR_STAGE_RUNNING, XR_STAGE_FAILED };

static volatile LONG g_stage;          /* XR_STAGE_*; only the headset thread moves it past LOADING */
static volatile LONG g_live;           /* 1 while the session runs: the game thread shares pictures only then */
static int g_proj_layers;
static int g_test_pattern;             /* OPENXR_TEST_PATTERN = 1: solid colours instead of the game picture (v2) */
static volatile DWORD g_xr_tid;        /* the headset thread; any Present on it is the runtime's (hooks.c) */
static const float g_tp_colour[2][4] = { { 0.8f, 0.1f, 0.1f, 1.0f }, { 0.1f, 0.2f, 0.9f, 1.0f } };   /* left red, right blue */

static XrInstance g_inst;
static XrSystemId g_sys;
static LUID g_req_luid;
static D3D_FEATURE_LEVEL g_req_fl;

#define XR_FN(name) static PFN_##name p_##name
static PFN_xrGetInstanceProcAddr p_gipa;
XR_FN(xrCreateInstance); XR_FN(xrDestroyInstance); XR_FN(xrGetSystem); XR_FN(xrCreateSession);
XR_FN(xrCreateReferenceSpace); XR_FN(xrPollEvent); XR_FN(xrBeginSession); XR_FN(xrEndSession);
XR_FN(xrEnumerateSwapchainFormats); XR_FN(xrCreateSwapchain); XR_FN(xrDestroySwapchain);
XR_FN(xrEnumerateSwapchainImages); XR_FN(xrWaitFrame); XR_FN(xrBeginFrame); XR_FN(xrEndFrame);
XR_FN(xrAcquireSwapchainImage); XR_FN(xrWaitSwapchainImage); XR_FN(xrReleaseSwapchainImage);
XR_FN(xrGetD3D11GraphicsRequirementsKHR); XR_FN(xrLocateSpace); XR_FN(xrLocateViews);

/* ---- Headset-thread state (no other thread touches these) ----------------------------------------- */
typedef struct {
    XrSwapchain sc;
    XrSwapchainImageD3D11KHR *img;
    uint32_t count;
    uint32_t pending;          /* 1 + index of an image acquired but not yet ready (its wait timed out); 0 = none */
} EyeChain;

static ID3D11Device *g_xdev;
static ID3D11DeviceContext *g_xctx;
static XrSession g_sess;
static XrSpace g_space, g_view_space;
static int g_running;
static EyeChain g_chain[2];
static int64_t g_sc_fmt;
static XrVector3f g_anchor;
static int g_anchored;
static unsigned long g_frames, g_wait_timeouts;
static unsigned long g_hframes, g_layer_frames;   /* v2 diagnostics: all headset frames / ones that carried layers */
static int g_last_layers, g_quad_logged, g_head_logged;
static TewXrEyes g_x;

/* ---- Start-up, on the headset thread ------------------------------------------------------------- */
static int knob(const char *name, char *buf, DWORD size) {
    DWORD n = tewvr_getenv(name, buf, size);
    return n > 0 && n < size;
}

static int load_fn(XrInstance inst, const char *name, PFN_xrVoidFunction *out) {
    return XR_SUCCEEDED(p_gipa(inst, name, out)) && *out;
}
#define LOAD(inst, name) load_fn(inst, #name, (PFN_xrVoidFunction *)&p_##name)

static int load_all(XrInstance i) {
    return LOAD(i, xrDestroyInstance) && LOAD(i, xrGetSystem) && LOAD(i, xrCreateSession) &&
           LOAD(i, xrCreateReferenceSpace) && LOAD(i, xrPollEvent) && LOAD(i, xrBeginSession) &&
           LOAD(i, xrEndSession) && LOAD(i, xrEnumerateSwapchainFormats) && LOAD(i, xrCreateSwapchain) &&
           LOAD(i, xrDestroySwapchain) && LOAD(i, xrEnumerateSwapchainImages) && LOAD(i, xrWaitFrame) &&
           LOAD(i, xrBeginFrame) && LOAD(i, xrEndFrame) && LOAD(i, xrAcquireSwapchainImage) &&
           LOAD(i, xrWaitSwapchainImage) && LOAD(i, xrReleaseSwapchainImage) &&
           LOAD(i, xrGetD3D11GraphicsRequirementsKHR) && LOAD(i, xrLocateSpace) && LOAD(i, xrLocateViews);
}

static int start_runtime(void) {
    char path[MAX_PATH], rt[XR_KNOB_LEN], *slash;
    HMODULE lib;
    XrResult r;
    if (knob("TEWVR_OPENXR_RUNTIME_JSON", rt, sizeof rt)) {    /* this process only; Steam relaunches drop outside vars */
        SetEnvironmentVariableA("XR_RUNTIME_JSON", rt);
        log_msg("tew_xr: runtime for this game only: %s", rt);
    }
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strncat(path, XR_LOADER_NAME, MAX_PATH - strlen(path) - 1);
    lib = LoadLibraryA(path);
    if (!lib) { log_msg("tew_xr: %s not found beside the exe - headset output OFF", XR_LOADER_NAME); return 0; }
    p_gipa = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(lib, "xrGetInstanceProcAddr");
    if (!p_gipa || !LOAD(XR_NULL_HANDLE, xrCreateInstance)) { log_msg("tew_xr: the loader has no entry point"); return 0; }
    {
        const char *exts[1] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ici;
        memset(&ici, 0, sizeof ici);
        ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
        strcpy(ici.applicationInfo.applicationName, XR_APP_NAME);
        ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = exts;
        r = p_xrCreateInstance(&ici, &g_inst);
        if (XR_FAILED(r)) { log_msg("tew_xr: xrCreateInstance failed (XrResult %d): no 64-bit D3D11 OpenXR runtime", (int)r); return 0; }
    }
    if (!load_all(g_inst)) { log_msg("tew_xr: a required OpenXR function is missing"); return 0; }
    {
        XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
        XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        r = p_xrGetSystem(g_inst, &gi, &g_sys);
        if (XR_FAILED(r)) { log_msg("tew_xr: no headset system (XrResult %d)", (int)r); return 0; }
        r = p_xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req);   /* required before xrCreateSession */
        if (XR_FAILED(r)) { log_msg("tew_xr: graphics requirements failed (XrResult %d)", (int)r); return 0; }
        g_req_luid = req.adapterLuid;
        g_req_fl = req.minFeatureLevel;
    }
    return 1;
}


/* ---- Own device + session, on the headset thread ------------------------------------------------------------- */
static int make_own_device(void) {
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *chosen = NULL;
    D3D_FEATURE_LEVEL fl = g_req_fl > D3D_FEATURE_LEVEL_11_0 ? g_req_fl : D3D_FEATURE_LEVEL_11_0;
    UINT i;
    HRESULT hr;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) {
        for (i = 0; IDXGIFactory1_EnumAdapters1(fac, i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            IDXGIAdapter1_GetDesc1(ad, &d);
            if (!chosen && memcmp(&d.AdapterLuid, &g_req_luid, sizeof(LUID)) == 0) chosen = ad;
            else IDXGIAdapter1_Release(ad);
        }
        IDXGIFactory1_Release(fac);
    }
    hr = D3D11CreateDevice((IDXGIAdapter *)chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                           &fl, 1, D3D11_SDK_VERSION, &g_xdev, NULL, &g_xctx);
    if (chosen) IDXGIAdapter1_Release(chosen);
    if (FAILED(hr)) log_msg("tew_xr: could not make the headset thread's D3D11 device (hr=0x%08lX)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

static int create_session(void) {
    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    XrResult r;
    bind.device = g_xdev;
    sci.next = &bind;
    sci.systemId = g_sys;
    r = p_xrCreateSession(g_inst, &sci, &g_sess);
    if (XR_FAILED(r)) { log_msg("tew_xr: xrCreateSession failed (XrResult %d)", (int)r); return 0; }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    p_xrCreateReferenceSpace(g_sess, &rs, &g_space);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;     /* the head, to place the screen at eye height */
    p_xrCreateReferenceSpace(g_sess, &rs, &g_view_space);
    log_msg("tew_xr: session created on the headset thread's own device; waiting for the runtime to start it");
    return 1;
}

static void poll_events(void) {
    XrEventDataBuffer ev;
    memset(&ev, 0, sizeof ev);
    ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (p_xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(p_xrBeginSession(g_sess, &bi))) {
                    g_running = 1;
                    InterlockedExchange(&g_live, 1);
                    log_msg("tew_xr: session running");
                }
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(g_sess);
                g_running = 0;
                InterlockedExchange(&g_live, 0);
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = 0;
                InterlockedExchange(&g_live, 0);
                InterlockedExchange(&g_stage, XR_STAGE_FAILED);
                log_msg("tew_xr: the runtime ended the session - headset output stopped");
            }
        }
        memset(&ev, 0, sizeof ev);
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

/* ---- Swapchains, on the headset thread ----------------------------------------------------------------------- */
static void destroy_chains(void) {
    int e;
    for (e = 0; e < 2; ++e) {
        if (g_chain[e].sc) p_xrDestroySwapchain(g_chain[e].sc);
        free(g_chain[e].img);
        memset(&g_chain[e], 0, sizeof g_chain[e]);
    }
}

static int make_chain(int64_t fmt, UINT w, UINT h, EyeChain *c) {
    XrSwapchainCreateInfo ci;
    uint32_t i;
    memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.width = w; ci.height = h;
    ci.sampleCount = 1; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(p_xrCreateSwapchain(g_sess, &ci, &c->sc))) return 0;
    p_xrEnumerateSwapchainImages(c->sc, 0, &c->count, NULL);
    c->img = (XrSwapchainImageD3D11KHR *)calloc(c->count ? c->count : 1, sizeof *c->img);
    if (!c->img) return 0;
    for (i = 0; i < c->count; ++i) c->img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    return XR_SUCCEEDED(p_xrEnumerateSwapchainImages(c->sc, c->count, &c->count, (XrSwapchainImageBaseHeader *)c->img));
}

/* (Re)build both eye swapchains for the shared pictures' size and format. */
static int build_chains(void) {
    int64_t fmts[XR_MAX_FORMATS];
    uint32_t n = 0;
    destroy_chains();
    if (XR_FAILED(p_xrEnumerateSwapchainFormats(g_sess, XR_MAX_FORMATS, &n, fmts))) return 0;
    g_sc_fmt = tew_eyes_pick_format(fmts, n, g_x.fmt);
    if (!g_sc_fmt) {
        log_msg("tew_xr: the runtime offers no swapchain format matching back buffer format %d - headset output stopped",
                (int)g_x.fmt);
        return 0;
    }
    if (!make_chain(g_sc_fmt, g_x.w, g_x.h, &g_chain[0]) || !make_chain(g_sc_fmt, g_x.w, g_x.h, &g_chain[1])) {
        log_msg("tew_xr: swapchain setup failed - headset output stopped");
        destroy_chains();
        return 0;
    }
    log_msg("tew_xr: two %ux%u eye swapchains, format %d", g_x.w, g_x.h, (int)g_sc_fmt);
    return 1;
}

/* Returns 1 when the eye's swapchain image got the eye's held picture and was released (usable in a layer). Never
 * waits longer than XR_IMAGE_WAIT_NS: a timed-out image stays acquired (the spec forbids releasing it unwaited). */
static int fill_eye(int e) {
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    uint32_t idx = 0;
    XrResult r;
    if (g_chain[e].pending) {
        idx = g_chain[e].pending - 1;
    } else if (XR_FAILED(p_xrAcquireSwapchainImage(g_chain[e].sc, &ai, &idx)) || idx >= g_chain[e].count) {
        return 0;
    }
    wi.timeout = XR_IMAGE_WAIT_NS;
    r = p_xrWaitSwapchainImage(g_chain[e].sc, &wi);
    if (r == XR_TIMEOUT_EXPIRED) {
        g_chain[e].pending = idx + 1;
        if ((g_wait_timeouts++ % XR_LOG_EVERY_FRAMES) == 0)
            log_msg("tew_xr: swapchain image not ready within %d ms (%lu time(s)) - headset frame skipped",
                    (int)(XR_IMAGE_WAIT_NS / 1000000), g_wait_timeouts);
        return 0;
    }
    g_chain[e].pending = 0;
    if (XR_FAILED(r)) return 0;
    if (g_test_pattern) {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        ID3D11RenderTargetView *rtv = NULL;
        memset(&rd, 0, sizeof rd);
        rd.Format = (DXGI_FORMAT)g_sc_fmt;           /* swapchain images may be typeless: name the format */
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_xdev, (ID3D11Resource *)g_chain[e].img[idx].texture, &rd, &rtv))) {
            ID3D11DeviceContext_ClearRenderTargetView(g_xctx, rtv, g_tp_colour[e]);
            ID3D11RenderTargetView_Release(rtv);
        }
    } else {
        ID3D11DeviceContext_CopyResource(g_xctx, (ID3D11Resource *)g_chain[e].img[idx].texture,
                                         (ID3D11Resource *)g_x.held[e]);
    }
    return XR_SUCCEEDED(p_xrReleaseSwapchainImage(g_chain[e].sc, &ri));
}

/* v2: one line that says where the pictures stop, every XR_DIAG_EVERY headset frames. */
static void diag_line(int should_render) {
    TewShareStats st;
    tew_share_stats(&st);
    log_msg("tew_xr diag: headset frames %lu, with layers %lu (last %d), shouldRender %d; game copies %lu, game busy %lu; "
            "headset pulls %lu, headset busy %lu; open fails %lu (last hr 0x%08lX); serial L %ld/%ld R %ld/%ld "
            "(game/seen); have L%d R%d; swapchain image timeouts %lu%s",
            g_hframes, g_layer_frames, g_last_layers, should_render, st.game_copies, st.game_busy, st.xr_copies,
            st.xr_busy, st.open_fail, (unsigned long)st.last_open_hr, (long)st.serial[0], (long)g_x.seen[0],
            (long)st.serial[1], (long)g_x.seen[1], g_x.have[0], g_x.have[1], g_wait_timeouts,
            g_test_pattern ? "; TEST PATTERN" : "");
}

/* ---- One headset frame, on the headset thread ---------------------------------------------------------------- */
static void submit_frame(void) {
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    XrCompositionLayerQuad quad[2];
    XrCompositionLayerProjection proj;
    XrCompositionLayerProjectionView pv[2];
    XrView views[2];
    const XrCompositionLayerBaseHeader *layers[2];
    int have = 0, e, views_ok = 0;

    if (XR_FAILED(p_xrWaitFrame(g_sess, &fwi, &fs))) { Sleep(XR_IDLE_SLEEP_MS); return; }
    if (XR_FAILED(p_xrBeginFrame(g_sess, &fbi))) return;
    if (!g_anchored && g_view_space) {               /* the screen goes where the head is at the start, then stays */
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(p_xrLocateSpace(g_view_space, g_space, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            g_anchor = loc.pose.position;
            g_anchored = 1;
            log_msg("tew_xr: screen placed at head height %.2f m", (double)g_anchor.y);
        }
    }
    if (head_track_on() && g_view_space) {           /* the newest head orientation, for the game's next frame */
        XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(p_xrLocateSpace(g_view_space, g_space, fs.predictedDisplayTime, &hl)) &&
            (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            HeadQuat q;
            q.x = hl.pose.orientation.x; q.y = hl.pose.orientation.y;
            q.z = hl.pose.orientation.z; q.w = hl.pose.orientation.w;
            head_track_publish_pose(q);
        }
    }
    if (g_proj_layers) {
        XrViewLocateInfo vi = { XR_TYPE_VIEW_LOCATE_INFO };
        XrViewState vst = { XR_TYPE_VIEW_STATE };
        uint32_t nv = 0;
        memset(views, 0, sizeof views);
        views[0].type = views[1].type = XR_TYPE_VIEW;
        vi.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vi.displayTime = fs.predictedDisplayTime;
        vi.space = g_space;
        views_ok = XR_SUCCEEDED(p_xrLocateViews(g_sess, &vi, &vst, 2, &nv, views)) && nv == 2;
    }
    tew_share_pull(&g_x, g_xctx);                    /* newest finished picture per eye; never waits for the game */
    if (fs.shouldRender && g_chain[0].sc && (g_test_pattern || (g_x.have[0] && g_x.have[1]))) {
        int filled = 0;
        for (e = 0; e < 2; ++e) filled += fill_eye(e);
        if (filled != 2) {
            /* an eye without a released image cannot go in a layer: submit an empty frame */
        } else if (g_proj_layers && views_ok) {
            HeadLens lens;
            int head = head_track_lens(&lens), rect[4];
            for (e = 0; e < 2; ++e) {
                memset(&pv[e], 0, sizeof pv[e]);
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[e].pose = views[e].pose;
                pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = g_chain[e].sc;
                pv[e].subImage.imageRect.extent.width = (int32_t)g_x.w;
                pv[e].subImage.imageRect.extent.height = (int32_t)g_x.h;
                if (head) {   /* HEAD TRACKING: the picture is the game's lens, turned the way the head pointed */
                    float l, r, u, d;
                    pv[e].pose.orientation.x = g_x.pose[e][0]; pv[e].pose.orientation.y = g_x.pose[e][1];
                    pv[e].pose.orientation.z = g_x.pose[e][2]; pv[e].pose.orientation.w = g_x.pose[e][3];
                    if (head_lens_tangents(&lens, &l, &r, &u, &d)) {
                        pv[e].fov.angleLeft = atanf(l); pv[e].fov.angleRight = atanf(r);
                        pv[e].fov.angleUp = atanf(u);   pv[e].fov.angleDown = atanf(d);
                    }
                    head_lens_image_rect(&lens, g_x.w, g_x.h, rect);
                    pv[e].subImage.imageRect.offset.x = rect[0]; pv[e].subImage.imageRect.offset.y = rect[1];
                    pv[e].subImage.imageRect.extent.width = rect[2]; pv[e].subImage.imageRect.extent.height = rect[3];
                }
            }
            if (head && !g_head_logged) {
                g_head_logged = 1;
                log_msg("tew_xr: head-tracked views: fov L%.1f R%.1f U%.1f D%.1f deg, picture %d,%d %dx%d of %ux%u",
                        (double)(pv[0].fov.angleLeft * 57.29578f), (double)(pv[0].fov.angleRight * 57.29578f),
                        (double)(pv[0].fov.angleUp * 57.29578f), (double)(pv[0].fov.angleDown * 57.29578f),
                        pv[0].subImage.imageRect.offset.x, pv[0].subImage.imageRect.offset.y,
                        pv[0].subImage.imageRect.extent.width, pv[0].subImage.imageRect.extent.height, g_x.w, g_x.h);
            }
            memset(&proj, 0, sizeof proj);
            proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            proj.space = g_space;
            proj.viewCount = 2;
            proj.views = pv;
            layers[0] = (const XrCompositionLayerBaseHeader *)&proj;
            have = 1;
        } else if (!g_proj_layers) {
            for (e = 0; e < 2; ++e) {
                memset(&quad[e], 0, sizeof quad[e]);
                quad[e].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                quad[e].space = g_space;
                quad[e].eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                quad[e].subImage.swapchain = g_chain[e].sc;
                quad[e].subImage.imageRect.extent.width = (int32_t)g_x.w;
                quad[e].subImage.imageRect.extent.height = (int32_t)g_x.h;
                quad[e].pose.orientation.w = 1.0f;
                quad[e].pose.position.x = g_anchor.x;
                quad[e].pose.position.y = g_anchor.y;
                quad[e].pose.position.z = g_anchor.z - XR_SCREEN_DIST_M;
                quad[e].size.width = XR_SCREEN_WIDTH_M;
                quad[e].size.height = XR_SCREEN_WIDTH_M * (float)g_x.h / (float)g_x.w;
                layers[e] = (const XrCompositionLayerBaseHeader *)&quad[e];
            }
            have = 2;
            if (!g_quad_logged) {
                g_quad_logged = 1;
                log_msg("tew_xr: quad layers at (%.2f, %.2f, %.2f) m, %.2f x %.2f m, facing +z (identity), %ux%u px",
                        (double)quad[0].pose.position.x, (double)quad[0].pose.position.y,
                        (double)quad[0].pose.position.z, (double)quad[0].size.width, (double)quad[0].size.height,
                        g_x.w, g_x.h);
            }
        }
    }
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (uint32_t)have;
    fei.layers = have ? layers : NULL;
    {
        XrResult er = p_xrEndFrame(g_sess, &fei);
        if (XR_FAILED(er)) log_msg("tew_xr: xrEndFrame failed (XrResult %d) with %d layer(s)", (int)er, have);
    }
    g_last_layers = have;
    if (have) g_layer_frames++;
    if ((g_hframes++ % XR_DIAG_EVERY) == 0) diag_line(fs.shouldRender);
    if (have && (g_frames++ % XR_LOG_EVERY_FRAMES) == 0) log_msg("tew_xr: %lu headset frames submitted", g_frames);
}

/* ---- The headset thread -------------------------------------------------------------------------------------- */
static void xr_main(void) {
    g_xr_tid = GetCurrentThreadId();
    if (!start_runtime() || !make_own_device()) {
        InterlockedExchange(&g_stage, XR_STAGE_FAILED);
        return;
    }
    /* v2: the session (and with it the simulator's preview window) only starts once the GAME has presented, so the
     * game's swapchain is captured first. The v1 own-device build started earlier and the preview was taken for the
     * game's swapchain. */
    if (!d3d_capture_ready()) log_msg("tew_xr: waiting for the game's first frame before starting the session");
    while (!d3d_capture_ready() && g_stage == XR_STAGE_LOADING) Sleep(XR_IDLE_SLEEP_MS);
    if (g_stage != XR_STAGE_LOADING || !create_session()) {
        InterlockedExchange(&g_stage, XR_STAGE_FAILED);
        return;
    }
    InterlockedExchange(&g_stage, XR_STAGE_RUNNING);
    while (g_stage == XR_STAGE_RUNNING) {
        int opened;
        poll_events();
        if (!g_running) { Sleep(XR_IDLE_SLEEP_MS); continue; }
        opened = tew_share_open(&g_x, g_xdev);
        if (opened == 1 && !build_chains()) { InterlockedExchange(&g_stage, XR_STAGE_FAILED); break; }
        if (g_test_pattern && opened < 0 && !g_chain[0].sc) {     /* pattern needs no game picture: fixed-size chains */
            g_x.w = XR_TP_W; g_x.h = XR_TP_H; g_x.fmt = XR_TP_FMT;
            if (!build_chains()) { InterlockedExchange(&g_stage, XR_STAGE_FAILED); break; }
        }
        if (g_test_pattern && g_chain[0].sc) { submit_frame(); continue; }
        if (opened < 0 || !g_chain[0].sc) {
            /* nothing shared yet; the frame loop must still run once the session is running, or the runtime never
             * reaches SYNCHRONIZED/FOCUSED - so submit empty frames meanwhile */
            XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
            XrFrameState fs = { XR_TYPE_FRAME_STATE };
            XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
            XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
            if (XR_FAILED(p_xrWaitFrame(g_sess, &fwi, &fs))) { Sleep(XR_IDLE_SLEEP_MS); continue; }
            p_xrBeginFrame(g_sess, &fbi);
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            p_xrEndFrame(g_sess, &fei);
            continue;
        }
        submit_frame();
    }
    InterlockedExchange(&g_live, 0);
    log_msg("tew_xr: headset thread stopped");
    /* Resources are left to process exit: tearing the session down here could race a shutdown from DllMain. */
}

static DWORD WINAPI xr_thread(LPVOID unused) {
    (void)unused;
    xr_main();
    return 0;
}

void tew_xr_init(void) {
    char v[XR_KNOB_LEN];
    HANDLE h;
    if (g_stage != XR_STAGE_OFF) return;
    if (!knob("TEWVR_OPENXR", v, sizeof v) || atoi(v) == 0) { log_msg("tew_xr: off (OPENXR is not 1)"); return; }
    g_proj_layers = knob("TEWVR_OPENXR_LAYERS", v, sizeof v) && _stricmp(v, "projection") == 0;
    if (head_track_on()) g_proj_layers = 1;   /* a head-turned picture only makes sense as per-eye views */
    g_test_pattern = knob("TEWVR_OPENXR_TEST_PATTERN", v, sizeof v) && atoi(v) != 0;
    tew_share_init();
    InterlockedExchange(&g_stage, XR_STAGE_LOADING);
    h = CreateThread(NULL, 0, xr_thread, NULL, 0, NULL);
    if (!h) { InterlockedExchange(&g_stage, XR_STAGE_FAILED); log_msg("tew_xr: could not start the headset thread"); return; }
    CloseHandle(h);
    log_msg("tew_xr: ON - headset thread started (own device); eye output as %s%s",
            g_proj_layers ? "per-eye projection views" : "a floating screen per eye",
            g_test_pattern ? ", TEST PATTERN (solid colours, not the game picture)" : "");
}

DWORD tew_xr_headset_tid(void) {
    return g_xr_tid;
}

void tew_xr_shutdown(void) {
    InterlockedExchange(&g_stage, XR_STAGE_FAILED);   /* the headset thread leaves its loop; the process is going away */
    InterlockedExchange(&g_live, 0);
}

/* GAME THREAD, at Present, before the real Present. Never calls OpenXR, never waits for the headset thread. */
void tew_xr_on_present(IDXGISwapChain *sc) {
    if (!g_live || !d3d_capture_ready()) return;
    /* The frame being presented was drawn for the eye stereo_afr holds until it flips, just after the real Present. */
    {
        float pose[4];
        stereo_afr_current_pose(pose);
        tew_share_capture(sc, stereo_afr_current_eye(), pose);
    }
}

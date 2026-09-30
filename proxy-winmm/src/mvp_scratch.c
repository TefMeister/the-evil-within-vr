/* Split out of mvp_patch.c on 2026-09-30 (move-only; see mvp_patch.c for the
 * module overview). The per-thread scratch constant-buffer rings and their lazy creation. */

#include "mvp_patch_internal.h"

/* ---- scratch buffer pool (lazy, real device obtained from a live ctx) ---- */

ID3D11Buffer *g_scratch[MVP_SCRATCH_TOTAL];
volatile LONG g_scratch_ready = 0;
CRITICAL_SECTION g_scratch_init_cs; /* serializes the lazy-create-on-first-use race
                                               (Important finding #6: without this, two
                                               threads racing the first patchable draw could
                                               both see g_scratch_ready==0 and both create a
                                               full 64-buffer set, leaking one set) */
int g_scratch_init_cs_ready = 0;

/* ---- per-thread scratch-ring assignment (see file header) ---- */

DWORD g_tls_index = TLS_OUT_OF_INDEXES;
static volatile LONG g_next_bucket = 0;
static volatile LONG g_bucket_next[MVP_THREADS_MAX];
static volatile LONG g_overflow_log_count = 0;

/* ================= per-thread scratch-ring allocation ================= */

/* Returns a scratch-buffer index in [0, MVP_SCRATCH_TOTAL), guaranteed to
 * never collide with an index concurrently in use by another thread (see
 * file header) - or -1 if TLS was never set up. Lock-free on the hot path
 * (a TLS read plus one InterlockedIncrement); only the FIRST call from a
 * given thread pays a bucket assignment (also lock-free). */
int mvp_alloc_scratch_index(void) {
    LONG_PTR raw;
    int bucket;
    LONG cursor;

    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        return -1;
    }

    raw = (LONG_PTR)TlsGetValue(g_tls_index);
    if (raw == 0) {
        LONG assigned = InterlockedIncrement(&g_next_bucket) - 1;
        if (assigned >= MVP_THREADS_MAX) {
            assigned = MVP_THREADS_MAX - 1;
            if (mvp_rate_limit_should_fire(&g_overflow_log_count)) {
                log_msg("mvp_patch: thread-ring pool exhausted (%d distinct threads seen); "
                         "further threads share the last bucket - patch stays correct via "
                         "WRITE_DISCARD renaming, may rarely contend under unusually many threads",
                         MVP_THREADS_MAX);
            }
        }
        bucket = (int)assigned;
        TlsSetValue(g_tls_index, (LPVOID)(LONG_PTR)(assigned + 1)); /* +1: 0 means "unset" */
    } else {
        bucket = (int)(raw - 1);
    }

    cursor = InterlockedIncrement(&g_bucket_next[bucket]);
    return bucket * MVP_SLOTS_PER_THREAD + (int)(((ULONG)cursor) % (ULONG)MVP_SLOTS_PER_THREAD);
}

/* ================= lazy scratch-pool creation ================= */

int ensure_scratch_ready(ID3D11Device *dev) {
    D3D11_BUFFER_DESC sd;
    HRESULT hr;
    int i;

    memset(&sd, 0, sizeof(sd));
    sd.ByteWidth = MVP_CB_BYTES;
    sd.Usage = D3D11_USAGE_DYNAMIC;
    sd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    for (i = 0; i < MVP_SCRATCH_TOTAL; i++) {
        hr = ID3D11Device_CreateBuffer(dev, &sd, NULL, &g_scratch[i]);
        if (FAILED(hr) || g_scratch[i] == NULL) {
            int j;
            log_msg("mvp_patch: CreateBuffer for scratch[%d/%d] failed (hr=0x%08lX); "
                     "per-draw MVP override DISABLED this session",
                     i, MVP_SCRATCH_TOTAL, (unsigned long)hr);
            for (j = 0; j < i; j++) {
                if (g_scratch[j] != NULL) {
                    ID3D11Buffer_Release(g_scratch[j]);
                    g_scratch[j] = NULL;
                }
            }
            return 0;
        }
    }
    return 1;
}

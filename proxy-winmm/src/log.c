#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

static CRITICAL_SECTION g_log_cs;
static FILE *g_log_fp = NULL;
static volatile LONG g_log_cs_ready = 0;

void log_init(void) {
    if (InterlockedCompareExchange(&g_log_cs_ready, 1, 0) != 0) {
        return; /* already initialised */
    }
    InitializeCriticalSection(&g_log_cs);

    wchar_t local_appdata[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%s\\TEWVR", local_appdata);

    /* Create the TEWVR directory if it does not already exist. */
    if (!CreateDirectoryW(dir, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return;
        }
    }

    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\tewvr.log", dir);

    EnterCriticalSection(&g_log_cs);
    g_log_fp = _wfopen(path, L"a");
    LeaveCriticalSection(&g_log_cs);
}

void log_msg(const char *fmt, ...) {
    if (!g_log_cs_ready) {
        return;
    }
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        ULONGLONG ms = GetTickCount64();
        DWORD tid = GetCurrentThreadId();
        fprintf(g_log_fp, "[%llu][tid=%lu] ", (unsigned long long)ms, (unsigned long)tid);

        va_list args;
        va_start(args, fmt);
        vfprintf(g_log_fp, fmt, args);
        va_end(args);

        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
    }
    LeaveCriticalSection(&g_log_cs);
}

void log_shutdown(void) {
    if (InterlockedCompareExchange(&g_log_cs_ready, 0, 1) != 1) {
        return; /* never initialised, or already shut down */
    }
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    LeaveCriticalSection(&g_log_cs);
    DeleteCriticalSection(&g_log_cs);
}

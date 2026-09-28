#include "config.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "log.h"

/* See config.h for the why and the contract. */

#define CFG_MAX_ENTRIES   64
#define CFG_KEY_MAX       48   /* normalised key, WITHOUT the TEWVR_ prefix */
#define CFG_VALUE_MAX     256
#define CFG_FILE_MAX      65536 /* a settings file larger than this is not one */
#define CFG_SOURCE_MAX    MAX_PATH

struct CfgEntry {
    char key[CFG_KEY_MAX];       /* upper-case, prefix stripped */
    char value[CFG_VALUE_MAX];
    char source[CFG_SOURCE_MAX]; /* the file it came from, for the log */
    LONG announced;              /* tewvr_getenv() has logged this key once */
};

static struct CfgEntry g_cfg[CFG_MAX_ENTRIES];
static int g_cfg_count = 0;
static int g_cfg_loaded = 0;

/* One-shot "which source answered" log gate per key for values that came
 * from the ENVIRONMENT (file-sourced keys carry their own flag above). A
 * small fixed table keyed by name is enough - there are ten knobs. */
#define CFG_ENV_ANNOUNCE_MAX 32
static char g_env_announced[CFG_ENV_ANNOUNCE_MAX][CFG_KEY_MAX];
static volatile LONG g_env_announced_count = 0;

/* Upper-cases `in` into `out`, dropping a leading "TEWVR_" if present.
 * Returns 0 if the result would be empty or too long. */
static int cfg_normalise_key(const char *in, size_t in_len, char *out, size_t out_size) {
    size_t i, n = 0;
    if (in_len >= 6 && _strnicmp(in, "TEWVR_", 6) == 0) {
        in += 6;
        in_len -= 6;
    }
    if (in_len == 0 || in_len >= out_size) {
        return 0;
    }
    for (i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return 1;
}

static int cfg_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const struct CfgEntry *cfg_find(const char *norm_key) {
    int i;
    for (i = 0; i < g_cfg_count; i++) {
        if (strcmp(g_cfg[i].key, norm_key) == 0) {
            return &g_cfg[i];
        }
    }
    return NULL;
}

/* Parses one "key = value" line. Returns 1 if a setting was ADDED, 0 if the
 * line was blank/comment/duplicate/empty-value, -1 if it did not parse. */
static int cfg_parse_line(const char *line, size_t len, const char *source) {
    const char *eq, *k_end, *v_start, *v_end;
    char key[CFG_KEY_MAX];
    size_t vlen;

    /* trim leading whitespace */
    while (len > 0 && cfg_is_space(*line)) {
        line++;
        len--;
    }
    if (len == 0 || *line == '#' || *line == ';') {
        return 0;
    }
    eq = memchr(line, '=', len);
    if (eq == NULL) {
        return -1;
    }
    /* key: [line, eq) trimmed */
    k_end = eq;
    while (k_end > line && cfg_is_space(k_end[-1])) {
        k_end--;
    }
    if (!cfg_normalise_key(line, (size_t)(k_end - line), key, sizeof(key))) {
        return -1;
    }
    /* value: (eq, line+len) trimmed */
    v_start = eq + 1;
    v_end = line + len;
    while (v_start < v_end && cfg_is_space(*v_start)) {
        v_start++;
    }
    while (v_end > v_start && cfg_is_space(v_end[-1])) {
        v_end--;
    }
    vlen = (size_t)(v_end - v_start);
    if (vlen == 0) {
        return 0; /* "KEY=" neutralises a line without deleting it: counts as unset */
    }
    if (vlen >= CFG_VALUE_MAX) {
        return -1;
    }
    if (cfg_find(key) != NULL) {
        return 0; /* first definition wins - across files AND within one */
    }
    if (g_cfg_count >= CFG_MAX_ENTRIES) {
        return -1;
    }
    strcpy(g_cfg[g_cfg_count].key, key);
    memcpy(g_cfg[g_cfg_count].value, v_start, vlen);
    g_cfg[g_cfg_count].value[vlen] = '\0';
    strncpy(g_cfg[g_cfg_count].source, source, CFG_SOURCE_MAX - 1);
    g_cfg[g_cfg_count].source[CFG_SOURCE_MAX - 1] = '\0';
    g_cfg[g_cfg_count].announced = 0;
    g_cfg_count++;
    return 1;
}

int config_load_file(const wchar_t *path) {
    HANDLE h;
    DWORD size, got = 0;
    char *data;
    char source[CFG_SOURCE_MAX];
    int added = 0, bad = 0;
    const char *p, *end;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > CFG_FILE_MAX) {
        CloseHandle(h);
        log_msg("config: ignoring %ls (size %lu is not a settings file)", path, (unsigned long)size);
        return -1;
    }
    data = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (data == NULL) {
        CloseHandle(h);
        return -1;
    }
    if (!ReadFile(h, data, size, &got, NULL)) {
        got = 0;
    }
    CloseHandle(h);
    data[got] = '\0';

    /* the source string is only ever logged - a lossy narrow conversion is fine */
    snprintf(source, sizeof(source), "%ls", path);

    p = data;
    end = data + got;
    /* skip a UTF-8 BOM, which Notepad likes to add */
    if (got >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        int r = cfg_parse_line(p, len, source);
        if (r > 0) {
            added++;
        } else if (r < 0) {
            bad++;
            log_msg("config: %ls: cannot parse line \"%.*s\" - skipped", path,
                    (int)(len > 80 ? 80 : len), p);
        }
        p = nl ? nl + 1 : end;
    }
    HeapFree(GetProcessHeap(), 0, data);
    log_msg("config: read %ls: %d setting(s) taken%s", path, added,
            bad ? " (some lines skipped, see above)" : "");
    return added;
}

void config_load(void) {
    wchar_t path[MAX_PATH];
    wchar_t *slash;
    DWORD len;
    int found_any = 0;

    if (g_cfg_loaded) {
        return;
    }
    g_cfg_loaded = 1;

    /* 1st file: beside the game executable (GetModuleFileNameW(NULL) is the
     * process's own exe; winmm.dll sits in the same folder anyway). */
    len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        slash = wcsrchr(path, L'\\');
        if (slash != NULL) {
            slash[1] = L'\0';
            if (wcslen(path) + wcslen(L"tewvr.ini") < MAX_PATH) {
                wcscat(path, L"tewvr.ini");
                if (config_load_file(path) >= 0) {
                    found_any = 1;
                }
            }
        }
    }

    /* 2nd file: %LOCALAPPDATA%\TEWVR\tewvr.ini, next to the logs. */
    len = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (len > 0 && len < MAX_PATH && len + wcslen(L"\\TEWVR\\tewvr.ini") < MAX_PATH) {
        wcscat(path, L"\\TEWVR\\tewvr.ini");
        if (config_load_file(path) >= 0) {
            found_any = 1;
        }
    }

    if (!found_any) {
        log_msg("config: no tewvr.ini beside the exe or in %%LOCALAPPDATA%%\\TEWVR - "
                "TEWVR_* knobs come from the environment only this run");
    }
}

static void cfg_announce_env_once(const char *name, const char *value) {
    LONG i, n = g_env_announced_count;
    if (n > CFG_ENV_ANNOUNCE_MAX) {
        n = CFG_ENV_ANNOUNCE_MAX;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(g_env_announced[i], name) == 0) {
            return;
        }
    }
    i = InterlockedIncrement(&g_env_announced_count) - 1;
    if (i >= 0 && i < CFG_ENV_ANNOUNCE_MAX) {
        strncpy(g_env_announced[i], name, CFG_KEY_MAX - 1);
        g_env_announced[i][CFG_KEY_MAX - 1] = '\0';
    }
    log_msg("config: %s=%s (from the process environment)", name, value);
}

DWORD tewvr_getenv(const char *name, char *buf, DWORD size) {
    DWORD len;
    char key[CFG_KEY_MAX];
    const struct CfgEntry *e;

    /* 1. environment - unchanged behaviour, still wins. */
    len = GetEnvironmentVariableA(name, buf, size);
    if (len > 0) {
        if (len < size) {
            cfg_announce_env_once(name, buf);
        }
        return len;
    }

    /* 2./3. the files, in the order config_load() read them. */
    if (!cfg_normalise_key(name, strlen(name), key, sizeof(key))) {
        return 0;
    }
    e = cfg_find(key);
    if (e == NULL) {
        return 0;
    }
    len = (DWORD)strlen(e->value);
    if (len >= size) {
        /* GetEnvironmentVariableA contract: return the size needed, write nothing */
        return len + 1;
    }
    memcpy(buf, e->value, len + 1);
    if (InterlockedCompareExchange((volatile LONG *)&e->announced, 1, 0) == 0) {
        log_msg("config: %s=%s (from %s)", name, e->value, e->source);
    }
    return len;
}

void config_reset_for_test(void) {
    memset(g_cfg, 0, sizeof(g_cfg));
    g_cfg_count = 0;
    g_cfg_loaded = 0;
    memset(g_env_announced, 0, sizeof(g_env_announced));
    g_env_announced_count = 0;
}

#pragma once

/*
 * Thin wrapper around MinHook (see third_party/minhook/) that centralises
 * init/create/enable/shutdown diagnostics logging, so hooks.c (and any
 * future *_hook.c files added by later tasks) don't each repeat
 * MH_STATUS-to-string boilerplate.
 *
 * Every failure path here logs once via log_msg() and returns a plain
 * boolean - callers must always be able to fall back to running the game
 * mono (unhooked); nothing in here crashes or hangs on failure.
 */

/* Calls MH_Initialize(). Returns non-zero on success (including the
 * already-initialised case), zero on failure. */
int mh_glue_init(void);

/* Calls MH_CreateHook() then MH_EnableHook() for one target/detour pair,
 * logging the outcome (tagged with the given human-readable `name`) at
 * each step. Returns non-zero only if both steps succeeded; on partial
 * failure (create ok, enable failed) it removes the half-created hook
 * before returning. */
int mh_glue_create_and_enable(void *target, void *detour, void **original, const char *name);

/* Disables all hooks and uninitialises MinHook. Safe to call even if
 * mh_glue_init() was never called or failed. */
void mh_glue_shutdown(void);

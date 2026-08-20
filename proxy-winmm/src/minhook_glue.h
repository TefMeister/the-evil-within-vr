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

/*
 * Split create/enable pair (Task 5 addendum-3 review fix): use these
 * instead of mh_glue_create_and_enable() whenever the caller needs to
 * publish `*original` somewhere (e.g. a lookup table another thread's
 * live detour call will consult) BEFORE the hook can possibly be
 * reached. mh_glue_create_and_enable() fills `*original` via
 * MH_CreateHook() and only then calls MH_EnableHook() - fine when
 * `*original` is a plain static variable written directly (the write
 * happens-before the hook goes live), but NOT fine when the caller needs
 * an extra step (e.g. inserting into a table under a lock) between the
 * two, because MH_EnableHook() already made the target live before that
 * extra step ran, leaving a window where the hook is enabled but not yet
 * discoverable.
 *
 * Correct usage: mh_glue_create() (fills `*original`, target NOT yet
 * live) -> publish `*original` wherever it needs to go -> mh_glue_enable()
 * (target now live, and any call that reaches the detour can already
 * find `*original`).
 */

/* Calls MH_CreateHook() only. Fills `*original` with the trampoline on
 * success; the target's code is NOT yet redirected (MH_EnableHook() has
 * not run), so nothing can reach `detour` yet. Returns non-zero on
 * success. */
int mh_glue_create(void *target, void *detour, void **original, const char *name);

/* Calls MH_EnableHook() for a target already created via mh_glue_create().
 * On failure, removes the hook (MH_RemoveHook()) before returning, same
 * fail-safe cleanup as mh_glue_create_and_enable()'s partial-failure
 * path. Returns non-zero on success. */
int mh_glue_enable(void *target, const char *name);

/* Disables all hooks and uninitialises MinHook. Safe to call even if
 * mh_glue_init() was never called or failed. */
void mh_glue_shutdown(void);

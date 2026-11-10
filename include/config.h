#ifndef PROCWARDEN_CONFIG_H
#define PROCWARDEN_CONFIG_H

#include <stdbool.h>

/*
 * config.h — tunables. Every knob in ProcWarden lives here with a built-in
 * default (CLAUDE.md: no magic numbers in the logic). Values come from a
 * key=value file (~/.procwarden/config); missing file or missing key means
 * "use the default".
 *
 * Format: one `key = value` per line, '#' starts a comment, blank lines are
 * ignored. Unknown keys are logged and skipped, never fatal.
 */

/* Load overrides from path. NULL → ~/.procwarden/config. Returns 0 on success
 * or when the file simply doesn't exist (defaults only); -1 on a real error. */
int  config_load(const char *path);

/* Drop all overrides → back to pure defaults (used on SIGHUP reload, Phase 5). */
void config_reset(void);

/* Typed getters: overrides win, then the defaults table, then the caller's
 * fallback (which should only matter for keys not yet in the table). */
long        config_int(const char *key, long fallback);
bool        config_bool(const char *key, bool fallback);
const char *config_str(const char *key, const char *fallback);

#endif /* PROCWARDEN_CONFIG_H */

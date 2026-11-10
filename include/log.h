#ifndef PROCWARDEN_LOG_H
#define PROCWARDEN_LOG_H

/*
 * log.h — one line per event:
 *     ISO-time level module workload rule message
 * Written to the log file (default ~/.procwarden/warden.log), optionally
 * mirrored to stderr. Everything in ProcWarden reports through this so the
 * demo log can be read straight as lab evidence.
 */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
} log_level;

/* Log one event line. workload/rule may be NULL; they are printed as "-".
 * Example:
 *   log_event(LOG_WARN, "monitor", "wl-3", "PAUSE", "growth stopped");
 * produces:
 *   2026-11-10T20:11:02+0300 WARN  monitor wl-3 PAUSE growth stopped
 */
void log_event(log_level lvl, const char *module, const char *workload,
               const char *rule, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Open the log for appending ("a"). path == NULL means ~/.procwarden/warden.log
 * (created if missing). Returns 0 on success, -1 on failure. */
int  log_open(const char *path);
void log_close(void);

/* Default verbosity covers INFO and above; DEBUG is for the debug build. */
void log_set_level(log_level lvl);

/* Mirror every line to stderr (useful while the log file path is under test). */
void log_set_console(int on);

/* Convenience wrappers used everywhere per CLAUDE.md:
 *   log_err("msgsnd: %s", strerror(errno));          → level ERROR
 *   log_warn("config", "unknown key '%s'", key);     → level WARN with a module
 */
#define log_err(...)      log_event(LOG_ERROR, "general", NULL, NULL, __VA_ARGS__)
#define log_warn(mod, ...) log_event(LOG_WARN, (mod), NULL, NULL, __VA_ARGS__)
#define log_info(mod, ...) log_event(LOG_INFO, (mod), NULL, NULL, __VA_ARGS__)

#endif /* PROCWARDEN_LOG_H */

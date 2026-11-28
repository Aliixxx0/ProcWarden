/*
 * log.c — event logger for ProcWarden.
 *
 * Format (one line per event):
 *     ISO-time level module workload rule message
 *
 * A pthread_mutex keeps lines whole once the daemon runs three threads
 * (Phase 5); before then it is harmless. Timestamps are local-time ISO-8601.
 */
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>

#define LOG_MSG_MAX  256
#define LOG_LINE_MAX 512

static FILE       *log_file;
static log_level   min_level = LOG_INFO;
static int         to_console;            /* mirror lines to stderr */
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(log_level lv)
{
    switch (lv) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    default:        return "ERROR";
    }
}

static const char *default_log_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        home = (pw != NULL) ? pw->pw_dir : NULL;
    }
    if (home == NULL)
        return NULL;
    snprintf(buf, n, "%s/.procwarden/warden.log", home);
    return buf;
}

void log_set_level(log_level lv) { min_level = lv; }
void log_set_console(int on)     { to_console = on; }

int log_open(const char *path)
{
    char buf[512];

    if (path == NULL) {
        path = default_log_path(buf, sizeof(buf));
        if (path == NULL) {
            errno = EINVAL;
            return -1;          /* no HOME at all — caller reports it */
        }
        /* Ensure ~/.procwarden exists; ignore EEXIST, report anything else. */
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (mkdir(dir, 0700) != 0 && errno != EEXIST)
                return -1;
        }
    }

    pthread_mutex_lock(&log_mu);
    FILE *f = fopen(path, "a");
    if (f != NULL) {
        if (log_file != NULL)
            fclose(log_file);
        log_file = f;
    }
    pthread_mutex_unlock(&log_mu);
    return (f != NULL) ? 0 : -1;
}

void log_close(void)
{
    pthread_mutex_lock(&log_mu);
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mu);
}

void log_event(log_level lvl, const char *module, const char *workload,
               const char *rule, const char *fmt, ...)
{
    if (lvl < min_level)
        return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char stamp[48];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z", &tm);

    char msg[LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "%s %-5s %s %s %s %s\n",
             stamp, level_name(lvl),
             module   ? module   : "-",
             workload ? workload : "-",
             rule     ? rule     : "-",
             msg);

    pthread_mutex_lock(&log_mu);
    if (log_file != NULL) {
        fputs(line, log_file);
        fflush(log_file);       /* one write per line: the log stays crash-safe */
    }
    if (to_console || log_file == NULL)   /* file not opened yet → stderr backup */
        fputs(line, stderr);
    pthread_mutex_unlock(&log_mu);
}

/*
 * config.c — key=value parser with defaults.
 *
 * The defaults table below is the single home of every tunable; it grows one
 * phase at a time. Values fixed by the rules/proposal are already seeded:
 *   enforce=false            (Rule 2.5: observe mode is the default)
 *   sample_interval_ms=1000  (monitor tick, Phase 5)
 *   grace_ms=5000            (SIGTERM → SIGKILL grace, Rule 2.8)
 *
 * Overrides are kept in a small fixed table guarded by a mutex, because from
 * Phase 5 on the monitor thread reads it while the command thread reloads it.
 */
#include "config.h"

#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_KEY_MAX       48
#define CONFIG_VAL_MAX       96
#define CONFIG_MAX_OVERRIDES 64
#define CONFIG_LINE_MAX      256

typedef enum { CFG_INT, CFG_BOOL, CFG_STR } cfg_type;

typedef struct {
    const char *key;
    cfg_type    t;
    long        i;
    bool        b;
    const char *s;
} config_def;

static const config_def CONFIG_DEFAULTS[] = {
    /* Seed set, Phase 0. Later phases append their knobs here. */
    { .key = "enforce",            .t = CFG_BOOL, .b = false },
    { .key = "sample_interval_ms", .t = CFG_INT,  .i = 1000 },
    { .key = "grace_ms",           .t = CFG_INT,  .i = 5000 },
    { .key = "log_level",          .t = CFG_STR,  .s = "info" },
};

typedef struct {
    char     key[CONFIG_KEY_MAX];
    cfg_type t;
    long     i;
    bool     b;
    char     s[CONFIG_VAL_MAX];
} config_val;

static config_val OVERRIDES[CONFIG_MAX_OVERRIDES];
static int        n_overrides;
static pthread_mutex_t cfg_mu = PTHREAD_MUTEX_INITIALIZER;

static const config_def *find_def(const char *key)
{
    for (int i = 0; i < (int)(sizeof(CONFIG_DEFAULTS) / sizeof(CONFIG_DEFAULTS[0])); i++)
        if (strcmp(CONFIG_DEFAULTS[i].key, key) == 0)
            return &CONFIG_DEFAULTS[i];
    return NULL;
}

static config_val *find_override(const char *key)
{
    for (int i = 0; i < n_overrides; i++)
        if (strcmp(OVERRIDES[i].key, key) == 0)
            return &OVERRIDES[i];
    return NULL;
}

static char *trim(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    *e = '\0';
    return s;
}

static bool parse_bool(const char *v)
{
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
           strcmp(v, "yes") == 0 || strcmp(v, "on") == 0;
}

/* Parse one non-empty, non-comment line from the config file. */
static void parse_line(char *line)
{
    char *hash = strchr(line, '#');
    if (hash != NULL)
        *hash = '\0';

    char *eq = strchr(line, '=');
    if (eq == NULL) {
        if (*trim(line) != '\0')
            log_warn("config", "line without '=' ignored");
        return;
    }
    *eq = '\0';

    char *key = trim(line);
    char *val = trim(eq + 1);
    if (key[0] == '\0' || val[0] == '\0') {
        log_warn("config", "empty key or value ignored");
        return;
    }

    if (strlen(key) >= CONFIG_KEY_MAX || strlen(val) >= CONFIG_VAL_MAX) {
        log_warn("config", "key/value too long ignored: '%.32s'", key);
        return;
    }

    const config_def *d = find_def(key);
    if (d == NULL) {
        log_warn("config", "unknown key '%s' ignored", key);
        return;
    }

    config_val v = { .t = d->t };
    snprintf(v.key, sizeof(v.key), "%s", key);

    switch (d->t) {
    case CFG_INT: {
        errno = 0;
        char *end;
        v.i = strtol(val, &end, 10);
        if (errno != 0 || end == val || *end != '\0') {
            log_warn("config", "key '%s': '%s' is not a number, keeping default",
                     key, val);
            return;
        }
        break;
    }
    case CFG_BOOL:
        v.b = parse_bool(val);
        break;
    case CFG_STR:
        snprintf(v.s, sizeof(v.s), "%s", val);
        break;
    }

    pthread_mutex_lock(&cfg_mu);
    config_val *old = find_override(key);       /* re-set → replace in place */
    if (old != NULL) {
        *old = v;
    } else if (n_overrides < CONFIG_MAX_OVERRIDES) {
        OVERRIDES[n_overrides++] = v;
    } else {
        log_warn("config", "override table full, key '%s' ignored", key);
    }
    pthread_mutex_unlock(&cfg_mu);
}

int config_load(const char *path)
{
    char buf[512];
    if (path == NULL) {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return 0;                    /* no HOME → defaults only */
        snprintf(buf, sizeof(buf), "%s/.procwarden/config", home);
        path = buf;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return (errno == ENOENT) ? 0 : -1;   /* missing file is fine */

    char line[CONFIG_LINE_MAX];
    while (fgets(line, sizeof(line), f) != NULL)
        parse_line(line);
    fclose(f);
    return 0;
}

void config_reset(void)
{
    pthread_mutex_lock(&cfg_mu);
    n_overrides = 0;
    pthread_mutex_unlock(&cfg_mu);
}

long config_int(const char *key, long fallback)
{
    long out;
    pthread_mutex_lock(&cfg_mu);
    config_val *ov = find_override(key);
    const config_def *d = (ov != NULL) ? NULL : find_def(key);
    if (ov != NULL)
        out = (ov->t == CFG_BOOL) ? (ov->b ? 1 : 0) : (ov->t == CFG_INT ? ov->i : fallback);
    else if (d != NULL)
        out = (d->t == CFG_BOOL) ? (d->b ? 1 : 0) : (d->t == CFG_INT ? d->i : fallback);
    else
        out = fallback;
    pthread_mutex_unlock(&cfg_mu);
    return out;
}

bool config_bool(const char *key, bool fallback)
{
    bool out;
    pthread_mutex_lock(&cfg_mu);
    config_val *ov = find_override(key);
    const config_def *d = (ov != NULL) ? NULL : find_def(key);
    if (ov != NULL)
        out = (ov->t == CFG_BOOL) ? ov->b : (ov->t == CFG_INT ? ov->i != 0 : fallback);
    else if (d != NULL)
        out = (d->t == CFG_BOOL) ? d->b : (d->t == CFG_INT ? d->i != 0 : fallback);
    else
        out = fallback;
    pthread_mutex_unlock(&cfg_mu);
    return out;
}

const char *config_str(const char *key, const char *fallback)
{
    const char *out;
    pthread_mutex_lock(&cfg_mu);
    config_val *ov = find_override(key);
    const config_def *d = (ov != NULL) ? NULL : find_def(key);
    if (ov != NULL)
        out = (ov->t == CFG_STR) ? ov->s : fallback;
    else if (d != NULL)
        out = (d->t == CFG_STR) ? d->s : fallback;
    else
        out = fallback;
    pthread_mutex_unlock(&cfg_mu);
    return out;
}

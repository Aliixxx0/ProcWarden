/*
 * tests/lab_check.c — Phase 0 warm-up.
 *
 * One small function per mechanism ProcWarden leans on, in lab order:
 *   Lab 3: fork/waitpid + exit-status macros, execvp, orphan → launchd
 *   Lab 4: pthread_create/join with a context struct and a heap result
 *   Lab 5: signal + kill + pause with SIGUSR1; pipe with unused ends closed
 *   Lab 6: shmget/shmat round trip; msgsnd/msgrcv with mtype routing
 *          and negative msgtyp ordering
 *
 * It also prints the macOS System V IPC limits we depend on
 * (CLAUDE.md Rule 4): msg_qbytes via msgctl(IPC_STAT) and
 * kern.sysv.shmmax via sysctl.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

static int n_fail;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        n_fail++;
}

/* reap in an EINTR loop — CLAUDE.md Rule 3: every blocking call handles EINTR */
static pid_t reap(pid_t pid, int *st)
{
    pid_t r;
    do {
        r = waitpid(pid, st, 0);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ------------------------------------------------------------------ Lab 3 */

/* Lab 3: fork + waitpid; the child's _exit(42) is decoded into
 * WIFEXITED/WEXITSTATUS — the exit-code vocabulary the launcher reports on. */
static void lab3_fork_waitpid_exit(void)
{
    fflush(stdout);              /* don't duplicate buffered output into forked children */
    pid_t pid = fork();
    if (pid < 0) {
        log_err("fork: %s", strerror(errno));
        check("lab3: fork+waitpid WEXITSTATUS", 0);
        return;
    }
    if (pid == 0)
        _exit(42);

    int st = 0;
    pid_t r = reap(pid, &st);
    check("lab3: fork+waitpid WEXITSTATUS",
          r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42);
}

/* Lab 3: execvp swaps the child image for /usr/bin/true; on exec failure we
 * exit 127, the same code the launcher reports through the exec-error pipe. */
static void lab3_execvp(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check("lab3: execvp + waitpid", 0);
        return;
    }
    if (pid == 0) {
        char *av[] = { "true", NULL };
        execvp("true", av);
        _exit(127);
    }

    int st = 0;
    pid_t r = reap(pid, &st);
    check("lab3: execvp + waitpid",
          r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Lab 3: WIFSIGNALED/WTERMSIG — a child that dies from a default signal. */
static void lab3_signal_death(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (kill(getpid(), SIGTERM) != 0)
            _exit(30);
        _exit(31);               /* unreachable if SIGTERM had its default effect */
    }

    int st = 0;
    pid_t r = reap(pid, &st);
    check("lab3: WIFSIGNALED + WTERMSIG",
          r == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
}

/* Lab 3: when the parent dies, the live child is re-parented to launchd
 * (getppid() == 1) and launchd reaps it. This is exactly the situation the
 * proposal means by "an orphan under launchd is not proof of unwanted work". */
static void lab3_orphan_reparent(void)
{
    fflush(stdout);
    int rp[2];
    if (pipe(rp) != 0) {
        log_err("pipe: %s", strerror(errno));
        check("lab3: orphan is re-parented to launchd", 0);
        return;
    }

    pid_t mid = fork();          /* middle process */
    if (mid == 0) {
        pid_t leaf = fork();
        if (leaf == 0) {
            close(rp[0]);        /* the leaf writes the verdict */
            struct timespec ts = { 0, 50 * 1000 * 1000 }; /* 50 ms */
            for (int i = 0; i < 200 && getppid() != 1; i++)
                nanosleep(&ts, NULL);
            unsigned char v = (getppid() == 1) ? 'y' : 'n';
            if (write(rp[1], &v, 1) != 1)
                _exit(30);
            _exit(9);
        }
        _exit(0);                /* middle dies → leaf becomes an orphan */
    }
    close(rp[1]);

    int st = 0;
    pid_t r = reap(mid, &st);
    unsigned char v = 'x';
    ssize_t got;
    do {
        got = read(rp[0], &v, 1);
    } while (got < 0 && errno == EINTR);
    close(rp[0]);
    check("lab3: orphan is re-parented to launchd",
          r == mid && WIFEXITED(st) && WEXITSTATUS(st) == 0 && got == 1 && v == 'y');
}

/* ------------------------------------------------------------------ Lab 4 */

/* Lab 4: each thread gets its work through a context struct passed as void*,
 * and returns its totals through a heap-allocated result the joiner frees. */
typedef struct { int leak_mb; int chunks; }       sim_worker_ctx;
typedef struct { long dirty_bytes; }              sim_worker_result;

static void *sim_worker(void *arg)
{
    sim_worker_ctx *ctx = arg;
    sim_worker_result *res = malloc(sizeof(*res));
    if (res == NULL)
        return NULL;
    res->dirty_bytes = (long)ctx->leak_mb * ctx->chunks;
    return res;
}

static void lab4_pthread_create_join(void)
{
    fflush(stdout);
    sim_worker_ctx ctx = { .leak_mb = 3, .chunks = 10 };
    pthread_t th;
    int rc = pthread_create(&th, NULL, sim_worker, &ctx);
    if (rc != 0) {
        log_err("pthread_create: %s", strerror(rc));
        check("lab4: pthread_create + join, ctx struct, heap result", 0);
        return;
    }

    void *out = NULL;
    rc = pthread_join(th, &out);
    long got = -1;
    if (rc == 0 && out != NULL) {
        got = ((sim_worker_result *)out)->dirty_bytes;
        free(out);               /* the joiner owns and frees the result */
    }
    check("lab4: pthread_create + join, ctx struct, heap result",
          rc == 0 && got == 30);
}

/* ------------------------------------------------------------------ Lab 5 */

static volatile sig_atomic_t got_usr1;   /* Rule 3: handlers only set flags */

static void on_usr1(int sig)
{
    (void)sig;
    got_usr1 = 1;
}

/* Lab 5: a child parked in pause() stays asleep until kill(SIGUSR1) arrives.
 * The handler is installed in the parent BEFORE the fork so the child inherits
 * it — otherwise kill() could land while the child is still between fork and
 * signal(), and SIGUSR1's default disposition would kill it. The handler sets
 * only a sig_atomic_t flag; pause() returns EINTR when the handler runs, and
 * the loop guard makes the race (signal before pause) safe. */
static void lab5_signal_pause(void)
{
    fflush(stdout);
    got_usr1 = 0;
    signal(SIGUSR1, on_usr1);       /* inherited by the child across fork */

    int wake[2] = { -1, -1 }, ack[2] = { -1, -1 };
    if (pipe(wake) != 0 || pipe(ack) != 0) {
        log_err("pipe: %s", strerror(errno));
        check("lab5: kill(SIGUSR1) wakes pause()", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(wake[1]);          /* Lab 5: close the ends this process doesn't use */
        close(ack[0]);
        unsigned char b;
        if (read(wake[0], &b, 1) != 1)   /* wait for "parent armed, proceed" */
            _exit(20);
        while (!got_usr1)
            pause();
        b = 'y';
        if (write(ack[1], &b, 1) != 1)
            _exit(21);
        _exit(7);
    }
    close(wake[0]);
    close(ack[1]);

    unsigned char g = 'g';
    if (write(wake[1], &g, 1) != 1)
        log_err("wake write: %s", strerror(errno));
    close(wake[1]);

    /* Lab 5: kill() is the only thing that wakes the parked child: the
     * handler runs, pause() returns with EINTR, and the loop guard exits. */
    if (kill(pid, SIGUSR1) != 0)
        log_err("kill: %s", strerror(errno));

    unsigned char v = 'x';
    ssize_t got;
    do {
        got = read(ack[0], &v, 1);
    } while (got < 0 && errno == EINTR);
    close(ack[0]);

    int st = 0;
    pid_t r = reap(pid, &st);
    check("lab5: kill(SIGUSR1) wakes pause()",
          r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7 &&
          got == 1 && v == 'y');
}

/* Lab 5: pipe + fork, ping/pong both ways, unused ends closed on each side. */
static void lab5_pipe_fork(void)
{
    fflush(stdout);
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        log_err("pipe: %s", strerror(errno));
        check("lab5: pipe between parent and child", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(p2c[1]);
        close(c2p[0]);
        char buf[8];
        ssize_t got;
        do {
            got = read(p2c[0], buf, (size_t)4);
        } while (got < 0 && errno == EINTR);
        if (got != 4 || memcmp(buf, "ping", 4) != 0)
            _exit(30);
        if (write(c2p[1], "pong", 4) != 4)
            _exit(31);
        close(p2c[0]);
        close(c2p[1]);
        _exit(0);
    }
    close(p2c[0]);
    close(c2p[1]);

    int ok = (write(p2c[1], "ping", 4) == 4);
    close(p2c[1]);

    char buf[8] = { 0 };
    ssize_t got;
    do {
        got = read(c2p[0], buf, (size_t)4);
    } while (got < 0 && errno == EINTR);
    close(c2p[0]);
    ok = ok && got == 4 && memcmp(buf, "pong", 4) == 0;

    int st = 0;
    pid_t r = reap(pid, &st);
    check("lab5: pipe between parent and child",
          ok && r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* ------------------------------------------------------------------ Lab 6 */

/* Lab 6: System V shared memory round trip — the write pattern we put in must
 * come back out; this later carries the live status table for the panel. */
static void lab6_shm_roundtrip(void)
{
    const size_t SZ = 64 * 1024;   /* far below kern.sysv.shmmax */
    int shmid = shmget(IPC_PRIVATE, SZ, 0666);
    if (shmid < 0) {
        log_err("shmget: %s", strerror(errno));
        check("lab6: shmget/shmat round trip", 0);
        return;
    }

    void *p = shmat(shmid, NULL, 0);
    if (p == (void *)-1) {
        log_err("shmat: %s", strerror(errno));
        check("lab6: shmget/shmat round trip", 0);
        shmctl(shmid, IPC_RMID, NULL);
        return;
    }

    unsigned char *m = p;
    int ok = 1;
    for (size_t i = 0; i < SZ; i += 4096)
        m[i] = 0xA5;
    for (size_t i = 0; i < SZ && ok; i += 4096)
        ok = (m[i] == 0xA5);

    if (shmdt(p) != 0)
        ok = 0;
    int gone = (shmctl(shmid, IPC_RMID, NULL) == 0);
    check("lab6: shmget/shmat round trip", ok && gone);
}

/* Lab 6 message shape (the real one lands in include/ipc.h in Phase 1). */
typedef struct { long mtype; int a; } test_msg;

/* Lab 6: two-way routing. mtype routes messages to exactly one receiver:
 * the child "client" asks on type 2 and listens on mtype == its own pid;
 * the parent "daemon" only ever picks up types ≤ 2, so replies to the
 * child (type = child pid) never come back to it. */
static void lab6_msg_mtype_routing(void)
{
    int q = msgget(IPC_PRIVATE, 0666);
    if (q < 0) {
        log_err("msgget: %s", strerror(errno));
        check("lab6: msgsnd/msgrcv mtype = client pid", 0);
        return;
    }

    fflush(stdout);
    int rp[2];
    if (pipe(rp) != 0) {
        check("lab6: msgsnd/msgrcv mtype = client pid", 0);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(rp[0]);
        test_msg m = { .mtype = 2, .a = 77 };        /* NORMAL request */
        if (msgsnd(q, &m, sizeof(m) - sizeof(m.mtype), 0) != 0)
            _exit(30);
        test_msg rep;
        if (msgrcv(q, &rep, sizeof(rep) - sizeof(rep.mtype), getpid(), 0) < 0)
            _exit(31);
        unsigned char v = (rep.a == 88) ? 'y' : 'n'; /* reply reached THIS client */
        if (write(rp[1], &v, 1) != 1)
            _exit(32);
        _exit(0);
    }
    close(rp[1]);

    test_msg m;
    ssize_t got = msgrcv(q, &m, sizeof(m) - sizeof(m.mtype), -2, 0);
    int ok = (got >= 0 && m.mtype == 2 && m.a == 77);
    if (ok) {
        test_msg rep = { .mtype = (long)pid, .a = 88 };   /* reply: mtype = client pid */
        ok = (msgsnd(q, &rep, sizeof(rep) - sizeof(rep.mtype), 0) == 0);
    }

    unsigned char v = 'x';
    ssize_t b;
    do {
        b = read(rp[0], &v, 1);
    } while (b < 0 && errno == EINTR);
    close(rp[0]);

    int st = 0;
    pid_t r = reap(pid, &st);
    int gone = (msgctl(q, IPC_RMID, NULL) == 0);
    check("lab6: msgsnd/msgrcv mtype = client pid",
          ok && b == 1 && v == 'y' &&
          r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 && gone);
}

/* Lab 6: negative msgtyp as the type filter. On macOS (DEV-001, see
 * docs/DEVIATIONS.md) a negative msgtyp is an *upper bound*: the call returns
 * the oldest message whose type ≤ |msgtyp|, in arrival order — it does NOT
 * sort by type. The daemon uses this as a two-step receive: probe
 * msgrcv(-1) for URGENT first, then block on msgrcv(-2) for NORMAL, which
 * preserves the spec's invariant (URGENT always served before NORMAL).
 * This check demonstrates exactly that behaviour. */
static void lab6_msg_negative_msgtyp(void)
{
    int q = msgget(IPC_PRIVATE, 0666);
    if (q < 0) {
        log_err("msgget: %s", strerror(errno));
        check("lab6: URGENT (mtype 1) served before NORMAL (mtype 2)", 0);
        return;
    }

    test_msg normal = { .mtype = 2, .a = 20 };   /* NORMAL queued FIRST ... */
    test_msg urgent = { .mtype = 1, .a = 10 };   /* ... URGENT arrives after */
    if (msgsnd(q, &normal, sizeof(normal) - sizeof(normal.mtype), 0) != 0 ||
        msgsnd(q, &urgent, sizeof(urgent) - sizeof(urgent.mtype), 0) != 0) {
        log_err("msgsnd: %s", strerror(errno));
        msgctl(q, IPC_RMID, NULL);
        check("lab6: URGENT (mtype 1) served before NORMAL (mtype 2)", 0);
        return;
    }

    /* Step 1 of the daemon's receive: URGENT first, even though it queued second. */
    test_msg m;
    ssize_t got = msgrcv(q, &m, sizeof(m) - sizeof(m.mtype), -1, 0);
    int ok = (got >= 0 && m.mtype == 1 && m.a == 10);

    /* Step 2: the bound -2 excludes replies (types > 2) and gets the NORMAL. */
    got = msgrcv(q, &m, sizeof(m) - sizeof(m.mtype), -2, 0);
    ok = ok && got >= 0 && m.mtype == 2 && m.a == 20;
    int gone = (msgctl(q, IPC_RMID, NULL) == 0);
    check("lab6: URGENT (mtype 1) served before NORMAL (mtype 2)", ok && gone);
}

/* --------------------------------------------------- Phase 0 own modules */

static void check_config(void)
{
    char path[64] = "/tmp/procwarden_cfgXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        check("config: key=value parser with defaults + overrides", 0);
        return;
    }

    const char *body =
        "# ProcWarden test config\n"
        "\n"
        "grace_ms = 2500\n"
        "enforce = true\n"
        "sample_interval_ms = 700\n";
    int ok = (write(fd, body, strlen(body)) == (ssize_t)strlen(body));
    close(fd);

    ok = ok && (config_load(path) == 0);
    ok = ok && config_int("grace_ms", 5000) == 2500;              /* parsed */
    ok = ok && config_int("sample_interval_ms", 1000) == 700;     /* parsed */
    ok = ok && config_bool("enforce", false) == true;             /* parsed */
    ok = ok && strcmp(config_str("log_level", "info"), "info") == 0; /* default survives */
    ok = ok && config_int("no_such_key", -7) == -7;               /* caller fallback */

    config_reset();
    ok = ok && config_int("grace_ms", 5000) == 5000;              /* reset → defaults */
    unlink(path);
    check("config: key=value parser with defaults + overrides", ok);
}

static void check_log(void)
{
    char path[64] = "/tmp/procwarden_logXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        check("log: ISO-time level module workload rule message", 0);
        return;
    }
    close(fd);

    log_set_level(LOG_DEBUG);
    int ok = (log_open(path) == 0);
    log_event(LOG_WARN, "test_module", "wl-9", "TEST_RULE", "value=%d words=%s", 5, "ok");
    log_set_level(LOG_INFO);                    /* DEBUG goes back under the filter */
    log_event(LOG_DEBUG, "test_module", NULL, NULL, "hidden check only");
    log_close();

    char buf[4096] = { 0 };
    int f2 = open(path, 0);
    if (f2 >= 0) {
        ssize_t got;
        do {
            got = read(f2, buf + strlen(buf), sizeof(buf) - 1 - strlen(buf));
        } while (got < 0 && errno == EINTR);
        close(f2);
    }
    ok = ok && strstr(buf, " WARN ") != NULL &&
         strstr(buf, "test_module wl-9 TEST_RULE value=5 words=ok") != NULL &&
         strstr(buf, "DEBUG") == NULL;    /* second event logged after level reset */
    log_set_level(LOG_INFO);
    unlink(path);
    check("log: ISO-time level module workload rule message", ok);
}

/* --------------------------------------------- macOS System V IPC limits */

/* CLAUDE.md Rule 4: the System V limits are small on macOS. The daemon must
 * log these at startup; the check prints them so we know our Macs' numbers. */
static void print_sysv_limits(void)
{
    int q = msgget(IPC_PRIVATE, 0666);
    if (q >= 0) {
        struct msqid_ds qs;
        if (msgctl(q, IPC_STAT, &qs) == 0)
            printf("note: msg_qbytes (max bytes in one queue) = %lu\n",
                   (unsigned long)qs.msg_qbytes);
        else
            printf("note: msgctl(IPC_STAT) failed: %s\n", strerror(errno));
        msgctl(q, IPC_RMID, NULL);
    } else {
        printf("note: msgget failed: %s\n", strerror(errno));
    }

    int64_t shmmax = 0;
    size_t len = sizeof(shmmax);
    if (sysctlbyname("kern.sysv.shmmax", &shmmax, &len, NULL, 0) == 0)
        printf("note: kern.sysv.shmmax = %lld bytes\n", (long long)shmmax);
    else
        printf("note: sysctl kern.sysv.shmmax unavailable: %s\n", strerror(errno));
}

int main(void)
{
    printf("== ProcWarden Phase 0 lab check ==\n");
    printf("== lab mechanisms ==\n");
    lab3_fork_waitpid_exit();
    lab3_execvp();
    lab3_signal_death();
    lab3_orphan_reparent();
    lab4_pthread_create_join();
    lab5_signal_pause();
    lab5_pipe_fork();
    lab6_shm_roundtrip();
    lab6_msg_mtype_routing();
    lab6_msg_negative_msgtyp();
    printf("== phase 0 modules ==\n");
    check_config();
    check_log();
    print_sysv_limits();

    if (n_fail > 0) {
        printf("== %d CHECK(S) FAILED ==\n", n_fail);
        return 1;
    }
    printf("== ALL CHECKS PASSED ==\n");
    return 0;
}

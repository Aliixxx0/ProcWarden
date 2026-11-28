/*
 * warden-sim — the workload simulator (Labs 3, 4, 5, plus one Lab 6
 * segment for --shared-mb).
 *
 *   profiles: install | devserver | watcher | agents
 *   options:  -n CHILDREN  --mb PER_CHILD_MB  --leak MB_S  --threads T
 *             --leak-after SECONDS  --ignore-hup  --escape
 *             --shared-mb MB  --port PORT
 *
 * The parent forks the children (Lab 3). Every child sends {pid, MB} once
 * per second through a report pipe with the unused ends closed (Lab 5).
 * Leaking starts when the parent sends SIGUSR1 (Lab 5) — either after
 * --leak-after seconds or the moment the parent itself gets SIGUSR1.
 * On SIGTERM the children join their leak threads and report their totals
 * (Lab 4), and the parent reaps everyone with waitpid (Lab 3).
 */
#include "sim.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Rule 3: handlers only set flags. */
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_leak_now;   /* someone sent us SIGUSR1 */

static void on_usr1(int s) { (void)s; g_leak_now = 1; }
static void on_stop(int s) { (void)s; g_stop = 1; }

static void usage(FILE *out)
{
    fprintf(out,
        "usage: warden-sim <profile> [options]\n"
        "profiles: install | devserver | watcher | agents\n"
        "options:\n"
        "  -n CHILDREN        children to fork           (default 4/1/2/4)\n"
        "  --mb PER_CHILD     base MB per child           (default 256/128/16/32)\n"
        "  --leak MB_S        leak rate per child, 0=none (default 0)\n"
        "  --threads T        agents: leak threads per child (default 4)\n"
        "  --leak-after SEC   auto-start the leak after SEC s (default 3)\n"
        "  --ignore-hup       SIGHUP -> SIG_IGN (survive terminal close)\n"
        "  --escape           child 0 leaves a setsid() grandchild under launchd\n"
        "  --shared-mb MB     one System V segment touched by all children\n"
        "  --port PORT        devserver: bind+listen on 127.0.0.1:PORT\n"
        "signals: SIGUSR1 -> start leaking now; SIGTERM/SIGINT -> clean stop\n"
        "examples:\n"
        "  warden-sim agents -n 4 --threads 4 --mb 32 --leak 50\n"
        "  warden-sim devserver --port 3000 --mb 128\n");
}

static int arg_int(const char *what, const char *s, int lo, int hi)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "warden-sim: bad %s '%s' (expected %d..%d)\n", what, s, lo, hi);
        exit(64);
    }
    return (int)v;
}

static void set_defaults(sim_opts *o, const char *profile)
{
    memset(o, 0, sizeof(*o));
    o->profile      = profile;
    o->leak_after_s = 3;
    if (strcmp(profile, "install") == 0)   { o->n_children = 4; o->per_child_mb = 256; }
    else if (strcmp(profile, "devserver") == 0) { o->n_children = 1; o->per_child_mb = 128; o->port = 3000; }
    else if (strcmp(profile, "watcher") == 0)   { o->n_children = 2; o->per_child_mb = 16; }
    else if (strcmp(profile, "agents") == 0)    { o->n_children = 4; o->per_child_mb = 32; o->threads = 4; }
    else { usage(stderr); fprintf(stderr, "warden-sim: unknown profile '%s'\n", profile); exit(64); }
}

static void parse_args(int argc, char **argv, sim_opts *o)
{
    if (argc < 2) { usage(stderr); exit(64); }
    set_defaults(o, argv[1]);

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-n") == 0 && i + 1 < argc)            o->n_children   = arg_int("-n", argv[++i], 1, SIM_MAX_CHILDREN);
        else if (strcmp(a, "--mb") == 0 && i + 1 < argc)     o->per_child_mb = arg_int("--mb", argv[++i], 0, 1 << 20);
        else if (strcmp(a, "--leak") == 0 && i + 1 < argc)   o->leak_mb_s    = arg_int("--leak", argv[++i], 0, 4096);
        else if (strcmp(a, "--threads") == 0 && i + 1 < argc) o->threads     = arg_int("--threads", argv[++i], 1, SIM_MAX_THREADS);
        else if (strcmp(a, "--leak-after") == 0 && i + 1 < argc) o->leak_after_s = arg_int("--leak-after", argv[++i], 0, 3600);
        else if (strcmp(a, "--ignore-hup") == 0)             o->ignore_hup   = 1;
        else if (strcmp(a, "--escape") == 0)                 o->escape       = 1;
        else if (strcmp(a, "--shared-mb") == 0 && i + 1 < argc) o->shared_mb = arg_int("--shared-mb", argv[++i], 1, 4096);
        else if (strcmp(a, "--port") == 0 && i + 1 < argc)   o->port         = arg_int("--port", argv[++i], 1, 65535);
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(stdout); exit(0); }
        else { fprintf(stderr, "warden-sim: unknown option '%s'\n", a); usage(stderr); exit(64); }
    }
    if (o->port != 0 && strcmp(o->profile, "devserver") != 0) {
        fprintf(stderr, "warden-sim: --port applies to the devserver profile\n");
        exit(64);
    }
}

static void nap_100ms(void)
{
    struct timespec ts = { 0, 100L * 1000L * 1000L };
    nanosleep(&ts, NULL);
}

static int64_t now_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

/* Drain every pending report (non-blocking). Reports are matched back to
 * the fork slot by pid; the escape grandchild keeps its own slot. */
static void drain_reports(int fd, const pid_t *kids, int64_t *latest, int nk,
                          pid_t *grandkid, int64_t *grand_latest)
{
    for (;;) {
        sim_report r;
        ssize_t n = read(fd, &r, sizeof(r));
        if (n < 0 && errno == EINTR)
            continue;
        if (n != (ssize_t)sizeof(r))
            break;                        /* EAGAIN (or short read): done */

        if (r.kind == R_GRANDKID) {
            *grandkid = r.pid;
            printf("[sim] child 0 spawned grandchild %d (setsid escape, re-parented to launchd)\n", r.pid);
        } else if (r.kind == R_FINAL) {
            printf("[sim] %d finished, lifetime total %d MB\n", r.pid, r.mb);
        } else {
            int placed = 0;
            for (int j = 0; j < nk && !placed; j++) {
                if (kids[j] == r.pid) {
                    latest[j] = r.mb;
                    placed = 1;
                }
            }
            if (!placed && *grandkid == r.pid)
                *grand_latest = r.mb;
        }
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    sim_opts o;
    parse_args(argc, argv, &o);

    printf("[sim] warden-sim %s n=%d mb=%d/child leak=%dMB/s after=%ds threads=%d "
           "shared=%d port=%d escape=%s hup=%s\n",
           o.profile, o.n_children, o.per_child_mb, o.leak_mb_s, o.leak_after_s,
           o.threads, o.shared_mb, o.port, o.escape ? "on" : "off",
           o.ignore_hup ? "ignore" : "default");

    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_usr1);
    signal(SIGTERM, on_stop);
    signal(SIGINT,  on_stop);
    signal(SIGHUP,  o.ignore_hup ? SIG_IGN : on_stop);

    /* Lab 6: one System V segment the children inherit across fork, so
     * the [footprint, RSS] range later shows shared pages. */
    int shmid = -1;
    if (o.shared_mb > 0) {
        shmid = shmget(IPC_PRIVATE, (size_t)o.shared_mb << 20, 0600);
        if (shmid < 0) {
            log_err("shmget(shared %d MB): %s (kern.sysv.shmmax is 4 MB)", o.shared_mb, strerror(errno));
            return 70;
        }
        o.shared_map   = shmat(shmid, NULL, 0);
        o.shared_bytes = (size_t)o.shared_mb << 20;
        if (o.shared_map == (void *)-1) {
            log_err("shmat(shared): %s", strerror(errno));
            return 70;
        }
        printf("[sim] shared segment id=%d (%d MB), touched by every child\n", shmid, o.shared_mb);
    }

    /* Lab 5: the report pipe; each side closes the end it does not use. */
    int rp[2];
    if (pipe(rp) != 0) {
        log_err("pipe: %s", strerror(errno));
        return 70;
    }

    pid_t kids[SIM_MAX_CHILDREN];
    int nk = o.n_children;
    for (int i = 0; i < nk; i++) {
        fflush(stdout);
        sim_opts co = o;                   /* Lab 3: child gets its own copy */
        co.is_escape_child = (i == 0);
        co.report_fd       = rp[1];
        pid_t p = fork();
        if (p < 0) {
            log_err("fork: %s", strerror(errno));
            return 70;
        }
        if (p == 0) {
            close(rp[0]);                  /* unused end closed (Lab 5) */
            sim_child_main(&co);
        }
        kids[i] = p;
    }
    close(rp[1]);                          /* unused end closed (Lab 5) */
    fcntl(rp[0], F_SETFL, O_NONBLOCK);

    int64_t latest[SIM_MAX_CHILDREN];
    for (int j = 0; j < nk; j++)
        latest[j] = -1;
    int64_t grand_latest = -1;
    pid_t  grandkid = -1;
    int    alive = nk, leak_sent = 0, stop_sent = 0, t = 0, last_t = 0;
    int64_t stop_deadline_usec = 0;
    /* Ticks are wall-clock driven: nanosleep(100ms) can overshoot for
     * background processes, so counting naps would drift. */
    const int64_t start_usec = now_usec();

    while (alive > 0) {
        if (g_stop && !stop_sent) {
            printf("[sim] stop: SIGTERM -> children\n");
            for (int i = 0; i < nk; i++)
                if (kids[i] > 0) kill(kids[i], SIGTERM);
            if (grandkid > 0)
                kill(grandkid, SIGTERM);
            stop_sent = 1;
            stop_deadline_usec = now_usec() + 5000000;
        }

        nap_100ms();
        t = (int)((now_usec() - start_usec) / 1000000);

        drain_reports(rp[0], kids, latest, nk, &grandkid, &grand_latest);

        /* Lab 3: reap finished children without blocking. */
        for (int i = 0; i < nk; i++) {
            if (kids[i] <= 0)
                continue;
            int st = 0;
            pid_t r = waitpid(kids[i], &st, WNOHANG);
            if (r == kids[i]) {
                if (WIFEXITED(st))
                    printf("[sim] child %d exited %d\n", kids[i], WEXITSTATUS(st));
                else if (WIFSIGNALED(st))
                    printf("[sim] child %d killed by signal %d\n", kids[i], WTERMSIG(st));
                kids[i] = -1;
                alive--;
            }
        }

        /* Lab 5: SIGUSR1 starts the leak — either on schedule or because
         * someone sent SIGUSR1 to the parent. */
        if (!leak_sent && o.leak_mb_s > 0 &&
            (g_leak_now || t >= o.leak_after_s)) {
            printf("[sim] leak trigger (SIGUSR1) -> children, %d MB/s each\n", o.leak_mb_s);
            for (int i = 0; i < nk; i++)
                if (kids[i] > 0) kill(kids[i], SIGUSR1);
            if (grandkid > 0)
                kill(grandkid, SIGUSR1);
            leak_sent = 1;
        }

        /* Enforcer-style force kill after the 5 s grace (mirrors Phase 7). */
        if (stop_sent && now_usec() >= stop_deadline_usec) {
            for (int i = 0; i < nk; i++) {
                if (kids[i] > 0) {
                    printf("[sim] SIGKILL %d\n", kids[i]);
                    kill(kids[i], SIGKILL);
                }
            }
            if (grandkid > 0) {
                printf("[sim] SIGKILL grandchild %d\n", grandkid);
                kill(grandkid, SIGKILL);
                grandkid = -1;
            }
        }

        int64_t total = 0;
        int reporters = 0;
        for (int j = 0; j < nk; j++)
            if (latest[j] > 0) { total += latest[j]; reporters++; }
        if (grand_latest > 0) { total += grand_latest; reporters++; }
        if (t > 0 && t != last_t) {
            last_t = t;
            printf("[sim] t=%ds alive=%d/%d reported=%d total=%lldMB leak=%s\n",
                   t, alive, nk, reporters, (long long)total,
                   leak_sent ? "on" : (o.leak_mb_s > 0 ? "off" : "n/a"));
        }
    }

    /* Final blocking reap for anyone the WNOHANG loop missed. */
    for (int i = 0; i < nk; i++) {
        if (kids[i] <= 0)
            continue;
        int st = 0;
        while (waitpid(kids[i], &st, 0) < 0 && errno == EINTR)
            ;
    }
    /* give a still-running grandchild a moment to deliver its R_FINAL */
    nap_100ms(); nap_100ms(); nap_100ms();
    drain_reports(rp[0], kids, latest, nk, &grandkid, &grand_latest);
    close(rp[0]);
    if (shmid >= 0)
        shmctl(shmid, IPC_RMID, NULL);     /* Lab 6: segment no longer needed */
    if (grandkid > 0)
        printf("[sim] note: grandchild %d was reaped by launchd\n", grandkid);
    printf("[sim] warden-sim finished\n");
    return 0;
}

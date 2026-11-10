/*
 * children.c — the forked side of warden-sim.
 *   Lab 3: fork()ed children with their own copy of sim_opts
 *   Lab 4: pthread leak workers with struct args and heap results
 *   Lab 5: signal()/kill()/pause(), SIGUSR1 leak trigger, report pipe
 * Every child reports {pid, allocated_mb} once per second and starts
 * leaking when the parent sends SIGUSR1.
 */
#include "sim.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Rule 3: handlers only set sig_atomic_t flags. */
static volatile sig_atomic_t g_wake;   /* SIGALRM: 1 s tick       */
static volatile sig_atomic_t g_leak;   /* SIGUSR1: start leaking  */
static volatile sig_atomic_t g_stop;   /* SIGTERM/SIGINT/SIGHUP   */

static _Atomic uint64_t g_leaked_by_threads;   /* live total for reports */

static void on_alrm(int s) { (void)s; g_wake = 1; }
static void on_usr1(int s) { (void)s; g_leak = 1; }
static void on_stop(int s) { (void)s; g_stop = 1; }

/* Lab 5: signal() as taught; SIGPIPE ignored so a dead parent shows up as
 * a write error instead of killing the child silently. */
static void install_child_handlers(int ignore_hup)
{
    signal(SIGALRM, on_alrm);
    signal(SIGUSR1, on_usr1);
    signal(SIGTERM, on_stop);
    signal(SIGINT,  on_stop);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  ignore_hup ? SIG_IGN : on_stop);
}

/* Dirty `bytes` of new memory page by page (a write every 4096 bytes) so
 * the kernel really accounts for it; malloc alone rarely touches pages.
 * The memory is never freed — that is the point of a leak. */
static uint64_t touch_memory(uint64_t bytes)
{
    if (bytes == 0)
        return 0;
    unsigned char *p = malloc((size_t)bytes);
    if (p == NULL)
        return 0;
    for (uint64_t i = 0; i < bytes; i += 4096)
        p[i] = (unsigned char)(i >> 12);
    return bytes;
}

static void send_report(int fd, int32_t kind, int32_t mb)
{
    sim_report r = { .kind = kind, .mb = mb, .pid = getpid() };
    if (write(fd, &r, sizeof(r)) != (ssize_t)sizeof(r))
        _exit(41);               /* report pipe broken → parent is gone */
}

/* Lab 5: alarm(1) + pause() — sleep until SIGALRM fires (the 1 s tick) or
 * SIGUSR1/SIGTERM arrives; the handler only sets a flag and pause() then
 * returns. The loop guard makes a trigger that lands early harmless. */
static void wait_a_second(void)
{
    g_wake = 0;
    alarm(1);
    while (!g_wake && !g_leak && !g_stop)
        pause();
    alarm(0);                    /* cancel if a trigger woke us first */
}

/* ------------------------------------------------------------ Lab 4 */

typedef struct { uint64_t chunk_bytes; }  leak_ctx;    /* struct argument */
typedef struct { uint64_t leaked_bytes; } leak_result; /* heap result     */

/* Lab 4: a pthread worker. It gets its work through a struct argument and
 * returns its totals through a heap-allocated result the joiner frees. */
static void *leak_thread(void *arg)
{
    leak_ctx *ctx = arg;
    leak_result *res = malloc(sizeof(*res));
    if (res == NULL)
        return NULL;
    res->leaked_bytes = 0;

    while (!g_stop) {
        if (g_leak && ctx->chunk_bytes > 0) {
            res->leaked_bytes += touch_memory(ctx->chunk_bytes);
            atomic_fetch_add(&g_leaked_by_threads, ctx->chunk_bytes);
        }
        /* threads cannot use alarm+pause (both are per-process), so the
         * tick is four 250 ms nanosleep slices that notice g_stop fast */
        for (int i = 0; i < 4 && !g_stop; i++) {
            struct timespec ts = { 0, 250L * 1000L * 1000L };
            nanosleep(&ts, NULL);
        }
    }
    return res;
}

/* agents worker: T threads leak leak_mb_s MB/s in total. On SIGTERM the
 * main thread joins them (Lab 4) and reports the final total. */
static void child_agents(const sim_opts *o)
{
    uint64_t total = touch_memory((uint64_t)o->per_child_mb << 20);

    int n = o->threads;
    if (o->leak_mb_s <= 0)
        n = 0;                   /* nothing to leak → no workers */
    pthread_t th[SIM_MAX_THREADS];
    leak_ctx  ctx[SIM_MAX_THREADS];
    int started = 0;

    uint64_t chunk = 0;
    if (n > 0) {
        chunk = ((uint64_t)o->leak_mb_s << 20) / (uint64_t)n;
        if (chunk == 0)
            chunk = 4096;
    }
    for (int i = 0; i < n; i++) {
        ctx[i].chunk_bytes = chunk;
        if (pthread_create(&th[i], NULL, leak_thread, &ctx[i]) == 0)
            started++;
    }

    while (!g_stop) {
        uint64_t now = total + atomic_load(&g_leaked_by_threads);
        send_report(o->report_fd, R_REPORT, (int32_t)(now >> 20));
        wait_a_second();
    }

    /* Lab 4: join the workers, collect and free their heap results. */
    uint64_t joined = 0;
    for (int i = 0; i < started; i++) {
        void *out = NULL;
        if (pthread_join(th[i], &out) == 0 && out != NULL) {
            joined += ((leak_result *)out)->leaked_bytes;
            free(out);
        }
    }
    send_report(o->report_fd, R_FINAL, (int32_t)((total + joined) >> 20));
    _exit(0);
}

/* ------------------------------------------------------ listener */

/* devserver: bind+listen on 127.0.0.1:port so the port finder (Phase 9)
 * has something to find. Non-blocking; accepts are drained each tick. */
static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 8) != 0) {
        log_err("devserver bind/listen port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

static void drain_accepts(int lfd)
{
    if (lfd < 0)
        return;
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0)
            break;               /* EAGAIN: nothing pending */
        close(c);                /* connection just gets dropped */
    }
}

/* ------------------------------------------------------ plain children */

/* Shared tick loop for install / devserver / watcher (and the escape
 * grandchild): report once per second; leak leak_mb_s per tick after the
 * SIGUSR1 trigger; ramp=1 grows to per_child_mb in steps (install). */
static void plain_child(const sim_opts *o, int ramp, int lfd)
{
    uint64_t total = 0;
    uint64_t goal  = (uint64_t)o->per_child_mb << 20;
    int      step  = o->per_child_mb / 10;

    if (!ramp)
        total = touch_memory(goal);
    if (step < 1)
        step = 1;

    for (;;) {
        if (g_stop)
            break;
        if (ramp && total < goal)
            total += touch_memory((uint64_t)step << 20);
        else if (g_leak && o->leak_mb_s > 0)
            total += touch_memory((uint64_t)o->leak_mb_s << 20);
        drain_accepts(lfd);
        send_report(o->report_fd, R_REPORT, (int32_t)(total >> 20));
        wait_a_second();
    }
    send_report(o->report_fd, R_FINAL, (int32_t)(total >> 20));
    _exit(0);
}

/* ------------------------------------------------------ escape child */

/* --escape: the grandchild calls setsid() (new session → new pgid) and
 * its parent exits immediately after, so the grandchild is re-parented to
 * launchd: outside the workload's process group, but it still reports
 * through the inherited pipe fd and still carries the inherited environ
 * (WARDEN_ID under `warden run`). */
static void grandchild_main(const sim_opts *o)
{
    install_child_handlers(o->ignore_hup);
    setsid();
    plain_child(o, 0, -1);
}

static void escape_child(const sim_opts *o)
{
    fflush(NULL);
    pid_t g = fork();            /* Lab 3: the double-fork pattern */
    if (g == 0)
        grandchild_main(o);      /* never returns */
    sim_report r = { .kind = R_GRANDKID, .mb = 0, .pid = g };
    if (write(o->report_fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) {
        if (g > 0)
            kill(g, SIGKILL);
        _exit(42);
    }
    _exit(0);                    /* orphan the grandchild under launchd */
}

/* ------------------------------------------------------------ entry */

void sim_child_main(const sim_opts *o)
{
    install_child_handlers(o->ignore_hup);

    if (o->escape && o->is_escape_child) {
        escape_child(o);
        _exit(0);
    }

    /* --shared-mb: touch the inherited System V segment (Lab 6) so the
     * pages are resident and shared by every child. */
    if (o->shared_map != NULL) {
        unsigned char *sh = o->shared_map;
        for (size_t i = 0; i < o->shared_bytes; i += 4096)
            sh[i] = (unsigned char)(i >> 12);
    }

    if (strcmp(o->profile, "agents") == 0) {
        child_agents(o);
    } else if (strcmp(o->profile, "devserver") == 0) {
        int lfd = -1;
        if (o->port > 0) {
            lfd = listen_on(o->port);
            if (lfd < 0)
                _exit(48);       /* port is the point of the profile */
        }
        plain_child(o, 0, lfd);
    } else if (strcmp(o->profile, "install") == 0) {
        plain_child(o, 1, -1);
    } else {                     /* watcher: idle, low footprint */
        plain_child(o, 0, -1);
    }
    _exit(0);
}

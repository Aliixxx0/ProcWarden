/*
 * tests/ipc_test.c — Phase 1: the IPC layer.
 *
 *  1. msg flow: this process plays the fake daemon; six forked clients
 *     send three URGENT and three NORMAL requests. Every client must get
 *     its own reply (mtype = pid routing) and all URGENT requests must be
 *     served before all NORMAL ones (DEV-001 two-step receive).
 *  2. reply timeout: with nobody answering, ipc_wait_reply must return
 *     ETIMEDOUT within the configured ~2 s (dead-daemon detection).
 *  3. seqlock: a writer child publishes paired fields 20000 times while a
 *     read-only "panel" child copies concurrently; a torn row would break
 *     the pairing (footprint == budget, rss == 2*footprint).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "log.h"

static int n_fail;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        n_fail++;
}

static pid_t reap(pid_t pid, int *st)
{
    pid_t r;
    do {
        r = waitpid(pid, st, 0);
    } while (r < 0 && errno == EINTR);
    return r;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------- test 1: flow */

struct cli_result {
    pid_t   pid;
    int     urgent;
    int32_t rep_op;     /* echoed request op from the reply */
    int32_t seq;        /* daemon's processing order stamp */
    int     ok;
};

/* Child: one client — send one request, wait for the reply addressed to
 * this pid, report what came back through the pipe. */
static void client_run(int urgent, int delay_ms, int wfd)
{
    if (delay_ms > 0)
        sleep_ms(delay_ms);

    int op = urgent ? OP_KILL : OP_HEARTBEAT;
    int rc = urgent
        ? ipc_send_urgent(op, 7, 3, 0, 0, "client request")
        : ipc_send_normal(op, 7, 60, 0, 0, NULL);

    struct cli_result r;
    memset(&r, 0, sizeof(r));
    r.pid    = getpid();
    r.urgent = urgent;

    warden_msg rep;
    if (rc == 0 && ipc_wait_reply(&rep, 3000) == 0 &&
        rep.op == OP_REPLY && rep.client_pid == (int32_t)getpid()) {
        r.rep_op = rep.b;      /* the fake daemon echoes the request op in b */
        r.seq    = rep.a;      /* ...and stamps its processing order in a    */
        r.ok     = 1;
    }

    if (write(wfd, &r, sizeof(r)) != (ssize_t)sizeof(r))
        _exit(40);
    _exit(r.ok ? 0 : 1);
}

/* Fake daemon: receive n requests (URGENT first), reply to each client. */
static int fake_daemon(int n)
{
    for (int seq = 1; seq <= n; seq++) {
        warden_msg m;
        if (ipc_recv_request(&m, 1) != 0)
            return -1;
        if (m.mtype != MSGT_URGENT && m.mtype != MSGT_NORMAL)
            return -1;
        if (ipc_send_reply(m.client_pid, OP_REPLY, seq, m.op, 0, NULL) != 0)
            return -1;
    }
    return 0;
}

static void test_msg_flow(void)
{
    fflush(stdout);
    int rp[2];
    if (pipe(rp) != 0) {
        log_err("pipe: %s", strerror(errno));
        check("ipc: URGENT served first + per-client replies", 0);
        return;
    }

    pid_t kids[6];
    int nk = 0;
    for (int i = 0; i < 3; i++) {          /* three URGENT clients, immediate */
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) {
            close(rp[0]);
            client_run(1, 0, rp[1]);
        }
        kids[nk++] = p;
    }
    for (int i = 0; i < 3; i++) {          /* three NORMAL clients, 400 ms later */
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) {
            close(rp[0]);
            client_run(0, 400, rp[1]);
        }
        kids[nk++] = p;
    }
    close(rp[1]);

    sleep_ms(100);                         /* let the URGENT requests queue up */

    int drc = fake_daemon(6);

    struct cli_result res[6];
    int got = 0;
    while (got < 6) {
        ssize_t r = read(rp[0], &res[got], sizeof(res[0]));
        if (r < 0 && errno == EINTR)
            continue;
        if (r != (ssize_t)sizeof(res[0]))
            break;
        got++;
    }
    close(rp[0]);

    int ok = (drc == 0 && got == 6);
    int max_urgent_seq = 0, min_normal_seq = 1 << 30;
    for (int i = 0; i < got; i++) {
        ok = ok && res[i].ok;
        ok = ok && res[i].rep_op == (res[i].urgent ? OP_KILL : OP_HEARTBEAT);
        if (res[i].urgent) {
            if (res[i].seq > max_urgent_seq)
                max_urgent_seq = res[i].seq;
        } else if (res[i].seq < min_normal_seq) {
            min_normal_seq = res[i].seq;
        }
    }
    ok = ok && max_urgent_seq >= 1 && max_urgent_seq < min_normal_seq;

    for (int i = 0; i < nk; i++) {
        int st = 0;
        pid_t r = reap(kids[i], &st);
        ok = ok && r == kids[i] && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }

    printf("note: URGENT reply seqs <= %d, NORMAL reply seqs >= %d\n",
           max_urgent_seq, min_normal_seq);
    check("ipc: URGENT served first + per-client replies", ok);
}

/* --------------------------------------------------- test 2: timeout */

static void test_reply_timeout(void)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        /* A client whose daemon never replies must notice within ~2 s. */
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        warden_msg rep;
        int timeout_ms = (int)config_int("ipc_reply_timeout_ms", 2000);
        int rc = ipc_wait_reply(&rep, timeout_ms);
        gettimeofday(&t1, NULL);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
        int ok = (rc == -1 && errno == ETIMEDOUT && ms >= timeout_ms - 100 && ms <= timeout_ms + 900);
        printf("note: wait_reply gave up after %ld ms (errno=%s)\n",
               ms, strerror(errno));
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    reap(p, &st);
    check("ipc: dead daemon noticed within ~2 s (ETIMEDOUT)",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* --------------------------------------------------- test 3: seqlock */

enum { PUBLISH_ROUNDS = 20000 };

static void writer_child(status_shm *shm)
{
    status_shm snap;                       /* the daemon's private draft */
    for (uint64_t i = 1; i <= PUBLISH_ROUNDS; i++) {
        memset(&snap, 0, sizeof(snap));
        snap.sample_no      = i;
        snap.t_usec         = (int64_t)i;
        snap.n_wl           = 1;
        snap.wl[0].id       = 1;
        snap.wl[0].footprint = i;          /* paired fields: a torn copy  */
        snap.wl[0].budget    = i;          /* would break the pairing     */
        snap.wl[0].rss       = i * 2;
        status_publish(shm, &snap);
        /* a real daemon samples once a second; scale that down so the
         * reader mostly sees a stable (even) seq between publishes */
        struct timespec ts = { 0, 50 * 1000L };
        nanosleep(&ts, NULL);
    }
    _exit(0);
}

static void reader_child(const status_shm *ro, int stopfd)
{
    fcntl(stopfd, F_SETFL, O_NONBLOCK);
    unsigned long reads = 0, gave_up = 0, mismatched = 0, backwards = 0;
    uint64_t last = 0;
    status_shm copy;
    unsigned char b;

    for (;;) {
        if (status_copy(ro, &copy) == 0) {
            reads++;
            if (copy.wl[0].footprint != copy.wl[0].budget ||
                copy.wl[0].rss != copy.wl[0].footprint * 2)
                mismatched++;              /* a torn row — must never happen */
            if (copy.sample_no < last)
                backwards++;               /* time going backwards = tearing */
            last = copy.sample_no;
        } else {
            gave_up++;                     /* bounded retries exhausted: ok, rare */
        }
        if (read(stopfd, &b, 1) == 1)
            break;
    }
    printf("note: seqlock reader: %lu consistent copies, %lu torn rows, "
           "%lu gave up, %lu went backwards\n",
           reads, mismatched, gave_up, backwards);
    _exit((mismatched == 0 && backwards == 0 && reads > 0) ? 0 : 1);
}

static void test_seqlock(void)
{
    fflush(stdout);
    status_shm *rw = NULL;
    if (ipc_status_create(&rw) < 0) {
        check("ipc: seqlock publish/copy (no torn rows)", 0);
        return;
    }
    status_shm *ro = NULL;                 /* panel-style read-only attach */
    if (ipc_status_open(&ro) < 0) {
        ipc_status_detach(rw);
        check("ipc: seqlock publish/copy (no torn rows)", 0);
        return;
    }

    int stop[2];
    if (pipe(stop) != 0) {
        ipc_status_detach(rw);
        ipc_status_detach(ro);
        check("ipc: seqlock publish/copy (no torn rows)", 0);
        return;
    }

    fflush(stdout);
    pid_t wr = fork();
    if (wr == 0) {
        close(stop[0]);
        writer_child(rw);                  /* inherited rw mapping */
    }
    pid_t rd = fork();
    if (rd == 0) {
        close(stop[1]);
        reader_child(ro, stop[0]);         /* inherited read-only mapping */
    }
    close(stop[0]);

    int stw = 0, str_ = 0;
    reap(wr, &stw);
    if (write(stop[1], "x", 1) != 1)
        log_err("stop write: %s", strerror(errno));
    close(stop[1]);
    reap(rd, &str_);

    ipc_status_detach(rw);
    ipc_status_detach(ro);

    check("ipc: seqlock publish/copy (no torn rows)",
          WIFEXITED(stw) && WEXITSTATUS(stw) == 0 &&
          WIFEXITED(str_) && WEXITSTATUS(str_) == 0);
}

/* ------------------------------------------------------------- main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);      /* forked children must not inherit buffers */

    printf("== ProcWarden Phase 1 IPC test ==\n");

    ipc_queue_destroy();                   /* start from clean objects */
    ipc_status_destroy();

    printf("== message queue ==\n");
    if (ipc_queue_create() < 0) {
        printf("FATAL: could not create the message queue\n");
        return 1;
    }
    test_msg_flow();
    test_reply_timeout();

    printf("== shared memory ==\n");
    test_seqlock();

    ipc_queue_destroy();
    ipc_status_destroy();

    if (n_fail > 0) {
        printf("== %d CHECK(S) FAILED ==\n", n_fail);
        return 1;
    }
    printf("== ALL CHECKS PASSED ==\n");
    return 0;
}

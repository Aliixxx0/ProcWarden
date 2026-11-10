/*
 * ipc.c — Lab 6 implementation: the single message queue and the status
 * shared-memory segment with its single-writer seqlock.
 *
 * IPC objects come from ftok("~/.procwarden/key", 'Q' | 'S') with 0600
 * permissions (CLAUDE.md Rule 4). Every blocking call runs in an EINTR
 * loop (Rule 3), and every syscall's return value is checked.
 */
#include "ipc.h"

#include "config.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define STATUS_COPY_RETRIES 64

static int qid   = -1;          /* cached queue id  */
static int shmid = -1;          /* cached segment id */

static const char *home_dir(void)
{
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0')
        return home;
    struct passwd *pw = getpwuid(getuid());
    return (pw != NULL) ? pw->pw_dir : NULL;
}

/* Rule 4: keys come from ftok("~/.procwarden/key", flavor). The directory
 * and the key file are created on demand (ftok needs the file to exist). */
static key_t ipc_key(int flavor)
{
    const char *home = home_dir();
    if (home == NULL) {
        log_err("no HOME directory for the ftok key file");
        errno = EINVAL;
        return (key_t)-1;
    }

    char dir[300], kpath[340];
    snprintf(dir, sizeof(dir), "%s/.procwarden", home);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        log_err("mkdir %s: %s", dir, strerror(errno));
        return (key_t)-1;
    }

    snprintf(kpath, sizeof(kpath), "%s/key", dir);
    int fd = open(kpath, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        log_err("key file %s: %s", kpath, strerror(errno));
        return (key_t)-1;
    }
    close(fd);

    key_t k = ftok(kpath, flavor);
    if (k == (key_t)-1)
        log_err("ftok(%s, '%c'): %s", kpath, flavor, strerror(errno));
    return k;
}

/* ------------------------------------------------------------- limits */

int ipc_log_limits(void)
{
    int ok = 0;

    int q = qid;
    if (q < 0) {
        key_t k = ipc_key('Q');
        if (k != (key_t)-1)
            q = msgget(k, 0600);
    }
    if (q >= 0) {
        struct msqid_ds qs;
        memset(&qs, 0, sizeof(qs));
        if (msgctl(q, IPC_STAT, &qs) == 0) {
            log_info("ipc", "limits: msg_qbytes=%lu per queue, one message=%zu bytes",
                     (unsigned long)qs.msg_qbytes, WARDEN_MSGSZ);
            if ((size_t)qs.msg_qbytes < WARDEN_MSGSZ) {
                log_err("msg_qbytes=%lu is too small for one %zu-byte message (Rule 4)",
                        (unsigned long)qs.msg_qbytes, WARDEN_MSGSZ);
                ok = -1;
            }
        } else {
            log_warn("ipc", "msgctl(IPC_STAT): %s", strerror(errno));
        }
    }

    int64_t shmmax = 0;
    size_t len = sizeof(shmmax);
    if (sysctlbyname("kern.sysv.shmmax", &shmmax, &len, NULL, 0) == 0) {
        log_info("ipc", "limits: kern.sysv.shmmax=%lld bytes, status_shm=%zu bytes",
                 (long long)shmmax, sizeof(status_shm));
        if ((uint64_t)shmmax < sizeof(status_shm)) {
            log_err("kern.sysv.shmmax=%lld is smaller than status_shm=%zu bytes (Rule 4)",
                    (long long)shmmax, sizeof(status_shm));
            ok = -1;
        }
    } else {
        log_warn("ipc", "sysctl kern.sysv.shmmax unavailable: %s", strerror(errno));
    }

    return ok;
}

/* ------------------------------------------------------- message queue */

int ipc_queue_open(void)
{
    if (qid >= 0)
        return qid;
    key_t k = ipc_key('Q');
    if (k == (key_t)-1)
        return -1;
    qid = msgget(k, 0600);
    if (qid < 0)
        log_err("msgget(queue): %s (is wardend running?)", strerror(errno));
    return qid;
}

int ipc_queue_create(void)
{
    if (qid >= 0)
        return qid;
    key_t k = ipc_key('Q');
    if (k == (key_t)-1)
        return -1;

    qid = msgget(k, IPC_CREAT | IPC_EXCL | 0600);
    if (qid < 0 && errno == EEXIST) {
        /* Rule 4: a stale queue from an earlier daemon is reused as-is;
         * the queue carries no header — version checking applies to shm. */
        qid = msgget(k, 0600);
        if (qid >= 0)
            log_info("ipc", "reusing stale message queue id=%d", qid);
    }
    if (qid < 0) {
        log_err("msgget(create): %s", strerror(errno));
        return -1;
    }

    if (ipc_log_limits() != 0) {
        qid = -1;
        return -1;              /* limits too small: fail loudly, Rule 4 */
    }
    return qid;
}

int ipc_queue_destroy(void)
{
    key_t k = ipc_key('Q');
    if (k == (key_t)-1)
        return -1;
    int q = msgget(k, 0600);
    qid = -1;
    if (q < 0)
        return 0;               /* nothing to remove is fine */
    return (msgctl(q, IPC_RMID, NULL) == 0) ? 0 : -1;
}

/* ------------------------------------------------------- send / receive */

static void msg_fill(warden_msg *m, long mtype, int client_pid, int op,
                     int wl_id, int a, int b, int64_t c, const char *text)
{
    memset(m, 0, sizeof(*m));
    m->mtype      = mtype;
    m->op         = op;
    m->client_pid = client_pid;
    m->wl_id      = wl_id;
    m->a          = a;
    m->b          = b;
    m->c          = c;
    if (text != NULL)
        snprintf(m->text, sizeof(m->text), "%s", text);
}

/* Lab 6: msgsnd of one fixed-size struct; EINTR loop per Rule 3. */
static int send_msg(const warden_msg *m)
{
    int q = ipc_queue_open();
    if (q < 0)
        return -1;
    int r;
    do {
        r = msgsnd(q, m, WARDEN_MSGSZ, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        log_err("msgsnd(op=%d): %s", m->op, strerror(errno));
    return r;
}

int ipc_send_urgent(int op, int wl_id, int a, int b, int64_t c, const char *text)
{
    warden_msg m;
    msg_fill(&m, MSGT_URGENT, (int)getpid(), op, wl_id, a, b, c, text);
    return send_msg(&m);
}

int ipc_send_normal(int op, int wl_id, int a, int b, int64_t c, const char *text)
{
    warden_msg m;
    msg_fill(&m, MSGT_NORMAL, (int)getpid(), op, wl_id, a, b, c, text);
    return send_msg(&m);
}

int ipc_send_reply(int client_pid, int op, int a, int b, int64_t c, const char *text)
{
    warden_msg m;
    /* Lab 6: the reply's mtype is the client pid — mtype routing delivers
     * it to exactly that client and nobody else. */
    msg_fill(&m, (long)client_pid, client_pid, op, 0, a, b, c, text);
    return send_msg(&m);
}

int ipc_recv_request(warden_msg *out, int blocking)
{
    int q = ipc_queue_open();
    if (q < 0)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Lab 6 + DEV-001: a negative msgtyp on macOS is an *upper bound*
     * (oldest message with type <= |msgtyp|), not a lowest-first sort.
     * Two steps on the one queue keep URGENT served before NORMAL:
     *   step 1: drain mtype <= 1 (URGENT) without blocking;
     *   step 2: block on mtype <= 2 (NORMAL); replies (type > 2) are
     *           excluded by the bound in both steps. */
    ssize_t r = msgrcv(q, out, WARDEN_MSGSZ, -(long)MSGT_URGENT, IPC_NOWAIT);
    if (r >= 0)
        return 0;
    if (!blocking || errno != ENOMSG)
        return -1;              /* ENOMSG (nothing urgent) or EINTR — caller decides */

    for (;;) {
        r = msgrcv(q, out, WARDEN_MSGSZ, -(long)MSGT_NORMAL, 0);
        if (r >= 0)
            return 0;
        if (errno != EINTR) {
            log_err("msgrcv(normal): %s", strerror(errno));
            return -1;
        }
        /* Rule 3: EINTR — try again */
    }
}

static int64_t now_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

int ipc_wait_reply(warden_msg *out, int timeout_ms)
{
    int q = ipc_queue_open();
    if (q < 0)
        return -1;

    int sleep_ms = (int)config_int("ipc_retry_sleep_ms", 50);
    if (sleep_ms <= 0)
        sleep_ms = 50;
    int64_t deadline = now_usec() + (int64_t)timeout_ms * 1000;

    for (;;) {
        memset(out, 0, sizeof(*out));
        /* Lab 6: mtype == getpid() picks THIS client's reply and no other. */
        ssize_t r = msgrcv(q, out, WARDEN_MSGSZ, (long)getpid(), IPC_NOWAIT);
        if (r >= 0)
            return 0;
        if (errno != ENOMSG && errno != EINTR) {
            log_err("msgrcv(reply): %s", strerror(errno));
            return -1;
        }
        if (now_usec() >= deadline) {
            errno = ETIMEDOUT;  /* dead daemon noticed within timeout_ms */
            return -1;
        }
        struct timespec ts = { sleep_ms / 1000, (long)(sleep_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
}

/* ------------------------------------------------------------ status shm */

int ipc_status_create(status_shm **out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    key_t k = ipc_key('S');
    if (k == (key_t)-1)
        return -1;

    int fresh = 1;
    int id = shmget(k, sizeof(status_shm), IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0 && errno == EEXIST) {
        /* Rule 4: stale segment — reuse only after the header checks out. */
        id = shmget(k, sizeof(status_shm), 0600);
        fresh = 0;
        if (id >= 0)
            log_info("ipc", "reusing stale status segment id=%d", id);
    }
    if (id < 0) {
        log_err("shmget(status): %s", strerror(errno));
        return -1;
    }

    status_shm *s = shmat(id, NULL, 0);
    if (s == (status_shm *)-1) {
        log_err("shmat(rw): %s", strerror(errno));
        return -1;
    }

    if (fresh) {
        status_init(s);
    } else if (s->magic != WARDEN_SHM_MAGIC || s->version != WARDEN_SHM_VERSION) {
        log_err("stale status segment header mismatch (magic=0x%08x version=%u); "
                "remove it with `ipcrm -m %d` or delete ~/.procwarden/key",
                s->magic, s->version, id);
        shmdt(s);
        return -1;
    } else {
        status_takeover(s);     /* previous daemon may have died mid-publish */
    }

    shmid = id;
    *out = s;
    return id;
}

int ipc_status_open(status_shm **out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    key_t k = ipc_key('S');
    if (k == (key_t)-1)
        return -1;
    int id = shmget(k, sizeof(status_shm), 0600);
    if (id < 0) {
        log_err("shmget(status): %s (is wardend running?)", strerror(errno));
        return -1;
    }
    /* The panel never writes: attach read-only. */
    status_shm *s = shmat(id, NULL, SHM_RDONLY);
    if (s == (status_shm *)-1) {
        log_err("shmat(ro): %s", strerror(errno));
        return -1;
    }
    *out = s;
    return id;
}

void ipc_status_detach(status_shm *s)
{
    if (s != NULL)
        shmdt(s);
}

int ipc_status_destroy(void)
{
    key_t k = ipc_key('S');
    if (k == (key_t)-1)
        return -1;
    int id = shmget(k, sizeof(status_shm), 0600);
    shmid = -1;
    if (id < 0)
        return 0;               /* nothing to remove is fine */
    return (shmctl(id, IPC_RMID, NULL) == 0) ? 0 : -1;
}

void status_init(status_shm *s)
{
    memset(s, 0, sizeof(*s));
    s->magic   = WARDEN_SHM_MAGIC;
    s->version = WARDEN_SHM_VERSION;
    atomic_store(&s->seq, 0);   /* even = stable */
}

void status_takeover(status_shm *s)
{
    uint64_t v = atomic_load(&s->seq);
    if ((v & 1u) != 0) {
        /* Single-writer rule: the previous daemon died between the two
         * increments; flip seq back to even so readers stop retrying. */
        atomic_fetch_add(&s->seq, 1);
        log_warn("ipc", "released a seqlock left odd by the previous daemon");
    }
}

/* Writer side of the single-writer seqlock. The two seq_cst fetch_adds
 * bracket the payload copy, so a reader that sees an even, unchanged seq
 * is guaranteed a consistent snapshot (write-side release / read-side
 * acquire come from the seq_cst ordering). */
void status_publish(status_shm *s, const status_shm *snap)
{
    atomic_fetch_add(&s->seq, 1);                       /* -> odd: writing */
    size_t off = offsetof(status_shm, sample_no);       /* payload starts after seq */
    memcpy((char *)s + off, (const char *)snap + off, sizeof(status_shm) - off);
    atomic_fetch_add(&s->seq, 1);                       /* -> even: stable */
}

/* Reader side: retry while a write is in progress (odd) or one landed
 * during our copy (seq changed). Bounded so a redrawing panel never hangs. */
int status_copy(const status_shm *src, status_shm *dst)
{
    for (int i = 0; i < STATUS_COPY_RETRIES; i++) {
        uint64_t s1 = atomic_load(&src->seq);
        if ((s1 & 1u) == 0) {
            memcpy(dst, src, sizeof(*dst));
            uint64_t s2 = atomic_load(&src->seq);
            if (s1 == s2)
                return 0;       /* consistent snapshot */
        }
    }
    return -1;                  /* writer hammering — caller retries later */
}

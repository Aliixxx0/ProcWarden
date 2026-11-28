#ifndef PROCWARDEN_IPC_H
#define PROCWARDEN_IPC_H

/*
 * ipc.h — Lab 6 is the backbone of this file: ONE System V message queue
 * carries every request/reply (mtype routing), and ONE System V shared
 * memory segment carries the live status table the panel reads.
 *
 * Queue protocol (CLAUDE.md Rule 4: messages stay <= 256 bytes because
 * macOS queues hold only a few KB):
 *   mtype 1        URGENT request to the daemon  (kill, cancel)
 *   mtype 2        NORMAL request to the daemon  (register, heartbeat, ...)
 *   mtype = pid    reply from the daemon to exactly that one client
 *                  (pids 1 and 2 are never clients, so no clash)
 *
 * Shared memory is written ONLY by the daemon (single writer) and read by
 * the panel through a seqlock: seq odd = write in progress, even = stable.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WARDEN_SHM_MAGIC   0x57415244u  /* "WARD" */
#define WARDEN_SHM_VERSION 2u

/* message types on the single queue */
#define MSGT_URGENT 1
#define MSGT_NORMAL 2

/* fixed message geometry */
#define WARDEN_MSG_TEXT_MAX 160
#define WARDEN_MSG_SIZE_MAX 256

/* status table geometry */
#define IPC_MAX_WL    64
#define IPC_MAX_PROCS 512

enum band { BAND_GREEN = 0, BAND_YELLOW, BAND_ORANGE, BAND_RED };

enum paused_by { PAUSED_NONE = 0, PAUSED_DAEMON = 1, PAUSED_USER = 2 };

enum wl_state { WL_ACTIVE = 0, WL_OWNER_LOST, WL_EXITED };

enum warden_op {
    /* requests to the daemon (mtype 1 URGENT / 2 NORMAL) */
    OP_REGISTER, OP_ADMIT_REQ, OP_EXITED, OP_DETACHED, OP_HEARTBEAT,
    OP_KILL, OP_PAUSE, OP_RESUME, OP_RENICE, OP_SETLIMIT, OP_ADOPT,
    OP_CANCEL, OP_PORT_LOOKUP, OP_PORT_FREE, OP_REPLY,
    /* daemon replies (mtype = requesting client's pid) */
    REP_ADMIT, REP_QUEUED, REP_REFUSED, REP_TIMEOUT, REP_OK, REP_ERR
};

/* One message. a/b carry small args (pid, signal, port, exit status ...),
 * c carries byte counts or start-time usec, text carries the project dir,
 * a truncated command line, or error text. */
typedef struct {
    long     mtype;                 /* 1 URGENT, 2 NORMAL, >2 reply-to-pid */
    int32_t  op;                    /* enum warden_op */
    int32_t  client_pid;            /* where the daemon sends the reply */
    int32_t  wl_id;                 /* workload id, 0 if none */
    int32_t  a, b;                  /* small args */
    int64_t  c;                     /* bytes / start-time usec */
    char     text[WARDEN_MSG_TEXT_MAX];
} warden_msg;

/* Lab 6: msgrcv/msgsnd size = payload without mtype. */
#define WARDEN_MSGSZ (sizeof(warden_msg) - sizeof(long))

_Static_assert(sizeof(warden_msg) <= WARDEN_MSG_SIZE_MAX,
               "warden_msg must fit in 256 bytes (macOS queue byte limit, Rule 4)");
_Static_assert(sizeof(warden_msg) - sizeof(long) <= 248,
               "warden_msg payload must fit in 248 bytes");

/* System memory sample: T, A, pressure, swapout rate, compressor, swap used,
 * free disk, effective band, enforce flag (Phase 6 fills it for real). */
typedef struct {
    uint64_t total_bytes;           /* T: hw.memsize */
    uint64_t avail_bytes;           /* A: free+inactive+purgeable+speculative */
    uint64_t compressor_pages;
    uint64_t swap_used_bytes;       /* vm.swapusage */
    uint64_t free_disk_bytes;       /* statfs("/") */
    double   swapout_rate;          /* S: swapouts per second */
    int32_t  pressure;              /* P: 1 normal, 2 warning, 4 critical */
    int32_t  band;                  /* enum band, after hysteresis */
    uint8_t  enforce;               /* 0 observe, 1 enforce (Rule 2.5) */
    uint8_t  pad1[7];               /* keep the layout identical everywhere */
} sys_mem;

/* One workload row of the status table. */
typedef struct {
    int32_t  id;                    /* wl-N id */
    int32_t  state;                 /* enum wl_state */
    int32_t  pgid;
    int32_t  owner_pid;             /* launcher pid */
    uint64_t footprint;             /* sum ri_phys_footprint */
    uint64_t rss;                   /* sum pti_resident_size */
    uint64_t budget;                /* B in bytes */
    double   slope;                 /* growth, MB/s */
    int32_t  paused_by;             /* enum paused_by */
    int32_t  n_members;
    char     project[24];
    char     last_rule[24];
} wl_row;

/* One process row of the status table (wl_id 0 = unmanaged). */
typedef struct {
    int32_t  pid, ppid, pgid, wl_id;
    uint64_t footprint;
    uint64_t rss;
    int32_t  cpu_permille;          /* CPU% x 10 */
    int32_t  start_sec;             /* pid-reuse guard, for display */
    uint8_t  state_ch;              /* 'R','S','Z',... like ps */
    uint8_t  escaped;               /* member that left the pgid */
    uint8_t  pad1[6];
    char     comm[32];
} proc_row;

/* Shared-memory status table. Written only by the daemon (single writer),
 * read by the panel. Synchronization is the seqlock on `seq`:
 *   writer: seq++ (odd) -> copy payload -> seq++ (even)
 *   reader: read seq, copy, read seq again; retry if odd or changed.
 * This is single-writer synchronization; the synchronization lab's
 * semaphore is the mutual-exclusion alternative. */
typedef struct {
    uint32_t         magic;
    uint32_t         version;
    _Atomic uint64_t seq;           /* odd = write in progress */
    uint64_t         sample_no;
    int64_t          t_usec;        /* sample time */
    sys_mem          mem;
    uint16_t         n_wl, n_unmanaged;
    wl_row           wl[IPC_MAX_WL];
    proc_row         procs[IPC_MAX_PROCS];
} status_shm;

_Static_assert(sizeof(status_shm) < 1024u * 1024u,
               "status_shm must stay under 1 MB (kern.sysv.shmmax is 4 MB, Rule 4)");

/* ---------------------------------------------------------- message queue */

/* Daemon side: create the queue (or reuse a stale one, Rule 4), log the
 * System V limits, fail with a clear message if they are too small.
 * Returns the queue id or -1. */
int ipc_queue_create(void);

/* Client side: open the existing queue. Returns the id or -1. */
int ipc_queue_open(void);

/* Remove the queue (msgctl IPC_RMID); removing nothing is not an error.
 * Returns 0, or -1 on a real failure. */
int ipc_queue_destroy(void);

/* Log msg_qbytes and kern.sysv.shmmax (Rule 4: the daemon logs them at
 * startup). Returns -1 if a limit is too small for our fixed structs. */
int ipc_log_limits(void);

/* ------------------------------------------------------- send / receive */

/* Send helpers fill client_pid = getpid() so the daemon knows where the
 * reply goes. text may be NULL. Return 0/-1. */
int ipc_send_urgent(int op, int wl_id, int a, int b, int64_t c, const char *text);
int ipc_send_normal(int op, int wl_id, int a, int b, int64_t c, const char *text);

/* Daemon -> one client: the reply's mtype is that client's pid, so no other
 * client can consume it (Lab 6 mtype routing). */
int ipc_send_reply(int client_pid, int op, int a, int b, int64_t c, const char *text);

/* Daemon receive. URGENT is always served before NORMAL: per DEV-001
 * (docs/DEVIATIONS.md) macOS negative msgtyp is an upper bound, not a
 * lowest-first sort, so this does a two-step receive on the single queue:
 * first drain mtype <= 1 with IPC_NOWAIT, then (blocking) mtype <= 2.
 * Replies (mtype > 2) are excluded by the bound either way.
 * Returns 0 on a message, -1 otherwise (ENOMSG/EINTR preserved in errno
 * for non-blocking callers). */
int ipc_recv_request(warden_msg *out, int blocking);

/* Client receive: poll for the reply addressed to getpid() with
 * IPC_NOWAIT + short sleeps, so a dead daemon is noticed within
 * timeout_ms instead of blocking forever. Returns 0, or -1 with
 * errno ETIMEDOUT. */
int ipc_wait_reply(warden_msg *out, int timeout_ms);

/* ---------------------------------------------------------- status shm */

/* Daemon side: create the segment (or reuse a stale one after checking
 * magic/version, Rule 4) and attach it read-write. Returns the shmid, or
 * -1 (also when kern.sysv.shmmax is too small). */
int ipc_status_create(status_shm **out);

/* Panel side: attach the existing segment read-only (SHM_RDONLY). */
int ipc_status_open(status_shm **out);

void ipc_status_detach(status_shm *s);

/* Mark the segment for removal (shmctl IPC_RMID); removing nothing is not
 * an error. Returns 0, or -1 on a real failure. */
int ipc_status_destroy(void);

/* Fresh table: magic, version, zero payload, seq = 0 (even, stable). */
void status_init(status_shm *s);

/* After a daemon restart: if the previous writer died mid-publish (seq
 * odd), flip it back to even so readers are never stuck. */
void status_takeover(status_shm *s);

/* Writer side of the seqlock: seq -> odd, copy the payload (everything
 * after `seq`) from the daemon's private draft, seq -> even. */
void status_publish(status_shm *s, const status_shm *snap);

/* Reader side: copy under the seqlock, retrying while a write is in
 * progress or one happened during the copy. 0 = consistent copy,
 * -1 = it never got one (reader just tries again next redraw). */
int status_copy(const status_shm *src, status_shm *dst);

#endif /* PROCWARDEN_IPC_H */

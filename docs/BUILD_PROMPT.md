# Build prompt: ProcWarden

> Paste this into your AI coding assistant at the start of the build. Put `CLAUDE.md` (project instructions) in the repo root and the revised proposal in `docs/`.

---

You are helping a four-person team build **ProcWarden** in C (C11) on macOS for the KFUPM ICS433 Operating Systems lab. The approved specification is `docs/ProcWarden_OS_Lab_Project_Proposal_v2.docx`. The rules in `CLAUDE.md` are binding, especially **Rule 1** (the core logic must be built on the Lab 3–6 mechanisms) and **Rule 2** (safety invariants).

The instructor's feedback on our first proposal has already been built into the specification. The implementation must keep these corrections:

1. Automatic actions apply **only to workloads registered with `warden run`**. An orphan under launchd is not proof of unwanted work.
2. A process tree and a process group are different. Membership is kept separately (pid + start time, parent-chain closure, pgid, `WARDEN_ID` env tag), so it survives re-parenting and members that leave the group.
3. **The launcher owns and reaps** the workload root with `waitpid()`. The daemon never reaps.
4. `SIGSTOP` stops growth. It does **not** free memory. Resume follows a defined policy.
5. The percentage rule is **experimental**. Automatic kill needs the kernel's pressure level to agree, and is off by default (observe mode).
6. Metric, shared memory, growth window, band boundaries, queue, no-victim behaviour and overshoot are all defined below.
7. We do not claim to beat Activity Monitor's hierarchy view. Our value is registered workloads, policies and coordinated control.

Work through the phases **in order**. At the end of each phase: build with `-Werror`, run that phase's tests, show me the output, and commit. Do not start the next phase until I confirm.

---

## Phase 0: Skeleton and lab warm-up (Labs 3–6)

- Create the repository layout from `CLAUDE.md`, a `Makefile` (`all`, `debug` with ASan/UBSan, `tsan`, `test`, `clean`), `log.c` and `config.c` (key=value parser with defaults).
- `tests/lab_check.c`: one small function per lab mechanism we depend on (fork/exec/waitpid with exit-status macros; pthread_create/join with a struct argument; signal + kill + pause with SIGUSR1; pipe between parent and child; shmget/shmat round trip; msgsnd/msgrcv with `mtype` = pid and with negative `msgtyp`). Print the macOS System V limits (`msg_qbytes`, `sysctl kern.sysv.shmmax`).
- **Done when:** `make test` passes and prints the IPC limits on our Macs.

## Phase 1: IPC layer (Lab 6)

`include/ipc.h`, `src/common/ipc.c`

**Message queue protocol.** There is one queue. Every message is a fixed struct of at most 256 bytes:

```c
typedef struct {
    long  mtype;          /* 1 = URGENT to daemon, 2 = NORMAL to daemon, >2 = reply to client pid */
    int32_t op;           /* OP_REGISTER, OP_ADMIT_REQ, OP_EXITED, OP_DETACHED, OP_HEARTBEAT,
                             OP_KILL, OP_PAUSE, OP_RESUME, OP_RENICE, OP_SETLIMIT, OP_ADOPT,
                             OP_CANCEL, OP_PORT_LOOKUP, OP_PORT_FREE, OP_REPLY */
    int32_t client_pid;   /* where to send the reply (reply mtype) */
    int32_t wl_id;        /* workload id, 0 if none */
    int32_t a, b;         /* small args: pid, signal, port, exit status ... */
    int64_t c;            /* bytes / start-time usec */
    char    text[160];    /* project dir, command line (truncated), or error text */
} warden_msg;             /* static_assert(sizeof(warden_msg) - sizeof(long) <= 248) */
```

- The daemon receives with `msgrcv(q, &m, SZ, -2, 0)`. Lab 6: a negative `msgtyp` returns the lowest type first, so URGENT (kill, cancel) is always served before NORMAL.
- Clients send to type 1 or 2 and wait for the reply with `msgrcv(q, &m, SZ, getpid(), 0)`. Lab 6: `mtype` routes the reply to that one client. Use a timeout loop: `IPC_NOWAIT` plus 50 ms sleeps, so a dead daemon is noticed within 2 s.
- PIDs 1 and 2 are never clients, so there is no clash with the request types.

**Shared-memory status table**, written only by the daemon and read by the panel:

```c
typedef struct {
    uint32_t magic, version;
    _Atomic uint64_t seq;          /* odd = write in progress (single-writer seqlock) */
    uint64_t sample_no; int64_t t_usec;
    sys_mem  mem;                  /* T, A, pressure, swapout rate, compressor, swap used, free disk, band, enforce flag */
    uint16_t n_wl, n_unmanaged;
    wl_row   wl[64];               /* id, state, pgid, owner pid, project, footprint, rss, budget, slope, paused, last rule */
    proc_row procs[512];           /* pid, ppid, pgid, wl_id (0 = unmanaged), footprint, rss, cpu%, state, escaped flag */
} status_shm;                      /* static_assert(sizeof(status_shm) < 1 MB) */
```

Writer: `seq++` (odd) → copy → `seq++` (even). Reader: read `seq`, copy, read `seq` again, and retry if it was odd or has changed. Comment that this is single-writer synchronization; the synchronization lab's semaphore is the alternative.

- **Done when:** a test runs one fake daemon and three clients sending mixed URGENT/NORMAL requests. Every client gets its own replies, URGENT requests are served first, and the panel-reader test never sees a torn row.

## Phase 2: Simulator `warden-sim` (Labs 3, 4, 5)

Profiles `install`, `devserver`, `watcher`, `agents`, with options `-n children`, `--mb per child`, `--leak MB/s`, `--threads T`, `--ignore-hup`, `--escape`, `--shared-mb`, `--port`.

- Children are created with `fork()`. Each child sends `{pid, allocated_mb}` once per second to the parent through a **pipe** (unused ends closed, as in Lab 5).
- Leaking starts when the parent sends `kill(child, SIGUSR1)`. The child sleeps in `pause()` until it arrives. Memory is touched page by page so it really becomes dirty.
- `agents`: each worker starts `T` threads with `pthread_create` and a struct argument `{leak_rate, chunk}`. On SIGTERM the workers join them and return totals through heap results (Lab 4).
- `--ignore-hup`: `signal(SIGHUP, SIG_IGN)`. `--escape`: one child forks a grandchild that calls `setsid()`; the child then exits, so the grandchild is re-parented to launchd and is in a new group.
- `--shared-mb`: workers attach one System V segment, so the [footprint, RSS] range shows shared pages.
- `devserver --port 3000` binds and listens, so the port finder has something to find.
- **Done when:** each profile behaves as described, as checked with `ps -o pid,ppid,pgid,rss,command`.

## Phase 3: Process snapshot and membership (Lab 3 concepts)

`src/common/proc.c`: `snapshot_take(snap*)` uses `proc_listpids(PROC_UID_ONLY, getuid())`. For each pid it reads `PROC_PIDTBSDINFO` (ppid, pgid, start time, status, tty) and `proc_pid_rusage(RUSAGE_INFO_V2)` (footprint, CPU time), plus `PROC_PIDTASKINFO` (resident size). Processes that disappear during the snapshot are skipped silently.

`src/daemon/membership.c`: a **pure function** with no system calls:

```
membership_update(workload W, snapshot S):
    alive  = { m in W.members : S has m.pid with the same start_time }      # pid-reuse guard
    seeds  = alive ∪ { p in S : p.pgid == W.pgid }
               ∪ { p in S : p is new AND env_tag(p) == W.id }                # tag lookup is cached per (pid,start)
    build children[ppid] from S once
    BFS from seeds through children[] → closure                              # tree closure, stops at a fixpoint
    W.members = closure  (each tagged with pid, start_time, pgid)
    W.escapees = { m in W.members : m.pgid != W.pgid }
    for each m whose ppid == 1 and was not 1 last time: log "re-parented to launchd, still a member"
```

Unit-test it with handmade snapshots: re-parenting, `setsid` escape, pid reuse (same pid, new start time → not a member), and a double-fork that clears its env tag between samples (the stated limitation; the test documents that it escapes).

## Phase 4: Launcher `warden run` (Labs 3, 5, 6)

```
warden run [--policy NAME] [--budget 2G] [--max-runtime 1h] [--on-detach stop|keep|stop-after=10m]
           [--pausable] [--wait=600|forever] -- CMD ARGS...
```

1. Send `OP_ADMIT_REQ` (NORMAL) with the requested budget, then wait for the reply on `mtype = getpid()`. Possible replies: ADMIT (with nice value and final budget), QUEUED (keep waiting, and print the position in the queue), REFUSED, or TIMEOUT → exit 75. If the user presses Ctrl+C while queued, send `OP_CANCEL` (URGENT) and exit 130. If no daemon replies within 2 s, print a warning and run **unmanaged** only with `--no-daemon`; otherwise exit 69.
2. `pipe(errpipe)` and set `FD_CLOEXEC` on the write end. `fork()`.
   - Child: `setpgid(0,0)`; `setenv("WARDEN_ID", id)`; `setpriority` if a nice value was given; `signal(SIGINT/SIGHUP/SIGTERM, SIG_DFL)`; `execvp(argv)`. If exec fails, `write(errpipe, &errno)` and `_exit(127)`.
   - Parent: also calls `setpgid(child, child)` (this closes the race); closes the write end; `read(errpipe)` → 0 bytes means exec succeeded (Lab 5 pipe + Lab 3 exec). Send `OP_REGISTER` with the root pid, its start time, pgid, the launcher's own pid and start time, the project directory (`getcwd`) and the policy.
3. Main loop: handlers for SIGINT, SIGTERM and SIGHUP only set flags. The loop forwards SIGINT and SIGTERM as `kill(-pgid, sig)`. On SIGHUP (terminal closed) it sends `OP_DETACHED`, applies `on_detach` (stop → SIGTERM to the group), then ignores further SIGHUP and SIGPIPE and stops writing to the tty. It sends `OP_HEARTBEAT` every 2 s and re-sends `OP_REGISTER` when a heartbeat fails and later works again (daemon restarted).
4. Reap: `waitpid(root, &st, 0)` in an EINTR loop. Decode with `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG`, send `OP_EXITED`, and exit with the same code (128 + signal if it was killed). **This is the only place in the system where the workload root is reaped.**

**Done when:** `warden run -- warden-sim agents -n 4` registers, Ctrl+C stops the whole group, `--escape` children appear as escapees in the daemon's log, and an exec failure is reported cleanly.

## Phase 5: Daemon core: threads, registry, monitor, status (Labs 4, 6)

`wardend` starts three threads with `pthread_create`, each with its own context struct (Lab 4), and joins them on SIGTERM/SIGINT (a flag set by the handler). On SIGHUP it reloads the config.

- **Command thread:** loops on `msgrcv(-2)`, validates the request, and updates the registry under the mutex. Enforcement requests go to the enforcer through a mutex + condition-variable queue. Replies go to `mtype = client_pid`.
- **Monitor thread:** every `sample_interval` (1 s): `snapshot_take` → `membership_update` for each workload → per-workload [footprint, RSS] sums and ring buffer → system memory sample → detector → decision → publish to shared memory. It measures its own loop time and logs a warning if a sample is late.
- **Enforcer thread:** waits on the condition variable and runs termination, pause, resume and renice jobs (Phase 7). The returned job summary is heap-allocated and freed by the joiner (Lab 4).
- **Registry:** `~/.procwarden/registry` is a text file, one workload per block. It is written to a temporary file and then `rename()`d after every change. On startup: load it, drop members whose (pid, start time) no longer match, re-run membership, and mark workloads whose launcher is gone as `OWNER_LOST`.
- **Owner check:** each sample checks the launcher's (pid, start time). If the launcher is gone → `OWNER_LOST` → apply `on_detach`.

**Done when:** `kill -9 <launcher>` gives `OWNER_LOST` within 1 s, and killing and restarting `wardend` restores every workload and its members.

## Phase 6: System memory, detector, decision engine (feedback 5 and 6)

`src/common/mem.c`, sampled every second: `T = hw.memsize`; `A = (free + inactive + purgeable + speculative) × pagesize` from `host_statistics64(HOST_VM_INFO64)`; `P = kern.memorystatus_vm_pressure_level`; `S` = change in `swapouts` per second; compressor page count; `vm.swapusage`; `statfs("/")` free bytes.

`detector.c` and `decision.c` are **pure functions** with unit tests:

```
growth(ring of last W=30 footprints):
    slope = least-squares slope in MB/s
    rise  = last - first
    pos   = fraction of positive deltas
    growing = slope >= 2 MB/s AND rise >= 64 MB AND pos >= 0.70

over_budget = footprint > B for 3 consecutive samples
B = min(user_limit, 25%·T)     (15%·T if admitted in YELLOW)

band_pct   = GREEN if A/T >= .40 ; YELLOW if .20 <= A/T < .40 ; ORANGE if .10 <= A/T < .20 ; RED if A/T < .10
band_kern  = RED if P == critical ; ORANGE if P == warning or free_disk < 5 GB ;
             YELLOW if S > 0 in >= 5 of the last 10 samples ; else GREEN
candidate  = worse(band_pct, band_kern)
hysteresis: move to a worse band after the candidate holds for 3 samples;
            move to a better band only after it holds for 10 samples AND A/T >= that band's lower bound + 0.05

actions (registered workloads only):
    any band : abandoned per on_detach / max_runtime expired      → TERMINATE (policy rule)
    ORANGE+  : pausable AND (growing OR over_budget) AND not paused → PAUSE      ("growth stopped")
    RED      : if enforce AND P >= warning:
                  victim = argmax(footprint - B) over workloads with footprint > B
                  if victim: TERMINATE victim; cooldown 5 samples
                  else: ALERT "pressure not caused by managed workloads" + top 5 users (including unmanaged)
    every action -> log line with rule id; if !enforce -> "WOULD ..." and do nothing automatic
```

**Admission queue** (`queue.c`): strict FIFO, capacity 16. The head is admitted when band ≤ YELLOW and `A − 0.10·T ≥ requested budget`. Requests behind it wait (no skipping, so no starvation). Each entry has a deadline; when it expires the launcher gets a TIMEOUT reply. `OP_CANCEL` removes the entry.

**Resume policy:** a workload paused by the daemon is resumed when the band has been ≤ YELLOW for 10 samples and its footprint ≤ `A − 0.10·T`. Only one workload is resumed per 5 s, oldest pause first. Manual pauses are never resumed automatically. A workload paused for more than 10 minutes raises an alert and is never killed for that reason.

Unit tests: boundary values exactly at 10%, 20% and 40%; a value that oscillates around 20% must not cause flapping; no victim → no kill; queue order and timeout.

## Phase 7: Enforcer (Lab 5)

```
terminate(W):
    for each member m: verify (pid, start_time) still matches   # Rule 2.2
    kill(-W.pgid, SIGTERM); for escapees e: kill(e.pid, SIGTERM) (or kill(-e.pgid) if the whole group is members)
    same targets: SIGCONT                                        # let stopped processes handle SIGTERM
    poll every 250 ms up to grace (5 s): stop when no member is left
    survivors (re-verified): SIGKILL
    report footprint before, A before/after (+5 s) → "reclaimed ≈ X MB (estimate)"
pause(W):  SIGSTOP the same targets; set paused_by = daemon|user; log "growth stopped"
resume(W): SIGCONT the same targets
renice(W, n): setpriority(PRIO_PGRP, pgid, n) + PRIO_PROCESS for each escapee
```

`ESRCH` = already gone (fine). `EPERM` = log and skip. Manual commands for **unmanaged** pids work only with a confirm flag and use the same pid + start-time check.

## Phase 8: Control panel `warden top` and `warden ctl` (Lab 6 reader)

- ncurses. Attach the status segment read-only (`shmat(..., SHM_RDONLY)`), redraw every 500 ms using the seqlock read, and detach on exit.
- Tabs: **Workloads** (id, project, state, members, [footprint, RSS], budget, slope, paused-by, last rule) and **Unmanaged** (info only; leftover orphans are highlighted but never acted on automatically). The header shows the T/A/P/S/band/observe-or-enforce gauge.
- Keys: `k` kill, `p` pause, `r` resume, `n` renice, `l` set limit, `a` adopt (unmanaged → workload, asks for confirmation), `f` free port, `e` expand members. Every action is sent as a message-queue request; kill and cancel go as URGENT.
- `warden ctl` offers the same operations from the shell for scripting tests: `warden ctl kill wl-3`, `warden ctl port 3000`, `warden ctl status --json`.

## Phase 9: Port finder

`ports.c`: for each user pid, `proc_pidinfo(PROC_PIDLISTFDS)` → for each socket fd, `proc_pidfdinfo(PROC_PIDFDSOCKETINFO)` → match a TCP listener on the given local port. Map it to a workload. Freeing it: if the owner is registered → terminate that workload (or only that member if `--member`); if it is unmanaged → show it and require confirmation.

## Phase 10: Evaluation and demo scripts

- `tests/scenarios/*.sh` for each demo step in the proposal (terminal close, launcher SIGKILL, daemon restart, escape, ORANGE queue + pause + resume, RED with a victim, RED without a victim, port free).
- `wardend --record eval.csv` logs every sample: time, T, A, A/T, P, S, compressor, swap, band_pct, band_kern, band, and every workload's footprint/RSS.
- `tools/eval.c` (C, no Python) reads the CSV and reports, for (a) the percentage rule alone and (b) the combined rule: false triggers, missed triggers (ground truth = P ≥ warning or S > 0 sustained), overshoot (peak footprint − B) for leak rates of 10/50/100 MB/s, and the share of `--escape` processes caught. Write the summary to `docs/evaluation.md`.

---

## How to work with me

- Before each phase, give a short plan: the files you will touch, which lab mechanism carries the logic, and the tests. Then implement.
- Show complete files for new code and minimal diffs for changes. Use no placeholder `TODO` logic in delivered code.
- If an API is different on my macOS version, stop and tell me, propose the smallest fix, and add it to `docs/DEVIATIONS.md`.
- Explain each OS concept in two or three sentences when you first use it, so I can defend it in the demo.

Start with **Phase 0**.

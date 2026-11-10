# ProcWarden: Project Instructions

These rules apply to every session on this repository. Read them before writing code. If a request conflicts with them, say so instead of silently breaking a rule.

## What we are building

ProcWarden is an ICS433 (Operating Systems, KFUPM) lab project. It is a supervisor for **registered** developer workloads on macOS, built from four C programs:

| Binary | Role |
|---|---|
| `warden` (`warden run …`, `warden top`, `warden ctl …`) | Launcher that owns and reaps a workload; ncurses control panel; one-shot commands |
| `wardend` | Daemon: tracks membership, measures memory, decides, enforces |
| `warden-sim` | Workload simulator (install / devserver / watcher / agents profiles) |
| `tests/*` | Small C test programs and shell scripts |

The approved design is in `docs/ProcWarden_OS_Lab_Project_Proposal_v2.docx`. The proposal is the specification. Do not add features it doesn't list without asking.

Team: Ali Lutfallah, Mohammed Alnasr, Qaseem Al Munayan, Mohammed Alrikhrikh.

## Rule 1: Build the logic on the lab mechanisms

The grade depends on clearly showing the OS concepts from the course labs. Whenever a lab mechanism can do the job, use it, and make it the core of the algorithm, not a side feature.

| Lab | Use these (as taught) | Where |
|---|---|---|
| Lab 3: Processes | `fork`, `execvp`, `waitpid`, `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG`, `getpid`/`getppid`, `exit` codes, orphans, zombies | launcher, simulator, detector |
| Lab 4: Threads | `pthread_create`, `pthread_join`, `pthread_self`, a context struct passed as `void*`, heap-allocated return values | daemon threads, simulator leak threads |
| Lab 5: Signals & pipes | `signal()`, handlers, `SIG_IGN`/`SIG_DFL`, `kill`, `pause`, `SIGUSR1`, `pipe` + `fork`, close unused pipe ends | enforcer, launcher forwarding, exec-error pipe, simulator control |
| Lab 6: Shared memory & message queues | `shmget`/`shmat`/`shmdt`/`shmctl`, `msgget`/`msgsnd`/`msgrcv`/`msgctl`, `mtype` routing, negative `msgtyp` | command channel, live status table |

Some things are **allowed beyond the labs**, but only where no lab mechanism can do the job: `setpgid`/`getpgid`/`kill(-pgid)`, `setpriority`, `libproc` (`proc_listpids`, `proc_pidinfo`, `proc_pid_rusage`, `proc_pidfdinfo`), `sysctl`, Mach `host_statistics64`, `statfs`, `pthread_mutex_*`/`pthread_cond_*` (covered in the synchronization lab), C11 `<stdatomic.h>` for the shared-memory sequence counter, ncurses.

**Not allowed** without asking first: Unix domain sockets, POSIX `mq_*`, `kqueue`/`dispatch`, Objective-C/Swift, any non-C language, third-party libraries other than ncurses.

Above any function that uses a lab mechanism, put a comment that names the lab, for example `/* Lab 6: negative msgtyp gives urgent requests priority */`. Students must be able to defend every line in the demo.

## Rule 2: Safety invariants (never break these)

1. **Automatic actions apply only to registered workloads.** An unmanaged process (no workload ID), including any orphan re-parented to launchd, is never signalled automatically. Manual actions on unmanaged processes need an explicit `--confirm` or a confirmation key press in the panel.
2. **Identity = (pid, start time).** Before *every* `kill()`, check again that the pid's start time (`proc_bsdinfo.pbi_start_tvsec`/`pbi_start_tvusec`) matches the stored value. If it doesn't, the pid has been reused: skip it and log it.
3. **Only the launcher reaps the workload root.** The daemon never calls `waitpid()` on workload processes. Orphaned descendants are reaped by launchd.
4. **Pausing never counts as reclaiming memory.** Logs and UI say "growth stopped". Only a termination counts towards "memory reclaimed".
5. **Observe mode is the default.** With `enforce=false` the daemon logs `WOULD <action> <workload> rule=<rule>` and sends no automatic termination signals. Manual commands still work.
6. **Two-signal rule for automatic termination:** RED band (after hysteresis) **and** kernel pressure ≥ warning **and** a registered workload over budget. Kill at most one victim per decision, then wait 5 samples.
7. **Never act on processes owned by another uid, PID 1, or the warden binaries themselves.**
8. **Termination sequence:** `SIGTERM` → `SIGCONT` (in case the process is stopped) → grace period (default 5 s, poll every 250 ms) → `SIGKILL` survivors. Signal the group with `kill(-pgid)`. Signal members that left the group one by one.

## Rule 3: Signal-handler discipline

- Handlers only set `volatile sig_atomic_t` flags (or `write()` one byte to a self-pipe). No `printf`, `malloc` or locks inside a handler.
- Install handlers with `signal()` as in Lab 5. On macOS, `signal()` has BSD semantics: the handler stays installed and interrupted syscalls are restarted. Use `sigaction()` without `SA_RESTART` only where you need a blocking call (`msgrcv`, `sleep`) to return with `EINTR`, and add a comment explaining why.
- Every blocking call (`msgrcv`, `msgsnd`, `waitpid`, `read`) handles `EINTR` in a loop.
- `SIGPIPE` is ignored (`SIG_IGN`) in the launcher and daemon.

## Rule 4: macOS facts to respect

- System V IPC limits are small by default. `kern.sysv.shmmax` is usually 4 MB, and a message queue holds only a few KB (check `msg_qbytes` with `msgctl(IPC_STAT)`). Keep every message ≤ 256 bytes. Log the limits at daemon startup and fail with a clear message if they are too small.
- IPC keys come from `ftok("~/.procwarden/key", 'Q')` (message queue) and `ftok(..., 'S')` (shared memory), permissions `0600`. The daemon creates both objects. On a clean shutdown it removes the queue (`msgctl IPC_RMID`) and marks the segment for removal (`shmctl IPC_RMID`). A stale object found at startup is reused after checking its version header.
- The memory metric is `rusage_info_v2.ri_phys_footprint` (from `proc_pid_rusage`). The RSS upper bound is `proc_taskinfo.pti_resident_size`. Always show both.
- The memory pressure level comes from `sysctl kern.memorystatus_vm_pressure_level` (1 = normal, 2 = warning, 4 = critical).
- The environment tag is read with `sysctl {CTL_KERN, KERN_PROCARGS2, pid}`. It works only for same-uid processes, and fails for some hardened processes, so handle failures quietly.
- SIP: never try to work around permission errors. `EPERM` → log it and skip.

## Code standards

- C11, `clang -Wall -Wextra -Werror -std=c11 -pthread`. `make`, `make debug` (ASan + UBSan), `make tsan`, `make test`.
- Check every system call's return value. Errors go through `log_err("context: %s", strerror(errno))`.
- No global mutable state except the registry, which is guarded by one mutex. Never hold a lock while making a blocking system call or sending a signal: copy what you need, unlock, then act.
- Fixed-size, `static_assert`-checked structs for anything that goes into IPC.
- Small, focused files (about 400 lines at most). One header per module in `include/`.
- Every tunable value (thresholds, windows, grace period, queue size) goes in `config.c` with a default plus a key in `~/.procwarden/config`. No magic numbers in the logic.
- Logs: one line per event, `ISO-time level module workload rule message`, written to `~/.procwarden/warden.log`.

## Repository layout

```
include/      shared headers (ipc.h, registry.h, proc.h, mem.h, policy.h, log.h, config.h)
src/common/   ipc.c (msg + shm), proc.c (libproc snapshot), mem.c (system memory), log.c, config.c
src/daemon/   main.c, monitor.c, membership.c, detector.c, decision.c, queue.c, enforcer.c, registry.c, status.c, ports.c
src/cli/      main.c, run.c (launcher), top.c (ncurses), ctl.c
src/sim/      warden-sim.c
tests/        unit tests (pure C) + scenario scripts (tests/scenarios/*.sh)
docs/         proposal, design notes, evaluation results (CSV + summary)
```

## Working style

- Work in the phase order given in `docs/BUILD_PROMPT.md`. Finish and test one phase before starting the next. Commit at the end of each phase.
- Make pure logic (membership closure, regression slope, band hysteresis, queue) **testable without macOS calls**: functions take snapshots or arrays and return decisions. Unit-test them in `tests/`.
- When something in the proposal turns out to be wrong on real macOS (an API behaves differently, a limit is lower), stop, explain it, propose the smallest change, and record it in `docs/DEVIATIONS.md`. Do not quietly redesign.
- Keep explanations short and use OS course vocabulary. The team has to present this.
- Graphify is used to map the codebase for the team. Keep function names descriptive and avoid giant files so the graph stays readable. It is not part of the shipped system.

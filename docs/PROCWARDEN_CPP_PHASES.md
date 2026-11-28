# ProcWarden: C++ Build Phases (agent brief)

> Feed this whole file to your coding agent at the start of the build. It is self-contained. Work through the phases **in order**. At the end of each phase, build, run that phase's tests, show the output, commit, and **wait for approval** before starting the next phase.

---

## 0. What you are building

**ProcWarden** is a macOS tool that watches the AI coding sessions on a developer's Mac (OpenCode agents, `npm`/`pnpm`/`bun` dev servers, app simulations, Biome). It shows **physical RAM, swap, process count and cached files** for each session, compares them with a **learned normal range**, and clears leftovers and caches **only after the user gives permission**. It lives inside the IDE (a right-side panel) and keeps its settings in a `.procwarden/` folder in each project. That folder can stay **local** (git-ignored) or be **shared** (committed), the same way Graphify keeps `graphify-out/`.

It is also a university **Operating Systems lab project** (KFUPM ICS433). The core logic **must** be built on the OS mechanisms taught in Labs 3–6. The C++ core is what gets graded. The IDE extension (TypeScript) is a thin UI that talks to the C++ core through a documented JSON stream, and it is **out of scope here**, apart from the contract in Phase 12.

### Binaries you will produce

| Binary | Role |
|---|---|
| `procwardend` | Background daemon: sampling, membership, baselines, decisions, enforcement, cleanup execution |
| `procwarden` | CLI: `install`, `init`, `run`, `watch --json`, `status`, `ctl …`, `cleanup plan/apply`, `top` |
| `pw-sim` | Workload simulator (app simulations, agent-like workers, leaks, escapes) |
| `pw-tests` | Unit and scenario tests (doctest) |

### Target numbers (the demo must reproduce them)

These numbers come from a scripted `pw-sim` scenario, not a real Mac. On a 16 GB Mac, four OpenCode phases (Plan, Build, Test, Review) plus six app simulations take RAM from about 4 GB to **14.2 GB** and swap to **3.8 GB**, with about **412** processes. An approved cleanup stops the six simulations (4.6 GB), the idle Test workers (2.1 GB) and two leftover dev servers (2.1 GB), and clears 7.1 GB of package caches. After that, RAM is about **5.4 GB**, swap **128 MB**, and there are about **196** processes. The normal range shown is **5–9 GB**.

---

## 1. Non-negotiable rules

### 1.1 Lab mechanisms carry the logic

| Lab | Mechanisms (use them as taught) | Where they are the core |
|---|---|---|
| Lab 3: Processes | `fork`, `execvp`, `waitpid`, `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG`, `getpid`/`getppid`, orphans, zombies | Launcher, simulator, leftover and zombie detection |
| Lab 4: Threads | `pthread_create`/`pthread_join`, a context struct passed as `void*`, heap-allocated results | Daemon threads, simulator leak threads |
| Lab 5: Signals & pipes | `signal()`, handlers, `SIG_IGN`/`SIG_DFL`, `kill`, `pause`, `SIGUSR1`, `pipe` + `fork`, closing unused ends | Enforcer, launcher signal forwarding, exec-error pipe, the JSON stream to the IDE, simulator control |
| Lab 6: Shared memory & message queues | `shmget`/`shmat`/`shmdt`/`shmctl`, `msgget`/`msgsnd`/`msgrcv`/`msgctl`, `mtype` routing, negative `msgtyp` | Command channel (with priority), live status table |

- **C++ wraps, never hides.** Write thin RAII classes around the raw system calls (`PosixThread`, `Pipe`, `MsgQueue`, `ShmSegment`, `SignalFlag`). Inside each wrapper, call the lab system call directly, and put a comment above it naming the lab, for example `// Lab 6: negative msgtyp → URGENT served first`.
- **Do not use** `std::thread`, `std::async`, `std::jthread`, Boost.Process or `posix_spawn` for the core. Use pthreads and fork/exec. `std::mutex` is not allowed either: wrap `pthread_mutex_t` and `pthread_cond_t` (from the synchronization lab).
- **Allowed beyond the labs, only where no lab mechanism fits:** `setpgid`/`getpgid`/`kill(-pgid)`, `setpriority`, `libproc` (`proc_listpids`, `proc_pidinfo`, `proc_pid_rusage`, `proc_pidfdinfo`), `sysctl`, `host_statistics64`, `statfs`, `fts`/`stat` for directory sizes, `<stdatomic.h>`/`std::atomic` **only** for the shared-memory seqlock counter, and ncurses.
- **Not allowed without asking:** Unix domain sockets, POSIX `mq_*`, kqueue/GCD, Objective-C/Swift, and third-party libraries other than the two vendored single headers: `nlohmann/json.hpp` and `doctest.h`.

### 1.2 Safety invariants (never break these)

1. **Only registered work is acted on automatically.** A process with no ProcWarden registration (for example a launchd agent, or any orphan under PID 1) is **never** signalled automatically. It is listed as *unmanaged*, and manual action on it needs explicit confirmation.
2. **Identity = (pid, start time).** Before *every* `kill()`, re-read `pbi_start_tvsec`/`pbi_start_tvusec`. If it doesn't match the stored value, the pid has been reused: skip it and log it.
3. **Only the launcher reaps the workload root** with `waitpid()`. The daemon never calls `waitpid()` on workload processes. launchd reaps orphaned descendants.
4. **Pausing is not reclaiming.** `SIGSTOP` is reported as "growth stopped". Only termination and cache deletion count as "freed".
5. **Permission before cleanup.** Nothing is stopped or deleted except through an **approved cleanup plan** (Phase 11). The plan names every item, and approval is a token tied to the plan's hash.
6. **Observe mode is the default** for automatic memory enforcement (`enforce=false`). It only logs `WOULD …`. Automatic termination needs the **two-signal rule**: RED band **and** kernel pressure ≥ warning **and** a registered workload over its budget. At most one victim per decision, then wait 5 samples.
7. **Never act on another uid, PID 1, or ProcWarden's own processes.**
8. **Termination order:** `SIGTERM` → `SIGCONT` → grace period (default 5 s, poll every 250 ms) → `SIGKILL` survivors (re-verified). Signal the group with `kill(-pgid)` and signal escapees individually.
9. **Caches are cleared through each tool's own command** when one exists (`npm cache clean --force`, `pnpm store prune`, `bun pm cache rm`). Raw deletion is allowed only for paths listed in `cleanup-rules.json` that are inside the user's home directory, after `realpath` and with no symlink crossing.

### 1.3 C++ standards

- C++20, Apple clang, CMake ≥ 3.24. Flags: `-Wall -Wextra -Wpedantic -Werror`. Presets: `debug` (ASan + UBSan), `tsan`, `release`.
- No exceptions across thread or IPC boundaries. Use `Result<T>` (value or `{errno, context}`) for system-call paths.
- Every IPC struct is trivially copyable, fixed-size, and checked with `static_assert`.
- Pure logic (membership, baselines, bands, growth, queue, cleanup planning) has **no system calls**. It takes snapshots in and returns decisions, so it can be unit-tested.
- Every tunable value lives in `Config` with a default and a key in `.procwarden/settings.json` or `~/.procwarden/config.json`. No magic numbers in the logic.
- Logs are one line per event: `ISO-time level module session rule message` → `~/.procwarden/warden.log`.

---

## 2. Repository layout

```
CMakeLists.txt  CMakePresets.json
third_party/    json.hpp  doctest.h
include/pw/     result.hpp config.hpp log.hpp
                os/ (thread.hpp sync.hpp pipe.hpp signals.hpp msgq.hpp shm.hpp proc.hpp mem.hpp fsize.hpp)
                core/ (snapshot.hpp membership.hpp session.hpp baseline.hpp decision.hpp cleanup.hpp registry.hpp)
                ipc/ (protocol.hpp status_table.hpp stream.hpp)
src/os/         wrappers (the only place raw lab syscalls appear)
src/core/       pure logic
src/daemon/     main.cpp monitor.cpp command_server.cpp enforcer.cpp cleaner.cpp publisher.cpp
src/cli/        main.cpp install.cpp init.cpp run.cpp watch.cpp ctl.cpp cleanup.cpp top.cpp
src/sim/        pw_sim.cpp
tests/unit/     *.cpp      tests/scenarios/*.sh
docs/           PROTOCOL.md  DEVIATIONS.md  EVALUATION.md
```

---

## 3. Data contracts (define them in Phase 0, then keep them stable)

### 3.1 Project folder `.procwarden/`

| File | Content |
|---|---|
| `settings.json` | `{ "version":1, "storage":"local"|"shared", "watch":{"opencode":true,"sims":true,"dev_servers":true,"package_caches":false}, "normal_range":{"mode":"auto"|"manual","low_gb":5,"high_gb":9}, "alerts":"quiet"|"balanced"|"proactive", "cleanup":{"always_ask":true,"include":["sims","idle_agents","leftover_servers","npm","pnpm","bun","biome"]}, "enforce":false }` (`always_ask` cannot be set to false) |
| `sessions.json` | The sessions the user chose to watch: `[{ "id":"s-…", "tool":"opencode", "project":"my-saas-app", "match":{...} }]` |
| `cleanup-rules.json` | Phase rules (command pattern → Plan/Build/Test/Review), cache locations and the command to clear each, and items that are never touched |
| `history/YYYY-MM-DD.jsonl` | One line per 10 s: totals and per-session footprint, used for the baselines |

- `storage:"local"` → the CLI makes sure `.procwarden/` is listed in the project `.gitignore`.
- `storage:"shared"` → the CLI removes that line, and keeps `history/` ignored (`.procwarden/history/`), so the team shares rules, not machine data.
- If `graphify-out/` exists, read `GRAPH_REPORT.md` **read-only** to show the project name and module count. Never write to it.

### 3.2 Message queue protocol (Lab 6)

There is one System V queue: key `ftok("~/.procwarden/key",'Q')`, permissions `0600`. Each message is at most **256 bytes**, because the default macOS queues are small. Log `msg_qbytes` at startup.

```cpp
struct PwMsg {                 // static_assert(sizeof(PwMsg) - sizeof(long) <= 248)
  long    mtype;               // 1 URGENT→daemon, 2 NORMAL→daemon, >2 reply to client pid
  int32_t op;                  // REGISTER, HEARTBEAT, EXITED, DETACHED, ADMIT_REQ, CANCEL,
                               // KILL, PAUSE, RESUME, RENICE, SET_BUDGET, ADOPT,
                               // PLAN_CLEANUP, APPROVE_CLEANUP, PORT_LOOKUP, SETTINGS_RELOAD, REPLY
  int32_t client_pid, sess_id, a, b;
  int64_t c;                   // bytes, start-time usec, or plan hash
  char    text[168];           // project path, label, phase, error text
};
```
The daemon reads with `msgrcv(q, &m, SZ, -2, 0)`. Lab 6: a negative `msgtyp` returns the lowest type first, so KILL, CANCEL and APPROVE are served before NORMAL requests. Replies use `mtype = client_pid`.

### 3.3 Shared-memory status table (Lab 6)

Key `ftok(...,'S')`, < 1 MB (default `kern.sysv.shmmax` is 4 MB). There is one writer (the daemon) and many readers.
```
header{magic,version,seq(atomic),sample_no,t_usec}
sys{total,used,phys_free,compressed,swap_used,swap_total,pressure_level,processes,band,enforce}
normal{low_gb,high_gb,mode}
sessions[64]{id,tool,project,phase_mask,state,members,footprint,rss_upper,usual_gb,ratio,owner_alive}
agents[16]{sess_id,phase(Plan|Build|Test|Review),footprint,idle_bytes,status}
procs[512]{pid,ppid,pgid,sess_id(0=unmanaged),phase,footprint,rss,state,escaped}
ring[120]{t,ram,swap}   // the "last 60 s" chart at 0.5 s resolution
cleanup{plan_hash,state,items[16]{kind,label,detail,bytes,mem_or_disk,status}}
```
Writer: `seq++` (odd) → write → `seq++` (even). Reader: retry while `seq` is odd or has changed.

### 3.4 JSON stream for the IDE (Lab 5 pipe)

`procwarden watch --json` attaches the shared memory **read-only** and writes **one JSON object per line** to stdout every 500 ms. The IDE extension spawns this command and reads its stdout through a pipe. The schema (`docs/PROTOCOL.md`) mirrors §3.3 with readable names:
```json
{"t":1732712345.5,"ram":{"used_gb":14.2,"total_gb":16},"swap":{"used_mb":3891},"processes":412,
 "pressure":"high","band":"RED","normal":{"low_gb":5,"high_gb":9},
 "agents":[{"phase":"Test","gb":1.4,"idle_gb":2.1,"status":"done"}],
 "sessions":[{"tool":"opencode","project":"my-saas-app","gb":7.7,"usual_gb":4.5,"ratio":1.7}],
 "chart":[[t,ram_gb,swap_gb],...],
 "cleanup":{"ram_gb":8.8,"disk_gb":7.1,"items":[{"label":"6 app simulations","bytes":4939212390,"kind":"ram","status":"ready"}]}}
```
The extension sends user actions back with `procwarden ctl …` and `procwarden cleanup approve <hash>`.

---

## Phase 0: Toolchain, skeleton, contracts

- CMake project, presets, warning flags, vendored headers, `Result<T>`, `Log`, `Config` (loads defaults, then `~/.procwarden/config.json`, then the project `settings.json`).
- Write `docs/PROTOCOL.md` from §3 and the corresponding headers in `include/pw/ipc/`. They only need to compile at this stage.
- **Tests:** config precedence; JSON round trip of `settings.json`; `static_assert` sizes.
- **Done when:** all presets build and `pw-tests` passes.

## Phase 1: OS wrappers + lab check (Labs 3–6)

- `PosixThread` (RAII `pthread_create`/`pthread_join`; the context struct goes through `void*`; the result is returned as a heap pointer and owned by the joiner), `Mutex`/`CondVar`/`LockGuard` over pthreads, `Pipe` (closes unused ends, can set `FD_CLOEXEC`), `SignalFlag` (installs with `signal()`; the handler only sets a `volatile sig_atomic_t`), `MsgQueue`, `ShmSegment` (`attach(readonly)`, detach in the destructor, `remove()`), and `ForkExec` helpers.
- Every blocking call loops on `EINTR`. `SIGPIPE` is set to `SIG_IGN` in the daemon and CLI.
- `tests/unit/lab_check.cpp`: one test per lab mechanism, including msgsnd/msgrcv with `mtype = pid` and with negative `msgtyp` (URGENT served first). It also prints the macOS IPC limits.
- **Done when:** the lab-check tests pass under ASan and TSan.

## Phase 2: Samplers: processes and system memory

- `proc::snapshot()`: `proc_listpids(PROC_UID_ONLY, getuid())`, and for each pid `PROC_PIDTBSDINFO` (ppid, pgid, start time, status, tty, comm), `proc_pid_rusage(RUSAGE_INFO_V2)` (`ri_phys_footprint`, CPU time) and `PROC_PIDTASKINFO` (`pti_resident_size`). Processes that disappear during the snapshot are skipped silently.
- `proc::cmdline_env(pid)`: `sysctl {CTL_KERN, KERN_PROCARGS2, pid}` → argv plus the `PW_SESSION=`/`PW_PHASE=` env tags. Cache the result per (pid, start time).
- `mem::sample()`: total RAM (`hw.memsize`), used memory, compressed memory and page counts (`host_statistics64(HOST_VM_INFO64)`), swap (`vm.swapusage`), pressure level (`kern.memorystatus_vm_pressure_level`: 1 = normal, 2 = warning, 4 = critical), swap-outs per second, and free disk (`statfs`).
- **Used RAM definition (put it in the docs):** `used = (active + wired + compressor) × pagesize`. This roughly matches the "Memory Used" figure people see in Activity Monitor. Label it as an estimate.
- **Tests:** parse fixtures; a smoke test on the real Mac prints one sample.
- **Done when:** `procwarden status` prints RAM, swap, process count and pressure that look reasonable.

## Phase 3: Membership engine (pure)

```
update(Session S, Snapshot X):
  alive   = {m ∈ S.members : X has m.pid with the same start_time}        // pid-reuse guard
  seeds   = alive ∪ {p : p.pgid == S.pgid} ∪ {p : tag(p).session == S.id}
  closure = BFS over the children map (ppid → pids) from seeds, to a fixpoint
  S.members  = closure (each with pid, start_time, pgid, phase)
  S.escapees = {m : m.pgid ≠ S.pgid}
  re-parented to launchd (ppid became 1) → stays a member; log once
```
- **Phase attribution:** use the `PW_PHASE` env tag if present. Otherwise apply the first matching rule in `cleanup-rules.json` (for example `opencode.*plan` → Plan; `tsc|vite build|next build` → Build; `bun test|vitest|jest|pw-sim|bun run sim` → Test; `review|diff` → Review). Otherwise inherit the parent's phase.
- **Idle workers:** members whose CPU time has not changed for ≥ 30 s while their phase lane is marked *done*.
- **Stated limitation (tested and documented):** a process that double-forks, calls `setsid()` **and** clears its environment within one sample interval can escape tracking.
- **Tests:** re-parenting, `setsid` escape, pid reuse, phase inheritance, idle detection.

## Phase 4: `pw-sim`, the workload simulator (Labs 3, 4, 5, 6)

- Profiles: `app` (one app simulation run), `agent --phase P` (agent-like worker tree), `devserver --port N`, `watcher`, plus the `demo` scenario that reproduces the target numbers in §0.
- Workers are created with `fork()`. Each reports `{pid, bytes}` to the parent once a second through a **pipe**. Leaking starts when the parent sends **`SIGUSR1`**; until then the worker waits in `pause()`. Memory is touched page by page so it really becomes dirty.
- `agent` workers start T leak threads with `pthread_create` and a context struct. On SIGTERM they join and return the totals as heap results.
- Options: `--mb`, `--leak MB/s`, `--threads`, `--ignore-hup` (`signal(SIGHUP, SIG_IGN)`), `--escape` (double-fork + `setsid`), `--shared-mb` (System V segment shared by the workers), `--linger` (stay resident after "finishing", to imitate finished simulations that still hold memory).
- **Done when:** `pw-sim demo` takes a 16 GB Mac from about 4 GB to about 14 GB in a controlled way, with `--scale` for smaller machines.

## Phase 5: Launcher `procwarden run` (Labs 3, 5, 6)

```
procwarden run [--session NAME] [--phase Plan|Build|Test|Review] [--budget 2G]
               [--on-detach stop|keep|stop-after=10m] [--pausable] [--wait=600|forever] -- CMD…
```
1. Send `ADMIT_REQ` (NORMAL) and wait for the reply on `mtype = getpid()`: ADMIT, QUEUED (show the position), REFUSED or TIMEOUT (exit 75). If the user presses Ctrl+C while queued, send `CANCEL` (URGENT) and exit 130. If the daemon is gone, refuse unless `--no-daemon` is given.
2. `Pipe errp` (write end close-on-exec), then `fork()`. **Child:** `setpgid(0,0)`, `setenv PW_SESSION/PW_PHASE`, reset the signal handlers to `SIG_DFL`, `execvp`. If exec fails, write `errno` to the pipe and `_exit(127)`. **Parent:** `setpgid(child,child)` (closes the race), close the write end, read: 0 bytes means exec succeeded. Then `REGISTER` (root pid + start time, pgid, own pid + start time, project path, session, phase, policy).
3. Loop: the signal handlers only set flags. The loop forwards SIGINT and SIGTERM with `kill(-pgid)`. On SIGHUP: send `DETACHED`, apply `on_detach`, then keep running to reap. `HEARTBEAT` every 2 s; re-`REGISTER` after a daemon restart.
4. `waitpid(root)` in an `EINTR` loop. Decode the status, send `EXITED`, and exit with the same code (128 + signal if it was killed). **This is the only place the root is reaped.**
- **Done when:** Ctrl+C stops the whole group, an exec failure is reported cleanly, and `--escape` children are tracked.

## Phase 6: Daemon core (Labs 4, 6)

- `procwardend` starts three `PosixThread`s, each with its own context struct:
  - **monitor:** every 1 s: snapshot → membership → per-session sums → system sample → ring buffer → baselines → decision → publish to shared memory.
  - **command server:** loops on `msgrcv(-2)`, validates the request, updates the registry under the mutex, and hands jobs to the enforcer or cleaner through a `CondVar` queue.
  - **enforcer/cleaner:** runs jobs.
- On SIGTERM/SIGINT (flags): stop the loops, join the threads, save the registry, `IPC_RMID` the queue and mark the shared memory for removal. On SIGHUP: reload the settings.
- **Registry:** `~/.procwarden/registry.json`, written to a temporary file and then `rename()`d after every change. On startup: reload it, drop members that fail the pid + start-time check, rebuild membership, and mark sessions whose launcher is gone as `OWNER_LOST`.
- **Session discovery (watch list):** sessions not started with `run` (for example an `opencode` already running) are found from `sessions.json` match rules (tool + project working directory, using `PROC_PIDVNODEPATHINFO`'s cwd). They are **registered with consent**: the IDE's "Start watching" calls `ADOPT`. Never adopt silently.
- **Done when:** `kill -9 <launcher>` shows `OWNER_LOST` within 1 s, and a daemon restart restores all sessions.

## Phase 7: Project folder and CLI `init`

- `procwarden init [--storage local|shared]` creates `.procwarden/{settings.json,sessions.json,cleanup-rules.json,history/}` with defaults and fixes `.gitignore` as described in §3.1.
- When a setting changes (from `settings.json` on disk or from a `SETTINGS_RELOAD` message), re-validate it. `always_ask=false` is rejected with a clear message.
- The history writer appends a JSONL line every 10 s and deletes files older than 30 days.
- **Tests:** `.gitignore` editing is idempotent; switching storage from local to shared and back; schema validation.

## Phase 8: Baselines: the "normal range" and "usual for this tool"

- **Normal range (system level):** from `history/`, take the samples from the last 14 days, from moments when watched sessions were active, and compute the 10th and 90th percentiles of used RAM. Round to 0.5 GB (for example 5–9 GB). Before 24 h of data, use `manual` defaults and show "Learning your normal range…".
- **Usual for this tool (per session type):** the median footprint per `(tool, phase)` from history, with the 90th percentile as the upper limit. `ratio = current / usual`. The UI says "3.4× more than usual" when the ratio is ≥ 2.0 for 30 s.
- **Pure functions + tests:** percentile maths, minimum data required, falling back to defaults, rounding.

## Phase 9: Decision engine (pure; observe mode by default)

- **Bands use half-open intervals of available memory** (available = total − used): GREEN ≥ 40%; YELLOW 20–40%; ORANGE 10–20%; RED < 10%.
- The kernel signals can raise the band: pressure **warning → at least ORANGE**, pressure **critical → RED**; swap-outs in ≥ 5 of the last 10 samples → at least YELLOW.
- **Hysteresis:** move to a worse band after 3 consecutive samples; move to a better band after 10 samples **plus a 5-point margin**.
- **Growth:** least-squares slope over the last 30 samples ≥ 2 MB/s **and** a rise ≥ 64 MB **and** ≥ 70% of the changes positive.
- **Budget:** min(user, 25% of RAM). Over budget = 3 consecutive samples above it.
- **Actions:**
  - Any band: an abandoned session (by its `on_detach` policy) → *suggest* stopping it.
  - ORANGE or worse: pause sessions that are pausable **and** growing (only when `enforce` is on).
  - RED with `enforce` **and** pressure ≥ warning: TERMINATE the largest overage, one per 5 samples.
  - No session over budget → no automatic kill; raise an alert listing the top users (unmanaged ones for information only).
- The **cleanup suggestion** is computed every sample (Phase 11 planner) and published. It is never executed without approval.
- **Tests:** exact boundary values; values oscillating around 20% must not flap; the no-victim case; observe mode emits `WOULD` only.

## Phase 10: Enforcer (Lab 5)

```
terminate(targets):  verify (pid,start) → kill(-pgid,SIGTERM) + kill(escapee,SIGTERM)
                     → SIGCONT same → poll 250 ms up to grace → re-verify → SIGKILL survivors
pause(S):  SIGSTOP (log "growth stopped")    resume(S): SIGCONT
renice(S,n): setpriority(PRIO_PGRP, pgid, n) + PRIO_PROCESS for escapees
```
`ESRCH` means the process is already gone, which is fine. `EPERM` → log it and skip. Report the footprint before termination and the change in used RAM 5 s later as "≈ freed (estimate)".

## Phase 11: Cleanup planner + executor (permission flow)

**Planner (pure):** builds an ordered `CleanupPlan` of exact items, each with a label, detail, bytes, `kind` (ram/disk) and a reason:

| Item | Rule |
|---|---|
| App simulations | Registered Test-phase members matching sim patterns whose run has finished (exit marker or idle ≥ 30 s) but which are still resident |
| Idle agent workers | Members marked idle in a phase lane that is *done*. **Active phase leaders are never included** |
| Leftover dev servers | Registered sessions in `OWNER_LOST` or detached past their policy, still listening on a port (`proc_pidfdinfo` socket scan) |
| Package caches | Sizes from an `fts` walk: npm (`~/.npm/_cacache`), pnpm (path from `pnpm store path`, cleared with `pnpm store prune`), bun (`~/.bun/install/cache` or `bun pm cache`), Biome (stop stray `biome` daemon/LSP processes; delete only cache/log paths listed in `cleanup-rules.json`). **Verify each path on the target Mac and record deviations** |
| Never | Unmanaged processes, anything the user is currently using (a port in use by an active session, the frontmost editor's processes), and paths outside `$HOME` |

- `plan_hash = sha256(sorted item ids + sizes)`. The plan is published to shared memory and the JSON stream.
- **Approval:** `procwarden cleanup approve <hash>` sends `APPROVE_CLEANUP` (URGENT). The daemon re-plans, and if the hash has changed (the state moved on) it rejects the approval and asks again.
- **Executor (cleaner thread):** runs the items in order: processes (Phase 10 terminate), then caches (each tool's own command via fork/exec/waitpid with captured output through a **pipe**; raw deletion only as described in §1.2 rule 9). Update each item's status (`ready → clearing → freed|failed`) live. Report the totals: RAM freed (the measured change in used RAM) and disk freed (the measured change in directory size).
- **Done when:** in the demo scenario the plan shows 4.6 + 2.1 + 2.1 GB RAM and 7.1 GB disk, approval clears exactly those items, the active agents keep running, and the end state is about 5.4 GB RAM and 128 MB swap. Swap shrinking takes time: report it as it really happens, and do not fake the number.

## Phase 12: Publishing, CLI and install

- **Publisher:** the shared-memory seqlock writer for the table in §3.3, plus the 120-point chart ring.
- `procwarden watch --json`: the JSON-lines stream for the IDE (§3.4). It flushes per line, exits when stdout closes (EPIPE), and has no other side effects.
- `procwarden top`: an ncurses fallback UI with the same data. Its keys send `ctl` messages.
- `procwarden ctl kill|pause|resume|renice|adopt|budget <session|pid> [--confirm]`.
- `procwarden install`:
  1. Detect the `code` or `cursor` CLI and run `<cli> --install-extension procwarden.vsix`, if the file is present.
  2. Write a launchd **user agent** plist to `~/Library/LaunchAgents/com.procwarden.daemon.plist` and load it with `launchctl bootstrap gui/$UID`.
  3. Run `init` in the current project.
  4. Print the checklist: found IDEs, panel added, `.procwarden/` created, `graphify-out/` linked if present.
  - `procwarden uninstall` reverses all of this.
- **Done when:** a fake extension (a shell script reading `watch --json`) receives valid JSON every 500 ms, and install/uninstall leave no stray files.

## Phase 13: Scenarios, evaluation, demo

- `tests/scenarios/`: terminal close (SIGHUP), launcher SIGKILL, daemon restart, `setsid` escape, pid reuse, queue + timeout, observe vs. enforce, cleanup approval with a stale-hash rejection, the full `pw-sim demo` script that reproduces the video storyline.
- `procwardend --record eval.csv` logs every sample. `tools/eval.cpp` reports false and missed triggers of (a) the percentage rule and (b) the combined rule against kernel pressure / swap-out ground truth, overshoot for leak rates of 10/50/100 MB/s, and the share of escapes caught → `docs/EVALUATION.md`.
- Final README: install, the privacy note ("everything stays on your Mac; no account"), the local vs. shared `.procwarden`, and how it relates to `graphify-out/`.

---

## How to work with me

- Before each phase, give a 5–10 line plan: files, the lab mechanism carrying the logic, and the tests. Then implement.
- Write complete files for new code and minimal diffs for changes. No `TODO` stubs in delivered phases.
- If macOS behaves differently from this brief (an API, a limit, a cache path), **stop**, explain, propose the smallest change, and record it in `docs/DEVIATIONS.md`.
- Explain each OS concept in 2–3 sentences the first time you use it. The team must be able to defend every line in the lab demo.
- Commit message format: `phase N: <summary>`.

**Start with Phase 0.**

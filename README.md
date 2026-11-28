From 2ef6df5723dfdd81bc37856b5fd9bde2038f9e2f Mon Sep 17 00:00:00 2001
From: Claude <noreply@anthropic.com>
Date: Sat, 28 Nov 2026 21:10:30 +0300
Subject: [PATCH] docs: update README with C++ rewrite progress

Replace the outdated C11/make instructions with the current C++20/CMake
build, a phase-by-phase progress table (0-13 + IDE extension), quick start,
.procwarden folder, safety rules, lab mapping and known deviations.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_017FZeqcfhFYCEEqmLNfi2zk
---
 README.md | 174 ++++++++++++++++++++++++++++++++++++++++++++++++------
 1 file changed, 156 insertions(+), 18 deletions(-)

diff --git a/README.md b/README.md
index bfc55da..85b94e5 100644
--- a/README.md
+++ b/README.md
@@ -1,30 +1,168 @@
 # ProcWarden
 
-ProcWarden is an ICS433 Operating Systems project for studying how to monitor and clean up related processes. The current build provides `warden-sim`, a simulator that creates repeatable workloads. Its parent process forks children, reads their memory reports through a pipe, and reaps them on shutdown.
+**A memory guardian for AI coding sessions on macOS.** ProcWarden watches the processes your coding sessions start: OpenCode agents (Plan, Build, Test, Review), `npm`/`pnpm`/`bun` dev servers, app simulations and Biome. It shows **physical RAM, swap, process count and cached files** for each session and compares them with a **learned normal range**. It clears leftovers and package caches **only after you approve a cleanup plan**.
 
-## Build and run
+Settings live in a `.procwarden/` folder in each project, which you can keep **local** (git-ignored) or **share** with your team, much like `graphify-out/`.
 
-From the repository root on macOS, use Make and Clang to build and run a small example:
+It is also our **ICS433 Operating Systems lab project (KFUPM)**. The core is written in C++20 on top of the mechanisms from Labs 3–6: `fork`/`exec`/`waitpid`, pthreads, signals and pipes, and System V message queues and shared memory.
+
+---
+
+## Progress
+
+**Current stage:** the C++ rewrite has code for Phases 0–12. Phases 0–1 are verified on macOS. Phases 2–12 have code and unit tests, and are waiting for a full test run on the team Macs. Phase 13 (scenario scripts and evaluation) and the IDE extension have not started.
+
+| Phase | Scope | Status |
+|---|---|---|
+| 0 | C++20 / CMake skeleton, presets, `Result<T>`, logging, config, contracts (`docs/PROTOCOL.md`) | ✅ Verified on macOS 26.5 |
+| 1 | RAII wrappers over the lab system calls (pthreads, mutex/condvar, pipes, signals, message queue, shared memory) + lab-check tests | ✅ Verified on macOS 26.5 (see DEV-001, DEV-002) |
+| 2 | Samplers: process snapshot (`libproc`), cmdline/env tags, cwd, system memory, swap, pressure, directory sizes, listening ports | 🧪 Code + unit tests |
+| 3 | Membership engine: pid + start-time identity, parent-chain closure, group, env tag, phase attribution, idle detection | 🧪 Code + unit tests |
+| 4 | `pw-sim` workload simulator: `app`, `agent`, `devserver`, `watcher`, `demo` | 🛠 Code; macOS run pending |
+| 5 | Launcher `procwarden run` (fork → setpgid → execvp → waitpid, exec-error pipe, signal forwarding) | 🛠 Code; macOS run pending |
+| 6 | Daemon `procwardend`: monitor / command server / enforcer threads, registry persistence | 🛠 Code; macOS run pending |
+| 7 | `.procwarden/` folder: `procwarden init`, local vs. shared storage, `.gitignore` handling | 🛠 Code (config is covered by unit tests) |
+| 8 | Baselines: normal range (p10/p90 over 14 days) and "usual for this tool" | 🧪 Code + unit tests |
+| 9 | Decision engine: bands, kernel-pressure bump, hysteresis, growth, budgets, observe vs. enforce | 🧪 Code + unit tests |
+| 10 | Enforcer: TERM → CONT → grace → KILL, pause/resume, renice | 🛠 Code; macOS run pending |
+| 11 | Cleanup planner and executor with plan-hash approval | 🧪 Planner unit-tested; executor run pending (see DEV-004) |
+| 12 | Status publishing (shared memory + JSON stream), CLI (`watch`, `status`, `top`, `ctl`, `cleanup`), `install` (LaunchAgent + editor detection) | 🛠 Code; macOS run pending |
+| 13 | Scenario scripts, evaluation (`--record`, `tools/eval`), demo script | ⏳ Not started |
+| — | IDE extension (right-side panel that reads `procwarden watch --json`) | ⏳ Not started (contract in `docs/PROTOCOL.md` §3) |
+
+✅ verified on macOS · 🧪 code + unit tests in the repo · 🛠 code written, end-to-end check on macOS still to do · ⏳ not started
+
+**Unit tests:** 56 doctest cases in `tests/unit/` covering config, IPC structs and the seqlock, the lab mechanisms, samplers, membership, baselines, decisions, registry and the cleanup planner.
+
+**Next steps:**
+1. Build all presets on the team Macs and run `pw-tests`.
+2. Run the `pw-sim demo` scenario end to end with the daemon.
+3. Add `tests/scenarios/` (terminal close, launcher SIGKILL, daemon restart, `setsid` escape, pid reuse, stale-hash cleanup).
+4. Write the evaluation and the Phase 3+ reports.
+5. Start the IDE extension.
+
+The earlier C11 prototype (Phases 0–2: IPC layer and `warden-sim`) is kept in [`legacy/c11/`](legacy/c11/) for reference. The Phase 1 and 2 reports in `docs/` describe that prototype.
+
+---
+
+## How it fits together
+
+```
+ IDE panel (planned) ──reads──▶ procwarden watch --json ──(pipe, JSON lines)
+                                         │ attaches read-only
+                                         ▼
+ procwarden run -- CMD ──msg queue──▶ procwardend ──writes──▶ shared-memory status table
+   │ fork / setpgid / execvp             │ monitor · command server · enforcer/cleaner threads
+   │ waitpid (only reaper of the root)   │ samples processes + memory every 1 s
+   ▼                                     ▼
+ your workload (own process group)     signals (TERM/CONT/KILL, STOP/CONT) to registered work only
+```
+
+| Binary | Role |
+|---|---|
+| `procwardend` | Daemon: sampling, membership, baselines, decisions, enforcement, cleanup execution |
+| `procwarden` | CLI: `run`, `init`, `install`, `watch`, `status`, `top`, `ctl`, `cleanup` |
+| `pw-sim` | Workload simulator for repeatable demos and tests |
+| `pw-tests` | Unit tests (doctest) |
+
+---
+
+## Build and test (macOS)
+
+You need the Xcode Command Line Tools (Apple clang), CMake ≥ 3.24 and ncurses (included with macOS).
 
 ```sh
-make
-./build/bin/warden-sim watcher -n 2 --mb 8
+cmake --preset release
+cmake --build --preset release
+./build/release/bin/pw-tests          # unit tests
 ```
 
-This starts two idle children with 8 MB each. After about a second, look for `alive=2/2` and `total=16MB` in the output. Press Ctrl+C to stop the simulator and its children.
+Other presets: `debug` (ASan + UBSan), `ubsan`, `tsan`. On macOS 26.5 the ASan and TSan runtimes are broken, so use `ubsan` as the sanitizer check there (see DEV-002).
+
+## Quick start
+
+```sh
+B=./build/release/bin
+
+$B/procwardend &                       # start the daemon (or: $B/procwarden install → LaunchAgent)
+$B/procwarden init                     # create .procwarden/ here (add --shared to commit it)
+$B/procwarden status                   # RAM, swap, processes, pressure (works without the daemon)
+
+# run work inside a tracked session
+$B/procwarden run --phase Test -- $B/pw-sim app --mb 200
+$B/procwarden watch                    # live view;  --json for the IDE stream
+$B/procwarden top                      # ncurses process view
+
+# the scripted demo (scale it down on smaller Macs)
+$B/pw-sim demo --scale 0.5
+
+# cleanup always goes through a plan you approve
+$B/procwarden cleanup plan             # lists every item with its size and a plan hash
+$B/procwarden cleanup apply <plan-hash>
+```
+
+`procwarden ctl kill|pause|resume|renice|budget|adopt …` controls a single session. `kill` needs `--confirm`.
+
+## The `.procwarden/` folder
+
+| File | Purpose |
+|---|---|
+| `settings.json` | What to watch, normal range (auto/manual), alert level, cleanup classes, `enforce` (default `false`). `always_ask` cannot be turned off. |
+| `sessions.json` | Sessions you chose to watch |
+| `cleanup-rules.json` | Phase rules (command pattern → Plan/Build/Test/Review), cache locations and commands, items never to touch |
+| `history/` | Samples every 10 s, used to learn the normal range. Always git-ignored |
+
+**Local** (default) adds `.procwarden/` to `.gitignore`. **Shared** (`init --shared`) commits the rules and settings but keeps `history/` out of git.
+
+---
+
+## Safety rules
+
+1. Only **registered** work (started with `procwarden run` or adopted with your consent) is ever acted on automatically. Other processes, including launchd agents and orphans, are shown for information only.
+2. Every signal is sent only after re-checking the **pid + start time**, so a reused PID is never hit.
+3. The launcher is the **only** component that reaps the workload root. The daemon never calls `waitpid()` on it.
+4. Pausing (`SIGSTOP`) is reported as "growth stopped", never as memory freed.
+5. Nothing is stopped or deleted without an **approved cleanup plan**. If the system state changes, the plan hash changes and the approval is rejected.
+6. Automatic memory enforcement is **off by default**. When it is on, it needs the RED band **and** kernel pressure ≥ warning **and** a session over budget, and it stops at most one session per decision.
+7. Caches are cleared with each tool's own command (`npm cache clean --force`, `pnpm store prune`, `bun pm cache rm`). Paths must resolve inside `$HOME`.
+
+## OS concepts used (lab mapping)
+
+| Lab | Mechanisms | Used in |
+|---|---|---|
+| Lab 3: Processes | `fork`, `execvp`, `waitpid` + exit-status macros, orphans, zombies | `procwarden run`, `pw-sim`, leftover detection |
+| Lab 4: Threads | `pthread_create`/`join`, context structs, heap results; mutex + condvar | Daemon threads, simulator leak threads |
+| Lab 5: Signals & pipes | `signal`, `kill`, `pause`, `SIGUSR1`, `pipe` + `fork` | Enforcer, signal forwarding, exec-error pipe, JSON stream, simulator control |
+| Lab 6: IPC | `msgget`/`msgsnd`/`msgrcv` (mtype routing, urgent first), `shmget`/`shmat` (seqlock status table) | Command channel, live status |
+
+Also used: process groups (`setpgid`, `kill(-pgid)`), `setpriority`, `libproc`, `sysctl`, `host_statistics64`, file-system walks for cache sizes.
+
+## Known deviations
+
+These are recorded in [`docs/DEVIATIONS.md`](docs/DEVIATIONS.md):
+- **DEV-001:** on macOS a negative `msgtyp` behaves as a FIFO upper bound rather than "lowest type first". `MsgQueue::recv()` checks for urgent messages first to get the intended priority.
+- **DEV-002:** the ASan and TSan runtimes are broken on macOS 26.5 / Apple clang 17, so the `ubsan` preset is used as the sanitizer check.
+- **DEV-003:** `ps` RSS under-reports memory that has been compressed, so `phys_footprint` is the main metric and RSS is shown as an upper bound.
+- **DEV-004:** cache paths differ between machines. Sizes are measured and never invented; a missing path is reported as 0 bytes.
+
+## Repository layout
+
+```
+include/pw/   headers: os/ (lab wrappers), core/ (pure logic), ipc/ (protocol, status table, stream)
+src/          os/  core/  ipc/  common/  daemon/  cli/  sim/
+tests/unit/   doctest unit tests
+docs/         proposal v2, C++ phase brief, protocol, deviations, phase reports
+legacy/c11/   original C11 prototype (superseded)
+third_party/  json.hpp, doctest.h
+```
 
-## Workload profiles
+## Documentation
 
-| Profile | What it simulates |
-| --- | --- |
-| `install` | Children gradually allocate memory. |
-| `devserver` | A local development server listens on a TCP port. |
-| `watcher` | Idle children hold memory and report their usage. |
-| `agents` | Worker threads can simulate a growing memory leak. |
+- [`docs/PROCWARDEN_CPP_PHASES.md`](docs/PROCWARDEN_CPP_PHASES.md): the phase-by-phase build plan
+- [`docs/PROTOCOL.md`](docs/PROTOCOL.md): message queue, shared-memory table, JSON stream, project folder
+- [`docs/DEVIATIONS.md`](docs/DEVIATIONS.md): where macOS differs from the plan, and the fix for each
+- [`docs/ProcWarden_OS_Lab_Project_Proposal_v2.docx`](docs/ProcWarden_OS_Lab_Project_Proposal_v2.docx): the revised proposal
 
-## Project files
+## Team
 
-- `src/sim/` contains the parent simulator and the child workload implementations.
-- `src/common/` contains shared code for configuration, logging, and interprocess communication. Its headers and the simulator interface are in `include/`.
-- `tests/` contains C checks and a shell script that exercises the simulator profiles.
-- `docs/` contains the project proposal, phase reports, and implementation notes.
+Ali Lutfallah · Mohammed Alnasr · Qaseem Al Munayan · Mohammed Alrikhrikh. ICS433 Operating Systems, KFUPM.
-- 
2.43.0
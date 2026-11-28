# DEVIATIONS

Documented differences between `docs/PROCWARDEN_CPP_PHASES.md` and what this
macOS machine actually does, plus the smallest fix we chose. Nothing is
quietly redesigned; every entry names a repro and a test.

Measured limits on the team Macs: `msg_qbytes` = **2048 bytes** and
`kern.sysv.shmmax` = **4194304 bytes** (4 MB). The status table is ~35 KB.

---

## DEV-001 — negative `msgtyp` is a bound, not a priority ordering (Phase 1)

**Spec (§3.2):** "The daemon reads with `msgrcv(q, &m, SZ, -2, 0)`. Lab 6: a
negative `msgtyp` returns the lowest type first, so KILL, CANCEL and APPROVE
are served before NORMAL requests."

**Observed:** On macOS 26.5.2 / Apple clang 17, a negative `msgtyp` behaves as
an **upper bound in FIFO order**: the call returns the *oldest* message whose
type is `<= |msgtyp|`, not the lowest type. Repro (types 3, 1, 2 sent in that
order):

```
msgrcv(-3) -> type 3
msgrcv(-3) -> type 1
msgrcv(-3) -> type 2
```

Test that catches it: `tests/unit/test_os.cpp`, case
"Lab 6: negative msgtyp serves URGENT before NORMAL" (sends NORMAL then
URGENT and expects URGENT back).

**Fix (smallest possible):** keep one queue and the negative `msgtyp` API, but
make `MsgQueue::recv()` emulate "lowest type first": when `msgtyp < 0`, probe
exact types `1..|msgtyp|` with `IPC_NOWAIT`, then fall back to the kernel's
blocking `msgrcv(msgtyp)`. For the daemon's `-2` this means URGENT (type 1) is
always drained before NORMAL (type 2). The only window is a URGENT message
arriving between the probe and the blocking call; the loop re-probes every
iteration, so the worst case is one NORMAL request served first.

---

## DEV-002 — ASan hangs and TSan crashes at runtime on macOS 26.5 (Phase 0)

**Spec (§1.3):** presets `debug` (ASan + UBSan), `tsan`, `release`; Phase 1
"done when the lab-check tests pass under ASan and TSan".

**Observed:** both presets **build** cleanly, but the sanitizer runtimes
themselves are broken on this OS/toolchain pair (Apple clang 17.0.0, arm64):

- ASan: hangs during runtime initialization, before `main`
  (`AsanInitInternal -> InitializeShadowMemory -> get_dyld_hdr`). A
  hello-world built with `clang++ -fsanitize=address` also hangs.
- TSan: immediate SIGSEGV (exit 139) for any program, including hello-world.
- UBSan alone works correctly.

Repro: `python3 -c 'import subprocess; subprocess.run(["build/debug/bin/pw-tests"], timeout=6)'`
times out; the TSan binary exits with signal 11.

**Fix (smallest possible):** keep the `debug` and `tsan` presets exactly as
specified (they work on a Mac with a working sanitizer runtime), and add a
`ubsan` preset (`-fsanitize=undefined`) as the sanitizer gate for this
machine. Release (`-Werror`) and UBSan are what we run in the phase checklist;
revisit when macOS or the Command Line Tools updates its sanitizer runtime.

---

## DEV-003 — `ps` RSS under-reports dirty-but-cold memory (Phase 2/4)

**Observed:** a process that has dirtied tens of MB (one write per 4096-byte
page, never freed) can still show an `rss` of a few hundred KB in `ps`. macOS
compresses dirty pages that are not re-accessed, and compressed pages leave
the resident set.

**Consequence:** RSS is only an upper bound for what a workload really holds.
The sampler therefore uses `ri_phys_footprint` from
`proc_pid_rusage(RUSAGE_INFO_V2)` as the primary metric (it includes
compressed pages), and publishes `rss` alongside it. The
`[footprint, RSS]` range is itself a teaching point in the proposal.

---

## DEV-004 — cache paths must be verified per machine (Phase 11)

The brief lists npm (`~/.npm/_cacache`), pnpm (from `pnpm store path`), bun
(`~/.bun/install/cache`) and Biome cache/log paths. On the lab Macs only some
of these exist. The planner records the *measured* size of each path and the
cleaner uses the tool's own command when present (`npm cache clean --force`,
`pnpm store prune`, `bun pm cache rm`); a missing path is reported as
`bytes: 0` and never invented. Paths are checked with `realpath` and must stay
inside `$HOME`.

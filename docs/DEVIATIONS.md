# DEVIATIONS

Documented differences between the approved specification and what our
macOS version actually does, plus the smallest fix we chose. Rule: nothing
is quietly redesigned; every entry has a repro / test.

---

## DEV-001 — negative `msgtyp` is a bound, not a priority ordering (found in Phase 0)

**Spec (docs/BUILD_PROMPT.md, Phase 1):** "The daemon receives with
`msgrcv(q, &m, SZ, -2, 0)`. A negative `msgtyp` returns the lowest type
first, so URGENT is always served before NORMAL."

**Observed:** On macOS 26.5.2 (Apple clang 17, Xcode CLT), a negative
`msgtyp` in `msgrcv()` behaves as an *upper bound*, not a sorting rule:
the call returns the **oldest message whose type ≤ |msgtyp|**, in queue
arrival order. Repro:

```
msgsnd types 3, 1, 2  (in that order)
msgrcv(-3) -> type 3   (arrival order, NOT lowest-first)
msgrcv(-3) -> type 1
msgrcv(-3) -> type 2
```

Test: `tests/lab_check.c` → the second Lab 6 message check demonstrates the
exact two-step behaviour we rely on.

**Fix (smallest possible):** keep one single queue and negative `msgtyp`
semantics, but make the daemon do a two-step receive (Phase 1 on):

1. `msgrcv(q, &m, SZ, -1, IPC_NOWAIT)` — if ENOMSG/EAGAIN, fall through;
   otherwise this is an URGENT request, process it first.
2. `msgrcv(q, &m, SZ, -2, 0)` (blocking) — receives the oldest NORMAL
   request (types > 2, i.e. replies addressed to client pids, are
   excluded by the bound either way).

This preserves the spec's invariant ("URGENT is always served before
NORMAL") with the same lab mechanism (negative `msgtyp` as a type filter).
The window in which a URGENT message arrives after the step-1 probe but
before step-2 completes is at most one serviced request — acceptable, and
the daemon loop re-probes URGENT every iteration.

---

## DEV-002 — ASan hangs and TSan crashes at runtime on macOS 26.5 (found in Phase 0)

**Spec (CLAUDE.md Code standards):** "`make debug` (ASan + UBSan), `make
tsan`" must be usable flavors.

**Observed:** On macOS 26.5.2 / Apple clang 17.0.0 (Command Line Tools,
ARM64), the sanitizer *runtimes themselves* are broken on this OS/toolchain
pair — both repro failures occur for trivially small programs:

- ASan: the process hangs during runtime initialization, before `main`:
  `__asan::AsanInitInternal → InitializeShadowMemory → get_dyld_hdr →
  dyld_shared_cache_iterate_text_swift`. Repro:
  `clang -fsanitize=address hello.c` with `int main` returning 0 hangs.
- TSan: immediate SIGSEGV in the runtime for any program that prints or
  forks (`clang -fsanitize=thread hello.c` printing "hi" → exit 139).
- UBSan alone works correctly (recovered us — see below).

**Fix (smallest possible):** the `debug` target is left exactly as specified
(ASan + UBSan) so it works unchanged on a Mac with an older toolchain pair;
a `make ubsan` flavor was added as the sanitizer fallback for this Mac, and
we run it (+ release, `-Werror`) as our sanitizer gate until the OS or CLT
fixes the ASan/TSan runtime bug. Nothing in ProcWarden's code is affected;
the test suite passes under UBSan
(`make ubsan && ./build/bin-ubsan/test-lab_check`).

To revisit: when macOS 26.x or the Command Line Tools updates their
sanitizer runtime, drop `ubsan` from active use and re-enable `debug`/`tsan`
runs in the phase checklist.

---

## DEV-003 — `ps -o rss` under-reports dirty-but-cold memory (found in Phase 2)

**Spec (docs/BUILD_PROMPT.md, Phase 2):** simulator behaviour is accepted
"as checked with `ps -o pid,ppid,pgid,rss,command`", implying RSS reflects
the touched (dirty) memory.

**Observed:** On macOS 26.5.2, a child that has dirtied tens of MB (one
write per 4096-byte page, never freed) still shows an `rss` of only a few
hundred KB in `ps`. The kernel compresses dirty pages that are not
re-accessed, and compressed pages leave the resident set. The memory is
real and is accounted in `ri_phys_footprint` (it includes compressed
pages), which is exactly why CLAUDE.md Rule 4 fixes footprint as the
metric and RSS only as the upper bound.

**Fix (smallest possible):** the scenario check uses a modest RSS floor
(> 300 KB, i.e. "process alive with resident pages") and treats the
simulator's own per-second pipe reports (self-reported allocated MB) as
the growth evidence. From Phase 3 on, `proc_pid_rusage(RUSAGE_INFO_V2)`
gives the true footprint. No redesign; the [footprint, RSS] range
showing the gap is itself a teaching point of the proposal.

---


On the team Macs: `msg_qbytes` = **2048 bytes**, so with our ≤ 256-byte
messages roughly 8 messages can be in flight; `kern.sysv.shmmax` =
**4194304 bytes** (4 MB), so the status table (Phase 1) must stay well
under that — it does, by design (< 1 MB).

## Side notes on System V limits (Rule 4, measured in Phase 0)

On the team Macs: `msg_qbytes` = **2048 bytes**, so with our ≤ 256-byte
messages roughly 8 messages can be in flight; `kern.sysv.shmmax` =
**4194304 bytes** (4 MB), so the status table (Phase 1) fits comfortably
(it is 47,720 bytes).

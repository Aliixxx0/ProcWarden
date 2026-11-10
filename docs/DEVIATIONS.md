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

Test: `tests/lab_check.c` → the second Lab 6 message check documents the
exact behaviour we rely on.

**Fix (smallest possible):** keep one single queue and negative `msgtyp`
semantics, but make the daemon do a two-step receive:

1. `msgrcv(q, &m, SZ, -1, IPC_NOWAIT)` — if ENOMSG/EAGAIN, fall through;
   otherwise this is an URGENT request, process it first.
2. `msgrcv(q, &m, SZ, -2, 0)` (blocking) — receives the oldest NORMAL
   request (types > 2, i.e. replies addressed to client pids, are
   excluded by the bound either way).

This preserves the spec's invariant ("URGENT is always served before
NORMAL") with the same lab mechanism (negative `msgtyp` as type filter).
The window in which a URGENT message arrives after the step-1 probe but
before step-2 completes is at most one serviced request — acceptable, and
the monitor loop re-probes URGENT every iteration.

**Side notes on limits (Rule 4):** on the team Macs, `msg_qbytes` is
2048 bytes, so with our ≤ 256-byte messages roughly 8 messages can be in
flight; `kern.sysv.shmmax` = 4194304 bytes (4 MB), so the status table
(Phase 1) must stay well under 1 MB — it does, by design.

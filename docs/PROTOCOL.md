# ProcWarden protocol

This document is the contract between the C++ core and any client (the CLI,
the IDE extension, the test scripts). It is stable from Phase 0 onward.

## 1. Message queue (Lab 6)

One System V queue, `key = ftok("~/.procwarden/key", 'Q')`, permissions `0600`.

`PwMsg` is fixed-size and trivially copyable; the payload after `mtype` is at
most 248 bytes because the default macOS queue (`msg_qbytes`, 2048 bytes on the
lab machines) is small.

| Field | Meaning |
|---|---|
| `mtype` | `1` URGENT to daemon, `2` NORMAL to daemon, `>2` reply to client pid |
| `op` | operation (`REGISTER`, `HEARTBEAT`, …) |
| `client_pid` | sender pid (used as reply `mtype`) |
| `sess_id` | session id, 0 if unknown |
| `a`, `b` | small integer arguments (reply code, flags) |
| `c` | 64-bit argument (bytes, start-time usec, plan hash) |
| `text[168]` | project path, label, phase, error text |

The daemon reads with `msgrcv(q, &m, SZ, -2, 0)`: a negative `msgtyp` returns
the lowest type first, so URGENT (KILL, CANCEL, APPROVE_CLEANUP) is served
before NORMAL requests. Replies use `mtype = client_pid`.

## 2. Shared-memory status table (Lab 6)

`key = ftok("~/.procwarden/key", 'S')`, under 1 MB. One writer (the daemon),
many readers. The writer increments `header.seq` to an odd value, writes, then
increments again to even. Readers retry while `seq` is odd or changed.

```
header{magic,version,seq,sample_no,t_usec}
sys{total,used,phys_free,compressed,swap_used,swap_total,pressure_level,processes,band,enforce}
normal{low_gb,high_gb,mode}
sessions[64]{id,tool,project,phase_mask,state,members,footprint,rss_upper,usual_gb,ratio,owner_alive}
agents[16]{sess_id,phase,footprint,idle_bytes,status}
procs[512]{pid,ppid,pgid,sess_id,phase,footprint,rss,state,escaped}
ring[120]{t_usec,ram,swap}          // last 60 s at 0.5 s resolution
cleanup{plan_hash,state,items[16]{kind,label,detail,bytes,status}}
```

`sess_id == 0` in `procs[]` means *unmanaged*: listed for information, never
acted on automatically.

## 3. JSON stream (Lab 5 pipe)

`procwarden watch --json` attaches the table read-only and writes one JSON
object per line to stdout every 500 ms. It has no other side effects and exits
when stdout closes (`EPIPE`).

```json
{"t":1732712345.5,
 "ram":{"used_gb":14.2,"total_gb":16},
 "swap":{"used_mb":3891},
 "processes":412,
 "pressure":"high",
 "band":"RED",
 "normal":{"low_gb":5,"high_gb":9},
 "agents":[{"phase":"Test","gb":1.4,"idle_gb":2.1,"status":"done"}],
 "sessions":[{"tool":"opencode","project":"my-saas-app","gb":7.7,"usual_gb":4.5,"ratio":1.7}],
 "chart":[[1732712345.5,14.2,3.8]],
 "cleanup":{"ram_gb":8.8,"disk_gb":7.1,
            "items":[{"label":"6 app simulations","bytes":4939212390,"kind":"ram","status":"ready"}]}}
```

`pressure` is one of `normal`, `warning`, `critical`. `band` is one of
`GREEN`, `YELLOW`, `ORANGE`, `RED`.

## 4. Project folder `.procwarden/`

| File | Content |
|---|---|
| `settings.json` | watch list, normal range, alerts, cleanup permission, enforce |
| `sessions.json` | sessions the user chose to watch |
| `cleanup-rules.json` | phase patterns, cache locations and clear commands, never-touch list |
| `history/YYYY-MM-DD.jsonl` | one line per 10 s: totals and per-session footprint |

`storage:"local"` keeps the folder in the project `.gitignore`;
`storage:"shared"` removes that line but keeps `.procwarden/history/` ignored.

## 5. Reply codes

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | ADMIT |
| 2 | QUEUED (position in `a`) |
| 3 | REFUSED |
| 4 | TIMEOUT |
| 5 | ERROR (text carries the message) |
| 6 | STALE_HASH (cleanup approval rejected) |
| 7 | BUSY |

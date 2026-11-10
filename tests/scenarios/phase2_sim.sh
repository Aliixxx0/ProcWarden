#!/bin/sh
# tests/scenarios/phase2_sim.sh — Phase 2 acceptance (BUILD_PROMPT):
# each warden-sim profile behaves as specified, checked with ps/lsof.
# Run from anywhere: paths are resolved relative to this script.
set -u
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SIM="$REPO/build/bin/warden-sim"
OUT=/tmp/procwarden_sim_$$.out
PARENT=0
GRAND=0
fails=0

cleanup() {
    [ "$PARENT" -gt 0 ] && kill -TERM "$PARENT" 2>/dev/null
    [ "$GRAND" -gt 0 ] && kill -TERM "$GRAND" 2>/dev/null
    sleep 1
    [ "$PARENT" -gt 0 ] && kill -KILL "$PARENT" 2>/dev/null
    [ "$GRAND" -gt 0 ] && kill -KILL "$GRAND" 2>/dev/null
    rm -f "$OUT"
    exit 0
}
trap cleanup EXIT INT TERM

# Shell convention: status 0 = the check passed.
check() {
    if [ "$2" -eq 0 ]; then
        echo "PASS $1"
    else
        echo "FAIL $1"
        fails=$((fails + 1))
    fi
}

# Poll $OUT until pattern appears, up to ~N seconds. Status 0 = found.
wait_for() {  # wait_for SECONDS PATTERN
    i=0
    while [ $i -lt $(( $1 * 5 )) ]; do
        grep -q "$2" "$OUT" 2>/dev/null && return 0
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

pgid_of() { ps -o pgid= -p "$1" 2>/dev/null | tr -d ' '; }
ppid_of() { ps -o ppid= -p "$1" 2>/dev/null | tr -d ' '; }
rss_of()  { ps -o rss=  -p "$1" 2>/dev/null | tr -d ' '; }

# total MB of the last tick line at or before second $1
total_at() {
    grep -o 't=[0-9]*s alive=[0-9]*/[0-9]* reported=[0-9]* total=[0-9]*MB' "$OUT" \
        | tail -1 | sed 's/.*total=\([0-9]*\)MB.*/\1/'
}

[ -x "$SIM" ] || { echo "FATAL: $SIM not built (run make)"; exit 1; }

# ---------------------------------------------------------- 1. agents leak
"$SIM" agents -n 4 --threads 2 --mb 16 --leak 20 --leak-after 2 >"$OUT" 2>&1 &
PARENT=$!
wait_for 10 'leak trigger'
EARLY=$(grep -o 't=1s alive=[0-9]*/[0-9]* reported=[0-9]* total=[0-9]*MB' "$OUT" | tail -1 | sed 's/.*total=\([0-9]*\)MB.*/\1/')
wait_for 12 't=3s'
LATE=$(total_at 3)
ok=1
[ -n "$EARLY" ] && [ -n "$LATE" ] && [ "$LATE" -gt $((EARLY + 40)) ] && ok=0
check "agents: per-second reports and leak growth after SIGUSR1 trigger ($EARLY -> $LATE MB)" $ok

# children share the parent's process group and have resident memory.
# NOTE: ps rss only shows *uncompressed* resident pages; macOS compresses
# dirty-but-cold pages (see docs/DEVIATIONS.md), so the floor is modest —
# the daemon's ri_phys_footprint (Phase 3) is the real metric.
PG=$(pgid_of "$PARENT"); KIDS=$(pgrep -P "$PARENT" | wc -l | tr -d ' ')
SAMES=1; NONZERO=1
for k in $(pgrep -P "$PARENT"); do
    [ "$(pgid_of "$k")" = "$PG" ] || SAMES=0
    [ "$(rss_of "$k")" -gt 300 ] || NONZERO=0
done
ok=1
[ "$KIDS" -eq 4 ] && [ "$SAMES" -eq 1 ] && ok=0
check "agents: 4 children in the parent's process group (pgid $PG)" $ok
ok=1; [ "$NONZERO" -eq 1 ] && ok=0
check "agents: children have resident memory" $ok

kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null
ok=1; grep -q 'finished, lifetime total' "$OUT" && grep -q 'warden-sim finished' "$OUT" && ok=0
check "agents: SIGTERM -> threads joined, totals reported, clean exit" $ok
PARENT=0

# ---------------------------------------------------------- 2. escape
"$SIM" agents -n 2 --mb 8 --leak 0 --escape >"$OUT" 2>&1 &
PARENT=$!
wait_for 10 'spawned grandchild'
GRAND=$(grep -o 'spawned grandchild [0-9]*' "$OUT" | tail -1 | sed 's/.* //')
ok=1
[ -n "$GRAND" ] && [ "$(ppid_of "$GRAND")" = "1" ] \
  && [ "$(pgid_of "$GRAND")" = "$GRAND" ] \
  && [ "$(pgid_of "$GRAND")" != "$(pgid_of "$PARENT")" ] && ok=0
check "escape: grandchild ${GRAND:-none} re-parented to launchd with its own pgid (setsid)" $ok
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0
kill -TERM "$GRAND" 2>/dev/null; GRAND=0

# ---------------------------------------------------------- 3. devserver
"$SIM" devserver -n 1 --mb 32 --port 3128 >"$OUT" 2>&1 &
PARENT=$!
ok=1
i=0
while [ $i -lt 25 ]; do
    if /usr/sbin/lsof -nP -iTCP:3128 -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then ok=0; break; fi
    sleep 0.2; i=$((i + 1))
done
check "devserver: binds and listens on 127.0.0.1:3128" $ok
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 4. install
"$SIM" install -n 3 --mb 40 >"$OUT" 2>&1 &
PARENT=$!
wait_for 10 't=2s'
KIDS=$(pgrep -P "$PARENT" | wc -l | tr -d ' ')
ok=1; [ "$KIDS" -eq 3 ] && grep -q 't=2s' "$OUT" && ok=0
check "install: 3 children alive, per-second reporting while ramping" $ok
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 5. watcher
"$SIM" watcher -n 2 --mb 16 >"$OUT" 2>&1 &
PARENT=$!
wait_for 10 't=1s'
KIDS=$(pgrep -P "$PARENT" | wc -l | tr -d ' ')
ok=1; [ "$KIDS" -eq 2 ] && grep -q 't=1s' "$OUT" && ok=0
check "watcher: 2 idle children reporting" $ok
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 6. shared-mb
"$SIM" agents -n 2 --mb 8 --threads 2 --shared-mb 4 >"$OUT" 2>&1 &
PARENT=$!
wait_for 10 'shared segment id='
ok=1; grep -q 'shared segment id=' "$OUT" && ok=0
check "shared-mb: one System V segment created and announced" $ok
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 7. ignore-hup
"$SIM" watcher -n 1 --mb 8 --ignore-hup >"$OUT" 2>&1 &
PARENT=$!
sleep 1
kill -HUP "$PARENT"
sleep 1
ok=1; kill -0 "$PARENT" 2>/dev/null && ok=0
check "ignore-hup: SIGHUP does not stop the simulator" $ok
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

if [ "$fails" -gt 0 ]; then
    echo "== $fails SCENARIO CHECK(S) FAILED =="
    exit 1
fi
echo "== ALL PHASE 2 SCENARIO CHECKS PASSED =="

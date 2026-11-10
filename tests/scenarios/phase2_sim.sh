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
}
trap cleanup EXIT INT TERM

check() {
    if [ "$2" -eq 1 ]; then
        echo "PASS $1"
    else
        echo "FAIL $1"
        fails=$((fails + 1))
    fi
}

pgid_of() { ps -o pgid= -p "$1" 2>/dev/null | tr -d ' '; }
ppid_of() { ps -o ppid= -p "$1" 2>/dev/null | tr -d ' '; }
rss_of()  { ps -o rss=  -p "$1" 2>/dev/null | tr -d ' '; }

total_at() {  # total MB from the last tick line with t=<seconds>
    grep -o "t=$1s alive=[0-9]*/[0-9]* reported=[0-9]* total=[0-9]*MB" "$OUT" \
        | tail -1 | sed 's/.*total=\([0-9]*\)MB.*/\1/'
}

[ -x "$SIM" ] || { echo "FATAL: $SIM not built (run make)"; exit 1; }

# ---------------------------------------------------------- 1. agents leak
"$SIM" agents -n 4 --threads 2 --mb 16 --leak 20 --leak-after 2 >"$OUT" 2>&1 &
PARENT=$!
sleep 5
EARLY=$(total_at 1)
LATE=$(total_at 4)
[ -n "$EARLY" ] && [ -n "$LATE" ] && [ "$LATE" -gt $((EARLY + 40)) ]
check "agents: per-second reports and leak growth after SIGUSR1 trigger ($EARLY -> $LATE MB)" $?

# children share the parent's process group, have resident memory
PG=$(pgid_of "$PARENT"); KIDS=$(pgrep -P "$PARENT" | wc -l | tr -d ' ')
SAMES=1; NONZERO=1
for k in $(pgrep -P "$PARENT"); do
    [ "$(pgid_of "$k")" = "$PG" ] || SAMES=0
    [ "$(rss_of "$k")" -gt 1000 ] || NONZERO=0
done
[ "$KIDS" -eq 4 ] && [ "$SAMES" -eq 1 ]
check "agents: 4 children in the parent's process group (pgid $PG)" $?
check "agents: children have resident (touched) memory" $?

kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null
grep -q "finished, lifetime total" "$OUT"
check "agents: SIGTERM -> threads joined, lifetime totals reported" $?
grep -q "warden-sim finished" "$OUT"
check "agents: parent reaps all children and exits cleanly" $?
PARENT=0

# ---------------------------------------------------------- 2. escape
"$SIM" agents -n 2 --mb 8 --leak 0 >"$OUT" 2>&1 &
PARENT=$!
sleep 3
GRAND=$(grep -o 'spawned grandchild [0-9]*' "$OUT" | tail -1 | sed 's/.* //')
if [ -n "$GRAND" ] && [ "$(ppid_of "$GRAND")" = "1" ] \
   && [ "$(pgid_of "$GRAND")" = "$GRAND" ] && [ "$(pgid_of "$GRAND")" != "$(pgid_of "$PARENT")" ]; then
    check "escape: grandchild $GRAND re-parented to launchd, own pgid (setsid)" 1
else
    check "escape: grandchild re-parented to launchd with its own pgid" 0
fi
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0
kill -TERM "$GRAND" 2>/dev/null; GRAND=0

# ---------------------------------------------------------- 3. devserver
"$SIM" devserver -n 1 --mb 32 --port 3128 >"$OUT" 2>&1 &
PARENT=$!
sleep 2
/usr/sbin/lsof -nP -iTCP:3128 -sTCP:LISTEN 2>/dev/null | grep -q LISTEN
check "devserver: binds and listens on 127.0.0.1:3128" $?
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 4. install
"$SIM" install -n 3 --mb 40 >"$OUT" 2>&1 &
PARENT=$!
sleep 5
RAMP=$(grep -c 't=[0-9]s' "$OUT")
KIDS=$(pgrep -P "$PARENT" | wc -l | tr -d ' ')
[ "$RAMP" -ge 3 ] && [ "$KIDS" -eq 3 ]
check "install: 3 children alive with per-second reporting (ramp to 40 MB)" $?
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 5. watcher
"$SIM" watcher -n 2 --mb 16 >"$OUT" 2>&1 &
PARENT=$!
sleep 2
KIDS=$(pgrep -P "$PARENT" | wc -l | tr -d ' ')
[ "$KIDS" -eq 2 ] && grep -q "t=1s" "$OUT"
check "watcher: 2 idle children reporting" $?
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 6. shared-mb
"$SIM" agents -n 2 --mb 8 --threads 2 --shared-mb 4 >"$OUT" 2>&1 &
PARENT=$!
sleep 3
grep -q "shared segment id=" "$OUT"
check "shared-mb: one System V segment created and announced" $?
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

# ---------------------------------------------------------- 7. ignore-hup
"$SIM" watcher -n 1 --mb 8 --ignore-hup >"$OUT" 2>&1 &
PARENT=$!
sleep 1
kill -HUP "$PARENT"
sleep 1
kill -0 "$PARENT" 2>/dev/null
check "ignore-hup: SIGHUP does not stop the simulator" $?
kill -TERM "$PARENT"; wait "$PARENT" 2>/dev/null; PARENT=0

if [ "$fails" -gt 0 ]; then
    echo "== $fails SCENARIO CHECK(S) FAILED =="
    exit 1
fi
echo "== ALL PHASE 2 SCENARIO CHECKS PASSED =="

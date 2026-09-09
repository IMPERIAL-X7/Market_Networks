#!/bin/sh
#
# Runs one assignment experiment and captures the system-level evidence the
# report needs at the moments it is actually observable.
#
#   tools/capture_experiment.sh <experiment_number> [output_file]
#
# Everything here uses only unprivileged FreeBSD tools. Packet-level captures
# (tcpdump) need root and are taken separately.

set -u

N=${1:?usage: capture_experiment.sh <experiment_number> [output]}
OUT=${2:-report/capture_exp$N.txt}
PORT=5000

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p "$(dirname "$OUT")"

pkill -x exchange_server >/dev/null 2>&1
sleep 1

say() { printf '\n===== %s =====\n' "$*" >> "$OUT"; }
run() { printf '\n$ %s\n' "$*" >> "$OUT"; sh -c "$*" >> "$OUT" 2>&1; }

server_pid() { pgrep -x exchange_server | head -1; }

snapshot() {  # $1 = label
    say "$1  (t+$(( $(date +%s) - START ))s)"
    run "sockstat -4 -p $PORT"
    run "netstat -an -p tcp | grep -E '$PORT|Proto|Recv-Q'"
    pid=$(server_pid)
    if [ -n "$pid" ]; then
        run "procstat -f $pid | head -20"
        run "ps -o pid,rss,%cpu,wchan,command -p $pid"
    fi
}

: > "$OUT"
{
    echo "Experiment $N capture"
    echo "host:   $(uname -a)"
    echo "date:   $(date)"
    echo "server: $(cd "$ROOT" && ./bin/exchange_server --help >/dev/null 2>&1; echo ok)"
} >> "$OUT"

START=$(date +%s)

# Experiment 3 is the one that needs the server's own message-framing trace.
if [ "$N" = "3" ]; then
    EXCHANGE_VERBOSE=1 export EXCHANGE_VERBOSE
fi

# No setsid(1) on FreeBSD: run the harness directly and stop it, and anything
# it spawned, by name afterwards.
python3 -u experiment.py "$N" > "$OUT.harness" 2>&1 &
HARNESS=$!

case "$N" in
  1)  sleep 4;  snapshot "client connected and idle" ;;
  2)  sleep 4;  snapshot "phase 1: connection established and idle"
      sleep 9;  snapshot "phase 2: just after the client closed"
      sleep 6;  snapshot "phase 2: a few seconds later" ;;
  3)  sleep 5;  snapshot "after the message was sent in four writes" ;;
  4)  sleep 3;  snapshot "client 1 connected, mid-message, silent"
      sleep 4;  snapshot "client 2 connected and answered"
      pid=$(server_pid)
      [ -n "$pid" ] && run "procstat -kk $pid" ;;
  5)  sleep 6;  snapshot "five connections, three of them active"
      sleep 6;  snapshot "observation phase" ;;
  6)  sleep 4;  snapshot "part A: after the orderly FIN"
      sleep 10; snapshot "part B: after the abortive RST"
      sleep 8;  snapshot "later" ;;
  7)  sleep 20; snapshot "traffic running: slow vs normal subscriber"
      sleep 25; snapshot "traffic running: later"
      sleep 25; snapshot "traffic running: later still" ;;
  8)  sleep 6;  snapshot "before the disconnection"
      sleep 12; snapshot "just after the client process was killed"
      sleep 10; snapshot "after further traffic" ;;
esac

say "server log"
cat "$OUT.harness" | grep -E '^\[|listening|shutting' >> "$OUT" 2>/dev/null

say "harness output"
cat "$OUT.harness" >> "$OUT" 2>/dev/null

kill -INT "$HARNESS" >/dev/null 2>&1
sleep 3
kill -TERM "$HARNESS" >/dev/null 2>&1
pkill -f 'experiment\.py' >/dev/null 2>&1
pkill -x exchange_server >/dev/null 2>&1
sleep 1
rm -f "$OUT.harness"

echo "wrote $OUT"

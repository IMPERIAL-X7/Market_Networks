#!/bin/sh
#
# Captures the packet-level view of one experiment. Must be run as root,
# because tcpdump needs access to the packet capture device:
#
#   su -m root -c 'sh tools/capture_tcpdump.sh <experiment_number>'
#
# The experiment itself is run as the owning user, so the build products stay
# owned by that user rather than by root.

set -u
N=${1:?usage: capture_tcpdump.sh <experiment_number>}
RUN_AS=${RUN_AS:-tejasvi}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/report/tcpdump_exp$N.txt"

case "$N" in
  2) DURATION=30 ;;
  6) DURATION=32 ;;
  8) DURATION=25 ;;
  *) DURATION=25 ;;
esac

pkill -x exchange_server 2>&- 
pkill -x tcpdump 2>&-
sleep 1

{
    echo "tcpdump capture for experiment $N"
    echo "host: $(uname -a)"
    echo "date: $(date)"
    echo "filter: 'port 5000' on lo0"
    echo
} > "$OUT"

# -tttt  absolute timestamps, so packets line up with the experiment's phases
# -S     absolute sequence numbers, which makes the FIN/ACK exchange readable
tcpdump -i lo0 -n -tttt -S 'port 5000' >> "$OUT" 2>&1 &
TCPDUMP_PID=$!
sleep 1

su -m "$RUN_AS" -c "cd '$ROOT' && python3 -u experiment.py $N" \
    > "$OUT.harness" 2>&1 &
HARNESS=$!

sleep "$DURATION"

kill -INT $HARNESS 2>&-
sleep 2
kill -TERM $HARNESS 2>&-
pkill -f 'experiment\.py' 2>&-
pkill -x exchange_server 2>&-
sleep 1
kill -TERM $TCPDUMP_PID 2>&-
sleep 1

{
    echo
    echo "===== server log ====="
    grep -E '^\[|listening' "$OUT.harness" 2>/dev/null
    echo
    echo "===== harness output ====="
    cat "$OUT.harness" 2>/dev/null
} >> "$OUT"

rm -f "$OUT.harness"
chown "$RUN_AS" "$OUT" 2>&-
echo "wrote $OUT"

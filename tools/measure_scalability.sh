#!/bin/sh
#
# Bonus experiment: the cost of maintaining many idle TCP connections.
#
#   tools/measure_scalability.sh [host] [port] [counts...]
#
# For each connection count this starts a fresh Exchange Server, opens that
# many idle connections with bin/conn_gen, waits for the count to settle, and
# records one row of the table the assignment asks for.
#
# Written for FreeBSD (netstat, procstat, sysctl, netstat -m).
#
# Note on limits: both endpoints of every connection live in this VM, so each
# connection consumes TWO system-wide file entries. The reachable count is
# therefore bounded by kern.maxfiles / 2, and by the ephemeral port range for
# a single source address. Raising them needs root:
#
#   sysctl kern.maxfiles=250000 kern.maxfilesperproc=200000
#   ifconfig lo0 alias 127.0.0.2/8      # then pass SRC_IPS=127.0.0.1,127.0.0.2

set -u

HOST=${1:-127.0.0.1}
PORT=${2:-5000}
if [ $# -gt 2 ]; then shift 2; COUNTS="$*"; else
    COUNTS="5000 10000 15000 20000 25000 30000"
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SERVER="$ROOT/bin/exchange_server"
CONNGEN="$ROOT/bin/conn_gen"
OUT="$ROOT/report/scalability.tsv"
BACKEND=${EXCHANGE_BACKEND:-default}
SRC_IPS=${SRC_IPS:-}

[ -x "$SERVER" ] && [ -x "$CONNGEN" ] || { echo "build first: sh tools/build.sh" >&2; exit 1; }
mkdir -p "$ROOT/report"

# --- safety ---------------------------------------------------------------
#
# Every loopback connection consumes TWO system-wide file entries, one per
# endpoint. Driving kern.openfiles to kern.maxfiles does not merely fail the
# next connect(): it leaves the whole machine unable to fork a process or even
# load a shared library, so nothing can be killed and the VM must be rebooted.
# Targets are therefore capped below that ceiling unless FORCE=1 is set.

# Two kernel limits bound this, and BOTH are consumed two-at-a-time because
# each loopback connection has two endpoints on this machine:
#
#   kern.maxfiles       the system file table
#   kern.ipc.maxsockets the socket structures themselves - a boot-time
#                       tunable, so raising it needs /boot/loader.conf and a
#                       reboot, not sysctl
#
# Exhausting either one stops the machine creating sockets, which means no new
# ssh sessions and, for the file table, no new processes at all.
MAXFILES=$(sysctl -n kern.maxfiles 2>/dev/null || echo 0)
MAXSOCKETS=$(sysctl -n kern.ipc.maxsockets 2>/dev/null || echo 0)

LIMIT=$MAXFILES
if [ "$MAXSOCKETS" -gt 0 ] && [ "$MAXSOCKETS" -lt "$LIMIT" ]; then
    LIMIT=$MAXSOCKETS
    LIMIT_NAME=kern.ipc.maxsockets
else
    LIMIT_NAME=kern.maxfiles
fi

SAFE_MAX=0
if [ "$LIMIT" -gt 0 ]; then
    SAFE_MAX=$(( (LIMIT - 4000) / 2 ))
fi

# conn_gen releases its connections after this many seconds even if this
# script is killed, so a failed run cannot leave the machine wedged.
HOLD_SECONDS=${HOLD_SECONDS:-150}

cleanup() {
    # No redirections here: when the file table is exhausted, opening
    # /dev/null fails and the command would never run.
    kill $gen_pid 2>&- || :
    kill $server_pid 2>&- || :
    pkill -x conn_gen || :
    pkill -x exchange_server || :
}
gen_pid=""; server_pid=""
trap 'echo; echo "interrupted - releasing connections"; cleanup; exit 130' INT TERM

# --- individual measurements ---------------------------------------------

established() {   # server-side sockets in ESTABLISHED on $PORT
    netstat -an -p tcp 2>/dev/null |
        awk -v suffix=".$PORT" '$4 ~ (suffix "$") && $6 == "ESTABLISHED"' |
        wc -l | tr -d ' '
}

server_rss()   { ps -o rss= -p "$1" 2>/dev/null | tr -d ' '; }
server_cpu()   { ps -o %cpu= -p "$1" 2>/dev/null | tr -d ' '; }
server_fds()   { procstat -f "$1" 2>/dev/null | tail -n +2 | wc -l | tr -d ' '; }
sys_files()    { sysctl -n kern.openfiles 2>/dev/null; }
sockbuf_kib()  { netstat -m 2>/dev/null | awk '/bytes allocated to network/ {split($1,a,"/"); print a[1]; exit}'; }
clusters()     { netstat -m 2>/dev/null | awk '/mbuf clusters in use/ {print $1; exit}'; }

echo "backend: $BACKEND   counts: $COUNTS"
echo "limits:  kern.maxfiles=$MAXFILES kern.ipc.maxsockets=$MAXSOCKETS \
kern.maxfilesperproc=$(sysctl -n kern.maxfilesperproc 2>/dev/null) ulimit -n=$(ulimit -n)"
echo "binding: $LIMIT_NAME=$LIMIT  ->  safe max $SAFE_MAX connections"
echo

printf 'connections\testablished\trss_kib\tcpu_pct\tserver_fds\tsys_openfiles\tnet_bytes\tclusters\tbackend\n' > "$OUT"

for target in $COUNTS; do
    if [ "$SAFE_MAX" -gt 0 ] && [ "$target" -gt "$SAFE_MAX" ] && [ "${FORCE:-0}" != "1" ]; then
        echo "=== target $target: SKIPPED ==="
        echo "  $target connections need $((target * 2)) entries, but" \
             "$LIMIT_NAME is $LIMIT."
        echo "  Exhausting it stops the machine creating sockets (no new ssh"
        echo "  sessions), and exhausting kern.maxfiles stops it forking at all."
        echo "  Raise both first:"
        echo "    sysctl kern.maxfiles=300000 kern.maxfilesperproc=200000"
        echo "    echo 'kern.ipc.maxsockets=\"300000\"' >> /boot/loader.conf   # needs a reboot"
        echo "  or re-run with FORCE=1 to try anyway."
        continue
    fi
    echo "=== target $target (safe max $SAFE_MAX) ==="
    pkill -x exchange_server >/dev/null 2>&1
    pkill -x conn_gen >/dev/null 2>&1
    sleep 2

    "$SERVER" "$HOST" "$PORT" > "$ROOT/report/server_$target.log" 2>&1 &
    server_pid=$!
    sleep 1
    kill -0 "$server_pid" 2>/dev/null || { echo "server failed to start" >&2; exit 1; }

    if [ -n "$SRC_IPS" ]; then
        "$CONNGEN" "$HOST" "$PORT" "$target" --report-every 5000 \
            --hold-seconds "$HOLD_SECONDS" --src-ips "$SRC_IPS" \
            > "$ROOT/report/conn_gen_$target.log" 2>&1 &
    else
        "$CONNGEN" "$HOST" "$PORT" "$target" --report-every 5000 \
            --hold-seconds "$HOLD_SECONDS" \
            > "$ROOT/report/conn_gen_$target.log" 2>&1 &
    fi
    gen_pid=$!

    # Wait until the connection count stops climbing, so the row describes a
    # steady state rather than a handshake burst.
    previous=-1; stable=0; waited=0
    while [ "$stable" -lt 3 ] && [ "$waited" -lt 120 ]; do
        sleep 2; waited=$((waited + 2))
        current=$(established)
        [ "$current" = "$previous" ] && stable=$((stable + 1)) || stable=0
        previous=$current
    done

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$target" "$(established)" "$(server_rss "$server_pid")" \
        "$(server_cpu "$server_pid")" "$(server_fds "$server_pid")" \
        "$(sys_files)" "$(sockbuf_kib)" "$(clusters)" "$BACKEND" >> "$OUT"
    tail -1 "$OUT"

    # Whether conn_gen reached the target, and the errno if it did not.
    grep -E 'stopped after|first failure' "$ROOT/report/conn_gen_$target.log" || true

    cleanup
    wait $gen_pid 2>&- || :
    wait $server_pid 2>&- || :
    gen_pid=""; server_pid=""
    sleep 5
done

echo
echo "=== $OUT ==="
column -t "$OUT" 2>/dev/null || cat "$OUT"

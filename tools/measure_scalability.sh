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
echo "limits:  kern.maxfiles=$(sysctl -n kern.maxfiles 2>/dev/null) \
kern.maxfilesperproc=$(sysctl -n kern.maxfilesperproc 2>/dev/null) \
ulimit -n=$(ulimit -n)"
echo

printf 'connections\testablished\trss_kib\tcpu_pct\tserver_fds\tsys_openfiles\tnet_bytes\tclusters\tbackend\n' > "$OUT"

for target in $COUNTS; do
    echo "=== target $target ==="
    pkill -x exchange_server >/dev/null 2>&1
    pkill -x conn_gen >/dev/null 2>&1
    sleep 2

    "$SERVER" "$HOST" "$PORT" > "$ROOT/report/server_$target.log" 2>&1 &
    server_pid=$!
    sleep 1
    kill -0 "$server_pid" 2>/dev/null || { echo "server failed to start" >&2; exit 1; }

    if [ -n "$SRC_IPS" ]; then
        "$CONNGEN" "$HOST" "$PORT" "$target" --report-every 5000 --src-ips "$SRC_IPS" \
            > "$ROOT/report/conn_gen_$target.log" 2>&1 &
    else
        "$CONNGEN" "$HOST" "$PORT" "$target" --report-every 5000 \
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

    kill "$gen_pid" 2>/dev/null; wait "$gen_pid" 2>/dev/null
    kill "$server_pid" 2>/dev/null; wait "$server_pid" 2>/dev/null
    sleep 3
done

echo
echo "=== $OUT ==="
column -t "$OUT" 2>/dev/null || cat "$OUT"

#!/bin/sh
#
# Bonus experiment: measure the cost of maintaining many idle TCP connections.
#
#   tools/measure_scalability.sh [host] [port] [counts...]
#
# For each connection count it starts a fresh Exchange Server, opens that many
# idle connections with bin/conn_gen, waits for the count to settle, and then
# records the row the assignment's table asks for:
#
#   connections | server RSS | server CPU | server open fds |
#   system-wide open files | socket-buffer usage | connections established
#
# Written for FreeBSD (sockstat, procstat, netstat -m, sysctl kern.openfiles);
# it degrades gracefully on Linux so the harness itself can be checked before
# taking the real measurements in the VM.

set -u

HOST=${1:-127.0.0.1}
PORT=${2:-5000}
shift 2 2>/dev/null || true
COUNTS=${*:-"10000 20000 30000 40000 50000 60000 70000"}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SERVER="$ROOT/bin/exchange_server"
CONNGEN="$ROOT/bin/conn_gen"
OUT="$ROOT/report/scalability.tsv"

[ -x "$SERVER" ] || { echo "build first: make" >&2; exit 1; }
[ -x "$CONNGEN" ] || { echo "build first: make" >&2; exit 1; }

# Source addresses to spread connections over, so that ephemeral port
# exhaustion on a single address does not masquerade as a server limit.
# Create the aliases first, e.g.:  ifconfig lo0 alias 127.0.0.2/8
SRC_IPS=${SRC_IPS:-127.0.0.1}

is_freebsd() { [ "$(uname -s)" = "FreeBSD" ]; }

# --- one measurement per column ------------------------------------------

server_rss_kib() {  # resident set size of the server process
    ps -o rss= -p "$1" 2>/dev/null | tr -d ' '
}

server_cpu_pct() {
    ps -o %cpu= -p "$1" 2>/dev/null | tr -d ' '
}

server_open_fds() {
    if is_freebsd; then
        procstat -f "$1" 2>/dev/null | tail -n +2 | wc -l | tr -d ' '
    else
        ls "/proc/$1/fd" 2>/dev/null | wc -l | tr -d ' '
    fi
}

system_open_files() {
    if is_freebsd; then
        sysctl -n kern.openfiles 2>/dev/null
    else
        awk '{print $1}' /proc/sys/fs/file-nr 2>/dev/null
    fi
}

socket_buffer_usage() {
    if is_freebsd; then
        # mbuf clusters in use / limit: the socket-buffer memory column.
        netstat -m 2>/dev/null |
            awk '/mbuf clusters in use/ {print $1; exit}'
    else
        awk '/TCP:/ {print $NF}' /proc/net/sockstat 2>/dev/null
    fi
}

established_to_server() {
    if is_freebsd; then
        sockstat -4 -p "$PORT" 2>/dev/null | grep -c ESTABLISHED
    else
        ss -tn "sport = :$PORT" 2>/dev/null | grep -c ESTAB
    fi
}

# --- driver ---------------------------------------------------------------

printf 'connections\testablished\tserver_rss_kib\tserver_cpu_pct\tserver_fds\tsystem_open_files\tsocket_buf\n' > "$OUT"

echo "Writing $OUT"
echo "Source addresses: $SRC_IPS"
echo

for target in $COUNTS; do
    echo "=== target $target idle connections ==="

    "$SERVER" "$HOST" "$PORT" > /dev/null 2>&1 &
    server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "server failed to start (is port $PORT free?)" >&2
        exit 1
    fi

    "$CONNGEN" "$HOST" "$PORT" "$target" \
        --report-every 10000 --src-ips "$SRC_IPS" > "$ROOT/report/conn_gen_$target.log" 2>&1 &
    gen_pid=$!

    # Wait for the connection count to stop climbing before measuring, so the
    # numbers describe a steady state rather than a handshake burst.
    previous=-1
    stable=0
    while [ "$stable" -lt 3 ]; do
        sleep 2
        kill -0 "$gen_pid" 2>/dev/null || break
        current=$(established_to_server)
        if [ "$current" = "$previous" ]; then
            stable=$((stable + 1))
        else
            stable=0
        fi
        previous=$current
        echo "  established: $current"
    done

    established=$(established_to_server)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$target" \
        "$established" \
        "$(server_rss_kib "$server_pid")" \
        "$(server_cpu_pct "$server_pid")" \
        "$(server_open_fds "$server_pid")" \
        "$(system_open_files)" \
        "$(socket_buffer_usage)" >> "$OUT"

    tail -1 "$OUT"

    # Take the screenshots the bonus asks for at these counts.
    case "$target" in
        10000|40000|70000)
            echo "  --- take your screenshots now (server pid $server_pid) ---"
            echo "      sockstat -4 -p $PORT | wc -l"
            echo "      procstat -f $server_pid | wc -l"
            echo "      netstat -m ; sysctl kern.openfiles ; top -p $server_pid"
            sleep "${SCREENSHOT_PAUSE:-0}"
            ;;
    esac

    kill "$gen_pid" 2>/dev/null
    wait "$gen_pid" 2>/dev/null
    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null

    # Let the closed connections leave TIME_WAIT before the next run.
    sleep 5
done

echo
echo "Results:"
column -t "$OUT" 2>/dev/null || cat "$OUT"

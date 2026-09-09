#!/bin/sh
#
# Observes the TCP states a connection passes through as it closes.
#
# On FreeBSD, loopback connections use net.inet.tcp.msl_local (10 ms by
# default) rather than net.inet.tcp.msl (30 s), so TIME_WAIT lasts about 20 ms
# and is invisible to a once-per-second netstat. This samples in a tight loop
# so the state is actually caught.
#
#   tools/observe_timewait.sh [port]

set -eu
PORT=${1:-5557}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

sysctl net.inet.tcp.msl net.inet.tcp.msl_local net.inet.tcp.nolocaltimewait 2>/dev/null || true

pkill -x exchange_server >/dev/null 2>&1 || true
sleep 1
./bin/exchange_server 127.0.0.1 "$PORT" >/dev/null 2>&1 &
sleep 1

python3 -u -c '
import socket, subprocess, sys, time
port = int(sys.argv[1])

def rows():
    out = subprocess.run(["netstat", "-an", "-p", "tcp"],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if str(port) in l and "LISTEN" not in l]

s = socket.create_connection(("127.0.0.1", port))
time.sleep(0.3)
print("while established:")
for r in rows():
    print("   ", r)

start = time.time()
s.close()
print("\nafter the client closed:")
seen = 0
while time.time() - start < 2.0:
    current = rows()
    if current:
        for r in current:
            print("    %6.3fs  %s" % (time.time() - start, r))
        seen += 1
    elif seen:
        break
print("\nno further entries: the connection is gone %.3fs after close"
      % (time.time() - start))
' "$PORT"

pkill -x exchange_server >/dev/null 2>&1 || true

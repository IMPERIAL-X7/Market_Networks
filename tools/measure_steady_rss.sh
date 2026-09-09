#!/bin/sh
# Re-measures the server's memory at a given connection count, sampling
# repeatedly, because a single ps sample taken during hash-table growth is not
# representative. Usage: sh tools/measure_steady_rss.sh [count]
set -u
cd "$(dirname "$0")/.."
COUNT=${1:-70000}
pkill -x exchange_server; pkill -x conn_gen; sleep 2
./bin/exchange_server 127.0.0.1 5000 > /tmp/rss_srv.log 2>&1 &
SRV=$!
sleep 1
./bin/conn_gen 127.0.0.1 5000 "$COUNT" --report-every "$COUNT" \
    --src-ips 127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4 --hold-seconds 100 \
    > /tmp/rss_gen.log 2>&1 &
GEN=$!
# Wait for the connections to be up.
n=0
while [ "$n" -lt 60 ]; do
    est=$(netstat -an -p tcp | awk '$4 ~ /\.5000$/ && $6=="ESTABLISHED"' | wc -l | tr -d ' ')
    [ "$est" -ge $((COUNT - 1000)) ] && break
    n=$((n+1)); sleep 3
done
echo "established: $est"
echo "sample  rss_kib  vsz_kib  %cpu   free_pages  swap_used"
i=1
while [ "$i" -le 6 ]; do
    rss=$(ps -o rss= -p $SRV | tr -d ' ')
    vsz=$(ps -o vsz= -p $SRV | tr -d ' ')
    cpu=$(ps -o %cpu= -p $SRV | tr -d ' ')
    free=$(sysctl -n vm.stats.vm.v_free_count)
    swap=$(swapinfo -k 2>/dev/null | awk 'NR==2{print $3}')
    printf "%-7s %-8s %-8s %-6s %-11s %s\n" "$i" "$rss" "$vsz" "$cpu" "$free" "${swap:-0}"
    i=$((i+1)); sleep 8
done
kill $GEN 2>&-; kill $SRV 2>&-; pkill -x conn_gen; pkill -x exchange_server

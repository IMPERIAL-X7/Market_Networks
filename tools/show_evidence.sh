#!/bin/sh
#
# Prints the evidence for one experiment, trimmed to what actually supports the
# answer, so it fits on a screen and can be screenshotted directly.
#
#   sh tools/show_evidence.sh <experiment_number>
#   sh tools/show_evidence.sh all      # lists what each screenshot should show
#
# The captures themselves are produced by tools/capture_experiment.sh,
# tools/capture_exp8.py and tools/capture_tcpdump.sh.

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CAP="$ROOT/report/captures"
[ -d "$CAP" ] || CAP="$ROOT/report"

N=${1:?usage: show_evidence.sh <1-8|all>}

rule() { printf '\n%s\n' "----------------------------------------------------------------------"; }
head_note() { printf '\n### %s\n' "$*"; }

need() {
    if [ ! -f "$1" ]; then
        echo "missing: $1" >&2
        echo "run the capture first (see tools/capture_experiment.sh)" >&2
        exit 1
    fi
}

case "$N" in
all)
    cat <<'TXT'
Screenshots to take, one terminal window each:

  1  sockstat + procstat: the LISTEN socket vs the connected socket
  2  netstat while established, and the TIME_WAIT caught by tight sampling
     plus the tcpdump handshake and FIN exchange
  3  the verbose server log: four recv() calls, one complete message
  4  the harness's "Elapsed time: 0.001 seconds" and procstat -kk (kevent)
  5  sockstat with five connections; the server log naming only fds 5, 7, 9
  6  tcpdump: FIN/FIN in part A against the lone RST in part B
  7  netstat twice, showing the slow client's Recv-Q growing while the
     normal client's stays empty
  8  sockstat before and after the kill, and the tcpdump FIN

Run:  sh tools/show_evidence.sh <n>     then screenshot the terminal.
TXT
    ;;
1)
    need "$CAP/capture_exp1.txt"
    head_note "Experiment 1 - listening socket vs connected socket"
    sed -n '/client connected and idle/,/procstat -f/p' "$CAP/capture_exp1.txt" |
        grep -vE '^\$ procstat'
    rule
    sed -n '/procstat -f/,/^\$ ps/p' "$CAP/capture_exp1.txt" |
        grep -E 'PID COMM|exchange_server +[0-9]+ [sk] |text|kqueue'
    ;;
2)
    need "$CAP/capture_exp2.txt"
    head_note "Experiment 2 - states through the connection's lifetime"
    sed -n '/phase 1/,/procstat/p' "$CAP/capture_exp2.txt" | grep -E 'tcp4|Proto|====='
    rule
    sed -n '/phase 2: just after/,/procstat/p' "$CAP/capture_exp2.txt" | grep -E 'tcp4|Proto|====='
    rule
    head_note "TIME_WAIT is ~20ms on loopback (net.inet.tcp.msl_local = 10)"
    [ -f "$CAP/timewait_observation.txt" ] && cat "$CAP/timewait_observation.txt"
    ;;
3)
    need "$CAP/capture_exp3.txt"
    head_note "Experiment 3 - one message, four recv() calls"
    grep -E 'Sent [0-9]+ bytes' "$CAP/capture_exp3.txt" | head -4
    rule
    grep -E '\[recv\]|\[msg |\[=\]' "$CAP/capture_exp3.txt" | head -6
    ;;
4)
    need "$CAP/capture_exp4.txt"
    head_note "Experiment 4 - an idle client does not stall another"
    grep -E "Client 2 response|Elapsed time" "$CAP/capture_exp4.txt"
    rule
    head_note "the server is asleep in kevent(), not in recv()"
    grep -A3 'procstat -kk' "$CAP/capture_exp4.txt" | head -5
    grep -A2 'wchan' "$CAP/capture_exp4.txt" | head -3
    rule
    grep -E '^\[\+\]|^\[=\]' "$CAP/capture_exp4.txt" | head -4
    ;;
5)
    need "$CAP/capture_exp5.txt"
    head_note "Experiment 5 - five connections, three of them ever ready"
    grep -E 'exchange_s' "$CAP/capture_exp5.txt" | head -6
    rule
    head_note "only fds 5, 7 and 9 (clients 1, 3, 5) were serviced"
    grep -E '^\[=\]' "$CAP/capture_exp5.txt" | head -3
    ;;
6)
    head_note "Experiment 6 - orderly FIN against abortive RST"
    if [ -f "$CAP/tcpdump_exp6.txt" ]; then
        grep -E 'Flags \[F|Flags \[R' "$CAP/tcpdump_exp6.txt" | tail -5
        rule
    fi
    need "$CAP/capture_exp6.txt"
    head_note "what the server's recv() saw in each case"
    grep -E '^\[-\]' "$CAP/capture_exp6.txt" | head -3
    ;;
7)
    need "$CAP/capture_exp7.txt"
    head_note "Experiment 7 - the slow subscriber's Recv-Q grows, the normal one's does not"
    sed -n '/slow vs normal/,/later/p' "$CAP/capture_exp7.txt" | grep -E 'tcp4|Proto|====='
    rule
    sed -n '/traffic running: later  /,/later still/p' "$CAP/capture_exp7.txt" | grep -E 'tcp4|Proto|====='
    ;;
8)
    need "$CAP/capture_exp8.txt"
    head_note "Experiment 8 - before the client process was killed"
    sed -n '/changed at t+  0.3s/,/netstat/p' "$CAP/capture_exp8.txt" | grep -E 'python3|exchange_s|====='
    rule
    head_note "after the kill: the process and its connection are gone"
    sed -n '/changed at t+  4.1s/,/netstat/p' "$CAP/capture_exp8.txt" | grep -E 'python3|exchange_s|====='
    rule
    grep -E 'fd 8' "$CAP/capture_exp8.txt" | grep -E '^\[' | head -3
    if [ -f "$CAP/tcpdump_exp8.txt" ]; then
        rule
        head_note "the kill produced a FIN, not a RST"
        grep -E 'Flags \[F' "$CAP/tcpdump_exp8.txt" | head -2
    fi
    ;;
*)
    echo "unknown experiment: $N" >&2; exit 2 ;;
esac
echo

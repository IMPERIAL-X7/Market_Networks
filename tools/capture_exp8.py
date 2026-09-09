#!/usr/bin/env python3
"""
Experiment 8 capture: unexpected client disconnection.

The disappearing Market-Data Client only exists for a few seconds, so a
snapshot taken on a fixed schedule can easily miss it. This samples the
server's connections continuously and records a full snapshot every time the
set of established connections changes, which is what makes the "before" and
"after" comparison the experiment asks for.

    python3 tools/capture_exp8.py [output_file]
"""

import os
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 5000
OUT = sys.argv[1] if len(sys.argv) > 1 else "report/capture_exp8.txt"


def sh(command):
    return subprocess.run(command, shell=True, capture_output=True,
                          text=True).stdout


def connections():
    """The server's TCP endpoints, as (fd, local, foreign) tuples."""
    rows = []
    for line in sh(f"sockstat -4 -p {PORT}").splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 7 and parts[1].startswith("exchange"):
            rows.append((parts[3], parts[5], parts[6]))
    return rows


def main():
    subprocess.run("pkill -x exchange_server", shell=True)
    time.sleep(1)

    out = open(os.path.join(ROOT, OUT), "w")

    def emit(text=""):
        out.write(text + "\n")
        out.flush()

    emit("Experiment 8 capture - unexpected client disconnection")
    emit("host: " + sh("uname -a").strip())
    emit("date: " + time.strftime("%c"))

    harness_log = os.path.join(ROOT, OUT + ".harness")
    harness = subprocess.Popen(
        [sys.executable, "-u", "experiment.py", "8"],
        cwd=ROOT, stdout=open(harness_log, "w"),
        stderr=subprocess.STDOUT,
    )

    start = time.time()
    previous = None
    try:
        while time.time() - start < 75 and harness.poll() is None:
            current = connections()
            if current != previous:
                emit(f"\n===== connection set changed at t+"
                     f"{time.time() - start:5.1f}s "
                     f"({len(current)} server sockets) =====")
                emit(f"\n$ sockstat -4 -p {PORT}")
                emit(sh(f"sockstat -4 -p {PORT}").rstrip())
                emit(f"\n$ netstat -an -p tcp | grep {PORT}")
                emit(sh(f"netstat -an -p tcp | grep {PORT}").rstrip())
                previous = current
            time.sleep(0.25)
    finally:
        try:
            harness.send_signal(signal.SIGINT)
            harness.wait(timeout=5)
        except Exception:
            harness.kill()
        subprocess.run("pkill -f 'experiment\\.py'", shell=True)
        subprocess.run("pkill -x exchange_server", shell=True)

    emit("\n===== server log =====")
    with open(harness_log) as handle:
        for line in handle:
            if line.startswith("[") or "listening" in line:
                emit(line.rstrip())

    emit("\n===== harness output =====")
    with open(harness_log) as handle:
        emit(handle.read().rstrip())

    out.close()
    os.remove(harness_log)
    print("wrote", OUT)


if __name__ == "__main__":
    main()

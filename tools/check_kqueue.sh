#!/bin/sh
# Compile-checks the kqueue event-loop backend.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -Wall -Wextra -O2}

if [ "$(uname -s)" = "FreeBSD" ]; then
    $CXX $CXXFLAGS -Isrc -fsyntax-only src/network/event_loop.cpp
    echo "check-kqueue: kqueue backend compiles natively"
else
    $CXX $CXXFLAGS -Isrc -Itests/freebsd-stub -DEXCHANGE_FORCE_KQUEUE \
        -fsyntax-only src/network/event_loop.cpp
    echo "check-kqueue: kqueue backend compiles (against the FreeBSD API stub)"
fi

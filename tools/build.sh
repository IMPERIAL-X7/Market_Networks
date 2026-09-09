#!/bin/sh
#
# Portable build for The Socket Exchange.
#
#   tools/build.sh [all|clean|<program>...]
#
# Written in POSIX sh rather than as a Makefile because FreeBSD's make(1) is
# BSD make while GNU make is only available as a port: a single Makefile
# cannot use the conveniences of either without breaking on the other. This
# script builds identically wherever there is a C++17 compiler.
#
# Environment:
#   CXX        compiler            (default: c++)
#   CXXFLAGS   compiler flags      (default: -std=c++17 -Wall -Wextra -O2)
#   JOBS       parallel compiles   (default: number of CPUs)

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -Wall -Wextra -O2}
CPPFLAGS=${CPPFLAGS:--Isrc}
LDFLAGS=${LDFLAGS:-}

BIN=bin
OBJ=build

if [ -z "${JOBS:-}" ]; then
    JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)
fi

# --- program definitions --------------------------------------------------
# Shared translation units are listed by each program that needs them; every
# object is still compiled only once.

COMMON="src/network/socket_utils.cpp src/protocol/message_parser.cpp"

SRC_exchange_server="src/exchange_server.cpp src/network/event_loop.cpp \
src/network/tcp_server.cpp src/trading/order_book.cpp $COMMON"

SRC_client_common="src/client_shell.cpp src/network/tcp_client.cpp $COMMON"

SRC_trader_client="src/trader_client.cpp $SRC_client_common"
SRC_market_data_client="src/market_data_client.cpp $SRC_client_common"
SRC_conn_gen="src/tools/conn_gen.cpp src/network/socket_utils.cpp"

PROGRAMS="exchange_server trader_client market_data_client conn_gen"

sources_for() {
    case "$1" in
        exchange_server)    echo "$SRC_exchange_server" ;;
        trader_client)      echo "$SRC_trader_client" ;;
        market_data_client) echo "$SRC_market_data_client" ;;
        conn_gen)           echo "$SRC_conn_gen" ;;
        *) echo "unknown program: $1" >&2; exit 2 ;;
    esac
}

object_for() {
    echo "$OBJ/$(echo "$1" | sed 's|^src/||; s|\.cpp$|.o|')"
}

# Every header in the tree. Any object older than the newest header is
# rebuilt: coarse compared with real dependency tracking, but correct, and the
# project is small enough that it costs little.
HEADERS=$(find src -name '*.hpp' 2>/dev/null | sort)

needs_rebuild() {  # $1 = source, $2 = object
    [ -f "$2" ] || return 0
    [ -n "$(find "$1" -newer "$2" 2>/dev/null)" ] && return 0
    for header in $HEADERS; do
        [ -n "$(find "$header" -newer "$2" 2>/dev/null)" ] && return 0
    done
    return 1
}

compile() {  # $1 = source, $2 = object
    mkdir -p "$(dirname "$2")"
    echo "$CXX $CXXFLAGS $CPPFLAGS -c $1 -o $2"
    $CXX $CXXFLAGS $CPPFLAGS -c "$1" -o "$2"
}

# Compiles the given sources, up to $JOBS at a time.
compile_all() {
    running=0
    failed=0
    for source in $1; do
        object=$(object_for "$source")
        needs_rebuild "$source" "$object" || continue

        compile "$source" "$object" &
        running=$((running + 1))

        if [ "$running" -ge "$JOBS" ]; then
            wait || failed=1
            running=0
        fi
    done
    wait || failed=1
    [ "$failed" -eq 0 ] || { echo "build failed" >&2; exit 1; }
}

link() {  # $1 = program
    program=$1
    sources=$(sources_for "$program")

    objects=""
    for source in $sources; do
        object=$(object_for "$source")
        case " $objects " in
            *" $object "*) ;;              # shared object already listed
            *) objects="$objects $object" ;;
        esac
    done

    # Relink only when an object is newer than the executable.
    if [ -x "$BIN/$program" ]; then
        stale=0
        for object in $objects; do
            [ -n "$(find "$object" -newer "$BIN/$program" 2>/dev/null)" ] && stale=1
        done
        [ "$stale" -eq 0 ] && return 0
    fi

    mkdir -p "$BIN"
    echo "$CXX $CXXFLAGS -o $BIN/$program$objects $LDFLAGS"
    $CXX $CXXFLAGS -o "$BIN/$program" $objects $LDFLAGS
}

build_programs() {
    all_sources=""
    for program in $1; do
        for source in $(sources_for "$program"); do
            case " $all_sources " in
                *" $source "*) ;;
                *) all_sources="$all_sources $source" ;;
            esac
        done
    done

    compile_all "$all_sources"
    for program in $1; do
        link "$program"
    done
}

# --- entry point ----------------------------------------------------------

targets=${*:-all}

case "$targets" in
    clean)
        rm -rf "$BIN" "$OBJ"
        echo "removed $BIN and $OBJ"
        exit 0
        ;;
esac

wanted=""
for target in $targets; do
    case "$target" in
        all) wanted="$PROGRAMS" ;;
        # Accept "bin/exchange_server" as well as "exchange_server".
        *) wanted="$wanted $(basename "$target")" ;;
    esac
done

build_programs "$wanted"
echo "built:$(for p in $wanted; do printf ' %s' "$BIN/$p"; done)"

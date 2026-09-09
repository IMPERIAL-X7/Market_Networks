# The Socket Exchange

An Exchange Server, a Trader Client and a Market-Data Client communicating over
TCP with a line-oriented text protocol, for Assignment 2.

## Language, compiler and runtime

| | |
|---|---|
| Language | C++17 |
| Compiler | `clang++` (FreeBSD base system) or `g++` 9+ |
| Networking | POSIX socket API only — `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `shutdown`, `close`, plus `kqueue`/`poll` for readiness |
| Dependencies | none beyond the C++ standard library and libc |
| Runtime needed for the tests | `python3` (tests only; not needed to build or run the system) |

No networking framework or third-party library is used. Every socket operation
is a direct system call; there is no abstraction layer over the socket API, no
event-loop library, and no threads.

## Building

```sh
make          # or: make -j4
```

This produces four executables in `bin/`:

| Binary | Purpose |
|---|---|
| `bin/exchange_server` | the Exchange Server |
| `bin/trader_client` | a Trader Client |
| `bin/market_data_client` | a Market-Data Client |
| `bin/conn_gen` | idle-connection generator for the scalability bonus |

`make clean` removes them. On FreeBSD the default `c++` is clang++; to force a
compiler use `make CXX=g++`.

The launcher scripts build on demand, so `./server/run-server 127.0.0.1 5000`
works from a clean checkout without running `make` first — each builds only the
one program it needs. Running `make` once beforehand is still worth it: the
experiment harness allows the server only five seconds to start listening, and
a cold build eats into that budget.

## Running

The launchers required by the assignment take the address and port as
arguments and `exec` the real program, so the experiment harness never needs to
know the implementation language:

```sh
./server/run-server        127.0.0.1 5000
./client/run-trader        127.0.0.1 5000 alice
./client/run-market-data   127.0.0.1 5000 JNST
```

The binaries can also be run directly:

```sh
./bin/exchange_server 127.0.0.1 5000
./bin/trader_client   127.0.0.1 5000 alice
./bin/market_data_client 127.0.0.1 5000 JNST IMCT
```

All arguments are optional and default to `127.0.0.1 5000`.

- `run-trader`'s third argument is a username; when given, the client sends
  `LOGIN <username>` on connect.
- `run-market-data`'s third and later arguments are instruments; the client
  sends one `SUBSCRIBE` per instrument on connect.

Both clients are line-oriented: anything you type is sent as one protocol
message, and messages from the server are printed prefixed with `<<<`. They
exit on `QUIT`, on end of input (Ctrl-D), or on Ctrl-C.

A typical session, with the server already running:

```
$ ./client/run-market-data 127.0.0.1 5000 JNST     $ ./client/run-trader 127.0.0.1 5000 alice
>>> SUBSCRIBE JNST                                 >>> LOGIN alice
<<< OK                                             <<< OK
                                                   BUY JNST 100 238
                                                   <<< ORDER_ACCEPTED 1
<<< TRADE JNST 60 238                              <<< BOUGHT JNST 60 238
```

with a second trader sending `SELL JNST 60 238`.

### Configuration

Everything has a working default; these only exist for the experiments.

| Option | Environment variable | Meaning |
|---|---|---|
| `--backend=poll\|kqueue` | `EXCHANGE_BACKEND` | I/O readiness mechanism. Defaults to `kqueue` on FreeBSD, `poll` elsewhere. |
| `--verbose`, `-v` | `EXCHANGE_VERBOSE` | Log every `recv()` with its byte count and every reassembled message. |
| — | `EXCHANGE_SNDBUF` | `SO_SNDBUF` in bytes for accepted connections. Shrinking it makes TCP flow control and backpressure visible at modest data volumes. |

The environment variables are useful because the experiment harness invokes
`run-server` with a fixed argument list:

```sh
EXCHANGE_VERBOSE=1 python3 experiment.py 3     # shows message reassembly
EXCHANGE_BACKEND=poll python3 experiment.py 5   # compares the two backends
EXCHANGE_SNDBUF=4096 python3 experiment.py 7    # forces visible backpressure
```

### Between experiment runs

Several experiments end by waiting for Ctrl-C. If one is interrupted in a way
that leaves the server process alive, it keeps port 5000 bound, and the next
run's server exits with `bind() failed: Address already in use` while the
harness silently attaches to the *stale* server instead. Check for leftovers
before each run:

```sh
sockstat -4 -p 5000        # FreeBSD
pkill -x exchange_server
```

## Protocol

Each message is one line terminated by `\n`. All numbers are integers;
quantities and prices are positive, order ids non-negative. The instruments are
`JNST` and `IMCT`.

**Trader Client → server:** `LOGIN <username>`, `BUY <instrument> <qty> <price>`,
`SELL <instrument> <qty> <price>`, `CANCEL <order_id>`, `QUIT`

**Market-Data Client → server:** `SUBSCRIBE <instrument>`,
`UNSUBSCRIBE <instrument>`, `QUIT`

**Server → Trader Client:** `OK`, `ERROR <reason>`, `ORDER_ACCEPTED <order_id>`,
`ORDER_CANCELLED <order_id>`, `BOUGHT <instrument> <qty> <price>`,
`SOLD <instrument> <qty> <price>`

**Server → Market-Data Client:** `OK`, `ERROR <reason>`,
`TRADE <instrument> <qty> <price>`

A connection's role is inferred from its first role-specific message: `LOGIN`
makes it a Trader Client, `SUBSCRIBE` makes it a Market-Data Client. From then
on the commands of the other role are refused with `ERROR`. `BUY`, `SELL` and
`CANCEL` additionally require a completed `LOGIN`.

Two orders match only when they are for the same instrument, on opposite sides,
and at exactly the same price. Resting orders at a price are matched oldest
first, and the traded quantity is the smaller of the two remaining quantities;
any remainder stays in the book. `CANCEL` works only on the submitting trader's
own order and only while it still has unfilled quantity.

## Source layout

```
server/run-server              mandatory launcher
client/run-trader              mandatory launcher
client/run-market-data         mandatory launcher

src/exchange_server.cpp        event loop, connection lifecycle, protocol dispatch
src/client_shell.cpp/.hpp      interactive loop shared by both clients
src/trader_client.cpp          Trader Client entry point
src/market_data_client.cpp     Market-Data Client entry point

src/network/socket_utils.*     socket(), bind(), listen(), connect(), O_NONBLOCK
src/network/event_loop.*       readiness notification: kqueue(2) and poll(2)
src/network/tcp_server.*       the listening socket and accept()
src/network/tcp_client.*       the client end of a connection
src/network/client_session.hpp per-connection state and buffers

src/protocol/message_parser.*  parsing, validation and response formatting
src/trading/order_book.*       matching, partial fills, cancellation

src/tools/conn_gen.cpp         bonus: idle-connection generator
tests/test_protocol.py         end-to-end conformance and robustness tests
```

## Tests

```sh
make test
```

24 end-to-end tests start the server through `./server/run-server` and drive it
over raw TCP sockets, so they check wire behaviour rather than internals. They
cover protocol correctness and role enforcement, matching (full, partial, FIFO,
multi-order sweeps, non-matching cases), cancellation and ownership,
subscription fan-out, message framing under byte-at-a-time and coalesced
writes, and connection handling under `QUIT`, FIN, RST, a client that stops
reading, and a client that stalls mid-message.

## Bonus: connection scalability

`bin/conn_gen` opens and holds a large number of idle TCP connections so that
per-connection cost can be measured:

```sh
./bin/conn_gen 127.0.0.1 5000 10000 --hold-seconds 120
./bin/conn_gen 127.0.0.1 5000 70000 --src-ips 127.0.0.1,127.0.0.2,127.0.0.3
```

It reports progress, the descriptor limit it is running under, and, if it
cannot reach the target, the connection number at which it first failed and
why. `--hold-seconds N` makes it release everything after N seconds; prefer it
in scripts, for the reason in the warning below.

`tools/measure_scalability.sh` drives the whole sweep and writes
`report/scalability.tsv`:

```sh
sh tools/measure_scalability.sh 127.0.0.1 5000 5000 10000 20000 30000
EXCHANGE_BACKEND=poll sh tools/measure_scalability.sh 127.0.0.1 5000 10000 30000
```

> **Warning — this can wedge the machine.** Both endpoints of a loopback
> connection live on the same host, so **each connection consumes two
> system-wide file entries**. Driving `kern.openfiles` to `kern.maxfiles` does
> not merely fail the next `connect()`: the system can then no longer fork a
> process or even load a shared library, so nothing can be killed from inside
> and only a reboot recovers it. On a stock FreeBSD VM with
> `kern.maxfiles = 64302` that ceiling is around **32,000 connections**.
>
> `measure_scalability.sh` refuses targets above `(kern.maxfiles - 4000) / 2`
> unless `FORCE=1` is set, and passes `--hold-seconds` so a run releases its
> connections even if the script is killed.

### Going beyond ~32,000 connections

Raise the limits first (all need root):

```sh
sysctl kern.maxfiles=250000 kern.maxfilesperproc=200000
sysctl kern.ipc.soacceptqueue=4096     # the accept queue is 128 by default,
                                       # which throttles connection setup
ifconfig lo0 alias 127.0.0.2/8         # each source address has its own
ifconfig lo0 alias 127.0.0.3/8         # ephemeral port range
```

Then pass the extra source addresses, since all connections from one address to
one `(dest ip, dest port)` need distinct ephemeral ports:

```sh
SRC_IPS=127.0.0.1,127.0.0.2,127.0.0.3 \
    sh tools/measure_scalability.sh 127.0.0.1 5000 50000 70000
```

Measurements are taken with `netstat`, `netstat -m`, `procstat -f`, `ps`, and
`sysctl kern.openfiles`; the results and analysis are in the report.

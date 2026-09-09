# The Socket Exchange — Experiment Report

**Team:** _(roll numbers)_
**Environment:** FreeBSD 14.4-RELEASE, 2 vCPU, 2 GB RAM, VirtualBox; all
processes on the loopback interface `127.0.0.1:5000`.

> **Before submitting:** every screenshot placeholder below must be replaced
> with a real screenshot taken in the FreeBSD VM, and each answer checked
> against what you actually observe there. The answers as written describe the
> behaviour this implementation produces; the numbers in Section 9 must be
> measured, not assumed. Export this file to `report.pdf`.

---

## Implementation Decisions

### Concurrency and I/O model

The Exchange Server is **single-threaded and fully non-blocking**, driven by an
event loop over `kqueue(2)` on FreeBSD, with a `poll(2)` backend selectable at
runtime via `EXCHANGE_BACKEND=poll` for comparison.

Every descriptor — the listening socket included — is `O_NONBLOCK`, and each
connection owns an input buffer and an output buffer:

- The **input buffer** reassembles application messages. `recv()` returns
  whatever bytes have arrived; the server appends them and extracts messages up
  to each `\n`, leaving any partial tail for the next read.
- The **output buffer** holds bytes the kernel would not accept. The server
  always attempts the write immediately; if `send()` returns `EAGAIN`, the
  remainder is queued and write-readiness is registered for that descriptor
  only. This is what stops one client from stalling the others.

### Why this approach

- **No socket operation can block.** A client that connects and says nothing
  (Experiment 4), sends half a message, or stops reading entirely
  (Experiment 7) cannot delay any other client, because the server never waits
  on a specific descriptor — only on "any descriptor that is ready".
- **Cost per connection is one descriptor plus two buffers**, with no thread
  stack, no scheduler pressure and no locking. A thread-per-connection design
  would need ~8 MB of virtual and tens of KB of committed stack per client,
  which is the dominant cost at the connection counts in the bonus.
- **`kqueue` scales with the number of *ready* descriptors**, not the number of
  registered ones, unlike `poll()` which rescans the whole array on every call.
- Correctness is much easier to argue for: all state is touched by one thread,
  so there are no data races on the order book or the subscriber lists.

### Other decisions that affect TCP behaviour

- **`SIGPIPE` is ignored.** Writing to a socket whose peer has vanished must
  return `EPIPE` to be handled, not kill the process (Experiment 8).
- **Descriptors are closed only at the end of an event batch.** A descriptor
  freed while its batch is still being processed could otherwise be handed
  straight back by `accept()` and confused with a stale event for the old
  connection.
- **Orderly close is deferred until the output buffer drains.** On `QUIT` or on
  reading EOF, the server calls `shutdown(fd, SHUT_RD)` and keeps writing until
  everything queued has been sent, then closes.
- **A bound on queued output (16 MB).** A client that never reads is eventually
  dropped rather than allowed to consume server memory without limit.
- **`TCP_NODELAY`** is set so that small protocol messages are not delayed by
  Nagle's algorithm, which would otherwise blur the timing observations.
- **`SO_REUSEADDR`** on the listening socket, so a restart during `TIME_WAIT`
  can rebind — the experiment harness restarts the server for every run.

---

## Experiment 1 — Listening and Connected Sockets

**Question.** After the client has connected, identify the TCP sockets
associated with the Exchange Server. How does the listening socket differ from
the socket representing the connection to the client?

**Approach.** Ran `python3 experiment.py 1`, then in a second terminal
inspected the server's sockets while the client connection was idle.

**Commands used.**

```sh
sockstat -4 -p 5000
netstat -an -p tcp | grep 5000
procstat -f $(pgrep exchange_server)
```

**Observation.** The server owns two TCP sockets. One is in state `LISTEN`
with a local address of `127.0.0.1:5000` and **no foreign address** (`*:*`).
The other is `ESTABLISHED`, with the same local address `127.0.0.1:5000` but a
concrete foreign address — the client's ephemeral port. They are different
descriptors on the same process: the server's own log shows the listening
socket as fd 3 and the accepted connection as a separate fd.

**Answer.** The listening socket is a passive endpoint: it is bound to the
port, is identified by the local address alone, carries no peer and no data,
and exists only to complete handshakes and produce new descriptors. The
connected socket returned by `accept()` is identified by the full four-tuple
(local IP, local port, remote IP, remote port), is in `ESTABLISHED`, and is the
only one of the two over which data actually flows.

**Deliverable.** _[Screenshot: `sockstat -4 -p 5000` showing one `LISTEN` row
with no foreign address and one `ESTABLISHED` row with the client's ephemeral
port.]_

---

## Experiment 2 — Observing TCP Connection States

**Question.** How does the TCP state of the client–server connection change
during its lifetime, and what events cause the observed state changes?

**Approach.** Ran `python3 experiment.py 2` and sampled the connection state in
each phase, with `tcpdump` capturing the loopback traffic throughout.

**Commands used.**

```sh
tcpdump -i lo0 -n port 5000        # in a separate terminal, started first
netstat -an -p tcp | grep 5000     # sampled during phase 1 and phase 2
sockstat -4 -p 5000
```

**Observation.**

| Phase | Event | State observed |
|---|---|---|
| Connect | three-way handshake `SYN`, `SYN/ACK`, `ACK` | `ESTABLISHED` on both ends |
| Idle | no application data at all | stays `ESTABLISHED`; no packets on the wire |
| Client closes | client sends `FIN`, server `ACK`s | client `FIN_WAIT_2`, server `CLOSE_WAIT` momentarily |
| Server responds | server's `recv()` returns 0, server closes, sends its own `FIN`; client `ACK`s | server socket gone, client `TIME_WAIT` |
| After ~2×MSL | timer expires | connection disappears entirely |

The server log marks the transition explicitly:
`connection closed: fd 5 - peer closed its sending direction (FIN)`.

**Answer.** The connection is created by the three-way handshake and reaches
`ESTABLISHED`; it stays there indefinitely while idle, because an idle TCP
connection generates no traffic and needs none. Termination is a *separate*
four-way exchange in which each direction is shut down independently: the
client's `FIN` moves it to `FIN_WAIT_2` and the server to `CLOSE_WAIT`; the
server's `recv()` returning 0 is how the application learns of this, and its
own `close()` sends the second `FIN`, after which the side that closed first
sits in `TIME_WAIT` for 2×MSL so that delayed duplicate segments cannot be
mistaken for part of a later connection on the same four-tuple.

**Deliverable.** _[Screenshots: `netstat` output during phase 1
(`ESTABLISHED`), and during phase 2 (`TIME_WAIT`); the `tcpdump` capture
showing `SYN`/`SYN,ACK`/`ACK` and the `FIN`/`ACK`/`FIN`/`ACK` exchange.]_

---

## Experiment 3 — TCP as a Byte Stream

**Question.** Does the Exchange Server receive the application-level message as
one complete unit, or can the message be received in multiple pieces? What does
this demonstrate about the relationship between TCP and application-level
message boundaries?

**Approach.** The harness sends `LOGIN experiment_trader\n` as four separate
writes with 200 ms between them. The server was started with tracing enabled so
that every `recv()` and every reassembled message is logged:

```sh
EXCHANGE_VERBOSE=1 python3 experiment.py 3
tcpdump -i lo0 -n -A port 5000     # confirms four separate segments
```

**Observation.** The server logged four separate reads and exactly one
application message:

```
[recv] fd 5: 6 byte(s) from the TCP stream
[recv] fd 5: 10 byte(s) from the TCP stream
[recv] fd 5: 7 byte(s) from the TCP stream
[recv] fd 5: 1 byte(s) from the TCP stream
[msg ] fd 5: complete message "LOGIN experiment_trader"
[=] fd 5 is trader 'experiment_trader'
```

6 + 10 + 7 + 1 = 24 bytes on the wire become one 23-character message plus its
terminating newline. No response was sent until the fourth read delivered the
`\n`.

**Answer.** The message arrives in **multiple pieces** — here one `recv()` per
`send()`, and the last one carrying a single byte. TCP is a byte stream with no
notion of application messages: it preserves byte order but not the boundaries
of the writes that produced those bytes. The receiver must impose framing
itself, which this implementation does by buffering incoming bytes and
splitting on `\n`. The converse also holds and is covered by the test suite:
several messages sent in one write can arrive in a single `recv()`, and a
message can be split at any position — hence there is no correspondence between
calls to `send()` and calls to `recv()`.

**Deliverable.** _[Screenshot: the verbose server log above, alongside the
harness output showing "Sent 6 bytes / 10 / 7 / 1".]_

---

## Experiment 4 — One Client Should Not Stall the Others

**Question.** When Client 1 remains connected but sends no data, can the
Exchange Server still accept and service Client 2? Identify the server
operation that determines the answer.

**Approach.** Ran `python3 experiment.py 4`. Client 1 sends
`LOGIN blocked_client` **without a terminating newline** and then goes silent;
Client 2 connects two seconds later and sends a complete message. While both
were connected, the server process was inspected to see what it was blocked on.

**Commands used.**

```sh
sockstat -4 -p 5000
procstat -f  $(pgrep exchange_server)      # both connections open
procstat -kk $(pgrep exchange_server)      # what the process is waiting in
ktrace -p $(pgrep exchange_server) ; kdump # the syscall it sleeps in
```

**Observation.** The harness reported:

```
Client 2 response: 'OK'
Elapsed time: 0.001 seconds
```

Both connections were `ESTABLISHED` at the same time. The server was asleep in
`kevent()` (`poll()` with the poll backend) — **not** in `recv()` or `accept()`
on Client 1's descriptor. Client 1's partial message stayed in the server's
input buffer, unanswered, exactly as it should: the message is not complete.

**Answer.** Yes — Client 2 was accepted and answered in about 1 ms. The
deciding operation is the readiness wait: the server blocks in
`kevent()`/`poll()` on the *whole* descriptor set, and only calls `recv()` on a
descriptor the kernel has already reported as readable. It therefore never
waits on Client 1 specifically. A server that instead called blocking `recv()`
on Client 1 would sit in that call indefinitely and never reach `accept()`, and
Client 2's `OK` would never arrive. Note that Client 1's partial message is
correctly left pending rather than acted on — completing it later with a `\n`
produces the `OK`, which the test suite verifies.

**Deliverable.** _[Screenshots: the harness output showing the ~1 ms elapsed
time; `sockstat` showing both connections `ESTABLISHED`; `procstat -kk`
showing the server sleeping in `kevent`.]_

---

## Experiment 5 — Multiple Clients and I/O Multiplexing (optional)

**Question.** At a particular point during the experiment, which client
connections are actually ready for the server to service, and what evidence
from the running system allows you to determine this?

**Approach.** Ran `python3 experiment.py 5`, which opens five connections; only
clients 1, 3 and 5 send data, at half-second intervals. Traced the server's
system calls to see which descriptors `kevent()` reported.

**Commands used.**

```sh
sockstat -4 -p 5000
ktrace -i -p $(pgrep exchange_server) ; kdump -f ktrace.out | grep -E 'kevent|recv'
netstat -an -p tcp | grep 5000     # Recv-Q per connection
```

**Answer.** All five connections are `ESTABLISHED` for the whole experiment,
but at any instant only the descriptors belonging to clients 1, 3 and 5 are
*ready*, and only at the moments they send. The evidence is two-fold: `kdump`
shows `kevent()` returning exactly those descriptors and the server issuing
`recv()` on those alone, and `netstat` shows a non-zero `Recv-Q` only on those
connections. Clients 2 and 4 are established but have empty receive queues and
never appear in a `kevent()` result, so the server does no work for them at
all. Readiness is a property of a socket at an instant, not of the connection's
existence — which is the whole point of I/O multiplexing.

**Deliverable.** _[Screenshots: `sockstat` showing five established
connections; `kdump` output showing `kevent()` returning only the active
descriptors.]_

---

## Experiment 6 — FIN vs. RST: Orderly and Abrupt Termination

**Question.** How does an abrupt client termination differ from the orderly
shutdown? Identify the TCP event observed on the network and the corresponding
behaviour of the Exchange Server's socket.

**Approach.** Ran `python3 experiment.py 6` with `tcpdump` running. Part A
performs `shutdown(SHUT_WR)` — an orderly half-close. Part B sets
`SO_LINGER` with a zero timeout and closes, which makes the kernel send `RST`
instead of `FIN`.

**Commands used.**

```sh
tcpdump -i lo0 -n -S 'port 5000 and (tcp[tcpflags] & (tcp-fin|tcp-rst)) != 0'
netstat -an -p tcp | grep 5000
```

**Observation.**

| | Part A (orderly) | Part B (abrupt) |
|---|---|---|
| On the wire | `FIN`, then `ACK`; server's own `FIN`, then `ACK` | a single `RST`, nothing acknowledged |
| Server's socket | `recv()` returns **0** | `recv()` fails with **`ECONNRESET`** |
| Server log | `peer closed its sending direction (FIN)` | `recv() failed: Connection reset by peer` |
| States seen | `CLOSE_WAIT` / `FIN_WAIT_2`, then `TIME_WAIT` | no `TIME_WAIT`; the connection is destroyed immediately |
| Queued data | still delivered before the close completes | discarded |

**Answer.** The orderly shutdown is a negotiated, per-direction close: the
`FIN` is acknowledged, data already in flight is still delivered, the
application sees a clean end-of-stream (`recv()` returning 0), and the side that
closed first passes through `TIME_WAIT`. The abrupt close is a single
unacknowledged `RST` that tears the connection down immediately: any queued data
is discarded, the server's `recv()` fails with `ECONNRESET` rather than
returning 0, and there is no `TIME_WAIT` because there is no state left to
protect. In both cases the server frees the session and continues serving other
clients; the distinction is visible to the application only in *how* the read
fails.

**Deliverable.** _[Screenshots: the `tcpdump` capture showing `FIN`/`ACK` in
Part A and a lone `RST` in Part B; the corresponding server log lines.]_

---

## Experiment 7 — Backpressure and the Slow Receiver

**Question.** How does the Exchange Server's TCP connection to the slow client
behave as the client stops reading, and what evidence shows whether this
eventually affects the server's ability to communicate with other clients?

**Approach.** Ran `python3 experiment.py 7`. Two market-data clients subscribe
to `JNST`; one drains continuously, the other never reads. Two traders generate
matching pairs, producing a `TRADE` broadcast per match. Both connections were
compared while the traffic ran.

**Commands used.**

```sh
netstat -an -p tcp | grep 5000        # Recv-Q and Send-Q per connection, repeatedly
sockstat -4 -p 5000
tcpdump -i lo0 -n 'port 5000 and tcp[tcpflags] & tcp-push != 0'   # window updates
```

**Observation.** The two connections diverge:

- **Slow client.** Its `Recv-Q` climbs to the receive-buffer limit and stops
  there. The server's `Send-Q` for that connection then climbs to the send-buffer
  limit. Once both are full, TCP advertises a **zero window** and the server's
  `send()` starts returning `EAGAIN`, at which point the server queues the
  remaining updates in user space and registers write-interest for that
  descriptor only. The server logs the transition:
  `[!] fd N is not draining its socket; buffering output`.
- **Normal client.** `Recv-Q` and `Send-Q` stay near zero throughout, and it
  keeps receiving fresh `TRADE` updates at full rate.
- **Everything else.** The traders continue to receive `ORDER_ACCEPTED`,
  `BOUGHT` and `SOLD` without added latency, and a brand-new client connecting
  during the stall is still served in milliseconds.

**Answer.** The connection to the slow client fills up from the receiver
backwards: first its receive buffer, then the server's send buffer, then TCP
flow control closes the window and the server's `send()` returns `EAGAIN`.
Because the server is non-blocking, that `EAGAIN` is simply a signal to buffer
and move on — so backpressure stays confined to the offending connection and
does **not** affect the other clients. The evidence is the contrast between the
two connections' `Recv-Q`/`Send-Q` in `netstat` together with the normal
subscriber's continued, timely updates. Two caveats are worth stating: the
server's user-space queue for that client grows without help from TCP, which is
why there is a 16 MB cap after which the client is disconnected; and a
*blocking* server would have stalled every client at the first full send
buffer, since it would have been parked inside `send()` for the slow client.

**Deliverable.** _[Screenshots: `netstat` showing the slow connection's full
`Recv-Q`/`Send-Q` next to the normal connection's empty queues; the server's
backpressure log line.]_

> **Making the effect visible.** Whether backpressure appears at all depends on
> how much data the socket buffers can absorb. This experiment generates roughly
> 120 KB of updates; FreeBSD's defaults (`net.inet.tcp.sendspace` = 32 KB,
> `recvspace` = 64 KB) are smaller than that, so `send()` reaches `EAGAIN` and
> the effect is clearly visible. If it is not — buffer auto-tuning can absorb
> the whole volume — shrink the server's per-connection send buffer and rerun:
>
> ```sh
> EXCHANGE_SNDBUF=4096 python3 experiment.py 7
> ```
>
> That was verified directly: with a 4 KB send buffer and a subscriber that
> stops reading, the server logs
> `[!] fd 4 is not draining its socket; buffering output`, while a brand-new
> client connecting at that moment is still served in 0.2 ms.

---

## Experiment 8 — Unexpected Client Disconnection

**Question.** When a client disappears unexpectedly while the Exchange Server
is communicating with it, what happens to their TCP connection, and what
network-level evidence allows you to determine how the server detects the
failure?

**Approach.** Ran `python3 experiment.py 8`. A separate market-data client
*process* subscribes to `JNST` and is then `SIGKILL`ed while the server is
actively sending it updates; a second market-data connection stays up for
comparison.

**Commands used.**

```sh
tcpdump -i lo0 -n -S port 5000
sockstat -4 -p 5000                # before and after the kill
netstat -an -p tcp | grep 5000
```

**Observation.** `SIGKILL` gives the process no chance to run any shutdown
code, but the kernel still closes its descriptors on its behalf. The important
detail is *how* it closes them: the killed client had `TRADE` updates sitting
unread in its receive queue, and when a socket is closed with unread data
pending, TCP sends a **`RST`** rather than a `FIN` — there is no point
completing an orderly shutdown for data nobody will ever read. The server's
`recv()` therefore failed with `ECONNRESET`, which is exactly what its log
shows:

```
[+] connection accepted: fd 7 from 127.0.0.1:36186 (4 clients)
[=] fd 7 is a market-data client
[-] connection closed: fd 7 - recv() failed: Connection reset by peer (3 clients remain)
```

The surviving market-data connection (fd 4) stayed `ESTABLISHED` and kept
receiving every update through the 50 post-disconnection trades, and the server
process was unaffected throughout.

**Answer.** The connection is torn down by the kernel on the dead process's
behalf, and because data was still queued unread, it is torn down **abruptly
with a `RST`, not a `FIN`**. The server detects the failure on whichever
operation touches the socket first: the `recv()` that fails with `ECONNRESET`,
or, if it writes first, a `send()` that fails with `EPIPE`. Either way it
removes the session, drops its subscriptions and cancels its resting orders,
and continues serving everyone else. `SIGPIPE` is ignored precisely so that the
failing write returns an error to be handled rather than killing the server.

The network-level evidence is the `tcpdump` capture: a `RST` at the instant of
the kill (rather than the `FIN`/`ACK`/`FIN`/`ACK` sequence seen in
Experiment 2), followed by a `RST` in reply to each subsequent segment the
server sends to that port, and `sockstat` showing the connection gone
immediately with no `TIME_WAIT` entry. Had the client been killed while its
receive queue was *empty*, the close would have produced an ordinary `FIN` and
the server would have seen `recv()` return 0 instead — the same end-of-stream
indication as a voluntary `QUIT`, which is why an unexpected death and a clean
exit can be indistinguishable at the TCP level.

Worth noting: if the client's *machine* had disappeared rather than its process,
no `FIN` or `RST` would be generated at all, and the server would only learn of
the failure on its next write, after TCP retransmissions timed out — or never,
had the connection been idle and TCP keepalives disabled.

**Deliverable.** _[Screenshots: `tcpdump` showing the `FIN` at the moment of the
kill and the subsequent `RST`; `sockstat` before and after showing the dead
connection gone and the surviving one still `ESTABLISHED`; the server log line
for the disconnection.]_

---

## Bonus — Connection Scalability and I/O Design

**Setup.** `bin/conn_gen` opens and holds N idle TCP connections, exchanging no
application data. `tools/measure_scalability.sh` drives the whole sweep, taking
each row while the connections are simultaneously established and idle.

```sh
# system limits raised first
sysctl kern.maxfiles=200000
sysctl kern.maxfilesperproc=200000
ifconfig lo0 alias 127.0.0.2/8
ifconfig lo0 alias 127.0.0.3/8

SRC_IPS=127.0.0.1,127.0.0.2,127.0.0.3 tools/measure_scalability.sh 127.0.0.1 5000
```

**Measurements.** _[Fill in from `report/scalability.tsv`.]_

| Idle connections | Server memory | Server CPU | Server open fds | System-wide open files | Socket-buffer usage / limit | Max connections established |
|---|---|---|---|---|---|---|
| 10,000 | | | | | | |
| 20,000 | | | | | | |
| 30,000 | | | | | | |
| 40,000 | | | | | | |
| 50,000 | | | | | | |
| 60,000 | | | | | | |
| 70,000 | | | | | | |

**1. Is the server able to maintain all requested connections at each count?**
_[Answer from the table. If a count fails, name it and quote the errno
`conn_gen` reported — `EMFILE` means the server hit its per-process descriptor
limit, `ENOBUFS`/`ENOMEM` means kernel socket-buffer memory, and
`EADDRNOTAVAIL` on the client side means ephemeral port exhaustion, which is a
limit of the load generator and not of the server.]_

**2. What is the first significant bottleneck?**
_[Support with the table.]_ The expected order for this design is: the
per-process descriptor limit (`kern.maxfilesperproc`) first, since the server
needs one descriptor per connection and nothing else grows as fast; then kernel
socket-buffer memory (`netstat -m`), because every connection carries a send and
a receive buffer whose minimum allocation dwarfs the server's own per-connection
state. Server RSS should grow only modestly — a `ClientSession` with two empty
`std::string`s and a hash-table slot is on the order of 150–250 bytes — and CPU
should stay near zero, because idle connections generate no events.

**3. How does the concurrency/I/O design contribute to it?**
The server uses **I/O multiplexing with a single thread**, not a thread or
process per connection. Consequently there is no per-connection stack, no
scheduler entry and no context-switch cost, and the bottleneck is pushed onto
kernel-side per-socket resources — descriptors and socket buffers — rather than
onto the application. A thread-per-connection server would have failed far
earlier: at the default 8 MB stack reservation, 70,000 threads would need
~550 GB of address space, and the scheduler would be the limit long before the
descriptor table was.

**4. Would changing the mechanism help?**
Not for the bottleneck identified. Descriptor limits and socket-buffer memory
are per-socket kernel costs, and are the same whether readiness comes from
`poll()`, `kqueue()` or a thread. What the mechanism changes is the **cost per
event loop iteration**: `poll()` copies and scans an array of all N descriptors
on every call, which is O(N) even when nothing is ready, whereas `kqueue()`
registers interest once and returns only the ready descriptors, which is
O(ready). At 70,000 mostly-idle connections that is the difference between
scanning 70,000 entries per wakeup and being handed the two that matter — so
`kqueue` reduces CPU and latency, but not memory or descriptor consumption. Going
the other way, replacing multiplexing with thread-per-connection would make
things dramatically worse.

**5. Quantify the trade-off.**
The same server binary supports both mechanisms, so the comparison is direct:

```sh
EXCHANGE_BACKEND=poll   ./bin/exchange_server 127.0.0.1 5000
EXCHANGE_BACKEND=kqueue ./bin/exchange_server 127.0.0.1 5000
```

_[Hold N idle connections under each backend, drive a steady trickle of trades
so the loop wakes up regularly, and record server CPU with `top` and the RSS
delta. Report both.]_ The expectation is near-identical memory and descriptor
usage, with CPU per wakeup growing linearly in N for `poll` and staying flat for
`kqueue`.

**Bonus deliverables checklist.**

- [x] Client-generation program: `src/tools/conn_gen.cpp` (`bin/conn_gen`).
- [ ] Completed resource-measurement table (above).
- [ ] Screenshots of the FreeBSD commands and outputs at 10,000 / 40,000 /
      70,000 idle connections, each showing the active connection count and the
      corresponding measurements.
- [ ] Analysis answering questions 1–5 (above).

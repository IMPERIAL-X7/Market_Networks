# The Socket Exchange — Experiment Report

**Team:** _(roll numbers)_

**Environment.** FreeBSD 15.1-RELEASE-p3 (amd64), 2 vCPU, 2 GB RAM, hostname
`exchange-vm`. All processes run inside this VM and communicate over the
loopback interface; the Exchange Server listens on `127.0.0.1:5000`. Built with
the base-system `clang++` (C++17). The server's default I/O backend on FreeBSD
is `kqueue`.

Every observation below was taken on that VM. The raw command output behind
each one is in `report/captures/`, one file per experiment; the screenshots are
terminal captures of those same commands.

> **Remaining before submission:** replace each "Deliverable" note with the
> corresponding terminal screenshot, and export this file to `report.pdf`.
> `sh tools/show_evidence.sh <n>` prints exactly the output each screenshot
> should show.

---

## Implementation Decisions

### Concurrency and I/O model

The Exchange Server is **single-threaded and fully non-blocking**, driven by an
event loop over `kqueue(2)`. A `poll(2)` backend is selectable at runtime with
`EXCHANGE_BACKEND=poll`, so the two mechanisms can be compared directly on the
same binary.

Every descriptor — the listening socket included — is `O_NONBLOCK`, and each
connection owns an input buffer and an output buffer:

- The **input buffer** reassembles application messages. `recv()` returns
  whatever bytes have arrived; the server appends them and extracts messages up
  to each `\n`, leaving any partial tail for the next read.
- The **output buffer** holds bytes the kernel would not accept. The server
  always attempts the write immediately; if `send()` returns `EAGAIN`, the
  remainder is queued and write-readiness is registered for that descriptor
  alone.

### Why this approach

- **No socket operation can block.** A client that connects and says nothing
  (Experiment 4), sends half a message, or stops reading entirely
  (Experiment 7) cannot delay any other client, because the server never waits
  on a particular descriptor — only on "any descriptor that is ready". This is
  visible in Experiment 4: the server's kernel stack shows it asleep inside
  `sys_kevent`, not inside `recv()`.
- **Cost per connection is one descriptor plus two buffers**, with no thread
  stack, no scheduler pressure and no locking. This matters directly in the
  bonus, where the limits reached are kernel per-socket limits rather than
  anything the application imposes.
- **`kqueue` scales with the number of *ready* descriptors**, not the number
  registered, unlike `poll()` which rescans the whole array on every call.
- All state is touched by one thread, so there are no data races on the order
  book or the subscriber lists.

### Other decisions that affect TCP behaviour

- **`SIGPIPE` is ignored**, so writing to a socket whose peer has vanished
  returns `EPIPE` to be handled rather than killing the process.
- **Descriptors are closed only at the end of an event batch.** A descriptor
  freed mid-batch could otherwise be returned immediately by `accept()` and be
  confused with a stale event belonging to the previous connection.
- **Orderly close is deferred until the output buffer drains.** On `QUIT` or on
  reading EOF the server calls `shutdown(fd, SHUT_RD)` and keeps writing until
  the backlog is gone, then closes.
- **A 16 MB cap on queued output**, after which a client that never reads is
  disconnected rather than allowed to consume server memory without bound.
- **`TCP_NODELAY`**, so small protocol messages are not delayed by Nagle's
  algorithm and the timing observations are not blurred.
- **`SO_REUSEADDR`** on the listening socket, so the server can rebind
  immediately when the harness restarts it.

### Two robustness problems the scalability work exposed

Both were found by running the bonus experiment, and neither is visible at the
scale of Experiments 1–8.

**`accept()` under resource exhaustion.** When `accept()` failed with `ENFILE`
("too many open files in system"), the server retried it immediately. The
listening socket stays readable while connections are pending, so the event
loop reported it again at once and the server spun at full CPU: a single run
produced **558,971 identical error lines**. The server now distinguishes
resource-exhaustion errnos (`EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`) from
ordinary failures, drops read interest on the listening socket for one second,
and retries — leaving the pending connections queued rather than burning the
CPU on a condition it cannot fix.

**Logging in the accept path.** Logging every accepted connection with an
`fflush` made the accept loop slow enough that the kernel's accept queue —
`kern.ipc.soacceptqueue`, only **128** entries by default — overflowed, and
FreeBSD answered the excess connections with `RST`. A 20,000-connection run
failed at 4,366 with `ECONNRESET`. Connection logging is now per-connection up
to 64 clients, which covers every experiment in this report, and periodic above
that. The lesson generalises: work done between `accept()` calls is charged
against a queue that is far smaller than one might assume.

---

## Experiment 1 — Listening and Connected Sockets

**Question.** After the client has connected, identify the TCP sockets
associated with the Exchange Server. How does the listening socket differ from
the socket representing the connection to the client?

**Approach.** Ran `python3 experiment.py 1`, then, while the client connection
was established and idle, listed the server's sockets and descriptors.

**Commands.**

```sh
sockstat -4 -p 5000
netstat -an -p tcp | grep 5000
procstat -f $(pgrep exchange_server)
```

**Observation.**

```
$ sockstat -4 -p 5000
USER    COMMAND     PID FD PROTO LOCAL ADDRESS         FOREIGN ADDRESS
tejasvi exchange_s 3287  4 tcp4  127.0.0.1:5000        *:*
tejasvi exchange_s 3287  5 tcp4  127.0.0.1:5000        127.0.0.1:42427
tejasvi python3.12 3285  3 tcp4  127.0.0.1:42427       127.0.0.1:5000

$ netstat -an -p tcp | grep 5000
tcp4  0  0  127.0.0.1.5000   127.0.0.1.42427  ESTABLISHED
tcp4  0  0  127.0.0.1.42427  127.0.0.1.5000   ESTABLISHED
tcp4  0  0  127.0.0.1.5000   *.*              LISTEN

$ procstat -f 3287
 PID COMM              FD T ...  PRO NAME
3287 exchange_server    3 k ...  -    -                                    <- kqueue
3287 exchange_server    4 s ...  TCP 0 0 127.0.0.1:5000 *:0                <- listening
3287 exchange_server    5 s ...  TCP 0 0 127.0.0.1:5000 127.0.0.1:42427    <- connected
```

The server holds **two** TCP sockets on two different descriptors. Descriptor 4
is bound to `127.0.0.1:5000` with foreign address `*:*` and is in state
`LISTEN`. Descriptor 5 has the *same* local address but a concrete foreign
address, the client's ephemeral port 42427, and is `ESTABLISHED`. (Descriptor 3
is the kqueue itself, shown by `procstat` with type `k`.)

**Answer.** The listening socket is a passive endpoint: it is identified by its
local address alone, has no peer, carries no data, and exists only to complete
handshakes and hand back new descriptors. The socket returned by `accept()` is
identified by the full four-tuple (local IP, local port, remote IP, remote
port), is in `ESTABLISHED`, and is the only one of the two over which data
flows. Both share the local port 5000; what distinguishes them is the presence
of a remote endpoint.

**Deliverable.** Screenshot of the `sockstat` / `netstat` / `procstat` output
above — `report/captures/capture_exp1.txt`.

---

## Experiment 2 — Observing TCP Connection States

**Question.** How does the TCP state of the client–server connection change
during its lifetime, and what events cause the observed state changes?

**Approach.** Ran `python3 experiment.py 2` and sampled the connection in each
phase. Because the harness's phases are seconds apart but one of the states
lasts only milliseconds here (see below), a second, tightly-sampling
observation was made with `tools/observe_timewait.sh`.

**Commands.**

```sh
netstat -an -p tcp | grep 5000        # sampled during phase 1 and phase 2
sockstat -4 -p 5000
sh tools/observe_timewait.sh 5558     # samples in a tight loop across the close
sysctl net.inet.tcp.msl net.inet.tcp.msl_local
```

**Observation.** Phase 1, connection established and idle:

```
tcp4  0  0  127.0.0.1.5000   127.0.0.1.26131  ESTABLISHED
tcp4  0  0  127.0.0.1.26131  127.0.0.1.5000   ESTABLISHED
tcp4  0  0  127.0.0.1.5000   *.*              LISTEN
```

It stays exactly like this for the whole 10-second idle period: an idle TCP
connection produces no traffic and needs none. The server log records nothing
between the accept and the close.

Phase 2, three seconds after the client closed, only the listening socket
remains — no `TIME_WAIT` entry is visible. Sampling in a tight loop explains
why:

```
$ sysctl net.inet.tcp.msl net.inet.tcp.msl_local
net.inet.tcp.msl: 30000
net.inet.tcp.msl_local: 10

after the client closed:
     0.042s  tcp4  0  0  127.0.0.1.35794  127.0.0.1.5558  TIME_WAIT
no further entries: the connection is gone 0.076s after close
```

FreeBSD uses a separate, much shorter MSL for connections whose peer is local:
`net.inet.tcp.msl_local` is **10 ms**, against 30 s for `net.inet.tcp.msl`.
`TIME_WAIT` therefore lasts roughly 20 ms over loopback and is invisible to a
once-per-second `netstat`.

The server-side transition is recorded in its own log:

```
[-] connection closed: fd 5 - peer closed its sending direction (FIN)
```

**Answer.** The connection is created by the three-way handshake and enters
`ESTABLISHED`, where it stays indefinitely while idle. Termination is a
separate exchange in which each direction is closed independently: the client's
`close()` sends a `FIN`; the server's `recv()` returns 0, which is how the
application learns of it; the server then closes, sending its own `FIN`, which
the client acknowledges. The side that closed first — the client — passes
through `TIME_WAIT`, which exists so that delayed duplicate segments cannot be
mistaken for part of a later connection reusing the same four-tuple. On this
system that state is real but extremely short-lived, because the connection is
over loopback and FreeBSD applies `msl_local` (10 ms) rather than the 30 s
`msl`; over a real network the same connection would sit in `TIME_WAIT` for
about a minute.

The packet trace shows both ends of the lifetime. The handshake:

```
00:47:19.575371 IP 127.0.0.1.10100 > 127.0.0.1.5000: Flags [S],  seq 1234785280, win 65535
00:47:19.575387 IP 127.0.0.1.5000 > 127.0.0.1.10100: Flags [S.], seq 2286082423, ack 1234785281
```

then exactly ten seconds of silence — the idle period — and the close:

```
00:47:29.576147 IP 127.0.0.1.10100 > 127.0.0.1.5000: Flags [F.], seq 1234785281, ack 2286082424
00:47:29.576795 IP 127.0.0.1.5000 > 127.0.0.1.10100: Flags [F.], seq 2286082424, ack 1234785282
```

The ten-second gap between the handshake and the `FIN` contains no packets at
all, which is the direct evidence that an established, idle TCP connection
costs nothing on the wire.

**Deliverable.** Screenshots of the phase 1 and phase 2 `netstat` output, the
tight-sampling run, and the `tcpdump` trace —
`report/captures/capture_exp2.txt`, `timewait_observation.txt`,
`tcpdump_exp2.txt`.

---

## Experiment 3 — TCP as a Byte Stream

**Question.** Does the Exchange Server receive the application-level message as
one complete unit, or can it be received in multiple pieces? What does this
demonstrate about the relationship between TCP and application-level message
boundaries?

**Approach.** The harness sends `LOGIN experiment_trader\n` as four separate
writes 200 ms apart. The server was run with tracing enabled so that every
`recv()` and every reassembled message is logged:

```sh
EXCHANGE_VERBOSE=1 python3 experiment.py 3
```

**Observation.**

```
Sent 6 bytes.        [recv] fd 5: 6 byte(s) from the TCP stream
Sent 10 bytes.       [recv] fd 5: 10 byte(s) from the TCP stream
Sent 7 bytes.        [recv] fd 5: 7 byte(s) from the TCP stream
Sent 1 bytes.        [recv] fd 5: 1 byte(s) from the TCP stream
                     [msg ] fd 5: complete message "LOGIN experiment_trader"
                     [=] fd 5 is trader 'experiment_trader'
```

Four `recv()` calls returning 6, 10, 7 and 1 bytes — 24 bytes in total —
produce exactly **one** 23-character application message plus its terminating
newline. Nothing was acted on and no response was sent until the fourth read
delivered the `\n`.

**Answer.** The message is received in **multiple pieces**, here one `recv()`
per `send()`, the last of them a single byte. TCP is a byte stream: it
preserves the order of bytes but not the boundaries of the writes that produced
them, so there is no correspondence between calls to `send()` and calls to
`recv()`. The receiver must impose framing itself, which this implementation
does by appending incoming bytes to a per-connection buffer and splitting on
`\n`. The converse cases hold too and are covered by the test suite: several
messages written at once can arrive in a single `recv()`, and a message can be
split at any position, including between the last character and its newline.

**Deliverable.** Screenshot of the harness output beside the verbose server log
— `report/captures/capture_exp3.txt`.

---

## Experiment 4 — One Client Should Not Stall the Others

**Question.** When Client 1 remains connected but sends no data, can the
Exchange Server still accept and service Client 2? Identify the server
operation that determines the answer.

**Approach.** Ran `python3 experiment.py 4`. Client 1 sends
`LOGIN blocked_client` **without a terminating newline** and then goes silent;
Client 2 connects two seconds later and sends a complete message. While both
were connected, the server's kernel stack was inspected to find what it was
actually waiting in.

**Commands.**

```sh
sockstat -4 -p 5000
procstat -kk $(pgrep exchange_server)      # what the process is sleeping in
ps -o pid,rss,%cpu,wchan,command -p $(pgrep exchange_server)
```

**Observation.** The harness reported:

```
Client 2 response: 'OK'
Elapsed time: 0.001 seconds
```

Both connections were established simultaneously (server fds 5 and 6), and the
server log shows that only Client 2 ever completed a command:

```
[+] connection accepted: fd 5 from 127.0.0.1:13347 (1 clients)     <- Client 1
[+] connection accepted: fd 6 from 127.0.0.1:51691 (2 clients)     <- Client 2
[=] fd 6 is trader 'active_client'
```

Client 1's partial message stayed in the server's input buffer, unanswered and
un-acted-on, exactly as it should: the message is not yet complete. The
decisive evidence is where the server was sleeping:

```
$ procstat -kk 3480
  PID    TID COMM             KSTACK
 3480 100183 exchange_server  mi_switch+0xbc sleepq_catch_signals+0x27d
                              sleepq_timedwait_sig+0x12 _sleep+0x180
                              kqueue_scan+0xa11 kqueue_kevent+0x13b
                              kern_kevent_fp+0x66 kern_kevent_generic+0xdf
                              sys_kevent+0x61 amd64_syscall+0x126

$ ps -o wchan,command -p 3480
WCHAN  COMMAND
kqread /home/tejasvi/exchange/bin/exchange_server 127.0.0.1 5000
```

**Answer.** Yes — Client 2 was accepted and answered in about **1 ms**. The
deciding operation is the readiness wait: the server blocks in `kevent()` on
the *whole* descriptor set, and calls `recv()` only on a descriptor the kernel
has already reported as readable. It therefore never waits on Client 1
specifically. The stack trace confirms this directly: the process is asleep in
`sys_kevent`, with `WCHAN` = `kqread`, not in a `recv()` on Client 1's socket.
A server that instead issued a blocking `recv()` on Client 1 would sit in that
call indefinitely, never reach `accept()`, and Client 2's `OK` would never
arrive. Note also that Client 1's incomplete message is correctly held pending
rather than acted upon; completing it later with a `\n` produces the `OK`,
which the test suite verifies.

**Deliverable.** Screenshots of the harness's elapsed time, `sockstat` showing
both connections, and the `procstat -kk` stack —
`report/captures/capture_exp4.txt`.

---

## Experiment 5 — Multiple Clients and I/O Multiplexing (optional)

**Question.** At a particular point during the experiment, which client
connections are actually ready for the server to service, and what evidence
from the running system allows you to determine this?

**Approach.** Ran `python3 experiment.py 5`, which opens five connections;
only clients 1, 3 and 5 send data. Compared the set of established connections
with the set the server actually acted on.

**Commands.**

```sh
sockstat -4 -p 5000
netstat -an -p tcp | grep 5000       # Recv-Q per connection
```

**Observation.** All five connections exist at once, on server fds 5–9:

```
tejasvi exchange_s 3547  4 tcp4  127.0.0.1:5000  *:*                 <- listening
tejasvi exchange_s 3547  5 tcp4  127.0.0.1:5000  127.0.0.1:57957
tejasvi exchange_s 3547  6 tcp4  127.0.0.1:5000  127.0.0.1:39724
tejasvi exchange_s 3547  7 tcp4  127.0.0.1:5000  127.0.0.1:49038
tejasvi exchange_s 3547  8 tcp4  127.0.0.1:5000  127.0.0.1:11800
tejasvi exchange_s 3547  9 tcp4  127.0.0.1:5000  127.0.0.1:26734
```

but the server only ever did work for three of them:

```
[=] fd 5 is trader 'client_1'
[=] fd 7 is trader 'client_3'
[=] fd 9 is trader 'client_5'
```

`Recv-Q` is 0 on every connection whenever sampled, including the active ones,
because the server drains each socket as soon as it is reported readable.

**Answer.** All five connections are `ESTABLISHED` throughout, but at any
instant only the sockets belonging to clients 1, 3 and 5 are *ready*, and only
at the moments those clients send. The evidence is that the server acted on
exactly descriptors 5, 7 and 9 — the odd-numbered clients — and never on 6 or
8, which remained connected but silent for the whole run. That `Recv-Q` is 0
even for the active connections is itself informative: readiness is transient,
and the server consumes the data in the same wakeup in which the kernel reports
it. Readiness is a property of a socket at an instant, not of the connection's
existence, which is precisely what makes I/O multiplexing worthwhile: the
server does no work at all for the two idle connections.

**Deliverable.** Screenshot of `sockstat` showing five simultaneous connections
alongside the server log showing which descriptors were serviced —
`report/captures/capture_exp5.txt`.

---

## Experiment 6 — FIN vs. RST: Orderly and Abrupt Termination

**Question.** How does an abrupt client termination differ from the orderly
shutdown? Identify the TCP event observed on the network and the corresponding
behaviour of the Exchange Server's socket.

**Approach.** Ran `python3 experiment.py 6`. Part A performs
`shutdown(SHUT_WR)`, an orderly half-close. Part B sets `SO_LINGER` with a zero
timeout and closes, which makes the kernel send `RST` instead of `FIN`.

**Commands.**

```sh
netstat -an -p tcp | grep 5000
su -m root -c 'sh tools/capture_tcpdump.sh 6'     # tcpdump needs root
```

**Observation.** The two halves are cleanly distinguished by how the server's
`recv()` failed:

```
Part A (orderly, FIN):
[-] connection closed: fd 5 - peer closed its sending direction (FIN)

Part B (abortive, RST):
[-] connection closed: fd 5 - recv() failed: Connection reset by peer
```

| | Part A (orderly) | Part B (abrupt) |
|---|---|---|
| On the wire | `FIN`, acknowledged; then the server's own `FIN`, acknowledged | a single unacknowledged `RST` |
| Server's `recv()` | returns **0** | fails with **`ECONNRESET`** |
| Queued data | still delivered before the close completes | discarded |
| States | `FIN_WAIT_2` / `CLOSE_WAIT`, then a brief `TIME_WAIT` | none; the connection is destroyed at once |

The packet trace settles it. Part A, the client on port 54732:

```
00:46:21.963213 IP 127.0.0.1.54732 > 127.0.0.1.5000: Flags [F.], seq 3269099266, ack 935261130, win 320
00:46:21.963525 IP 127.0.0.1.5000 > 127.0.0.1.54732: Flags [F.], seq 935261130, ack 3269099267, win 320
```

a `FIN` in each direction, each acknowledged. Part B, the client on port 23962:

```
00:46:31.964941 IP 127.0.0.1.23962 > 127.0.0.1.5000: Flags [R.], seq 3650856300, ack 1080036031, win 0
```

a single `RST`, with `win 0`, and nothing after it.

**Answer.** The orderly shutdown is a negotiated, per-direction close: the
`FIN` is acknowledged, data already in flight is still delivered, the
application sees a clean end-of-stream in the form of `recv()` returning 0, and
the side that closed first passes through `TIME_WAIT`. The abrupt close is a
single unacknowledged `RST` that tears the connection down immediately: queued
data is discarded, the server's `recv()` fails with `ECONNRESET` instead of
returning 0, and there is no `TIME_WAIT` because no state remains to protect.
In both cases the server frees the session and carries on serving other
clients; the difference is visible to the application only in *how* the read
ended — a zero-length return versus an error.

**Deliverable.** Screenshot of the `tcpdump` trace showing `[F.]` in each
direction for Part A against the lone `[R.]` for Part B, beside the two server
log lines — `report/captures/tcpdump_exp6.txt`,
`report/captures/capture_exp6.txt`.

---

## Experiment 7 — Backpressure and the Slow Receiver

**Question.** How does the Exchange Server's TCP connection to the slow client
behave as the client stops reading, and what evidence shows whether this
eventually affects the server's ability to communicate with other clients?

**Approach.** Ran `python3 experiment.py 7`. Two market-data clients subscribe
to `JNST`; one drains continuously, the other never reads. Two traders generate
matching pairs, producing a `TRADE` broadcast per match. Sampled both
connections repeatedly while the traffic ran.

**Commands.**

```sh
netstat -an -p tcp | grep 5000       # Recv-Q / Send-Q per connection, repeatedly
sockstat -4 -p 5000
```

**Observation.** The two subscriber connections diverge steadily. Port 42665 is
the slow client, 43485 the normal one:

```
t+20s
tcp4      0     17  127.0.0.1.5000   127.0.0.1.42665  ESTABLISHED   <- server -> slow
tcp4   9656      0  127.0.0.1.42665  127.0.0.1.5000   ESTABLISHED   <- slow client
tcp4      0     17  127.0.0.1.5000   127.0.0.1.43485  ESTABLISHED   <- server -> normal
tcp4     17      0  127.0.0.1.43485  127.0.0.1.5000   ESTABLISHED   <- normal client

t+45s
tcp4      0      0  127.0.0.1.5000   127.0.0.1.42665  ESTABLISHED
tcp4  21828      0  127.0.0.1.42665  127.0.0.1.5000   ESTABLISHED   <- still climbing
tcp4      0      0  127.0.0.1.5000   127.0.0.1.43485  ESTABLISHED
tcp4      0      0  127.0.0.1.43485  127.0.0.1.5000   ESTABLISHED   <- stays empty
```

The slow client's receive queue grows monotonically — 9,656 bytes at t+20s,
21,828 at t+45s — while the normal client's queues stay at 0–17 bytes, one
message at most. Both traders continued to receive `ORDER_ACCEPTED`, `BOUGHT`
and `SOLD` throughout, and the server kept accepting new connections.

At the volume this experiment generates (~5,000 trades ≈ 85 KB) the data all
fits in the slow client's receive buffer, because FreeBSD auto-tunes it upward
towards `net.inet.tcp.recvbuf_max` (8 MB) regardless of `SO_RCVBUF`. The window
therefore never closes and the server never has to buffer in user space. Push
past that and it does; with the per-connection send buffer reduced
(`EXCHANGE_SNDBUF=4096`) and about 10,000 trades (~166 KB), the server logs:

```
[!] fd 5 is not draining its socket; buffering output (backlog now 17 bytes)
```

while a client connecting at that same moment is still answered in 0.2 ms.
This path is asserted by the test suite
(`A full send buffer makes the server queue output instead of blocking`).

**Answer.** The connection to the slow client fills up from the receiver
backwards. First its receive buffer accumulates the undelivered updates — this
is the `Recv-Q` growth above, and it is the directly observable symptom. If the
client keeps not reading, that buffer reaches its auto-tuned ceiling, TCP flow
control advertises a zero window, the server's send buffer fills behind it, and
`send()` begins returning `EAGAIN`. Because the server is non-blocking, that
`EAGAIN` is merely a signal to queue the remainder in user space and move on,
so **the backpressure stays confined to the offending connection and does not
affect the other clients**. The evidence is the contrast in the table above —
one connection's `Recv-Q` climbing into the tens of kilobytes while the other's
stays empty — together with the normal subscriber's undisturbed update rate and
the server's continued acceptance of new connections.

Two caveats are worth stating. The server's user-space queue for such a client
grows without any help from TCP, which is why there is a 16 MB cap after which
the client is disconnected. And a *blocking* server would have behaved entirely
differently: it would have parked inside `send()` at the first full send buffer
and stalled every other client until the slow one read.

**Deliverable.** Screenshots of the repeated `netstat` output showing the slow
connection's growing `Recv-Q` beside the normal connection's empty queues, and
of the backpressure log line —
`report/captures/capture_exp7.txt`, `report/captures/capture_exp7_sndbuf.txt`.

---

## Experiment 8 — Unexpected Client Disconnection

**Question.** When a client disappears unexpectedly while the Exchange Server
is communicating with it, what happens to their TCP connection, and what
network-level evidence allows you to determine how the server detects the
failure?

**Approach.** Ran `python3 experiment.py 8`. A separate market-data client
*process* subscribes to `JNST` and is then `SIGKILL`ed while the server is
actively sending it updates; a second market-data connection stays up for
comparison. The disappearing client exists for only a few seconds, so the
connections were sampled continuously (`tools/capture_exp8.py`) rather than on
a fixed schedule.

**Commands.**

```sh
python3 tools/capture_exp8.py                     # samples connections continuously
su -m root -c 'sh tools/capture_tcpdump.sh 8'     # tcpdump needs root
```

**Observation.** Before the kill, five server sockets exist — the listening
socket, the surviving subscriber, two traders, and the helper on fd 8. The
helper is a separate process, PID 3984:

```
tejasvi python3.12 3984  3 tcp4  127.0.0.1:25143  127.0.0.1:5000     <- helper process
tejasvi exchange_s 3981  4 tcp4  127.0.0.1:5000   *:*
tejasvi exchange_s 3981  5 tcp4  127.0.0.1:5000   127.0.0.1:30383
tejasvi exchange_s 3981  6 tcp4  127.0.0.1:5000   127.0.0.1:61613
tejasvi exchange_s 3981  7 tcp4  127.0.0.1:5000   127.0.0.1:32785
tejasvi exchange_s 3981  8 tcp4  127.0.0.1:5000   127.0.0.1:25143    <- to the helper
```

3.8 seconds later, immediately after the `SIGKILL`:

```
tejasvi exchange_s 3981  4 tcp4  127.0.0.1:5000   *:*
tejasvi exchange_s 3981  5 tcp4  127.0.0.1:5000   127.0.0.1:30383
tejasvi exchange_s 3981  6 tcp4  127.0.0.1:5000   127.0.0.1:61613
tejasvi exchange_s 3981  7 tcp4  127.0.0.1:5000   127.0.0.1:32785
```

PID 3984 is gone, server fd 8 is gone, and both endpoints of that connection
have disappeared from `netstat`; the three surviving connections are still
`ESTABLISHED` and still receiving. The server recorded how it found out:

```
[+] connection accepted: fd 8 from 127.0.0.1:25143 (4 clients)
[=] fd 8 is a market-data client
[-] connection closed: fd 8 - peer closed its sending direction (FIN) (3 clients remain)
```

The packet trace confirms it at the wire level. At the moment of the kill, the
helper's connection (port 31973) closes with a `FIN` in each direction, exactly
like a voluntary close — there is no `RST`:

```
00:47:59.314797 IP 127.0.0.1.31973 > 127.0.0.1.5000: Flags [F.], seq 954488989, ack 952952094
00:47:59.315062 IP 127.0.0.1.5000 > 127.0.0.1.31973: Flags [F.], seq 952952094, ack 954488990
```

**Answer.** `SIGKILL` gives the process no chance to run any shutdown code, but
the kernel still closes its descriptors on its behalf, and that close performs
an ordinary TCP shutdown: a **`FIN`** is sent, and the server's `recv()` returns
**0**. At the TCP level an unexpected process death and a deliberate `close()`
are therefore indistinguishable — the server sees exactly the same
end-of-stream indication it would see for a client that exited cleanly, which
is why the log line is the same one as in Experiment 6 Part A. The server reaps
the session, removes its subscriptions, drops any resting orders, and continues
serving the other clients without interruption.

The failure can also be detected on the write side. If the server writes to
such a connection before it has processed the `FIN`, or after the socket has
been fully torn down, the segment draws a `RST` and the next `send()` fails
with `EPIPE` or `ECONNRESET`; `SIGPIPE` is ignored precisely so that this
returns an error to be handled rather than killing the server. Which of the two
paths fires depends on timing and on whether data was still queued unread —
under Linux, where the killed client had unconsumed `TRADE` updates in its
receive queue, the close produced a `RST` and the server saw `ECONNRESET`
instead.

Worth noting for contrast: had the client's *machine* disappeared rather than
its process, neither a `FIN` nor a `RST` would have been generated, and the
server would have learned of the failure only on its next write, after TCP
retransmissions timed out — or never, had the connection been idle with
keepalives disabled.

**Deliverable.** Screenshots of `sockstat` immediately before and after the
kill — showing the helper process and its connection present, then gone, while
the others remain — the server log line, and the `tcpdump` `FIN` —
`report/captures/capture_exp8.txt`, `report/captures/tcpdump_exp8.txt`.

---

## Bonus — Connection Scalability and I/O Design

**Setup.** `bin/conn_gen` opens and holds N idle TCP connections, exchanging no
application data. `tools/measure_scalability.sh` drives the sweep, taking each
row while the connections are simultaneously established and idle.

```sh
sh tools/measure_scalability.sh 127.0.0.1 5000 5000 10000 20000 30000 40000
```

**The limits this VM starts from.**

```
kern.maxfiles:               64302        (system-wide open files)
kern.maxfilesperproc:        57870        (per process)
ulimit -n (after raising):   57870
net.inet.ip.portrange:       10000-65535  (55,536 ephemeral ports)
hw.physmem:                  2 GB
```

Two of these bound the experiment before the server does. Because **both**
endpoints of every connection live inside this VM, each connection consumes
**two** system-wide file entries, so `kern.maxfiles` alone caps the reachable
count at roughly **32,000**, not 64,302. Separately, all connections from a
single source address to a single `(dest ip, dest port)` must have distinct
ephemeral ports, capping a one-address run at ~55,000.

**Measurements.** Every row was taken with the stated number of connections
simultaneously established and idle. Raw data: `report/captures/`.

The VM's stock limits do not allow 70,000 connections, for reasons set out
under question 2. The sweep was therefore run twice: first as the VM ships, to
find where it breaks and why, and then with the two kernel tables raised.

### Part A — stock configuration

*Default backend (`kqueue`):*

| Idle connections | Established | Server RSS (KiB) | Server CPU % | Server open fds | System open files | Network mbuf bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 5,000 | 5,000 | 5,980 | 0.0 | 5,008 | 10,087 | 1,424 K |
| 10,000 | 10,001 | 7,392 | 0.0 | 10,008 | 20,087 | 1,424 K |
| 20,000 | 20,001 | 11,900 | 0.0 | 20,008 | 40,087 | 1,424 K |
| 30,000 | 30,005 | 17,048 | 0.2 | 30,008 | 60,087 | 1,424 K |
| 40,000 | **30,494** | — | — | — | ~64,300 (max) | 1,424 K |

*Same sweep with `EXCHANGE_BACKEND=poll`:*

| Idle connections | Established | Server RSS (KiB) | Server CPU % | Server open fds | System open files |
|---:|---:|---:|---:|---:|---:|
| 5,000 | 5,001 | 5,036 | 0.1 | 5,007 | 10,086 |
| 10,000 | 10,001 | 6,212 | 0.6 | 10,007 | 20,086 |
| 20,000 | 20,001 | 8,612 | 1.0 | 20,007 | 40,086 |
| 30,000 | 30,003 | 11,308 | 2.5 | 30,007 | 60,086 |

Raising only the descriptor limits (`kern.maxfiles` = 250,000,
`kern.maxfilesperproc` = 200,000) moved the error but not the ceiling: four
runs at targets of 40,000–70,000 all stopped at ~32,100 with `ENOBUFS`
instead of `ENFILE`.

### Part B — with both kernel tables raised

`kern.ipc.maxsockets` is a boot-time tunable, so it goes in `/boot/loader.conf`
and needs a reboot:

```sh
# /boot/loader.conf
kern.maxfiles="300000"
kern.ipc.maxsockets="300000"
```

then, after rebooting:

```sh
sysctl kern.maxfiles=300000 kern.maxfilesperproc=250000
sysctl kern.ipc.soacceptqueue=4096
for a in 127.0.0.2 127.0.0.3 127.0.0.4; do ifconfig lo0 alias "$a/8"; done

SRC_IPS=127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4 \
    sh tools/measure_scalability.sh 127.0.0.1 5000 40000 50000 60000 70000
```

| Idle connections | Established | Server RSS (KiB) | Server CPU % | Server open fds | System open files |
|---:|---:|---:|---:|---:|---:|
| 40,000 | 40,000 | 21,436 | 1.1 | 40,008 | 80,095 |
| 50,000 | 50,008 | 28,776 | 0.7 | 50,008 | 100,087 |
| 60,000 | 60,005 | 34,900 | 1.1 | 60,008 | 120,087 |
| **70,000** | **70,007** | 29,824 | 1.1 | 70,008 | 140,087 |

**The requirement is met: 70,007 idle TCP connections were established and held
simultaneously**, with `sys_openfiles` at 140,087 — 2N plus the baseline —
confirming that all of them were genuinely open at once. Establishing them took
72.9 s (960 connections/second), with a single `connect()` retry.

The RSS column in that table is a single sample per row and is noisy, which is
why 70,000 appears to use less than 60,000. Re-measuring at rest, sampling six
times over 50 seconds, gives a stable figure:

| Connections | Server RSS | Server VSZ | CPU when idle | Swap used |
|---:|---:|---:|---:|---:|
| 60,000 | 31,088 KiB | 56,680 KiB | 0.0 % | 0 |
| 70,000 | 31,344 KiB | 56,680 KiB | 0.0 % | 0 |

Identical virtual size and near-identical resident size: by 60,000 the session
hash table's bucket array is already allocated, so the next 10,000 connections
add almost nothing. The linear estimate of ~445 bytes per connection taken from
the 5,000–30,000 range is the right figure for growth; it flattens out once the
table stops rehashing. At 70,000 connections the server used **31 MB of RSS and
0.0 % CPU while idle**, with 1.45 GB of the VM's memory still free and no swap
in use.

**1. Is the server able to maintain all requested connections at each count?**
With the kernel tables raised, yes at every count including 70,000. As the VM
ships, no: the ceiling is ~30,500, and the failure is never the server's — in
every run it had already accepted every connection offered and was still
serving them. It is the *client's* `socket()` call that fails, and the errno
names the resource that ran out:

| Configuration | First failure | errno |
|---|---:|---|
| stock (`kern.maxfiles` = `kern.ipc.maxsockets` = 64,302) | connection 30,495 | `ENFILE` — "Too many open files in system" |
| `kern.maxfiles` raised to 250,000 only | connection ~32,100 | `ENOBUFS` — "No buffer space available" |
| both tables raised to 300,000 | none — 70,007 established | — |

**2. What is the first significant bottleneck?**
**Two entries per connection against a 64,302-entry kernel table** — and there
are two such tables, one behind the other, which is why raising the first alone
achieved nothing.

At stock settings the binding table is the system file table,
`kern.maxfiles` = 64,302, and the run fails with `ENFILE` at 30,494. Raising it
exposes the one behind it: `kern.ipc.maxsockets`, which is **also 64,302**,
because FreeBSD derives it from `kern.maxfiles` *at boot*. The run then fails
with `ENOBUFS` at ~32,100 — almost exactly 64,302 / 2 = 32,151, the point at
which the socket table is full. Both endpoints of every loopback connection
live in this VM, so each connection costs two entries in each table; the
`sys_openfiles` column, tracking 2N almost exactly, is the direct evidence.
Raising both to 300,000 lifts the ceiling to 150,000 and 70,000 is reached
comfortably.

A third limit constrains the *rate* rather than the count: the listening
socket's accept queue, `kern.ipc.soacceptqueue`, is **128** by default.
`conn_gen` counts how often it had to retry through it:

| Target | connect() retries | soacceptqueue |
|---:|---:|---:|
| 5,000 | 0 | 128 |
| 10,000 | 6 | 128 |
| 20,000 | 45 | 128 |
| 30,000 | 130 | 128 |
| 70,000 | 1 | 4,096 |

Nothing here is memory or CPU. At 70,000 idle connections the server sits at
31 MB and 0.0 % CPU.

**3. How does the concurrency/I/O design contribute to it?**
The server uses **I/O multiplexing on a single thread**, not a thread or
process per connection. There is no per-connection stack, no scheduler entry
and no context-switch cost, so the limit lands on kernel per-socket resources
rather than on anything the application does. Per-connection cost measured over
the 5,000–30,000 range is about **445 bytes** of RSS under `kqueue` and **250
bytes** under `poll` — a hash-table entry and a session object, nothing more.
A thread-per-connection design could not have reached these numbers: at the
default 8 MB stack reservation, 70,000 threads would need roughly 550 GB of
address space, and on 2 vCPUs the scheduler would have collapsed long before
the descriptor table filled.

**4. Would changing the mechanism help?**
Not with the descriptor or socket limits, which are kernel per-socket costs
identical under `poll`, `kqueue` or threads — and Part A confirms it: both
backends reach the same ~30,000 and hold the same number of descriptors. What
the mechanism changes is the cost of each event-loop iteration, and that
difference is clearly visible in the CPU column.

**5. Quantify the trade-off.**
Running the identical sweep under both backends isolates it:

| Idle connections | CPU under `poll` | CPU under `kqueue` | RSS under `poll` | RSS under `kqueue` |
|---:|---:|---:|---:|---:|
| 5,000 | 0.1 % | 0.0 % | 5,036 KiB | 5,980 KiB |
| 10,000 | 0.6 % | 0.0 % | 6,212 KiB | 7,392 KiB |
| 20,000 | 1.0 % | 0.0 % | 8,612 KiB | 11,900 KiB |
| 30,000 | **2.5 %** | **0.2 %** | 11,308 KiB | 17,048 KiB |

`poll`'s CPU grows roughly linearly with the number of connections — 0.1 % to
2.5 % over a sixfold increase — because every call copies and scans an array of
all N descriptors even though none of them is ready. `kqueue`'s stays flat near
zero: interest is registered once, and each call returns only the descriptors
actually ready, so an idle connection costs nothing per iteration. The 70,000
run bears this out directly — still 0.0 % CPU at rest under `kqueue`, where
`poll`'s trend extrapolates to roughly 6 %.

The trade-off runs the other way on memory: `kqueue` costs about **5.7 MB more**
at 30,000 connections (195 bytes per connection). That is this implementation's
doing rather than the kernel's — the backend keeps an interest map keyed by
descriptor and sizes its `kevent` result buffer at two entries per registered
descriptor, which is 2.9 MB of the difference on its own and could be capped
without changing behaviour.

So `kqueue` trades a few megabytes for an order of magnitude less CPU at scale,
and the more idle the connections, the better that trade looks. `poll` remains
reasonable only at small connection counts, which is why it is kept as a
runtime option rather than removed.

> **A warning worth recording.** Exhausting either kernel table takes the whole
> machine with it, not just the next `connect()`.
>
> Filling `kern.maxfiles` left the VM unable to fork a process or even load a
> shared library — `/bin/sh` itself would not start, so nothing could be killed
> from inside and it had to be rebooted. Filling `kern.ipc.maxsockets` is
> milder but still locking: no process can create a socket, so no new `ssh`
> session can be established, and even `tail` failed with
> `unable to init casper: No buffer space available`.
>
> `tools/measure_scalability.sh` now takes the smaller of the two tables,
> refuses targets above `(limit - 4000) / 2` unless `FORCE=1`, and passes
> `conn_gen --hold-seconds` so a run releases its connections even if the
> controlling script is killed.

**Bonus deliverables checklist.**

- [x] Client-generation program: `src/tools/conn_gen.cpp` (`bin/conn_gen`).
- [x] Completed resource-measurement table (above), for both I/O backends and
      both kernel configurations, up to and including 70,000 connections.
- [ ] Screenshots at the required connection counts. The measurements are
      recorded in `report/captures/scalability_kqueue.tsv`,
      `scalability_poll.tsv` and `scalability_70k.tsv`; re-run
      `tools/measure_scalability.sh` and screenshot the terminal, or capture
      `netstat -an -p tcp | grep -c 5000`, `procstat -f <pid>`,
      `sysctl kern.openfiles kern.ipc.numopensockets` and `ps -o rss,%cpu`
      while a run is holding.
- [x] Analysis answering questions 1–5 (above).

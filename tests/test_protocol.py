#!/usr/bin/env python3
"""
Protocol conformance and robustness tests for The Socket Exchange.

Starts the Exchange Server through ./server/run-server (the same entry point
the assignment's experiment harness uses) and drives it with raw TCP sockets,
so the tests check wire behaviour rather than any internal API.

    python3 tests/test_protocol.py [-v]
"""

from __future__ import annotations

import os
import random
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"

VERBOSE = "-v" in sys.argv[1:]


# --------------------------------------------------------------------------
# Harness
# --------------------------------------------------------------------------

class Client:
    """A raw TCP connection with newline-based message reassembly."""

    def __init__(self, port, name="client"):
        self.name = name
        self.buf = b""
        self.sock = socket.create_connection((HOST, port), timeout=5.0)

    def send_raw(self, data: bytes):
        self.sock.sendall(data)

    def send(self, line: str):
        self.sock.sendall((line + "\n").encode())

    def recv(self, timeout=2.0):
        """Return the next complete message, or None on timeout/EOF."""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            except OSError:
                return None
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode(errors="replace").rstrip("\r")

    def recv_many(self, count, timeout=2.0):
        return [self.recv(timeout) for _ in range(count)]

    def drain(self, timeout=0.4):
        """Collect every message that arrives within a quiet window."""
        messages = []
        while True:
            message = self.recv(timeout)
            if message is None:
                return messages
            messages.append(message)

    def expect(self, expected, timeout=2.0):
        actual = self.recv(timeout)
        assert actual == expected, (
            f"{self.name}: expected {expected!r}, got {actual!r}"
        )
        return actual

    def expect_error(self, timeout=2.0):
        actual = self.recv(timeout)
        assert actual is not None and actual.startswith("ERROR "), (
            f"{self.name}: expected an ERROR response, got {actual!r}"
        )
        return actual

    def expect_silence(self, timeout=0.4):
        actual = self.recv(timeout)
        assert actual is None, (
            f"{self.name}: expected no message, got {actual!r}"
        )

    def abort(self):
        """Close abortively, sending RST instead of FIN."""
        try:
            self.sock.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
            )
            self.sock.close()
        except OSError:
            pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Server:
    def __init__(self, port, env=None, log_path=None):
        self.port = port
        self.log_path = log_path
        merged = dict(os.environ)
        merged.update(env or {})

        if log_path:
            self._log = open(log_path, "w")
            stdout = self._log
        else:
            self._log = None
            stdout = subprocess.DEVNULL if not VERBOSE else None

        self.proc = subprocess.Popen(
            [os.path.join(ROOT, "server", "run-server"), HOST, str(port)],
            cwd=ROOT,
            stdout=stdout,
            stderr=subprocess.STDOUT if log_path else None,
            start_new_session=True,
            env=merged,
        )
        self._wait_ready()

    def log_text(self):
        if not self.log_path:
            return ""
        if self._log:
            self._log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def _wait_ready(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"server exited early with code {self.proc.returncode}"
                )
            probe = socket.socket()
            try:
                probe.settimeout(0.3)
                probe.connect((HOST, self.port))
                probe.setsockopt(
                    socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
                )
                probe.close()
                return
            except OSError:
                probe.close()
                time.sleep(0.05)
        raise RuntimeError("server never became ready")

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, 15)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, 9)
                self.proc.wait(timeout=2)
        if self._log:
            self._log.close()
            self._log = None


TESTS = []


def test(name):
    def wrap(fn):
        TESTS.append((name, fn))
        return fn
    return wrap


def trader(server, username):
    client = Client(server.port, username)
    client.send(f"LOGIN {username}")
    client.expect("OK")
    return client


def market_data(server, *instruments, name="md"):
    client = Client(server.port, name)
    for instrument in instruments:
        client.send(f"SUBSCRIBE {instrument}")
        client.expect("OK")
    return client


# --------------------------------------------------------------------------
# Protocol basics
# --------------------------------------------------------------------------

@test("LOGIN succeeds and usernames are unique among connected traders")
def t_login(server):
    alice = trader(server, "alice")

    duplicate = Client(server.port, "duplicate")
    duplicate.send("LOGIN alice")
    duplicate.expect_error()

    # A second LOGIN on an already authenticated connection is rejected.
    alice.send("LOGIN alice2")
    alice.expect_error()

    # The name is released when the original trader disconnects.
    alice.close()
    time.sleep(0.3)
    reuse = Client(server.port, "reuse")
    reuse.send("LOGIN alice")
    reuse.expect("OK")

    duplicate.close()
    reuse.close()


@test("Client roles are enforced in both directions")
def t_roles(server):
    a_trader = trader(server, "role_trader")
    for forbidden in ("SUBSCRIBE JNST", "UNSUBSCRIBE JNST"):
        a_trader.send(forbidden)
        a_trader.expect_error()

    an_md = market_data(server, "JNST")
    for forbidden in (
        "LOGIN someone",
        "BUY JNST 1 100",
        "SELL JNST 1 100",
        "CANCEL 1",
    ):
        an_md.send(forbidden)
        an_md.expect_error()

    # Trading before LOGIN is refused even for an otherwise valid trader.
    fresh = Client(server.port, "fresh")
    fresh.send("BUY JNST 1 100")
    fresh.expect_error()

    a_trader.close()
    an_md.close()
    fresh.close()


@test("Malformed messages are rejected without dropping the connection")
def t_malformed(server):
    client = trader(server, "picky")

    bad = [
        "NONSENSE",
        "LOGIN",
        "BUY",
        "BUY JNST 1",
        "BUY JNST 1 100 extra",
        "BUY XXXX 1 100",
        "BUY JNST 0 100",
        "BUY JNST -1 100",
        "BUY JNST 1 0",
        "BUY JNST 1 -100",
        "BUY JNST 1.5 100",
        "BUY JNST 1 10.5",
        "BUY JNST one 100",
        "SELL JNST 1 +100",
        "CANCEL",
        "CANCEL abc",
        "CANCEL -1",
        "QUIT now",
    ]
    for line in bad:
        client.send(line)
        response = client.expect_error()
        if VERBOSE:
            print(f"      {line!r:32} -> {response}")

    # The connection is still fully usable afterwards.
    client.send("BUY JNST 5 100")
    assert client.recv().startswith("ORDER_ACCEPTED ")
    client.close()


@test("Order ids are unique and non-negative for the server's lifetime")
def t_order_ids(server):
    client = trader(server, "ids")
    seen = set()
    for _ in range(50):
        client.send("BUY JNST 1 999")
        response = client.recv()
        assert response.startswith("ORDER_ACCEPTED "), response
        order_id = int(response.split()[1])
        assert order_id >= 0, f"order id must be non-negative, got {order_id}"
        assert order_id not in seen, f"duplicate order id {order_id}"
        seen.add(order_id)
    client.close()


# --------------------------------------------------------------------------
# Matching
# --------------------------------------------------------------------------

@test("A full match notifies both traders and every subscriber")
def t_match_full(server):
    md = market_data(server, "JNST", name="md_full")
    buyer = trader(server, "buyer_full")
    seller = trader(server, "seller_full")

    buyer.send("BUY JNST 100 238")
    assert buyer.recv().startswith("ORDER_ACCEPTED ")
    md.expect_silence()  # resting orders are not trades

    seller.send("SELL JNST 100 238")
    assert seller.recv().startswith("ORDER_ACCEPTED ")

    buyer.expect("BOUGHT JNST 100 238")
    seller.expect("SOLD JNST 100 238")
    md.expect("TRADE JNST 100 238")

    # BOUGHT/SOLD are private: the market-data client sees only TRADE.
    md.expect_silence()
    buyer.expect_silence()

    for c in (md, buyer, seller):
        c.close()


@test("Partial fills leave the remainder resting and cancellable")
def t_partial(server):
    md = market_data(server, "JNST", name="md_partial")
    buyer = trader(server, "buyer_partial")
    seller = trader(server, "seller_partial")

    buyer.send("BUY JNST 100 238")
    buy_id = int(buyer.recv().split()[1])

    # The handout's worked example: 60 of 100 trade, 40 remain.
    seller.send("SELL JNST 60 238")
    assert seller.recv().startswith("ORDER_ACCEPTED ")
    buyer.expect("BOUGHT JNST 60 238")
    seller.expect("SOLD JNST 60 238")
    md.expect("TRADE JNST 60 238")

    # The remaining 40 still match later.
    seller.send("SELL JNST 40 238")
    assert seller.recv().startswith("ORDER_ACCEPTED ")
    buyer.expect("BOUGHT JNST 40 238")
    seller.expect("SOLD JNST 40 238")
    md.expect("TRADE JNST 40 238")

    # Now fully filled, so it can no longer be cancelled.
    buyer.send(f"CANCEL {buy_id}")
    buyer.expect_error()

    for c in (md, buyer, seller):
        c.close()


@test("Orders match only on identical price, instrument and opposite side")
def t_no_cross_match(server):
    md = market_data(server, "JNST", "IMCT", name="md_cross")
    buyer = trader(server, "buyer_cross")
    seller = trader(server, "seller_cross")

    # Different price: no match, even though the buyer would pay more.
    buyer.send("BUY JNST 10 240")
    buyer.recv()
    seller.send("SELL JNST 10 238")
    seller.recv()
    md.expect_silence()

    # Different instrument: no match.
    seller.send("SELL IMCT 10 240")
    seller.recv()
    md.expect_silence()

    # Same side: no match.
    other_buyer = trader(server, "buyer_cross_2")
    other_buyer.send("BUY JNST 10 240")
    other_buyer.recv()
    md.expect_silence()

    # Correct counterparty finally matches the resting 240 buy, oldest first.
    seller.send("SELL JNST 10 240")
    seller.recv()
    buyer.expect("BOUGHT JNST 10 240")
    seller.expect("SOLD JNST 10 240")
    md.expect("TRADE JNST 10 240")
    other_buyer.expect_silence()

    for c in (md, buyer, seller, other_buyer):
        c.close()


@test("Resting orders at one price are matched in submission order")
def t_fifo(server):
    first = trader(server, "fifo_first")
    second = trader(server, "fifo_second")
    seller = trader(server, "fifo_seller")

    first.send("BUY IMCT 5 500")
    first.recv()
    second.send("BUY IMCT 5 500")
    second.recv()

    seller.send("SELL IMCT 5 500")
    seller.recv()

    first.expect("BOUGHT IMCT 5 500")
    second.expect_silence()
    seller.expect("SOLD IMCT 5 500")

    seller.send("SELL IMCT 5 500")
    seller.recv()
    second.expect("BOUGHT IMCT 5 500")

    for c in (first, second, seller):
        c.close()


@test("One aggressive order sweeps several resting orders")
def t_sweep(server):
    md = market_data(server, "IMCT", name="md_sweep")
    resting = trader(server, "sweep_resting")
    sweeper = trader(server, "sweep_taker")

    for _ in range(3):
        resting.send("SELL IMCT 10 700")
        resting.recv()

    sweeper.send("BUY IMCT 25 700")
    assert sweeper.recv().startswith("ORDER_ACCEPTED ")

    # 10 + 10 + 5 against three resting orders.
    quantities = []
    for _ in range(3):
        message = sweeper.recv()
        assert message.startswith("BOUGHT IMCT "), message
        quantities.append(int(message.split()[2]))
    assert quantities == [10, 10, 5], quantities

    trades = [md.recv() for _ in range(3)]
    assert trades == [
        "TRADE IMCT 10 700",
        "TRADE IMCT 10 700",
        "TRADE IMCT 5 700",
    ], trades

    for c in (md, resting, sweeper):
        c.close()


@test("CANCEL removes a resting order and is owner-checked")
def t_cancel(server):
    owner = trader(server, "cancel_owner")
    other = trader(server, "cancel_other")

    owner.send("BUY JNST 10 111")
    order_id = int(owner.recv().split()[1])

    other.send(f"CANCEL {order_id}")
    other.expect_error()

    owner.send(f"CANCEL {order_id}")
    owner.expect(f"ORDER_CANCELLED {order_id}")

    # Cancelling twice fails, and the order no longer matches.
    owner.send(f"CANCEL {order_id}")
    owner.expect_error()
    owner.send("CANCEL 999999")
    owner.expect_error()

    other.send("SELL JNST 10 111")
    other.recv()
    owner.expect_silence()

    owner.close()
    other.close()


@test("A disconnected trader's resting orders leave the book")
def t_orders_die_with_owner(server):
    leaver = trader(server, "leaver")
    leaver.send("BUY JNST 10 321")
    leaver.recv()
    leaver.close()
    time.sleep(0.3)

    md = market_data(server, "JNST", name="md_leaver")
    survivor = trader(server, "survivor")
    survivor.send("SELL JNST 10 321")
    survivor.recv()

    # Nothing should match against the departed trader's order.
    survivor.expect_silence()
    md.expect_silence()

    md.close()
    survivor.close()


# --------------------------------------------------------------------------
# Subscriptions
# --------------------------------------------------------------------------

@test("Subscriptions are per instrument and can be cancelled")
def t_subscriptions(server):
    jnst = market_data(server, "JNST", name="md_jnst")
    imct = market_data(server, "IMCT", name="md_imct")
    both = market_data(server, "JNST", "IMCT", name="md_both")

    buyer = trader(server, "sub_buyer")
    seller = trader(server, "sub_seller")

    buyer.send("BUY JNST 1 400")
    buyer.recv()
    seller.send("SELL JNST 1 400")
    seller.recv()

    jnst.expect("TRADE JNST 1 400")
    both.expect("TRADE JNST 1 400")
    imct.expect_silence()

    both.send("UNSUBSCRIBE JNST")
    both.expect("OK")
    both.send("UNSUBSCRIBE JNST")
    both.expect_error()  # already unsubscribed

    buyer.send("BUY JNST 1 400")
    buyer.recv()
    seller.send("SELL JNST 1 400")
    seller.recv()

    jnst.expect("TRADE JNST 1 400")
    both.expect_silence()

    # Bad instruments are rejected for both verbs.
    imct.send("SUBSCRIBE NOPE")
    imct.expect_error()
    imct.send("UNSUBSCRIBE NOPE")
    imct.expect_error()

    for c in (jnst, imct, both, buyer, seller):
        c.close()


@test("Many market-data clients receive the same update")
def t_fanout(server):
    subscribers = [
        market_data(server, "JNST", name=f"md_{i}") for i in range(6)
    ]
    buyer = trader(server, "fanout_buyer")
    seller = trader(server, "fanout_seller")

    buyer.send("BUY JNST 7 850")
    buyer.recv()
    seller.send("SELL JNST 7 850")
    seller.recv()

    for subscriber in subscribers:
        subscriber.expect("TRADE JNST 7 850")

    for c in subscribers + [buyer, seller]:
        c.close()


# --------------------------------------------------------------------------
# Message framing
# --------------------------------------------------------------------------

@test("A message split across many writes is reassembled")
def t_framing_split(server):
    client = Client(server.port, "split")
    for piece in (b"LOG", b"IN ", b"spl", b"it_t", b"rader", b"\n"):
        client.send_raw(piece)
        time.sleep(0.05)
    client.expect("OK")
    client.close()


@test("A message split one byte at a time is reassembled")
def t_framing_bytewise(server):
    client = Client(server.port, "bytewise")
    for byte in b"LOGIN bytewise_trader\n":
        client.send_raw(bytes([byte]))
    client.expect("OK")

    for byte in b"BUY JNST 3 260\n":
        client.send_raw(bytes([byte]))
    assert client.recv().startswith("ORDER_ACCEPTED ")
    client.close()


@test("Several messages in one write are processed in order")
def t_framing_coalesced(server):
    client = Client(server.port, "coalesced")
    client.send_raw(
        b"LOGIN coalesced_trader\nBUY JNST 1 271\nBUY JNST 2 272\nCANCEL 0\n"
    )
    client.expect("OK")
    first = client.recv()
    second = client.recv()
    assert first.startswith("ORDER_ACCEPTED "), first
    assert second.startswith("ORDER_ACCEPTED "), second
    assert client.recv().startswith("ERROR "), "CANCEL 0 should fail"
    client.close()


@test("A message straddling two writes at an arbitrary point is reassembled")
def t_framing_straddle(server):
    client = Client(server.port, "straddle")
    payload = b"LOGIN straddle_trader\nBUY IMCT 4 613\n"
    # Cut mid-way through the second message, past the first one's newline.
    client.send_raw(payload[:26])
    time.sleep(0.15)
    client.send_raw(payload[26:])
    client.expect("OK")
    assert client.recv().startswith("ORDER_ACCEPTED ")
    client.close()


@test("A partial message never blocks another client")
def t_partial_does_not_block(server):
    stalled = Client(server.port, "stalled")
    stalled.send_raw(b"LOGIN never_finishes")  # no newline, then silence

    time.sleep(0.5)

    start = time.monotonic()
    active = Client(server.port, "active")
    active.send("LOGIN active_trader")
    active.expect("OK", timeout=3.0)
    elapsed = time.monotonic() - start

    assert elapsed < 1.0, (
        f"an idle client delayed another client by {elapsed:.2f}s"
    )
    stalled.expect_silence()  # still waiting for its newline

    # Completing the message afterwards works normally.
    stalled.send_raw(b"\n")
    stalled.expect("OK")

    stalled.close()
    active.close()


# --------------------------------------------------------------------------
# Connection handling
# --------------------------------------------------------------------------

@test("QUIT closes the connection from the server side")
def t_quit(server):
    client = trader(server, "quitter")
    client.send("QUIT")
    # No OK for QUIT; the server closes, so the next read reports EOF.
    assert client.recv(timeout=2.0) is None
    client.close()
    assert server.alive()


@test("An abrupt client reset does not disturb the server or other clients")
def t_reset(server):
    survivor = trader(server, "reset_survivor")
    md = market_data(server, "JNST", name="md_reset")

    for i in range(10):
        victim = Client(server.port, f"victim_{i}")
        victim.send(f"LOGIN victim_{i}")
        victim.expect("OK")
        victim.send("BUY JNST 5 950")
        victim.recv()
        victim.abort()

    time.sleep(0.5)
    assert server.alive(), "server died after abrupt client resets"

    survivor.send("BUY JNST 1 951")
    survivor.recv()
    other = trader(server, "reset_other")
    other.send("SELL JNST 1 951")
    other.recv()
    survivor.expect("BOUGHT JNST 1 951")
    md.expect("TRADE JNST 1 951")

    for c in (survivor, md, other):
        c.close()


@test("A half-closed client still receives queued messages")
def t_half_close(server):
    client = Client(server.port, "half_closed")
    client.send("LOGIN half_closed_trader")
    client.send("BUY JNST 2 480")
    client.sock.shutdown(socket.SHUT_WR)  # FIN, but keep reading

    assert client.recv() == "OK"
    assert client.recv().startswith("ORDER_ACCEPTED ")
    client.close()
    assert server.alive()


@test("At least 10 clients are served simultaneously (2 traders, 4 md)")
def t_many_clients(server):
    traders = [trader(server, f"many_trader_{i}") for i in range(4)]
    subscribers = [
        market_data(server, "JNST", name=f"many_md_{i}") for i in range(8)
    ]
    assert len(traders) + len(subscribers) >= 10

    traders[0].send("BUY JNST 9 640")
    traders[0].recv()
    traders[1].send("SELL JNST 9 640")
    traders[1].recv()

    traders[0].expect("BOUGHT JNST 9 640")
    traders[1].expect("SOLD JNST 9 640")
    for subscriber in subscribers:
        subscriber.expect("TRADE JNST 9 640")

    # Idle clients stay usable afterwards.
    traders[2].send("BUY IMCT 1 1)")
    traders[2].expect_error()
    traders[3].send("BUY IMCT 1 1")
    assert traders[3].recv().startswith("ORDER_ACCEPTED ")

    for c in traders + subscribers:
        c.close()


@test("A market-data client that stops reading does not stall the others")
def t_backpressure(server):
    slow = Client(server.port, "slow_md")
    slow.send("SUBSCRIBE JNST")
    slow.expect("OK")
    # From here on the slow client never reads again.

    fast = market_data(server, "JNST", name="fast_md")
    buyer = trader(server, "bp_buyer")
    seller = trader(server, "bp_seller")

    # Enough updates to overrun the slow client's receive window several times.
    count = 4000
    for _ in range(count):
        buyer.send("BUY JNST 1 238")
        seller.send("SELL JNST 1 238")
        buyer.drain(0.0)
        seller.drain(0.0)
        fast.drain(0.0)

    # The fast subscriber must still be getting fresh updates promptly.
    start = time.monotonic()
    received = 0
    while time.monotonic() - start < 5.0:
        message = fast.recv(0.5)
        if message is None:
            break
        if message.startswith("TRADE JNST"):
            received += 1

    assert server.alive(), "server died under backpressure"
    assert received > 0, "the fast subscriber received nothing"

    # And a brand-new client is still served immediately.
    start = time.monotonic()
    latecomer = Client(server.port, "latecomer")
    latecomer.send("LOGIN latecomer")
    latecomer.expect("OK", timeout=3.0)
    elapsed = time.monotonic() - start
    assert elapsed < 2.0, (
        f"a non-reading client delayed a new connection by {elapsed:.2f}s"
    )

    for c in (slow, fast, buyer, seller, latecomer):
        c.close()


@test("A full send buffer makes the server queue output instead of blocking")
def t_backpressure_forced(server):
    """Drives the EAGAIN path deliberately and checks the server noticed.

    The previous test relies on a client simply not reading, which need not
    actually fill the buffers: FreeBSD auto-tunes a receive buffer up to
    net.inet.tcp.recvbuf_max (8 MB by default) regardless of SO_RCVBUF, so a
    modest volume is absorbed. Here a dedicated server is started with a small
    per-connection send buffer and trades are generated in batches until the
    server reports buffering, so the user-space output queue and the
    write-readiness path are genuinely exercised on both systems.
    """
    port = random.randint(40001, 60000)
    log_path = os.path.join(ROOT, "tests", ".backpressure.log")
    own = Server(port, env={"EXCHANGE_SNDBUF": "4096"}, log_path=log_path)

    stop_draining = threading.Event()

    def keep_draining(sock):
        while not stop_draining.is_set():
            try:
                if not sock.recv(1 << 20):
                    return
            except OSError:
                return

    try:
        stuck = socket.socket()
        stuck.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        stuck.connect((HOST, port))
        stuck.sendall(b"SUBSCRIBE JNST\n")
        time.sleep(0.3)
        # Never read from `stuck` again.

        fast = market_data(own, "JNST", name="fast_forced")
        buyer = trader(own, "forced_buyer")
        seller = trader(own, "forced_seller")

        # The order senders must keep reading their own replies, or they would
        # become slow receivers themselves and confuse the measurement.
        for sock in (buyer.sock, seller.sock):
            threading.Thread(target=keep_draining, args=(sock,),
                             daemon=True).start()

        batch = 2500
        generated = 0
        deadline = time.monotonic() + 60.0
        backpressure = False

        while time.monotonic() < deadline:
            buyer.sock.sendall(b"BUY JNST 1 238\n" * batch)
            seller.sock.sendall(b"SELL JNST 1 238\n" * batch)
            generated += batch
            if "is not draining its socket" in own.log_text():
                backpressure = True
                break

        assert own.alive(), "server died while a client refused to read"
        assert backpressure, (
            f"no backpressure after {generated} trades; the send buffer never "
            f"filled, so the EAGAIN path was not exercised:\n"
            + own.log_text()
        )

        # Critically: the server queued rather than blocked, so a new
        # connection is still accepted and answered immediately.
        start = time.monotonic()
        latecomer = Client(port, "forced_latecomer")
        latecomer.send("LOGIN forced_latecomer")
        latecomer.expect("OK", timeout=3.0)
        elapsed = time.monotonic() - start
        assert elapsed < 2.0, (
            f"a non-reading client delayed a new connection by {elapsed:.2f}s"
        )

        stop_draining.set()
        stuck.close()
        for c in (fast, buyer, seller, latecomer):
            c.close()
    finally:
        stop_draining.set()
        own.stop()
        if os.path.exists(log_path):
            os.remove(log_path)


@test("Oversized input without a newline is rejected, not buffered forever")
def t_oversized(server):
    client = Client(server.port, "oversized")
    blob = b"A" * 100000
    try:
        client.send_raw(b"LOGIN " + blob)
    except OSError:
        pass
    response = client.recv(timeout=3.0)
    assert response is None or response.startswith("ERROR "), response
    client.close()
    assert server.alive()


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

def main():
    port = random.randint(20000, 40000)
    server = Server(port)

    passed, failed = 0, []
    print(f"Running {len(TESTS)} tests against 127.0.0.1:{port}\n")

    try:
        for name, fn in TESTS:
            sys.stdout.write(f"  {name} ... ")
            sys.stdout.flush()
            try:
                fn(server)
                if not server.alive():
                    raise AssertionError("the Exchange Server exited")
                print("ok")
                passed += 1
            except Exception:
                print("FAIL")
                failed.append((name, traceback.format_exc()))
                if not server.alive():
                    print("\nThe server died; remaining tests cannot run.")
                    break
    finally:
        server.stop()

    print()
    for name, tb in failed:
        print(f"--- {name} ---")
        print(tb)

    print(f"{passed} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

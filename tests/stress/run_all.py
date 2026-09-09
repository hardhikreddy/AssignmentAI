#!/usr/bin/env python3
"""
Stress / conformance test suite for The Socket Exchange.

Drives the real launcher scripts (server/run-server) exactly like experiment.py
does, one fresh server instance per test on its own port. Standard library only.

    python3 tests/stress/run_all.py            # run everything
    python3 tests/stress/run_all.py framing    # run tests whose name contains "framing"
    python3 tests/stress/run_all.py -v         # verbose (print every server line sent/recv)

Exit code 0 iff every test passes.
"""

from __future__ import annotations

import contextlib
import os
import selectors
import signal
import socket
import struct
import subprocess
import sys
import time
import traceback

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SERVER_LAUNCHER = os.path.join(REPO_ROOT, "server", "run-server")
HOST = "127.0.0.1"

VERBOSE = False
_port_counter = [24000 + (os.getpid() % 2000)]


def next_port() -> int:
    _port_counter[0] += 1
    return _port_counter[0]


# ---------------------------------------------------------------------------
# Connection helper with its own newline-framed receive buffer
# ---------------------------------------------------------------------------

class Conn:
    def __init__(self, port: int, name: str = "conn"):
        self.name = name
        self.sock = socket.create_connection((HOST, port), timeout=3.0)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    # -- raw send helpers --
    def send_raw(self, data: bytes) -> None:
        if VERBOSE:
            print(f"    {self.name} >> {data!r}")
        self.sock.sendall(data)

    def send(self, line: str) -> None:
        self.send_raw(line.encode() + b"\n")

    def send_pieces(self, line: str, sizes=None, delay: float = 0.02) -> None:
        data = line.encode() + b"\n"
        if sizes is None:
            sizes = [1] * len(data)
        i = 0
        for s in sizes:
            chunk = data[i:i + s]
            if not chunk:
                break
            self.send_raw(chunk)
            i += s
            time.sleep(delay)
        if i < len(data):
            self.send_raw(data[i:])

    # -- framed receive --
    def _fill(self, timeout: float) -> bool:
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(65536)
        except (socket.timeout, TimeoutError):
            return False
        except (ConnectionResetError, OSError):
            self.buf += b""  # noqa
            raise
        if chunk == b"":
            raise EOFError("server closed connection")
        if VERBOSE:
            print(f"    {self.name} << {chunk!r}")
        self.buf += chunk
        return True

    def recv_line(self, timeout: float = 1.5):
        """Return next line (str, no newline) or None on timeout."""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                self._fill(remaining)
            except EOFError:
                if b"\n" in self.buf:
                    break
                return None
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def recv_lines(self, count: int, timeout: float = 2.0):
        out = []
        for _ in range(count):
            ln = self.recv_line(timeout)
            if ln is None:
                break
            out.append(ln)
        return out

    def drain(self, settle: float = 0.15):
        """Collect all lines currently available (best effort)."""
        out = []
        while True:
            ln = self.recv_line(settle)
            if ln is None:
                return out
            out.append(ln)

    def expect(self, expected: str, timeout: float = 1.5):
        ln = self.recv_line(timeout)
        assert ln == expected, f"{self.name}: expected {expected!r}, got {ln!r}"
        return ln

    def expect_silence(self, dur: float = 0.4):
        ln = self.recv_line(dur)
        assert ln is None, f"{self.name}: expected silence, got {ln!r}"

    def rst_close(self):
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                             struct.pack("ii", 1, 0))
        self.sock.close()

    def half_close(self):
        with contextlib.suppress(OSError):
            self.sock.shutdown(socket.SHUT_WR)

    def close(self):
        with contextlib.suppress(OSError):
            self.sock.close()


class Server:
    def __init__(self, port: int):
        self.port = port
        self.proc = subprocess.Popen(
            [SERVER_LAUNCHER, HOST, str(port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self._wait_ready()

    def _wait_ready(self, timeout: float = 5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited early ({self.proc.returncode})")
            try:
                s = socket.create_connection((HOST, self.port), timeout=0.3)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                             struct.pack("ii", 1, 0))
                s.close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server never became ready")

    def rss_kb(self):
        try:
            with open(f"/proc/{self.proc.pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS"):
                        return int(line.split()[1])
        except OSError:
            pass
        return None

    def open_fds(self):
        try:
            return len(os.listdir(f"/proc/{self.proc.pid}/fd"))
        except OSError:
            return None

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.proc.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(self.proc.pid, signal.SIGKILL)


@contextlib.contextmanager
def server():
    srv = Server(next_port())
    try:
        yield srv
        assert srv.alive(), "server died during test"
    finally:
        srv.stop()


def trader(srv, name):
    c = Conn(srv.port, name)
    c.send(f"LOGIN {name}")
    c.expect("OK")
    return c


def md(srv, name, *instruments):
    c = Conn(srv.port, name)
    for ins in instruments:
        c.send(f"SUBSCRIBE {ins}")
        c.expect("OK")
    return c


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


# ---- framing ----

@test
def framing_byte_by_byte(_):
    with server() as srv:
        c = Conn(srv.port, "t")
        c.send_pieces("LOGIN alice", delay=0.01)
        c.expect("OK")


@test
def framing_split_midtoken(_):
    with server() as srv:
        c = Conn(srv.port, "t")
        c.send_raw(b"LOG")
        time.sleep(0.1)
        c.send_raw(b"IN bo")
        time.sleep(0.1)
        c.send_raw(b"b\n")
        c.expect("OK")


@test
def framing_multiple_messages_one_send(_):
    with server() as srv:
        c = Conn(srv.port, "t")
        c.send_raw(b"LOGIN alice\nBUY JNST 5 100\nBUY JNST 3 100\n")
        assert c.expect("OK")
        assert c.recv_line().startswith("ORDER_ACCEPTED")
        assert c.recv_line().startswith("ORDER_ACCEPTED")


@test
def framing_crlf_tolerated(_):
    with server() as srv:
        c = Conn(srv.port, "t")
        c.send_raw(b"LOGIN alice\r\n")
        c.expect("OK")


@test
def framing_two_orders_split_across_boundary(_):
    with server() as srv:
        c = trader(srv, "alice")
        c.send_raw(b"BUY JNST 1 10\nBU")
        assert c.recv_line().startswith("ORDER_ACCEPTED")
        time.sleep(0.05)
        c.send_raw(b"Y JNST 2 10\n")
        assert c.recv_line().startswith("ORDER_ACCEPTED")


# ---- roles ----

@test
def role_login_locks_trader(_):
    with server() as srv:
        c = trader(srv, "alice")
        c.send("SUBSCRIBE JNST")
        assert c.recv_line().startswith("ERROR")


@test
def role_subscribe_locks_marketdata(_):
    with server() as srv:
        c = md(srv, "m1", "JNST")
        c.send("BUY JNST 1 1")
        assert c.recv_line().startswith("ERROR")
        c.send("LOGIN x")
        assert c.recv_line().startswith("ERROR")


@test
def role_unassigned_unsubscribe_locks_md(_):
    with server() as srv:
        c = Conn(srv.port, "m")
        c.send("UNSUBSCRIBE JNST")
        c.expect("OK")
        c.send("BUY JNST 1 1")
        assert c.recv_line().startswith("ERROR")


@test
def role_md_both_instruments(_):
    with server() as srv:
        c = md(srv, "m", "JNST", "IMCT")
        t1 = trader(srv, "a")
        t2 = trader(srv, "b")
        t1.send("BUY IMCT 2 50"); t1.recv_line()
        t2.send("SELL IMCT 2 50"); t2.drain()
        assert "TRADE IMCT 2 50" in c.drain()


@test
def role_quit_both(_):
    with server() as srv:
        t = trader(srv, "a")
        t.send("QUIT")
        try:
            assert t.recv_line(1.0) is None
        except EOFError:
            pass
        m = md(srv, "m", "JNST")
        m.send("QUIT")
        try:
            assert m.recv_line(1.0) is None
        except EOFError:
            pass


# ---- login ----

@test
def login_duplicate_rejected_then_reusable(_):
    with server() as srv:
        a = trader(srv, "alice")
        b = Conn(srv.port, "b")
        b.send("LOGIN alice")
        assert b.recv_line().startswith("ERROR")
        a.close()
        time.sleep(0.2)
        c = Conn(srv.port, "c")
        c.send("LOGIN alice")
        c.expect("OK")


@test
def login_bad_arity(_):
    with server() as srv:
        c = Conn(srv.port, "t")
        c.send("LOGIN")
        assert c.recv_line().startswith("ERROR")
        c2 = Conn(srv.port, "t2")
        c2.send("LOGIN a b")
        assert c2.recv_line().startswith("ERROR")


# ---- matching ----

@test
def match_exact_price_only(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 10 240"); a.expect("ORDER_ACCEPTED 0")
        b.send("SELL JNST 10 238"); b.expect("ORDER_ACCEPTED 1")
        a.expect_silence(0.4)
        b.expect_silence(0.1)


@test
def match_partial_fill_leaves_remainder(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 100 238"); a.expect("ORDER_ACCEPTED 0")
        b.send("SELL JNST 60 238"); b.expect("ORDER_ACCEPTED 1")
        assert a.recv_line() == "BOUGHT JNST 60 238"
        assert b.recv_line() == "SOLD JNST 60 238"
        # 40 remain on the buy side; a second sell of 40 clears it
        b.send("SELL JNST 40 238"); b.expect("ORDER_ACCEPTED 2")
        assert a.recv_line() == "BOUGHT JNST 40 238"
        assert b.recv_line() == "SOLD JNST 40 238"


@test
def match_fifo_within_price_level(_):
    with server() as srv:
        s1 = trader(srv, "s1")
        s2 = trader(srv, "s2")
        b = trader(srv, "b")
        s1.send("SELL JNST 5 100"); s1.expect("ORDER_ACCEPTED 0")
        s2.send("SELL JNST 5 100"); s2.expect("ORDER_ACCEPTED 1")
        b.send("BUY JNST 5 100"); b.expect("ORDER_ACCEPTED 2")
        # order 0 (s1) should fill first
        assert s1.recv_line() == "SOLD JNST 5 100"
        s2.expect_silence(0.3)


@test
def match_two_partials_two_trades(_):
    """Piazza: SELL 40 vs two BUY 20 => two TRADE msgs, two BOUGHT/SOLD pairs."""
    with server() as srv:
        m = md(srv, "m", "JNST")
        b1 = trader(srv, "b1")
        b2 = trader(srv, "b2")
        s = trader(srv, "s")
        b1.send("BUY JNST 20 200"); b1.expect("ORDER_ACCEPTED 0")
        b2.send("BUY JNST 20 200"); b2.expect("ORDER_ACCEPTED 1")
        s.send("SELL JNST 40 200"); s.expect("ORDER_ACCEPTED 2")
        assert b1.recv_line() == "BOUGHT JNST 20 200"
        assert b2.recv_line() == "BOUGHT JNST 20 200"
        sold = s.recv_lines(2)
        assert sold == ["SOLD JNST 20 200", "SOLD JNST 20 200"], sold
        trades = m.drain()
        assert trades.count("TRADE JNST 20 200") == 2, trades


@test
def match_self_trade_allowed(_):
    with server() as srv:
        a = trader(srv, "a")
        a.send("BUY JNST 5 100"); a.expect("ORDER_ACCEPTED 0")
        a.send("SELL JNST 5 100"); a.expect("ORDER_ACCEPTED 1")
        got = set(a.recv_lines(2))
        assert got == {"BOUGHT JNST 5 100", "SOLD JNST 5 100"}, got


@test
def match_no_cross_instrument(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 5 100"); a.expect("ORDER_ACCEPTED 0")
        b.send("SELL IMCT 5 100"); b.expect("ORDER_ACCEPTED 1")
        a.expect_silence(0.4)


@test
def match_smaller_quantity_traded(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 3 100"); a.expect("ORDER_ACCEPTED 0")
        b.send("SELL JNST 9 100"); b.expect("ORDER_ACCEPTED 1")
        assert a.recv_line() == "BOUGHT JNST 3 100"
        assert b.recv_line() == "SOLD JNST 3 100"
        # 6 remain resting on sell side
        a.send("BUY JNST 6 100"); a.expect("ORDER_ACCEPTED 2")
        assert a.recv_line() == "BOUGHT JNST 6 100"


# ---- value validation ----

@test
def value_validation(_):
    with server() as srv:
        c = trader(srv, "a")
        bad = [
            "BUY JNST 0 100", "BUY JNST -1 100", "BUY JNST 1 0",
            "BUY JNST 1 -5", "BUY JNST 1.5 100", "BUY JNST 1 2147483648",
            "BUY JNST 2147483648 100", "BUY JNST  100", "BUY JNST 1 100 5",
            "BUY JNST 1", "BUY ABCD 1 100", "BUY JNST x 100",
        ]
        for cmd in bad:
            c.send(cmd)
            ln = c.recv_line()
            assert ln is not None and ln.startswith("ERROR"), (cmd, ln)
        # boundary max is accepted
        c.send("BUY JNST 2147483647 2147483647")
        assert c.recv_line().startswith("ORDER_ACCEPTED"), "max value must be ok"


# ---- cancel ----

@test
def cancel_flows(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 10 100"); a.expect("ORDER_ACCEPTED 0")
        a.send("CANCEL 0"); a.expect("ORDER_CANCELLED 0")
        a.send("CANCEL 0")
        assert a.recv_line().startswith("ERROR")       # already cancelled
        a.send("CANCEL 999")
        assert a.recv_line().startswith("ERROR")       # unknown
        a.send("BUY JNST 10 100"); a.expect("ORDER_ACCEPTED 1")
        b.send("CANCEL 1")
        assert b.recv_line().startswith("ERROR")       # not owner
        # filled order cannot be cancelled
        a.send("BUY JNST 5 50"); a.expect("ORDER_ACCEPTED 2")
        b.send("SELL JNST 5 50"); b.drain()
        a.recv_lines(1)
        a.send("CANCEL 2")
        assert a.recv_line().startswith("ERROR")


@test
def cancel_after_partial_fill(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 100 238"); a.expect("ORDER_ACCEPTED 0")
        b.send("SELL JNST 60 238"); b.drain()
        a.recv_lines(1)
        a.send("CANCEL 0"); a.expect("ORDER_CANCELLED 0")   # 40 remaining, cancellable
        b.send("SELL JNST 40 238"); b.expect("ORDER_ACCEPTED 2")
        b.expect_silence(0.4)


@test
def cancel_md_rejected(_):
    with server() as srv:
        c = md(srv, "m", "JNST")
        c.send("CANCEL 0")
        assert c.recv_line().startswith("ERROR")


# ---- notifications / subscriptions ----

@test
def notif_order_accepted_precedes_fill(_):
    with server() as srv:
        a = trader(srv, "a")
        a.send("SELL JNST 5 100"); a.expect("ORDER_ACCEPTED 0")
        b = trader(srv, "b")
        b.send("BUY JNST 5 100")
        assert b.recv_line() == "ORDER_ACCEPTED 1"
        assert b.recv_line() == "BOUGHT JNST 5 100"


@test
def notif_async_delivery_later(_):
    with server() as srv:
        a = trader(srv, "a")
        a.send("BUY JNST 5 100"); a.expect("ORDER_ACCEPTED 0")
        a.expect_silence(0.5)
        b = trader(srv, "b")
        b.send("SELL JNST 5 100"); b.drain()
        assert a.recv_line(2.0) == "BOUGHT JNST 5 100"


@test
def notif_unsubscribe_stops_trades(_):
    with server() as srv:
        m = md(srv, "m", "JNST")
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 1 100"); a.recv_line()
        b.send("SELL JNST 1 100"); b.drain()
        assert "TRADE JNST 1 100" in m.drain()
        m.send("UNSUBSCRIBE JNST"); m.expect("OK")
        a.send("BUY JNST 1 100"); a.recv_line()
        b.send("SELL JNST 1 100"); b.drain()
        m.expect_silence(0.5)


@test
def notif_trader_gets_no_trade(_):
    with server() as srv:
        a = trader(srv, "a")
        b = trader(srv, "b")
        a.send("BUY JNST 1 100"); a.recv_line()
        b.send("SELL JNST 1 100")
        lines = b.recv_lines(3)
        assert not any(x.startswith("TRADE") for x in lines), lines


# ---- lifecycle ----

@test
def lifecycle_half_close_flushes_reply(_):
    with server() as srv:
        c = Conn(srv.port, "t")
        c.send_raw(b"LOGIN alice\n")
        c.half_close()
        # server should still deliver the OK it owes, then EOF
        assert c.expect("OK")


@test
def lifecycle_resting_order_survives_disconnect(_):
    with server() as srv:
        m = md(srv, "m", "JNST")
        a = trader(srv, "a")
        a.send("BUY JNST 10 150"); a.expect("ORDER_ACCEPTED 0")
        a.rst_close()
        time.sleep(0.3)
        assert srv.alive()
        b = trader(srv, "b")
        b.send("SELL JNST 10 150"); b.expect("ORDER_ACCEPTED 1")
        assert b.recv_line() == "SOLD JNST 10 150"
        assert "TRADE JNST 10 150" in m.drain()


@test
def lifecycle_rst_midsession_isolated(_):
    with server() as srv:
        victim = trader(srv, "v")
        bystander = trader(srv, "by")
        victim.send("BUY JNST 1 1")
        victim.rst_close()
        time.sleep(0.2)
        bystander.send("BUY JNST 2 2")
        assert bystander.recv_line().startswith("ORDER_ACCEPTED")
        assert srv.alive()


@test
def lifecycle_many_abrupt_disconnects(_):
    with server() as srv:
        for _ in range(200):
            c = Conn(srv.port, "x")
            c.send_raw(b"LOGIN ")
            c.rst_close()
        good = trader(srv, "good")
        good.send("BUY JNST 1 1")
        assert good.recv_line().startswith("ORDER_ACCEPTED")
        assert srv.alive()


# ---- concurrency / isolation ----

@test
def concurrency_ten_clients(_):
    with server() as srv:
        traders = [trader(srv, f"t{i}") for i in range(4)]
        mds = [md(srv, f"m{i}", "JNST") for i in range(6)]
        traders[0].send("BUY JNST 5 100"); traders[0].recv_line()
        traders[1].send("SELL JNST 5 100"); traders[1].drain()
        for m in mds:
            assert "TRADE JNST 5 100" in m.drain(), m.name


@test
def isolation_incomplete_line_does_not_stall(_):
    with server() as srv:
        stuck = Conn(srv.port, "stuck")
        stuck.send_raw(b"LOGIN blocked_client")   # no newline, then silence
        time.sleep(0.5)
        t0 = time.monotonic()
        active = Conn(srv.port, "active")
        active.send("LOGIN active_client")
        assert active.expect("OK", timeout=2.0)
        assert time.monotonic() - t0 < 1.0, "idle client stalled the server"


@test
def isolation_slow_reader_does_not_block_loop(_):
    with server() as srv:
        slow = md(srv, "slow", "JNST")     # subscribes, then never reads
        fast = md(srv, "fast", "JNST")
        b = trader(srv, "b")
        s = trader(srv, "s")
        # generate a burst of trades
        for _ in range(400):
            b.send("BUY JNST 1 100")
            s.send("SELL JNST 1 100")
        # fast client must still be receiving TRADE updates promptly
        deadline = time.monotonic() + 5.0
        seen = 0
        while time.monotonic() < deadline and seen < 400:
            ln = fast.recv_line(1.0)
            if ln is None:
                break
            if ln == "TRADE JNST 1 100":
                seen += 1
        assert seen >= 300, f"fast MD client starved by slow reader: {seen}"
        assert srv.alive()
        b.drain(); s.drain()


@test
def isolation_flood_does_not_starve(_):
    with server() as srv:
        flood = trader(srv, "flood")
        other = trader(srv, "other")
        payload = ("BUY JNST 1 100\n" * 2000).encode()
        flood.sock.setblocking(False)
        with contextlib.suppress(BlockingIOError, OSError):
            flood.sock.sendall(payload)
        flood.sock.setblocking(True)
        t0 = time.monotonic()
        other.send("BUY JNST 1 1")
        assert other.recv_line(3.0).startswith("ORDER_ACCEPTED")
        assert time.monotonic() - t0 < 2.0, "flooding client starved a peer"
        flood.drain(0.3)


# ---- soak ----

@test
def soak_no_leak(_):
    with server() as srv:
        m = md(srv, "m", "JNST", "IMCT")
        b = trader(srv, "b")
        s = trader(srv, "s")
        time.sleep(0.3)
        rss0 = srv.rss_kb()
        fd0 = srv.open_fds()
        for rnd in range(6):
            for _ in range(2000):
                b.send("BUY JNST 1 100")
                s.send("SELL JNST 1 100")
            b.drain(0.05); s.drain(0.05); m.drain(0.05)
            # connection churn
            for i in range(200):
                c = Conn(srv.port, "c")
                c.send(f"LOGIN churn{rnd}_{i}")
                c.recv_line(0.5)
                c.close()
        b.drain(); s.drain(); m.drain()
        time.sleep(0.5)
        rss1 = srv.rss_kb()
        fd1 = srv.open_fds()
        assert srv.alive()
        if fd0 is not None and fd1 is not None:
            assert fd1 <= fd0 + 5, f"fd leak: {fd0} -> {fd1}"
        if rss0 is not None and rss1 is not None:
            # allow generous slack; catch runaway growth only
            assert rss1 < rss0 + 20000, f"rss growth: {rss0} -> {rss1} kB"


# ---------------------------------------------------------------------------

def main():
    global VERBOSE
    args = [a for a in sys.argv[1:]]
    if "-v" in args:
        VERBOSE = True
        args.remove("-v")
    flt = args[0] if args else ""

    if not os.access(SERVER_LAUNCHER, os.X_OK):
        print(f"launcher not executable: {SERVER_LAUNCHER}", file=sys.stderr)
        sys.exit(2)

    selected = [t for t in TESTS if flt in t.__name__]
    passed = failed = 0
    failures = []
    for t in selected:
        name = t.__name__
        try:
            t(None)
            print(f"  PASS  {name}")
            passed += 1
        except Exception as exc:  # noqa
            print(f"  FAIL  {name}: {exc}")
            if VERBOSE:
                traceback.print_exc()
            failures.append(name)
            failed += 1
    print(f"\n{passed} passed, {failed} failed, {len(selected)} total")
    if failures:
        print("failed:", ", ".join(failures))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

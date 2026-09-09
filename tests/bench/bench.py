#!/usr/bin/env python3
"""
Reproducible micro-benchmarks for The Socket Exchange server.

    python3 tests/bench/bench.py [label]

Prints a fixed set of metrics so before/after runs can be diffed directly.
Standard library only. Uses server/run-server on its own port.
"""

from __future__ import annotations

import os
import resource
import signal
import socket
import struct
import subprocess
import sys
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LAUNCHER = os.path.join(REPO_ROOT, "server", "run-server")
HOST = "127.0.0.1"
BASE_PORT = 26000 + (os.getpid() % 1000)


def _port(n):
    return BASE_PORT + n


class Srv:
    def __init__(self, port):
        self.port = port
        self.proc = subprocess.Popen([LAUNCHER, HOST, str(port)],
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL,
                                     start_new_session=True)
        for _ in range(100):
            try:
                s = socket.create_connection((HOST, port), 0.3)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                s.close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server not ready")

    def rss_kb(self):
        try:
            with open(f"/proc/{self.proc.pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS"):
                        return int(line.split()[1])
        except OSError:
            pass
        try:
            out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(self.proc.pid)], text=True)
            return int(out.strip())
        except (subprocess.SubprocessError, ValueError):
            return 0

    def cpu_seconds(self):
        try:
            with open(f"/proc/{self.proc.pid}/stat") as f:
                parts = f.read().split()
            utime, stime = int(parts[13]), int(parts[14])
            return (utime + stime) / os.sysconf("SC_CLK_TCK")
        except OSError:
            return 0.0

    def stop(self):
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(2)
            except Exception:
                os.killpg(self.proc.pid, signal.SIGKILL)


def c(port):
    s = socket.create_connection((HOST, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


_src_ips = [f"127.0.0.{b}" for b in range(1, 200)]


def c_from(port, i):
    """Connect using a rotating loopback source IP to dodge ephemeral-port
    exhaustion when opening tens of thousands of connections (Linux allows
    binding anywhere in 127.0.0.0/8)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((_src_ips[i % len(_src_ips)], 0))
    except OSError:
        pass
    s.connect((HOST, port))
    return s


def drain(s):
    s.setblocking(False)
    n = 0
    try:
        while True:
            d = s.recv(1 << 20)
            if not d:
                break
            n += len(d)
    except BlockingIOError:
        pass
    s.setblocking(True)
    return n


def readline(s, timeout=2.0):
    s.settimeout(timeout)
    b = b""
    while not b.endswith(b"\n"):
        d = s.recv(1)
        if not d:
            break
        b += d
    return b.decode().strip()


def bench_throughput():
    srv = Srv(_port(1))
    try:
        mds = [c(srv.port) for _ in range(4)]
        for m in mds:
            m.sendall(b"SUBSCRIBE JNST\n")
        b = c(srv.port); b.sendall(b"LOGIN b\n")
        s = c(srv.port); s.sendall(b"LOGIN s\n")
        time.sleep(0.2)
        for m in mds:
            drain(m)
        drain(b); drain(s)
        N = 100_000
        cpu0 = srv.cpu_seconds()
        t0 = time.time()
        for i in range(N):
            b.sendall(b"BUY JNST 1 238\n")
            s.sendall(b"SELL JNST 1 238\n")
            if i % 2000 == 0:
                drain(b); drain(s)
                for m in mds:
                    drain(m)
        drain(b); drain(s)
        for m in mds:
            drain(m)
        dt = time.time() - t0
        cpu = srv.cpu_seconds() - cpu0
        print(f"throughput      : {N/dt:9.0f} trades/s  "
              f"({dt:.3f}s wall, {cpu:.3f}s server-cpu, {cpu/N*1e6:.2f} us/trade)")
    finally:
        srv.stop()


def bench_fanout():
    srv = Srv(_port(2))
    try:
        NS = 200
        mds = [c(srv.port) for _ in range(NS)]
        for m in mds:
            m.sendall(b"SUBSCRIBE JNST\n")
        b = c(srv.port); b.sendall(b"LOGIN b\n")
        s = c(srv.port); s.sendall(b"LOGIN s\n")
        time.sleep(0.3)
        for m in mds:
            drain(m)
        drain(b); drain(s)
        N = 20_000
        cpu0 = srv.cpu_seconds()
        t0 = time.time()
        for i in range(N):
            b.sendall(b"BUY JNST 1 238\n")
            s.sendall(b"SELL JNST 1 238\n")
            if i % 500 == 0:
                drain(b); drain(s)
                for m in mds:
                    drain(m)
        for m in mds:
            drain(m)
        drain(b); drain(s)
        dt = time.time() - t0
        cpu = srv.cpu_seconds() - cpu0
        deliveries = N * NS
        print(f"fanout ({NS} sub) : {deliveries/dt:9.0f} TRADE-msgs/s  "
              f"({cpu/deliveries*1e9:.0f} ns/delivery server-cpu)")
    finally:
        srv.stop()


def bench_deep_book():
    srv = Srv(_port(3))
    try:
        t = c(srv.port); t.sendall(b"LOGIN t\n")
        time.sleep(0.1); drain(t)
        LEVELS = 20_000
        t0 = time.time()
        for p in range(1, LEVELS + 1):
            t.sendall(b"BUY JNST 1 %d\n" % p)
            if p % 2000 == 0:
                drain(t)
        drain(t)
        dt = time.time() - t0
        print(f"deep book       : {LEVELS/dt:9.0f} resting-orders/s inserted "
              f"({LEVELS} price levels)")
        # now hit each level with an exact match
        s = c(srv.port); s.sendall(b"LOGIN s\n")
        time.sleep(0.1); drain(s)
        cpu0 = srv.cpu_seconds()
        t0 = time.time()
        for p in range(1, LEVELS + 1):
            s.sendall(b"SELL JNST 1 %d\n" % p)
            if p % 2000 == 0:
                drain(s)
        drain(s)
        dt = time.time() - t0
        cpu = srv.cpu_seconds() - cpu0
        print(f"deep book match : {LEVELS/dt:9.0f} matches/s "
              f"({cpu/LEVELS*1e6:.2f} us/match server-cpu)")
    finally:
        srv.stop()


def bench_idle_scaling():
    resource.setrlimit(resource.RLIMIT_NOFILE, (200_000, 200_000))
    srv = Srv(_port(4))
    try:
        conns = []
        for target in (1000, 5000, 10000, 20000, 40000, 70000):
            try:
                while len(conns) < target:
                    conns.append(c_from(srv.port, len(conns)))
            except OSError as e:
                print(f"idle {target:6d}   : could not establish ({e})")
                break
            time.sleep(0.5)
            # One persistent probe connection; round-trip an ERROR reply so we
            # measure the server's event-loop latency, not connection churn.
            a = c_from(srv.port, 12345)
            a.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            lat = []
            rounds = int(os.environ.get("BENCH_ROUNDS", "300"))
            for _ in range(rounds):
                t0 = time.time()
                a.sendall(b"PING\n")
                readline(a)
                lat.append(time.time() - t0)
            a.close()
            lat.sort()
            rss = srv.rss_kb()
            try:
                fds = len(os.listdir(f"/proc/{srv.proc.pid}/fd"))
            except OSError:
                try:
                    out = subprocess.check_output(["procstat", "-f", str(srv.proc.pid)], text=True)
                    fds = len(out.strip().split("\n")) - 1
                except subprocess.SubprocessError:
                    fds = 0
            print(f"idle {target:6d}   : p50={lat[len(lat)//2]*1e6:8.0f}us "
                  f"p99={lat[int(len(lat)*0.99)]*1e6:8.0f}us  "
                  f"rss={rss/1024:6.1f}MB fds={fds}")
    finally:
        srv.stop()


ALL = {
    "throughput": bench_throughput,
    "fanout": bench_fanout,
    "deepbook": bench_deep_book,
    "idle": bench_idle_scaling,
}


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "run"
    which = sys.argv[2:] or list(ALL)
    print(f"=== bench [{label}] ===")
    for name in which:
        ALL[name]()


if __name__ == "__main__":
    main()

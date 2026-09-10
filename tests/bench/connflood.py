#!/usr/bin/env python3
"""
Idle-connection generator for the §6.9 connection-scalability bonus.

Opens and *holds* N TCP connections to the Exchange Server without sending any
application-level data, so the server's per-connection resource use can be
measured with sockstat / procstat / netstat / top while exactly N connections
are established and idle.

    python3 tests/bench/connflood.py <host> <port> <count> [--ramp N] [--hold S]

It prints a line every --ramp connections so you can pause and take
measurements, and keeps every socket open until Ctrl-C or --hold seconds pass.
Uses the socket API directly (socket/connect); no framework.

Reaching a high count (run tests/bench/freebsd-setup.sh once, as root, first):
  * raise THIS process's fd limit:   ulimit -n 200000   (before running)
  * raise system limits (FreeBSD):   sysctl kern.maxfiles kern.maxfilesperproc
  * a single source IP only has ~64K ephemeral ports to one destination, so
    for >60K connections use several loopback source IPs. On FreeBSD those
    must be added as aliases first:
        for i in 2 3 4 5 6 7 8; do ifconfig lo0 alias 127.0.0.$i/8; done
    then pass  --src-aliases 8  (Linux routes 127.0.0.0/8 without aliases).
"""

from __future__ import annotations

import argparse
import resource
import signal
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("count", type=int)
    ap.add_argument("--ramp", type=int, default=5000,
                    help="print progress every RAMP connections")
    ap.add_argument("--hold", type=float, default=0.0,
                    help="seconds to hold after reaching count (0 = until Ctrl-C)")
    ap.add_argument("--src-aliases", type=int, default=1,
                    help="rotate over 127.0.0.1..127.0.0.<N> as source address")
    args = ap.parse_args()

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = args.count + 64
    if soft < want:
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (min(want, hard), hard))
        except (ValueError, OSError):
            print(f"warning: fd soft limit {soft} < needed {want}; "
                  f"run 'ulimit -n {want}'", file=sys.stderr)

    conns: list[socket.socket] = []

    def cleanup(*_):
        for s in conns:
            try:
                s.close()
            except OSError:
                pass
        print(f"\nclosed {len(conns)} connections")
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    t0 = time.monotonic()
    for i in range(args.count):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if args.src_aliases > 1:
            try:
                s.bind((f"127.0.0.{1 + i % args.src_aliases}", 0))
            except OSError:
                pass
        try:
            s.connect((args.host, args.port))
        except OSError as exc:
            print(f"stopped at {len(conns)} connections: {exc}")
            break
        conns.append(s)
        if (i + 1) % args.ramp == 0:
            rate = (i + 1) / (time.monotonic() - t0)
            print(f"established {i + 1:>7} connections  ({rate:,.0f}/s) "
                  f"-- take measurements now")

    print(f"holding {len(conns)} idle connections")
    if args.hold > 0:
        time.sleep(args.hold)
        cleanup()
    else:
        while True:
            time.sleep(3600)


if __name__ == "__main__":
    sys.exit(main())

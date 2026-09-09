#!/usr/bin/env python3
# Opens N TCP connections to the exchange server and keeps them open, without
# sending anything. Used for the section 6.9 bonus: hold M idle connections and
# then measure the server's memory / fds / cpu with sockstat, procstat, top.
#
#   python3 connflood.py <host> <port> <count> [src_aliases]
#
# One source IP has only ~64k ephemeral ports to a single destination, so for
# more than ~60k connections give a src_aliases count and first add the
# aliases (FreeBSD: `ifconfig lo0 alias 127.0.0.2/8` ... ; Linux already has
# all of 127.0.0.0/8).  Raise the fd limit first: `ulimit -n 200000`.

import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
aliases = int(sys.argv[4]) if len(sys.argv) > 4 else 1

conns = []
start = time.time()
for i in range(count):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if aliases > 1:
        try:
            s.bind(("127.0.0.%d" % (1 + i % aliases), 0))
        except OSError:
            pass
    try:
        s.connect((host, port))
    except OSError as e:
        print("stopped at %d connections: %s" % (len(conns), e))
        break
    conns.append(s)
    if (i + 1) % 5000 == 0:
        rate = (i + 1) / (time.time() - start)
        print("%d connections open (%.0f/s) - measure now" % (i + 1, rate))

print("holding %d idle connections, press Ctrl-C to stop" % len(conns))
try:
    while True:
        time.sleep(3600)
except KeyboardInterrupt:
    pass

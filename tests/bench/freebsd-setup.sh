#!/bin/sh
# One-time host preparation for the §6.9 connection-scalability bonus on FreeBSD.
# Run as root:  su -m root -c 'sh tests/bench/freebsd-setup.sh'
#
# These are host tunables, not part of the Exchange Server. The server itself
# only raises its own RLIMIT_NOFILE (see raise_fd_limit in src/server.cpp).

set -eu

echo "== raising system file-descriptor limits =="
sysctl kern.maxfiles=1000000
sysctl kern.maxfilesperproc=800000

echo "== widening the ephemeral port range =="
sysctl net.inet.ip.portrange.first=10000
sysctl net.inet.ip.portrange.last=65535

echo "== raising the socket / listen backlog ceiling =="
sysctl kern.ipc.somaxconn=4096
sysctl kern.ipc.maxsockets=1000000

echo "== adding loopback aliases so the client can use many source IPs =="
for i in 2 3 4 5 6 7 8 9 10; do
    ifconfig lo0 alias "127.0.0.$i/8" 2>/dev/null || true
done
ifconfig lo0 | grep 'inet 127'

cat <<'EOF'

Done. In the shell that will run the client generator, also raise the per-shell
descriptor limit:

    limit descriptors 200000      # csh / tcsh (FreeBSD root default shell)
    ulimit -n 200000              # sh / bash

Then, e.g.:

    make clean && make POLLER=kqueue
    ./server/run-server 127.0.0.1 5000        # in terminal 1
    python3 tests/bench/connflood.py 127.0.0.1 5000 70000 \
            --ramp 10000 --hold 600 --src-aliases 9    # in terminal 2

Measure in terminal 3 while it holds:
    sockstat -4 | grep -c ':5000'
    procstat -v $(pgrep exchange_server) | wc -l      # server open FDs
    ps -o rss,%cpu -p $(pgrep exchange_server)
    sysctl kern.openfiles kern.maxfiles
    netstat -m | head                                 # mbuf / socket-buffer use
EOF

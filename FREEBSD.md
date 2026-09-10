# Running on FreeBSD 14.4

Everything below was verified on Linux/WSL as a proxy; run it once on the
actual VM before submitting. The VM needs: `git` (or the unpacked zip),
`gmake` **or** the base `make`, a C++17 compiler (base `clang++`), and
`python3` (`pkg install python3`).

## 1. Build

```sh
cd <submission-dir>
make                 # base bmake works; so does gmake
```

Produces `exchange_server`, `trader_client`, `market_data_client` at the root.
`server/run-server`, `client/run-trader`, `client/run-market-data` exec them.

Expected: three clean `clang++ -std=c++17 -O2` lines, no warnings.

## 2. Functional check

Terminal 1:
```sh
./server/run-server 127.0.0.1 5000
# -> Exchange Server listening on 127.0.0.1:5000 (event loop: poll)
```

Terminal 2:
```sh
./client/run-trader 127.0.0.1 5000 alice
BUY JNST 10 238
```
Terminal 3:
```sh
./client/run-trader 127.0.0.1 5000 bob
SELL JNST 10 238        # alice sees BOUGHT, bob sees SOLD
```
Terminal 4:
```sh
./client/run-market-data 127.0.0.1 5000 JNST   # sees TRADE JNST 10 238
```

## 3. Automated conformance + experiments

```sh
python3 tests/stress/run_all.py      # 36 protocol/concurrency assertions, expect "36 passed"
python3 experiment.py 1              # ... through 8; each prints a PID + cues
```

Use a second terminal with `sockstat -4 -p 5000`, `netstat -an | grep 5000`,
`procstat -f <pid>`, `tcpdump -i lo0 -n port 5000` for the experiment
screenshots.

## 4. Connection-scalability bonus (§6.9)

The server already raises its own `RLIMIT_NOFILE` and rejects excess
connections gracefully (no crash, no CPU spin). It does **not** pin socket
buffers, so per-connection memory stays ~0.3 KB of app state + kernel
auto-tuned buffers. Verified holding 72 000 idle connections in ~25 MB RSS.

### 4a. Prepare the host (once, as root)

```sh
su -m root -c 'sh tests/bench/freebsd-setup.sh'
```
This bumps `kern.maxfiles`, `kern.maxfilesperproc`, `kern.ipc.somaxconn`,
`kern.ipc.maxsockets`, widens `net.inet.ip.portrange`, and adds
`127.0.0.2 .. 127.0.0.10` as `lo0` aliases (needed because one source IP only
has ~64 K ephemeral ports to a single destination).

### 4b. Build the fast event loop

```sh
make clean && make POLLER=kqueue
./server/run-server 127.0.0.1 5000
# -> ... (event loop: kqueue)
```
(`poll` also holds 70 000 connections but spends ~70 ms per loop iteration at
that scale; `kqueue` is O(ready).)

### 4c. Generate and hold the connections

In a shell with a raised descriptor limit (`limit descriptors 200000` in
tcsh, or `ulimit -n 200000` in sh):

```sh
python3 tests/bench/connflood.py 127.0.0.1 5000 70000 \
        --ramp 10000 --hold 900 --src-aliases 9
```
It prints every 10 000 connections; pause there and record the table row.

### 4d. Measure (third terminal, while it holds)

```sh
PID=$(pgrep exchange_server)
sockstat -4 | grep -c ':5000'          # established connections
procstat -v $PID | wc -l               # server open FDs
ps -o rss,%cpu -p $PID                 # server memory / CPU
sysctl kern.openfiles kern.maxfiles    # system-wide FDs
netstat -m                             # mbuf clusters / socket-buffer memory
```

Fill the table in `OPTIMIZATION_REPORT.md` at 10 000 / 20 000 / … / 70 000 and
take the screenshots for 10 000, 40 000, 70 000 required by §6.9.1.

### 4e. Trade-off comparison (§6.9 Q5)

Repeat 4b–4d with `make clean && make POLLER=poll` and compare CPU / latency at
the same connection counts. `tests/bench/bench.py <label> idle` automates the
active-client-latency-vs-N measurement.

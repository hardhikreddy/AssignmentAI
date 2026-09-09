# COL334 Assignment 2 — The Socket Exchange
## Experiment Report

**Team:** `<roll number 1>`, `<roll number 2>`
**Language / build:** C++17, `clang++`/`g++`, `make`
**Environment:** FreeBSD 14.4-RELEASE (amd64) under `<VirtualBox / QEMU-KVM>`, 2 vCPU / `<N>` GB RAM

> **How to insert evidence:** each experiment has one or more
> `> [SCREENSHOT n: … ]` blocks. Replace each block with the image, e.g.
> `![Experiment 1 — sockstat](screenshots/exp1-sockstat.png)`.
> Put the image files in a `screenshots/` folder next to this report, then
> export to `report.pdf` (`pandoc report.md -o report.pdf` or print-to-PDF).

---

## Implementation Decisions

### Concurrency / I/O mechanism

The Exchange Server is a **single process with a single thread running one
event-driven readiness loop**. Every socket — the listening socket and every
client connection — is set non-blocking. The loop:

1. calls a readiness primitive that blocks until at least one socket needs
   attention;
2. `accept()`s all pending connections if the listening socket is ready;
3. for each ready client, `recv()`s available bytes into a per-connection
   input buffer, splits it on `\n`, and processes each complete message;
4. `send()`s as much of each client's queued output as the kernel accepts,
   keeping the remainder for later.

The readiness primitive is selected at build time through a small abstraction
(`src/event_poller.hpp`):

| Build | Primitive | Purpose |
|---|---|---|
| `make` (default) | `poll(2)` | portable; used for grading and all experiments below |
| `make POLLER=kqueue` | `kqueue` | O(ready) wakeups; used for the §6.9 scalability bonus |
| `make POLLER=epoll` | `epoll` | Linux-only, for cross-checking on a Linux host |

### Why this design

* **Isolation.** The assignment requires that one idle, slow, or
  abruptly-disconnecting client never disturbs the others (§4.3, §4.4). With a
  single `poll()` loop and non-blocking sockets the server never blocks inside
  `recv()` or `send()` for any one client — it only ever blocks in the
  readiness wait, which watches *all* clients simultaneously.
* **Cost.** No per-connection thread/process means no per-connection stack or
  scheduler overhead: application state is ~0.3 KB per connection, and the
  server holds 70 000+ idle connections in ~25 MB RSS. A thread-per-connection
  design would hit thread-stack and lock-contention limits well before the
  70 000-connection bonus target, for a workload that is I/O-bound rather than
  CPU-bound.
* **Observability.** A single explicit loop makes the underlying TCP
  behaviour — byte-stream framing, partial reads/writes, backpressure,
  FIN/RST detection — directly visible, which is the purpose of the
  experiments.

### Other decisions relevant to networking

* **Message framing.** Per-connection byte buffer, split on `\n` (a trailing
  `\r` is tolerated). A read cursor is advanced instead of erasing from the
  front, so re-framing is linear, not quadratic. A 64 KiB per-connection input
  cap is a safety valve for out-of-spec input; hitting it closes only that one
  connection.
* **Output queueing.** One contiguous per-connection output buffer plus a
  write offset. On `EAGAIN` the unsent tail is kept and write-interest is
  registered with the readiness loop; it drains on a later iteration. An 8 MiB
  cap bounds a non-reading client (§ Experiment 7); exceeding it drops that one
  connection.
* **Order book.** Per `(instrument, side)`: `std::map<price, list<Order>>`,
  FIFO within a price level, plus `unordered_map<order_id, iterator>` for O(1)
  `CANCEL`. Matching occurs only at an exactly equal price (§2.6).
* **Roles.** A connection is unassigned until its first command: `LOGIN`
  ⇒ Trader, `SUBSCRIBE`/`UNSUBSCRIBE` ⇒ Market-Data. A command not permitted
  for the established role is answered with `ERROR` and ignored (§4.5).
* **Connection lifetime.** A resting order is keyed to the submitting
  connection, not the username. It survives that client's disconnect and can
  still match later; no `BOUGHT`/`SOLD` is sent to the departed client, but
  subscribers still receive the `TRADE` (§2.6, confirmed on Piazza).
* **Scalability.** At startup the server raises its own `RLIMIT_NOFILE` soft
  limit to the hard limit, does **not** pin `SO_SNDBUF`/`SO_RCVBUF` (which
  would reserve tens of GiB across 70 000 sockets), and, when the descriptor
  ceiling is reached, rejects new connections without crashing or busy-spinning.

---

## Experiment 1 — Listening and Connected Sockets

**Run:** `python3 experiment.py 1`

**Question.** After the client has connected, identify the TCP sockets
associated with the Exchange Server. How does the listening socket differ from
the socket representing the connection to the client?

**Approach.** Start the experiment (server + one idle client). In a second
terminal, list the server's sockets and the server process's open file
descriptors, and read the TCP state and endpoint columns.

**Commands / tools.**
```sh
sockstat -4 | grep ':5000'
netstat -an -p tcp | grep '\.5000'
procstat -f $(pgrep exchange_server)
```

**Observation.**

> [SCREENSHOT 1a: `sockstat`/`netstat` output showing one `LISTEN` line for
> 127.0.0.1:5000 with no foreign address, plus `ESTABLISHED` line(s) for
> 127.0.0.1:5000 ↔ 127.0.0.1:<ephemeral> ]

> [SCREENSHOT 1b: `procstat -f` for the server PID showing the listening
> socket fd and a separate fd for the accepted connection ]

**Answer.** The server has two kinds of TCP socket. The **listening socket**
(from `socket()`+`bind()`+`listen()`) is in state `LISTEN`, is bound to the
local address `127.0.0.1:5000`, and has **no foreign address** — it never
carries data, it only produces new connections via `accept()`. Each
**connected socket** (one per client, returned by `accept()`) is in state
`ESTABLISHED` and is identified by the full 4-tuple: local `127.0.0.1:5000`
and a specific remote `127.0.0.1:<ephemeral-port>`. All application data
travels on the connected sockets; the listening socket and each connection are
distinct file descriptors in the server process.

---

## Experiment 2 — Observing TCP Connection States

**Run:** `python3 experiment.py 2`

**Question.** How does the TCP state of the client–server connection change
during its lifetime, and what events cause the observed state changes?

**Approach.** Observe the connection at three points the harness pauses for:
(1) while it is being established, (2) while established and idle, (3) after
the client closes it. Capture packets throughout to correlate states with
segments.

**Commands / tools.**
```sh
tcpdump -i lo0 -n 'tcp port 5000'          # in a terminal before the run
netstat -an -p tcp | grep '\.5000'         # repeat at each phase
sockstat -4 | grep ':5000'
```

**Observation.**

> [SCREENSHOT 2a: `netstat` during phase 1/2 — both endpoints `ESTABLISHED` ]

> [SCREENSHOT 2b: `netstat` right after the client closes — server side
> `CLOSE_WAIT` then gone; client side `FIN_WAIT_2` / `TIME_WAIT` ]

> [SCREENSHOT 2c: `tcpdump` showing SYN → SYN,ACK → ACK at start and
> FIN,ACK → ACK → FIN,ACK → ACK at close ]

**Answer.** The connection progresses:
`SYN_SENT`/`SYN_RCVD` → **`ESTABLISHED`** on completion of the three-way
handshake (SYN, SYN-ACK, ACK). It stays `ESTABLISHED` with no packets while
idle. When the client calls `close()`, it sends a **FIN**: the client moves
`FIN_WAIT_1`→`FIN_WAIT_2`; the server's socket moves to **`CLOSE_WAIT`** and
the server's `recv()` returns 0 (EOF). The server then flushes any queued
reply and closes, sending its own FIN; the server socket reaches `LAST_ACK`
then `CLOSED`, and the client — the side that closed first — sits in
**`TIME_WAIT`** for 2·MSL before `CLOSED`. Each state change is driven by a
handshake or FIN segment, or by the application calling `close()`.

---

## Experiment 3 — TCP as a Byte Stream

**Run:** `python3 experiment.py 3`

**Question.** Does the Exchange Server receive the application-level message as
one complete unit, or can it be received in multiple pieces? Explain what this
demonstrates about the relationship between TCP and application-level message
boundaries.

**Approach.** The harness sends the single message
`LOGIN experiment_trader\n` as four separate `send()` calls
(`"LOGIN "`, `"experiment"`, `"_trader"`, `"\n"`) with pauses between them.
Watch the segments on the wire and the server's `recv()` syscalls, and confirm
the server only replies `OK` after the final `\n`.

**Commands / tools.**
```sh
tcpdump -i lo0 -n -X 'tcp port 5000'
truss -f -p $(pgrep exchange_server)        # or: ktrace -p <pid> ; kdump
```

**Observation.**

> [SCREENSHOT 3a: `tcpdump` showing ~4 separate PSH segments carrying
> "LOGIN ", "experiment", "_trader", "\n" ]

> [SCREENSHOT 3b: `truss`/`kdump` showing multiple `recvfrom`/`read` returns
> (6, 10, 7, 1 bytes) and only one `sendto` of "OK\n" — after the last one ]

**Answer.** The server receives the message **in multiple pieces**: each
`recv()` returns only the bytes that have arrived so far (four partial
returns), not one framed message. The server buffers these bytes per
connection and does nothing with them until it sees the `\n`; only then does
it parse `LOGIN experiment_trader` and reply `OK`. This demonstrates that TCP
is a **byte stream with no message boundaries** — `send()` calls do not
correspond one-to-one to `recv()` calls — so the application must implement
its own framing, which here is the newline delimiter.

---

## Experiment 4 — One Client Should Not Stall the Others

**Run:** `python3 experiment.py 4`

**Question.** When Client 1 remains connected but sends no data, can the
Exchange Server still accept and service Client 2? Identify the server
operation that determines the answer.

**Approach.** Client 1 connects and sends a partial line
(`LOGIN blocked_client`, no newline), then stays silent. Two seconds later
Client 2 connects and sends a complete `LOGIN active_client\n`; the harness
times how long the `OK` takes. Separately, inspect what the server process is
doing while Client 1 is idle.

**Commands / tools.**
```sh
truss -p $(pgrep exchange_server)           # server is parked in poll(), not recv()
sockstat -4 | grep ':5000'                  # both connections ESTABLISHED
tcpdump -i lo0 -n 'tcp port 5000'
```

**Observation.** Harness output: `Client 2 response: 'OK'`,
`Elapsed time: ~0.001 seconds`.

> [SCREENSHOT 4a: harness output — Client 2 gets `OK` in ≈0 s while Client 1
> is silent ]

> [SCREENSHOT 4b: `truss`/`ktrace` of the server showing it blocked in
> `poll()` (not `recvfrom` on Client 1) and then handling Client 2 ]

> [SCREENSHOT 4c: `sockstat` showing both connections ESTABLISHED throughout ]

**Answer.** Yes — Client 2 is served immediately. The determining operation is
the server's **`poll()` call**: it is the only place the server ever blocks,
and it waits on *all* sockets at once. Because every socket is non-blocking,
the server never sits inside `recv()` or `send()` for one client. Client 1
having no data simply means `poll()` does not report it readable; `poll()`
still reports the listening socket and Client 2, which are serviced normally.
A blocking `recv(client1)` would have frozen the entire server.

---

## Experiment 5 — Multiple Clients and I/O Multiplexing *(optional)*

**Run:** `python3 experiment.py 5`

**Question.** At a particular point during the experiment, which client
connections are actually ready for the server to service, and what evidence
from the running system allows you to determine this?

**Approach.** Five connections are opened; only clients 1, 3, and 5 send a
`LOGIN`. Inspect which sockets have unread data and what `poll()` reports.

**Commands / tools.**
```sh
netstat -an -p tcp | grep '\.5000'         # Recv-Q column
truss -p $(pgrep exchange_server)           # poll() return value + ready fds
sockstat -4 | grep ':5000'
```

**Observation.**

> [SCREENSHOT 5a: `netstat` showing all 5 connections ESTABLISHED, but
> Recv-Q non-zero only for clients 1/3/5 at the moment they send ]

> [SCREENSHOT 5b: `truss` showing `poll(...) = 3` and three `recvfrom` calls
> for exactly those fds, then the loop returning to `poll()` ]

**Answer.** Only the connections with pending input are "ready" — clients 1, 3
and 5 at the instant they send (plus the listening socket when a new
connection is waiting). Clients 2 and 4 are `ESTABLISHED` but idle, so
`poll()` does not return them. The evidence: `poll()`'s return value equals
the number of ready fds and its `revents` name exactly clients 1/3/5; the
`netstat` Recv-Q is non-zero only for those sockets; and `sockstat` shows all
five as connected — demonstrating that "connected" is not the same as "ready".

---

## Experiment 6 — FIN vs. RST: Orderly and Abrupt Termination

**Run:** `python3 experiment.py 6`

**Question.** How does an abrupt client termination differ from the orderly
shutdown? Identify the TCP event observed on the network and the corresponding
behaviour of the Exchange Server's socket.

**Approach.** Part A: the client calls `shutdown(SHUT_WR)` (orderly). Part B: a
second client sets `SO_LINGER` to `{on, 0}` and closes (abortive). Capture the
packets and the server-side socket state for each.

**Commands / tools.**
```sh
tcpdump -i lo0 -n 'tcp port 5000'          # compare F (FIN) vs R (RST)
netstat -an -p tcp | grep '\.5000'         # TIME_WAIT vs nothing
truss -p $(pgrep exchange_server)           # recvfrom returning 0 vs ECONNRESET
```

**Observation.**

> [SCREENSHOT 6a: `tcpdump` of Part A — `F` flag, four-way FIN/ACK exchange ]

> [SCREENSHOT 6b: `tcpdump` of Part B — a single `R` (RST) flag, no FIN, no
> final ACK ]

> [SCREENSHOT 6c: `netstat` — Part A leaves a `TIME_WAIT`; Part B leaves
> nothing (connection vanishes immediately) ]

**Answer.**
* **Orderly (FIN).** The client sends a **FIN**. The server's `recv()` returns
  0; its socket enters `CLOSE_WAIT`; the server finishes any queued write and
  closes, producing a normal four-way termination. The side that closed first
  passes through `TIME_WAIT`. No data is lost.
* **Abrupt (RST).** The client sends a **RST**. The server's next `recv()` (or
  `poll()`) reports the error `ECONNRESET`/`POLLHUP`; there is **no
  `CLOSE_WAIT` and no `TIME_WAIT`** — the socket is torn down immediately and
  any bytes still in the server's receive buffer are discarded. The server
  detects this, marks the connection closing, and calls `close_client()`
  (removing it from the poller and all indexes). Per §2.6 any resting order
  from that client remains in the book.

---

## Experiment 7 — Backpressure and the Slow Receiver

**Run:** `python3 experiment.py 7`

**Question.** How does the Exchange Server's TCP connection to the slow client
behave as the client stops reading, and what evidence shows whether this
eventually affects the server's ability to communicate with other clients?

**Approach.** Two Market-Data clients subscribe to `JNST`; one keeps reading,
the other never reads. Two traders generate a sustained stream of matching
trades. Watch the send queue and TCP window on both Market-Data connections
and confirm the normal client keeps receiving updates.

**Commands / tools.**
```sh
netstat -an -p tcp | grep '\.5000'         # Send-Q for the slow vs normal conn
tcpdump -i lo0 -n 'tcp port 5000'          # window size; "win 0" from slow client
sockstat -4 | grep ':5000'
```

**Observation.**

> [SCREENSHOT 7a: `netstat` — Send-Q large and growing on the slow
> connection, ≈0 on the normal connection ]

> [SCREENSHOT 7b: `tcpdump` — the slow client advertising `win 0`
> (zero-window) and the server pausing sends to it, while data keeps flowing
> to the normal client ]

**Answer.** As the slow client stops reading, its kernel receive buffer fills,
then the server's kernel send buffer for that socket fills; the slow client
advertises a **zero TCP window** and the server's `send()` returns `EAGAIN`.
The server does **not** block: it stops writing to that socket and holds the
unsent bytes in that connection's application output queue (bounded at 8 MiB;
if exceeded, only that one connection is dropped). The **normal client and the
traders are unaffected** — they keep receiving `TRADE` messages at full rate,
because the event loop never waits on the slow socket. Evidence: the growing
Send-Q on the slow connection versus a near-zero Send-Q on the normal one, and
continuous data to the normal client in the capture. A single slow reader is
therefore isolated and does not degrade service to others.

---

## Experiment 8 — Unexpected Client Disconnection

**Run:** `python3 experiment.py 8`

**Question.** When a client disappears unexpectedly while the Exchange Server
is communicating with it, what happens to their TCP connection, and what
network-level evidence allows you to determine how the server detects the
failure?

**Approach.** A separate Market-Data **process** subscribes to `JNST` and
stays connected while trades flow to it; a second Market-Data connection stays
up for comparison. The first process is then `SIGKILL`ed. Capture the packets
around the kill and compare the two connections before and after.

**Commands / tools.**
```sh
tcpdump -i lo0 -n 'tcp port 5000'          # FIN/RST from the killed client's port
sockstat -4 | grep ':5000'                 # dead conn disappears, survivor stays
procstat -f $(pgrep exchange_server) | wc -l   # server fd count drops by one
```

**Observation.**

> [SCREENSHOT 8a: `tcpdump` at the moment of the kill — FIN (or RST) from the
> dead client's ephemeral port, then the server's response ]

> [SCREENSHOT 8b: `sockstat` before vs after — the killed connection is gone,
> the surviving Market-Data connection is still `ESTABLISHED` and receiving ]

**Answer.** When the process is killed, the OS closes its sockets, so the
kernel sends a **FIN** (or a **RST** if data is in flight / arrives
afterwards) to the server. TCP has no other "client gone" signal, so the
server learns of the failure only by doing I/O: `poll()` reports
`POLLHUP`/`POLLERR`, or the next `send()` fails with `EPIPE`/`ECONNRESET`, or
`recv()` returns 0/`ECONNRESET`. The server then `close_client()`s that
connection — it leaves the socket table, and the server's fd count drops by
one. The **surviving** connection is completely unaffected. The network
evidence is the lone FIN/RST from the dead port in the capture, together with
the connection disappearing from `sockstat` while the other remains.

---

## Bonus — Connection Scalability and I/O Design (§6.9)

**Setup.** `sh tests/bench/freebsd-setup.sh` (as root) raises `kern.maxfiles`,
`kern.maxfilesperproc`, `kern.ipc.somaxconn`, `kern.ipc.maxsockets`, widens
the ephemeral port range, and adds `lo0` aliases `127.0.0.2–10`. Build:
`make clean && make POLLER=kqueue`. Generate load:
`python3 tests/bench/connflood.py 127.0.0.1 5000 <N> --ramp 10000 --hold 900
--src-aliases 9` (shell with `limit descriptors 200000`).

**Design note.** The server maintains **no thread or process per connection** —
it multiplexes all sockets in one `kqueue` loop. Per-connection cost is
therefore just the socket, a file descriptor, and ~0.3 KB of application
state; there is no per-connection stack or scheduler entry.

### Resource-measurement table (FreeBSD VM)

Measured with `procstat -v <pid> | wc -l` (server FDs),
`ps -o rss,%cpu -p <pid>`, `sysctl kern.openfiles kern.maxfiles`,
`sockstat -4 | grep -c :5000`, `netstat -m`.

| Idle connections | Server memory (RSS) | Server CPU | Server open FDs | System-wide open FDs | Socket-buffer used / limit | Max connections established |
|---:|---|---|---|---|---|---|
| 10 000 | | | | | | |
| 20 000 | | | | | | |
| 30 000 | | | | | | |
| 40 000 | | | | | | |
| 50 000 | | | | | | |
| 60 000 | | | | | | |
| 70 000 | | | | | | |

> [SCREENSHOT B1: commands + output at 10 000 idle connections — connection
> count and all measurements visible ]

> [SCREENSHOT B2: same at 40 000 idle connections ]

> [SCREENSHOT B3: same at 70 000 idle connections ]

### Analysis

*(Local reference — WSL2 Linux proxy, optimized server: `poll` and `kqueue`
builds both held 72 000 idle connections at ~25–28 MB RSS and ~0 % CPU while
idle. Replace with the FreeBSD figures once measured.)*

1. **Does the server maintain all requested connections?**
   `<yes / no — if it fails, state the count and the OS error>`. The server
   raises its own `RLIMIT_NOFILE`, so the ceiling is the system limits
   (`kern.maxfiles`/`kern.maxfilesperproc`), not an inherited soft limit.

2. **First significant bottleneck.** With the `poll` build it is **CPU in the
   event loop**: `poll()` is handed the entire descriptor array on every
   wakeup and scans it, so per-iteration cost grows linearly with the *total*
   connection count even though almost all are idle — measured active-client
   round-trip latency rises roughly in proportion to N (≈1 ms per 1 000 idle
   connections locally). Memory is **not** the constraint in this range
   (~0.3 KB app state per connection). `<confirm which resource saturates
   first on the VM from the table — expect FDs or CPU, not RSS>`.

3. **How the concurrency/I/O design contributes.** The server uses a single
   thread with level-triggered readiness multiplexing — no per-connection
   thread/process (hence the small RSS and single scheduler entity), but the
   portable `poll(2)` interface forces O(total connections) work per wakeup.
   The cost is proportional to how many sockets exist, not how many are active.

4. **Would changing the mechanism help?** Yes. `kqueue` (FreeBSD) delivers only
   the sockets that are actually ready, making each wakeup O(ready) instead of
   O(total). Locally, active-client latency with the `epoll`/`kqueue`-style
   loop stays roughly flat (~0.4 ms) from 1 000 to 20 000 idle connections
   instead of growing linearly. Switching instead to thread-per-connection
   would **not** help — it would replace the `poll` scan with per-thread stack
   memory (MBs each) and scheduler/lock contention, capping the connection
   count far below 70 000.

5. **Trade-off.** `kqueue`/`epoll` add one `kevent`/`epoll_ctl` syscall each
   time a connection's read/write interest changes; on a small, trade-saturated
   deployment this costs ≈10 % throughput versus plain `poll` (the server
   minimises it by only calling the kernel on an actual interest change). For
   the mostly-idle many-connection regime `kqueue` is a large net win. That is
   why `poll` is the default build and `kqueue` is opt-in for this bonus.
   `<add your before/after numbers from `tests/bench/bench.py` and the table>`.

> [SCREENSHOT B4: `tests/bench/bench.py` idle-scaling output for the `poll`
> build vs the `kqueue` build, showing latency-vs-N ]

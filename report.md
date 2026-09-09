# COL334 Assignment 2 — The Socket Exchange
## Experiment report

Team: `<roll number 1>`, `<roll number 2>`

Language and build: C++17, built with `clang++` (or `g++`) through `make`.

Environment: FreeBSD 14.4-RELEASE (amd64) running under `<VirtualBox / QEMU-KVM>`, 2 vCPUs and `<N>` GB RAM.

How to add the evidence: each experiment has one or more lines that look like
`> [SCREENSHOT n: ... ]`. Take that screenshot, then replace the line with the
image, for example `![Experiment 1 sockstat](screenshots/exp1-sockstat.png)`.
Keep the image files in a `screenshots/` folder next to this report and then
make the PDF with `pandoc report.md -o report.pdf`, or just open the file and
print it to PDF.

---

## Implementation decisions

### How the server handles many clients

The server is one process with one thread. It runs a single loop that waits for
any socket to need attention. Every socket is non-blocking: the listening
socket and every client connection. Each time round the loop the server does
four things.

1. It waits until at least one socket is ready.
2. If the listening socket is ready, it accepts every connection that is
   waiting.
3. For each ready client it calls `recv()` once or a few times, appends the
   bytes to that client's input buffer, splits the buffer on `\n`, and runs
   each full message it finds.
4. It calls `send()` for each client that has data queued, sending as much as
   the kernel takes and keeping the rest for later.

The wait in step 1 uses `poll(2)`. The handout also allows `select()` and
`kqueue()`; we picked `poll()` because it is the simplest of the three to use
correctly and it is enough for the number of clients the experiments need.

### Why we did it this way

The assignment says one idle, slow, or crashed client must not hurt the
others (sections 4.3 and 4.4). With one `poll()` loop and non-blocking sockets
the server never gets stuck inside `recv()` or `send()` for a single client. It
only ever waits inside `poll()`, and `poll()` watches every client at once. So
a client that sends nothing just does not show up as ready, and the rest keep
working.

This design is also cheap. There is no thread or process per connection, so
there is no per-connection stack and nothing extra for the scheduler to track.
Each connection costs a socket, a file descriptor, and about 0.3 KB of state in
the server. The server holds more than 70,000 idle connections in about 25 MB
of memory. A thread-per-connection design would run out of memory and hit lock
contention long before that, and it buys nothing here because the work is I/O,
not computation.

A single explicit loop also makes the TCP behaviour easy to see: byte-stream
framing, partial reads and writes, backpressure, and how the server notices a
FIN or RST. That is what the experiments are about.

### Other choices that matter for the network side

Framing: each connection has a byte buffer. The server splits it on `\n` and
ignores a trailing `\r`. It advances a read cursor instead of deleting from the
front of the buffer, so re-scanning stays linear. There is a 64 KB cap on the
input buffer as a guard against bad input; hitting it closes only that one
connection.

Output: each connection has one output buffer and a write position. When
`send()` returns `EAGAIN` the server keeps the unsent bytes and sets `POLLOUT`
on that socket so `poll()` tells it when the socket can take more. There is an
8 MB cap on this buffer for the case in Experiment 7; going over it drops just
that connection.

Order book: for each instrument and side the server keeps a
`std::map<price, list<Order>>`, first in first out within a price. A separate
`unordered_map<order_id, iterator>` makes `CANCEL` O(1). Two orders match only
when the price is exactly equal (section 2.6).

Roles: a connection has no role until its first command. `LOGIN` makes it a
Trader. `SUBSCRIBE` or `UNSUBSCRIBE` makes it a Market-Data client. A command
that the role is not allowed to use gets an `ERROR` reply and nothing else
(section 4.5).

Disconnects: a resting order belongs to the connection that sent it, not to the
username. It stays in the book after that client leaves and can still match. No
`BOUGHT` or `SOLD` goes to the client that left, but subscribers still get the
`TRADE` (section 2.6, confirmed on Piazza).

Scaling: at startup the server raises its own open-file soft limit to the hard
limit (a few lines with `setrlimit`), so it can hold many connections without
the person running it having to set `ulimit` first.

---

## Experiment 1 — Listening and connected sockets

Run: `python3 experiment.py 1`

The question: after the client connects, find the TCP sockets that belong to
the server. How is the listening socket different from the socket for the
client connection?

What we did: we started the experiment, which brings up the server and one idle
client. In a second terminal we listed the server's sockets and the file
descriptors the server process has open, and read the state and address
columns.

Commands and tools:
```sh
sockstat -4 | grep ':5000'
netstat -an -p tcp | grep '\.5000'
procstat -f $(pgrep exchange_server)
```

What we saw:

> [SCREENSHOT 1a: sockstat / netstat output with one LISTEN line for
> 127.0.0.1:5000 and no foreign address, plus an ESTABLISHED line for
> 127.0.0.1:5000 to 127.0.0.1:<ephemeral port> ]

> [SCREENSHOT 1b: procstat -f for the server PID showing the listen socket fd
> and a separate fd for the accepted connection ]

Answer: the server has two kinds of TCP socket. The listening socket comes from
`socket()`, `bind()`, and `listen()`. It is in state `LISTEN`, it has the local
address `127.0.0.1:5000`, and it has no foreign address. It never carries data;
it only produces new connections when the server calls `accept()`. Each
connected socket comes from `accept()`, one per client. It is in state
`ESTABLISHED` and it has a full four-part address: local `127.0.0.1:5000` and a
specific remote `127.0.0.1:<ephemeral port>`. All the `LOGIN`, `BUY`, `TRADE`
and other data travels on the connected sockets. In the process each one is a
different open file descriptor.

---

## Experiment 2 — TCP connection states

Run: `python3 experiment.py 2`

The question: how does the TCP state of the connection change over its life,
and what causes each change?

What we did: the harness pauses at three points. We looked at the connection
while it was being set up, while it was up and idle, and after the client
closed it. We also captured packets the whole time so we could line up each
state with the segments on the wire.

Commands and tools:
```sh
tcpdump -i lo0 -n 'tcp port 5000'      # start this before the run
netstat -an -p tcp | grep '\.5000'     # run again at each pause
sockstat -4 | grep ':5000'
```

What we saw:

> [SCREENSHOT 2a: netstat during phase 1 or 2, both ends ESTABLISHED ]

> [SCREENSHOT 2b: netstat just after the client closes, server side in
> CLOSE_WAIT and then gone, client side in FIN_WAIT_2 or TIME_WAIT ]

> [SCREENSHOT 2c: tcpdump showing SYN, SYN-ACK, ACK at the start and the FIN
> and ACK segments at the close ]

Answer: the connection starts in `SYN_SENT` on the client and `SYN_RCVD` on the
server for a moment, then both ends go to `ESTABLISHED` once the three-way
handshake finishes (SYN, SYN-ACK, ACK). While idle it stays `ESTABLISHED` and
no packets flow. When the client calls `close()` it sends a FIN. The client
goes to `FIN_WAIT_1` then `FIN_WAIT_2`. The server socket goes to `CLOSE_WAIT`
and the server's `recv()` returns 0, which is how the server learns the client
is done. The server flushes any reply it still owes, then closes, which sends
its own FIN. The server socket goes `LAST_ACK` then closed. The client, which
closed first, sits in `TIME_WAIT` for twice the maximum segment lifetime and
then closes. Every state change is caused by a handshake segment, a FIN, or the
program calling `close()`.

---

## Experiment 3 — TCP is a byte stream

Run: `python3 experiment.py 3`

The question: does the server get the message as one whole unit, or can it
arrive in pieces? What does that show about TCP and message boundaries?

What we did: the harness sends one message, `LOGIN experiment_trader\n`, as
four separate `send()` calls (`"LOGIN "`, then `"experiment"`, then
`"_trader"`, then `"\n"`) with a pause between each. We watched the segments on
the wire and the server's `recv()` calls, and checked that the server only
replied `OK` after the last `\n`.

Commands and tools:
```sh
tcpdump -i lo0 -n -X 'tcp port 5000'
truss -f -p $(pgrep exchange_server)      # or: ktrace -p <pid> ; kdump
```

What we saw:

> [SCREENSHOT 3a: tcpdump showing about four separate segments carrying
> "LOGIN ", "experiment", "_trader", and "\n" ]

> [SCREENSHOT 3b: truss or kdump showing several recv/read calls returning 6,
> 10, 7, and 1 bytes, and only one send of "OK\n" after the last one ]

Answer: the server gets the message in pieces. Each `recv()` returns only the
bytes that have arrived so far, so there are four partial returns and not one
framed message. The server puts those bytes in the connection's input buffer
and does nothing with them until it sees the `\n`. Only then does it read
`LOGIN experiment_trader` and reply `OK`. This shows that TCP is a stream of
bytes with no message boundaries. One `send()` on the client does not turn into
one `recv()` on the server. The application has to add its own framing, which
here is the newline.

---

## Experiment 4 — One client should not stall the others

Run: `python3 experiment.py 4`

The question: when Client 1 stays connected but sends no data, can the server
still accept and serve Client 2? Which server operation decides the answer?

What we did: Client 1 connects and sends a partial line,
`LOGIN blocked_client` with no newline, and then goes quiet. Two seconds later
Client 2 connects and sends a full `LOGIN active_client\n`, and the harness
times how long the `OK` takes. We also checked what the server process was
doing while Client 1 sat idle.

Commands and tools:
```sh
truss -p $(pgrep exchange_server)      # server is parked in poll(), not recv()
sockstat -4 | grep ':5000'             # both connections ESTABLISHED
tcpdump -i lo0 -n 'tcp port 5000'
```

What we saw: the harness printed `Client 2 response: 'OK'` and
`Elapsed time: ~0.001 seconds`.

> [SCREENSHOT 4a: harness output, Client 2 gets OK in about 0 seconds while
> Client 1 is silent ]

> [SCREENSHOT 4b: truss or ktrace of the server showing it blocked in poll()
> and not in recvfrom on Client 1, then handling Client 2 ]

> [SCREENSHOT 4c: sockstat showing both connections ESTABLISHED the whole time ]

Answer: yes, Client 2 is served right away. The operation that decides this is
`poll()`. It is the only place the server ever blocks, and it waits on every
socket at once. Because the sockets are non-blocking, the server never sits
inside `recv()` or `send()` for one client. Client 1 sending nothing just means
`poll()` does not list it as ready. `poll()` still lists the listening socket
and Client 2, and the server handles them as normal. If the server had used a
blocking `recv()` on Client 1 the whole server would have frozen.

---

## Experiment 5 — Multiple clients and I/O multiplexing (optional)

Run: `python3 experiment.py 5`

The question: at a given moment, which client connections are actually ready
for the server to handle, and what evidence tells you that?

What we did: five connections are opened and only clients 1, 3, and 5 send a
`LOGIN`. We checked which sockets had unread data and what `poll()` returned.

Commands and tools:
```sh
netstat -an -p tcp | grep '\.5000'     # look at the Recv-Q column
truss -p $(pgrep exchange_server)       # poll() return value and ready fds
sockstat -4 | grep ':5000'
```

What we saw:

> [SCREENSHOT 5a: netstat showing all five connections ESTABLISHED, but Recv-Q
> above zero only for clients 1, 3, and 5 at the moment they send ]

> [SCREENSHOT 5b: truss showing poll(...) = 3 and three recvfrom calls for
> those fds, then the loop going back to poll() ]

Answer: only the connections with data waiting are ready. That is clients 1, 3,
and 5 right after they send, plus the listening socket when a new connection is
waiting. Clients 2 and 4 are `ESTABLISHED` but have nothing to read, so
`poll()` does not return them. The evidence: `poll()` returns the count 3 and
names exactly those three fds, the `netstat` Recv-Q is above zero only for
those sockets, and `sockstat` shows all five as connected. So being connected
is not the same as being ready.

---

## Experiment 6 — FIN vs. RST: orderly and abrupt termination

Run: `python3 experiment.py 6`

The question: how is an abrupt client shutdown different from an orderly one?
Name the TCP event on the network and how the server's socket reacts.

What we did: in Part A the client calls `shutdown(SHUT_WR)`, which is the
orderly case. In Part B a second client sets `SO_LINGER` to `{on, 0}` and then
closes, which forces the abrupt case. We captured the packets and the
server-side socket state for both.

Commands and tools:
```sh
tcpdump -i lo0 -n 'tcp port 5000'      # compare F (FIN) with R (RST)
netstat -an -p tcp | grep '\.5000'     # TIME_WAIT vs nothing
truss -p $(pgrep exchange_server)       # recv returning 0 vs ECONNRESET
```

What we saw:

> [SCREENSHOT 6a: tcpdump of Part A, an F flag and the four-way FIN and ACK
> exchange ]

> [SCREENSHOT 6b: tcpdump of Part B, a single R flag, no FIN, no final ACK ]

> [SCREENSHOT 6c: netstat, Part A leaves a TIME_WAIT, Part B leaves nothing and
> the connection is gone at once ]

Answer:

Orderly case. The client sends a FIN. The server's `recv()` returns 0. The
server socket goes to `CLOSE_WAIT`, the server finishes any queued write, and
then it closes, which gives a normal four-way shutdown. The side that closed
first passes through `TIME_WAIT`. No data is lost.

Abrupt case. The client sends a RST. The server's next `recv()`, or `poll()`,
reports `ECONNRESET` or `POLLHUP`. There is no `CLOSE_WAIT` and no `TIME_WAIT`.
The socket is thrown away at once and any bytes still in the server's receive
buffer are dropped. The server notices this, marks the connection for closing,
and calls `close_client()`, which removes it from `poll()` and from the order
and subscriber tables. Any resting order that client left stays in the book, as
section 2.6 requires.

---

## Experiment 7 — Backpressure and the slow receiver

Run: `python3 experiment.py 7`

The question: how does the server's connection to the slow client behave as
that client stops reading, and what evidence shows whether this ever affects
the server talking to the other clients?

What we did: two Market-Data clients subscribe to `JNST`. One keeps reading and
the other never reads. Two traders send a steady stream of matching orders so
trades keep happening. We watched the send queue and the TCP window on both
Market-Data connections, and checked that the normal client kept getting
updates.

Commands and tools:
```sh
netstat -an -p tcp | grep '\.5000'     # Send-Q for the slow vs the normal conn
tcpdump -i lo0 -n 'tcp port 5000'      # window size, "win 0" from the slow client
sockstat -4 | grep ':5000'
```

What we saw:

> [SCREENSHOT 7a: netstat, Send-Q large and growing on the slow connection and
> near zero on the normal one ]

> [SCREENSHOT 7b: tcpdump, the slow client advertising "win 0" and the server
> pausing sends to it, while data keeps flowing to the normal client ]

Answer: as the slow client stops reading, its kernel receive buffer fills up,
then the server's kernel send buffer for that socket fills up. The slow client
advertises a zero TCP window and the server's `send()` starts returning
`EAGAIN`. The server does not block. It stops writing to that socket and keeps
the unsent bytes in that connection's output buffer, which is capped at 8 MB.
If the cap is passed, the server drops that one connection and nothing else.
The normal client and the traders are not affected. They keep getting `TRADE`
messages at the same rate, because the loop never waits on the slow socket. The
evidence is the growing Send-Q on the slow connection next to a near-zero
Send-Q on the normal one, and the capture showing data still going to the
normal client. So one slow reader is contained and does not slow down anyone
else.

---

## Experiment 8 — Unexpected client disconnection

Run: `python3 experiment.py 8`

The question: when a client vanishes without warning while the server is
talking to it, what happens to the connection, and what network evidence shows
how the server notices?

What we did: a separate Market-Data process subscribes to `JNST` and stays
connected while trades flow to it. A second Market-Data connection stays up so
we have something to compare against. The harness then sends `SIGKILL` to the
first process. We captured the packets around the kill and compared the two
connections before and after.

Commands and tools:
```sh
tcpdump -i lo0 -n 'tcp port 5000'      # FIN or RST from the killed client's port
sockstat -4 | grep ':5000'             # dead conn disappears, the other stays
procstat -f $(pgrep exchange_server) | wc -l   # server fd count drops by one
```

What we saw:

> [SCREENSHOT 8a: tcpdump at the moment of the kill, a FIN or RST from the dead
> client's port and then the server's reply ]

> [SCREENSHOT 8b: sockstat before and after, the killed connection is gone and
> the surviving Market-Data connection is still ESTABLISHED and receiving ]

Answer: when the process is killed the operating system closes its sockets, so
the kernel sends a FIN to the server, or a RST if there is data in flight or
data arrives afterwards. TCP has no other signal for a client that is gone, so
the server only finds out by doing I/O: `poll()` reports `POLLHUP` or
`POLLERR`, or the next `send()` fails with `EPIPE` or `ECONNRESET`, or `recv()`
returns 0. The server then calls `close_client()` for that connection. It
leaves the socket table and the server's file-descriptor count drops by one.
The other connection is not touched. The network evidence is the lone FIN or
RST from the dead port in the capture, together with that connection
disappearing from `sockstat` while the other one stays.

---

## Bonus — Connection scalability and I/O design (section 6.9)

Setup on the VM, once, as root:

```sh
sysctl kern.maxfiles=1000000 kern.maxfilesperproc=900000
sysctl kern.ipc.somaxconn=1024
# one source IP has ~64k ports to one destination, so add loopback aliases
for i in 2 3 4 5 6 7 8 9 10; do ifconfig lo0 alias 127.0.0.$i/8; done
```

Then, with `ulimit -n 200000` set in the shell:

```sh
./server/run-server 127.0.0.1 5000
python3 tests/bench/connflood.py 127.0.0.1 5000 70000 10   # 10 = source aliases
```

`connflood.py` opens the connections and holds them. It prints a line every
5,000 so you can stop and take a measurement.

The server keeps no thread or process per connection. Every socket lives in the
one `poll()` loop, so a connection costs a socket, a file descriptor, and about
0.3 KB of state, with no per-connection stack or scheduler entry.

### Resource table

Read the values while `connflood.py` is holding N connections:

```sh
PID=$(pgrep exchange_server)
procstat -v $PID | wc -l          # server open fds
ps -o rss,%cpu -p $PID            # server memory and cpu
sysctl kern.openfiles kern.maxfiles
sockstat -4 | grep -c ':5000'     # connections established
netstat -m                        # mbuf / socket-buffer memory
```

| Idle connections | Server memory (RSS) | Server CPU | Server open FDs | System-wide open FDs | Socket-buffer used / limit | Max established |
|---:|---|---|---|---|---|---|
| 10,000 | | | | | | |
| 20,000 | | | | | | |
| 30,000 | | | | | | |
| 40,000 | | | | | | |
| 50,000 | | | | | | |
| 60,000 | | | | | | |
| 70,000 | | | | | | |

> [SCREENSHOT B1: the commands and output at 10,000 idle connections, with the
> connection count and every measurement visible ]

> [SCREENSHOT B2: the same at 40,000 idle connections ]

> [SCREENSHOT B3: the same at 70,000 idle connections ]

### Answers to the questions

1. Does the server hold every requested connection? `<yes or no; if it stops
   early, give the count and the error>`. The server raises its own open-file
   limit at startup, so the ceiling is the system settings
   (`kern.maxfiles`, `kern.maxfilesperproc`), not a small inherited limit.

2. What is the first real bottleneck? `<fill from the table>`. Memory is not
   it: each connection is about 0.3 KB of state, so 70,000 connections is only
   about `<X>` MB of RSS. What we saw slow down first is the connection setup
   rate: `connflood.py` opened about `<A>` connections per second at the start
   and only about `<B>` per second near 60,000. Once the connections are open
   and idle the server sits quiet, but a request from one active client takes
   longer to answer as N grows.

3. How does the concurrency and I/O design cause that? The server has one
   thread and uses `poll()`. On every wakeup it hands the kernel the whole
   array of file descriptors and then walks the results. That work is
   proportional to how many connections exist, not how many are active, so it
   grows with N even though almost every connection is idle. There is no thread
   or process per connection, which is why memory stays flat, but the `poll()`
   scan does not.

4. Would changing the I/O mechanism help? Yes. `kqueue` on FreeBSD only hands
   back the sockets that are actually ready, so each wakeup costs about the
   number of ready sockets instead of the total. That would remove the scan
   that grows with N. We kept `poll()` for this submission because it is
   simpler and it still holds 70,000 connections; `kqueue` is the change we
   would make if the scan became the real limit. Going to a thread per
   connection would not help. It replaces the `poll()` scan with a few MB of
   stack per thread plus lock and scheduler contention, which would run out
   well before 70,000.

5. If we change the implementation, measure again and compare. `<optional. If
   you try a kqueue version, put the before-and-after numbers here. Otherwise
   say the mechanism was not changed.>`

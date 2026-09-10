# COL334 Assignment 2 — The Socket Exchange

A TCP exchange server with a trader client and a market-data client.

## Language and build

- **Language:** C++17.
- **Compiler:** the system `c++` (FreeBSD base `clang++`); `g++` also works.
- **Build tool:** `make` (the FreeBSD base `make` is fine; no GNU make needed).

```sh
make
```

This produces three executables at the repository root: `exchange_server`,
`trader_client`, and `market_data_client`. The launcher scripts in `server/`
and `client/` `exec` these binaries.

`make clean` removes the executables.

No configuration, environment variables, or extra software are required.

## Running

Start the Exchange Server (host and port are arguments):

```sh
./server/run-server 127.0.0.1 5000
```

Start a Trader Client (host, port, username):

```sh
./client/run-trader 127.0.0.1 5000 alice
```

then type commands on stdin, one per line:

```text
BUY JNST 100 238
SELL JNST 50 238
CANCEL 0
QUIT
```

Start a Market-Data Client (host, port, then zero or more instruments to
subscribe to immediately):

```sh
./client/run-market-data 127.0.0.1 5000 JNST IMCT
```

It also accepts `SUBSCRIBE <instrument>`, `UNSUBSCRIBE <instrument>`, and
`QUIT` on stdin.

## Protocol summary

Instruments are `JNST` and `IMCT`. Quantities and prices are integers in
`1 .. 2147483647`. A Trader Client first sends `LOGIN <username>`. An accepted
order gets `ORDER_ACCEPTED <id>`; a fill sends `BOUGHT`/`SOLD` to the two
traders and `TRADE` to every client subscribed to that instrument. Two orders
match only when they are the same instrument, opposite sides, and exactly the
same price.

## Source layout

| File | Purpose |
| --- | --- |
| `src/server.cpp` | Exchange Server: `poll()` event loop, framing, order book. |
| `src/trader.cpp` | Trader Client. |
| `src/market_data.cpp` | Market-Data Client. |
| `src/net_utils.hpp` | Shared socket helpers (connect, listen, non-blocking). |
| `src/event_poller.hpp` | Readiness-loop wrapper (`poll` by default). |

## Concurrency / I/O

Single process, single thread, one `poll()` loop over all sockets, every socket
non-blocking. Per-client input and output are buffered, so an idle, slow, or
disconnected client never blocks the others. See `report.pdf` §"Implementation
Decisions" for the reasoning.

## Optional builds

The readiness primitive is selected at build time:

```sh
make                  # poll(2), the default; used for grading
make POLLER=kqueue    # kqueue, for the §6.9 scalability bonus (FreeBSD)
make POLLER=epoll     # epoll (Linux only)
```

## Tests and bonus tooling

Standard-library Python 3 only; run from the project root:

```sh
python3 tests/stress/run_all.py                 # conformance / stress suite
python3 tests/bench/connflood.py <host> <port> <N>   # §6.9 idle-connection generator
sh tests/bench/freebsd-setup.sh                 # one-time host tuning for the bonus (run as root)
```

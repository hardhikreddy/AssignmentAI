# COL334 Assignment 2 — The Socket Exchange

This is a C++17 TCP exchange server with interactive trader and market-data
clients. The server is single-process, single-threaded and event-driven: one
readiness-notification loop drives the listening socket, per-client buffered
input, and per-client non-blocking output queues. No client can block another.

## Build

Use a POSIX-like environment with a C++17 compiler and `make`.

```sh
make
```

This creates `exchange_server`, `trader_client`, and `market_data_client` at
the repository root. The supplied scripts in `server/` and `client/` launch
these binaries and are the interface used by `experiment.py`.

### Event-loop back end

The server's readiness mechanism is selected at build time via the `POLLER`
variable (see `src/event_poller.hpp`):

| Command | Back end | Notes |
| --- | --- | --- |
| `make` | `poll(2)` | Portable default; used for grading and all experiments. |
| `make POLLER=kqueue` | `kqueue` (FreeBSD) | O(ready) wakeups; use for the §6.9 connection-scalability bonus. |
| `make POLLER=epoll` | `epoll` (Linux) | For benchmarking on a Linux host only. |

All back ends are level-triggered and behave identically at the protocol
level. `make clean && make POLLER=kqueue` before the bonus scale run; the
default `poll` build is what the autograder and `experiment.py` use.

## Tests

```sh
python3 tests/stress/run_all.py     # protocol + concurrency conformance suite
python3 tests/bench/bench.py        # throughput / fan-out / idle-scaling numbers
```

## Run

Start the server:

```sh
./server/run-server 127.0.0.1 5000
```

Start a trader:

```sh
./client/run-trader 127.0.0.1 5000 alice
```

Example trader input:

```text
BUY JNST 100 238
SELL JNST 50 238
CANCEL 0
QUIT
```

Start a market-data client, optionally with initial subscriptions:

```sh
./client/run-market-data 127.0.0.1 5000 JNST IMCT
```

It also accepts `SUBSCRIBE`, `UNSUBSCRIBE`, and `QUIT` interactively.

## Protocol and experiments

Supported instruments are `JNST` and `IMCT`. A trader begins with
`LOGIN <username>`. Accepted orders receive `ORDER_ACCEPTED <id>`; matched
traders receive `BOUGHT`/`SOLD`, and subscribers receive `TRADE` updates.

After building, run the supplied harness from this directory:

```sh
python3 experiment.py 1
```

See [socket-exchange-guide.md](socket-exchange-guide.md) for the protocol and
[experiment-runbook.md](experiment-runbook.md) for the experiments. Run
`make clean` to remove compiled binaries.

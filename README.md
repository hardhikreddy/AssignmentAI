# COL334 Assignment 2 — The Socket Exchange

A TCP exchange server with a trader client and a market-data client, written in
C++17. The server uses a single `poll()` loop for the listening socket, reads,
and non-blocking writes, so one slow or idle client never blocks the others.

## Build

Needs a C++17 compiler and `make` (the base FreeBSD `make` is fine).

```sh
make
```

This builds `exchange_server`, `trader_client`, and `market_data_client` at the
repo root. The scripts in `server/` and `client/` just `exec` these binaries
and are the interface `experiment.py` uses.

No configuration or environment variables are needed.

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

Start a market-data client, with optional initial subscriptions:

```sh
./client/run-market-data 127.0.0.1 5000 JNST IMCT
```

It also takes `SUBSCRIBE`, `UNSUBSCRIBE`, and `QUIT` on stdin.

## Protocol

Instruments are `JNST` and `IMCT`. A trader starts with `LOGIN <username>`.
Accepted orders get `ORDER_ACCEPTED <id>`; matched traders get `BOUGHT`/`SOLD`;
subscribers get `TRADE`. Two orders match only at exactly the same price.

See [socket-exchange-guide.md](socket-exchange-guide.md) for the full protocol
and [experiment-runbook.md](experiment-runbook.md) for the experiments.

## Experiments

```sh
python3 experiment.py 1        # ... through 8
```

`tests/stress/run_all.py` is a local check script that drives the launchers and
asserts the protocol behaviour; it is not part of the submission.

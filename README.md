# COL334 Assignment 2 — The Socket Exchange

This is a C++17 TCP exchange server with interactive trader and market-data
clients. The server uses one `poll()` event loop for listening, reads, and
queued non-blocking writes.

## Build

Use a POSIX-like environment with a C++17 compiler and `make`.

```sh
make
```

This creates `exchange_server`, `trader_client`, and `market_data_client` at
the repository root. The supplied scripts in `server/` and `client/` launch
these binaries and are the interface used by `experiment.py`.

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

# Project Summary

The Socket Exchange is a newline-delimited TCP protocol with two client roles:

- Trader clients authenticate with `LOGIN <username>` and submit or cancel orders.
- Market-data clients subscribe to `JNST` and/or `IMCT` and receive trades.

`src/server.cpp` implements separate price-level books for each instrument and
side. Orders are FIFO within a price level and match only at the incoming
order's exact price. The server is non-blocking after setup, and a single
event-driven loop manages listening, buffered input, and per-client output
queues. The readiness mechanism is chosen at build time (`src/event_poller.hpp`):
`poll(2)` by default, `epoll`/`kqueue` via `make POLLER=...`. Thus idle or slow
clients do not block other connections.

The `Makefile` creates `exchange_server`, `trader_client`, and
`market_data_client` at the repository root. The required launchers are:

```text
server/run-server
client/run-trader
client/run-market-data
```

They `exec` the corresponding built binary. Build with `make` before using the
launchers or `experiment.py`. See [socket-exchange-guide.md](socket-exchange-guide.md)
for commands and [experiment-runbook.md](experiment-runbook.md) for the supplied harness.

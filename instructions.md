# Repository Instructions

## Requirements

Use a POSIX-like environment with a C++17 compiler, `make`, and Python 3 for
the supplied `experiment.py` harness. FreeBSD is appropriate when the
assignment requires FreeBSD networking-observation tools.

## Build and smoke test

```sh
make
./server/run-server 127.0.0.1 5000
```

In another terminal:

```sh
./client/run-trader 127.0.0.1 5000 alice
```

Then enter:

```text
BUY JNST 100 238
CANCEL 0
QUIT
```

For market data:

```sh
./client/run-market-data 127.0.0.1 5000 JNST
```

## Source map

| Path | Responsibility |
| --- | --- |
| `src/server.cpp` | Protocol, order books, subscriptions, and `poll()` loop. |
| `src/trader.cpp` | Interactive Trader client; sends `LOGIN` on startup. |
| `src/market_data.cpp` | Interactive Market-Data client and subscriptions. |
| `src/net_utils.hpp` | Shared TCP helpers. |
| `server/run-server` and `client/run-*` | Required executable launchers. |
| `experiment.py` | Provided controlled-client experiment harness. |

## Evaluation checklist

1. Run `make` successfully.
2. Ensure all launcher scripts are executable.
3. Start the server through `server/run-server`.
4. Free port 5000, then run `python3 experiment.py 1` at minimum.
5. Do not commit generated binaries; `.gitignore` covers them.

See [README.md](README.md) for usage and
[socket-exchange-guide.md](socket-exchange-guide.md) for the protocol.

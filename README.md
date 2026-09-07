# COL334 Assignment 2 — The Socket Exchange

C++17 implementation using direct POSIX TCP sockets and `poll()`.

## Build

```sh
make
```

## Run server

```sh
./server/run-server 127.0.0.1 5000
```

## Run trader

```sh
./client/run-trader 127.0.0.1 5000 alice
```

Then enter protocol messages such as:

```text
BUY JNST 100 238
SELL JNST 50 238
CANCEL 0
QUIT
```

## Run market-data client

```sh
./client/run-market-data 127.0.0.1 5000 JNST
```

Multiple instruments may be supplied:

```sh
./client/run-market-data 127.0.0.1 5000 JNST IMCT
```

The client also accepts interactive `SUBSCRIBE`, `UNSUBSCRIBE`, and `QUIT` commands.

## Directory layout

```text
COL334-A2/
├── server/
│   └── run-server
├── client/
│   ├── run-trader
│   └── run-market-data
├── src/
│   ├── net_utils.hpp
│   ├── server.cpp
│   ├── trader.cpp
│   └── market_data.cpp
├── Makefile
├── README.md
├── experiment.py       # provided by instructor; do not modify
└── report.pdf          # create for submission
```

The compiled executables are kept at the submission root because the required launcher interface can then use `exec` to start them.

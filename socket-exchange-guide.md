# Socket Exchange Protocol Guide

Messages are UTF-8 text lines terminated by `\n`. TCP is a byte stream, so
the server buffers bytes until a complete line is available.

## Roles and commands

A connection starts unassigned. `LOGIN <username>` selects the Trader role;
`SUBSCRIBE <instrument>` or `UNSUBSCRIBE <instrument>` selects the Market-Data
role. Roles cannot be switched. Valid instruments are `JNST` and `IMCT`;
quantities and prices are positive signed 32-bit integers.

| Command | Allowed role | Effect |
| --- | --- | --- |
| `LOGIN <username>` | Unassigned | Registers a unique trader and replies `OK`. |
| `BUY <instrument> <quantity> <price>` | Trader | Accepts a buy order. |
| `SELL <instrument> <quantity> <price>` | Trader | Accepts a sell order. |
| `CANCEL <order-id>` | Trader | Cancels the caller's resting order. |
| `SUBSCRIBE <instrument>` | Unassigned or Market-Data | Adds a subscription and replies `OK`. |
| `UNSUBSCRIBE <instrument>` | Unassigned or Market-Data | Removes a subscription and replies `OK`. |
| `QUIT` | Any | Closes the connection. |

Accepted orders return `ORDER_ACCEPTED <order-id>`, and a successful cancel
returns `ORDER_CANCELLED <order-id>`. Invalid syntax, values, role usage,
duplicate usernames, or cancellations return an `ERROR ...` line.

## Matching and notifications

Orders are grouped by instrument, side, and price. An incoming order matches
the opposite side only at the same price, FIFO within that price level. Any
unfilled quantity becomes a resting order.

For each fill of quantity `q` at price `p`, traders receive:

```text
BOUGHT <instrument> <q> <p>
SOLD <instrument> <q> <p>
```

Each subscriber to that instrument receives:

```text
TRADE <instrument> <q> <p>
```

## Example

```sh
./server/run-server 127.0.0.1 5000
./client/run-trader 127.0.0.1 5000 buyer
./client/run-market-data 127.0.0.1 5000 JNST
```

Entering `BUY JNST 10 238` for `buyer`, then `SELL JNST 10 238` from another
trader, produces `BOUGHT JNST 10 238`, `SOLD JNST 10 238`, and
`TRADE JNST 10 238` for the subscriber.

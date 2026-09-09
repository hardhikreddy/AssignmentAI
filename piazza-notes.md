# Piazza Clarifications (authoritative — answers by Shailesh Dagar / instructors)

Captured from the course Piazza thread. These resolve points the handout leaves
implicit and are treated as binding for this implementation.

## Client roles
- Only Trader Clients send `LOGIN`. Market-Data Clients send `SUBSCRIBE` /
  `UNSUBSCRIBE`. A connection that sends `SUBSCRIBE` without logging in is a
  Market-Data Client.
- A Market-Data Client may subscribe to both `JNST` and `IMCT`.
- Role-violating message sequences (e.g. two `LOGIN`s, `LOGIN` then
  `SUBSCRIBE`) are outside required behaviour — implement compliant-client
  behaviour, do not attempt to handle every malformed sequence. This
  implementation replies `ERROR` and keeps the first role.

## Numeric ranges
- Quantity and price: integers `1 .. 2147483647` inclusive.
- Order IDs: integers `0 .. 2147483647` inclusive.
- Decimal, negative, or out-of-range values are invalid → `ERROR <reason>`,
  order not accepted / not executed.
- No larger integers need to be supported.

## Message length / framing
- No maximum application-message length is imposed; do **not** assume one
  message arrives in a single `recv()`.
- Arbitrarily long messages, or a client that streams bytes without ever
  sending `\n`, are outside required behaviour. No specific policy (ERROR /
  discard / close) is required. (This implementation caps the per-connection
  input buffer at 64 KiB as a safety valve and then closes that one
  connection; unrelated clients are unaffected.)

## Matching
- Match only when: same instrument, opposite side, exactly equal price.
- No partial matches in the sense of "leave both partly filled and stop" — an
  incoming order must match every eligible resting order until it is exhausted
  or no more eligible orders remain. Quantity traded per fill = min of the two
  remaining quantities; leftover rests.
- FIFO within a price level is acceptable; which of several equal resting
  orders is chosen is an implementation detail, but a match must happen when a
  candidate exists.
- `SELL 40` matched against two `BUY 20` is **two trades**: two
  `TRADE <inst> 20 <p>` to each subscriber and two `BOUGHT`/`SOLD` pairs to
  the traders — not one aggregated `TRADE ... 40`.
- Self-trades (orders from the same Trader Client matching each other) are
  valid trades.

## Connection lifetime / username
- A username identifies a Trader Client only for the lifetime of its current
  TCP connection. After disconnect the username is free for reuse.
- Orders are associated with the submitting connection, not the username. A
  new connection reusing an old username inherits nothing and may not cancel
  the previous connection's orders.
- An outstanding order stays in the book after its Trader Client disconnects
  and may still match later. No `BOUGHT`/`SOLD` is sent to the gone client;
  subscribed Market-Data Clients still receive the `TRADE`.
- Recommended design: `order_id -> connection/fd`; a separate set of usernames
  only to enforce uniqueness among currently-connected traders.

## Environment / tooling
- QEMU/KVM is acceptable in place of VirtualBox.
- C++ is allowed; must use the POSIX socket API directly (no Boost.Asio etc.).
- C or C++ threading (pthreads, `std::thread`) is allowed.
- `kqueue`/`kevent` and Python `select`/`kqueue` are allowed.
- `tcpdump` recommended; Wireshark/TShark also allowed.
- Experiment evidence must be terminal **screenshots** (not just pasted text).
- Launcher-scripts-executable autograder check: known issue, will be fixed on
  the autograder side; does not affect marks if other checks pass.

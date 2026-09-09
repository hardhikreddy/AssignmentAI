# Structured Prompt — Optimize & Stress-Test The Socket Exchange

## Role
You are working on an existing, functionally-complete COL334 Assignment 2
submission (C++17 Exchange Server + Trader/Market-Data clients, `poll()` event
loop). The experiments and report are handled by the user. Your job is to make
the implementation **faster and more resource-efficient**, and to **stress-test
every protocol functionality** so regressions are impossible to miss.

## Ground rules (non-negotiable)
1. **`Assignment_2_Handout.pdf` is the single source of truth.** Where it is
   silent, the resolved Piazza answers from Shailesh Dagar are authoritative
   (captured in `piazza-notes.md` — create it if absent). Do **not** invent
   behaviour, limits, or policies that neither document states.
2. Target platform is **FreeBSD 14.4** (grader). Linux/WSL is the local test
   proxy only. Keep everything POSIX; `kqueue` is explicitly allowed by the
   handout, `poll()`/`select()` also allowed. No non-POSIX / high-level
   networking libraries (handout §4.1.2).
3. **Never trade correctness or protocol compliance for speed.** Every
   optimization must pass the full stress suite unchanged.
4. Preserve the mandatory submission interface: `server/run-server`,
   `client/run-trader`, `client/run-market-data`, `src/`, `Makefile`,
   `README.md` (handout §7).
5. Keep the concurrency model **single-process, single-threaded, event-driven**
   unless the user approves otherwise (this is what the report/viva describes).
6. Confirmed protocol constants: quantity/price ∈ [1, 2147483647];
   order IDs ∈ [0, 2147483647]; out-of-range ⇒ `ERROR <reason>`, order not
   accepted. No max message length is required; arbitrarily long messages /
   never-terminated lines are out of scope. Malformed role-violating sequences
   are out of scope — implement compliant-client behaviour.
7. Username identifies a trader only for the life of its TCP connection.
   Orders are keyed to the submitting connection, not the username. A closed
   trader's resting orders stay in the book and still match; no BOUGHT/SOLD is
   sent to the gone client; subscribers still get TRADE.

## Deliverables
- Optimized `src/` with each change explained in a commit-sized note (what,
  why, measured effect).
- `tests/stress/` — automated suite (Python, stdlib only, same style as
  `experiment.py`) driving the real launchers. Must be runnable on FreeBSD.
- `tests/bench/` — reproducible micro-benchmarks producing before/after numbers.
- `OPTIMIZATION_REPORT.md` — baseline vs optimized measurements, methodology,
  and the bottleneck analysis needed for handout §6.9 questions 2–5.
- Updated `README.md` if build/run steps change.

## Work plan

### Phase 0 — Baseline (measure first)
- Build clean (`make`, `-O2 -Wall -Wextra -Wpedantic`, zero warnings).
- Run and record baselines for:
  - trade throughput (matching BUY/SELL pairs/sec, N subscribers draining);
  - order-book ops with deep books (many price levels, many resting orders);
  - broadcast fan-out (1 trade → many subscribers);
  - active-client latency (p50/p99) while N idle connections are held
    (N = 1k, 5k, 10k, 20k, 50k, 70k);
  - server RSS, CPU, open FDs at each N;
  - connection churn (connect+login+close/sec).
- Commit baseline numbers to `OPTIMIZATION_REPORT.md`.

### Phase 1 — Stress-test suite (build before optimizing)
Cover, with assertions on exact protocol output:
- **Framing:** message split across many `send()`s; many messages in one
  `send()`; message boundary mid-token; CRLF vs LF; no-trailing-newline
  partial then completion; interleaved reads/writes.
- **Roles:** unassigned → LOGIN locks Trader; unassigned → SUBSCRIBE locks
  Market-Data; each role rejected from the other's commands with `ERROR`;
  QUIT valid for both; MD may SUBSCRIBE both instruments; duplicate SUBSCRIBE;
  UNSUBSCRIBE of non-subscribed instrument.
- **LOGIN:** valid; duplicate username while connected ⇒ `ERROR`; same
  username reusable after disconnect; bad syntax/arity.
- **Orders/matching:** exact-price-only match; opposite-side only; FIFO at a
  price level; partial fill leaves remainder resting; smaller-quantity-traded
  rule; self-trade allowed; multiple partial fills produce **one TRADE per
  fill** and one BOUGHT+SOLD pair per fill (Piazza: 40 vs 2×20 ⇒ two trades);
  no cross-instrument match; price 240 BUY does not touch 238 SELL.
- **Value validation:** 0, negative, decimal, 2147483647 (ok),
  2147483648 (reject), leading zeros, `+`/`-` signs, empty fields, extra
  fields, whitespace runs.
- **CANCEL:** own resting order; already-filled ⇒ `ERROR`; unknown ID ⇒
  `ERROR`; another trader's order ⇒ `ERROR`; cancel after partial fill
  (remaining quantity); MD client CANCEL ⇒ `ERROR`.
- **Notifications:** ORDER_ACCEPTED precedes any fill; asynchronous
  BOUGHT/SOLD delivery on later match; TRADE only to current subscribers of
  that instrument; unsubscribed client stops receiving; trader gets no TRADE.
- **Connection lifecycle:** graceful QUIT; half-close (SHUT_WR) then server
  flushes queued replies then closes; abrupt RST mid-session; disconnect of a
  trader with resting orders → orders persist, later match, subscribers still
  get TRADE, no crash; one client's failure never disturbs another
  (handout §4.4).
- **Concurrency / isolation:** ≥10 simultaneous clients (≥2 traders, ≥4 MD);
  idle client + never-newline client must not stall others (handout §4.3, Exp 4);
  slow/stalled reader must not block the event loop for other clients
  (Exp 7) — assert a normal client keeps getting timely updates while one
  subscriber never reads, and that the stalled client is eventually bounded
  (buffer cap) not unbounded.
- **Fairness:** one client sending a continuous flood must not starve others
  for more than a bounded time.
- **Soak:** run a mixed workload for several minutes; assert no leak (RSS
  plateau), no FD growth, no latency drift.

Each test: independent server instance on its own port, deterministic asserts,
clear pass/fail, `python3 tests/stress/run_all.py`.

### Phase 2 — Optimizations (apply only what benchmarks justify)
Candidate list, cheapest/safest first — measure each in isolation:
1. Per-message heap churn: replace `std::vector<string_view>` tokenizer with a
   fixed-capacity array; reuse scratch buffers for outbound message formatting
   (avoid `std::string` `+` chains — use `char` buffer + `to_chars`).
2. Poll-loop hash lookups: keep a parallel `vector<Client*>` aligned with
   `fds_` so the post-poll scan doesn't re-`find()` per fd.
3. Reserve `input`/`output_buf` capacity; tune compaction thresholds.
4. Bound work per client per loop iteration (cap bytes drained from one
   socket) to protect fairness under flood — verify against Exp 4/7 tests.
5. `broadcast_trade`: format once, avoid per-subscriber `find_client`
   (store `Client*` in the subscriber index or iterate the fd→Client map).
6. **Headline:** replace the O(N) `poll()` rebuild+scan with **`kqueue`**
   (FreeBSD) event registration — O(ready) per iteration, edge-or-level
   documented. This is the fix for the idle-connection-scaling bottleneck the
   baseline will show and directly answers §6.9 Q3–Q4. Provide a `poll()`
   fallback behind a compile guard **only if** it costs little; otherwise
   kqueue-only is acceptable on the FreeBSD target. **Get user sign-off before
   starting this one** (it's the big structural change).
7. `listen()` backlog, `SO_REUSEADDR`, socket buffer sizing sanity for the
   70k-idle bonus; confirm `accept()` loop drains fully (already does).
8. Raise `RLIMIT_NOFILE` from within the server (or document the required
   `ulimit`) for the bonus scale test.

### Phase 3 — Verify & report
- Full stress suite green, zero warnings, no leaks.
- Re-run all Phase 0 benchmarks; put before/after table in
  `OPTIMIZATION_REPORT.md` with the bottleneck narrative (memory vs CPU vs FD
  vs socket-buffer) for §6.9.
- Note any behaviour that is deliberately unspecified and how it's handled.
- Do not touch `experiment.py`.

## Constraints on process
- Show measured evidence for every claimed speedup; no speculative changes.
- Small, reviewable commits; keep the tree buildable at every step.
- If a handout ambiguity blocks you, stop and ask — do not guess.

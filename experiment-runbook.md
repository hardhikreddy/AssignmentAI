# Experiment Runbook

`experiment.py` launches `./server/run-server 127.0.0.1 5000` and creates
controlled TCP clients. Run it from the repository root in a POSIX environment;
its cleanup uses process groups and signals.

## Setup

```sh
make
chmod +x server/run-server client/run-trader client/run-market-data
python3 experiment.py <number>
```

Use a second terminal for observation. On FreeBSD:

```sh
sockstat -4 -p 5000
netstat -an | grep 5000
procstat -f <server-pid>
tcpdump -i lo0 -n port 5000
```

The harness supports experiments 1–8, prints the server PID and timing cues,
and cleans up the server it starts. Use `Ctrl-C` for experiments that wait.

| No. | Focus |
| --- | --- |
| 1 | Listening versus accepted sockets |
| 2 | TCP close lifecycle |
| 3 | TCP byte-stream framing |
| 4 | An idle client must not stall another |
| 5 | Multiplexing several clients |
| 6 | FIN versus RST |
| 7 | Slow market-data reader |
| 8 | Disconnect cleanup |

The implementation evidence for experiments 3, 4, 5, and 7 is in
`src/server.cpp`: newline-buffered input, an event-driven readiness loop
(`src/event_poller.hpp`; `poll(2)` by default), and per-client queued output
with non-blocking sends. `tests/stress/run_all.py` exercises the same
behaviours as automated assertions. Do not edit `experiment.py` during
normal runs. If startup fails, confirm `make` succeeded, port 5000 is free,
and `server/run-server` is executable.

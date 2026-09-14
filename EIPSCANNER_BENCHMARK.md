# I/O Messaging Benchmark: OpENer vs. EIPScanner

This document records a Class 1 (implicit/cyclic) I/O messaging performance
benchmark between **OpENer** (this repo, POSIX build) and
[**EIPScanner**](https://github.com/nimbuscontrols/EIPScanner) (commit
`12c89a5`), run as a follow-up to the functional interop test in
[`EIPSCANNER_TESTING.md`](EIPSCANNER_TESTING.md). It measures achievable
cyclic rate, packet loss, and round-trip latency across a range of RPIs
(Requested Packet Intervals).

## Environment

- **Adapter (OpENer)**: this host, `192.168.1.154`, Debian 13 "trixie",
  Linux 6.12. OpENer built via `bin/posix/setup_posix.sh` (POSIX port,
  static build), running the POSIX sample app bound to `eno1`.
- **Scanner (EIPScanner)**: a second physical host ("hp6z4"), `192.168.1.151`,
  same OS/kernel. EIPScanner built fresh from source (`cmake -DEXAMPLE_ENABLED=ON`)
  at the same commit (`12c89a5`) used in the earlier interop test.
- Two genuinely separate hosts on the same LAN — this avoids the same-host
  UDP port 2222 collision documented in `EIPSCANNER_TESTING.md`, so no
  network-namespace/container isolation was needed here.

## Methodology

OpENer's POSIX sample app wires up three assemblies for its demo I/O
connection (config 151, output 150, input 100) and mirrors output data
straight to the input assembly in `AfterAssemblyDataReceived()`
(`source/src/ports/POSIX/sample_application/sampleapplication.c:135-164`).
This mirror executes synchronously on UDP packet receipt, which makes it a
convenient loopback for round-trip latency measurement.

A custom EIPScanner example, [`BenchmarkExample.cpp`](BenchmarkExample.cpp),
opens a Class 1 exclusive-owner connection to assemblies 151/150/100 and:

1. Embeds an 8-byte `steady_clock` timestamp + 4-byte sequence number in the
   first 12 bytes of each 32-byte O2T payload.
2. On each T2O receive callback, computes round-trip latency as
   `now() - embedded_timestamp` using a single (scanner-side) monotonic
   clock, so no cross-host clock synchronization is required.
3. Runs for a fixed duration, then reports reception ratio (received vs.
   expected cycle count for the requested RPI), achieved cyclic rate, RTT
   percentiles, and cycle-to-cycle jitter.

The source is commented to make every fixed/deterministic element explicit
and reproducible run to run: the EPath assembly bytes, originator identity,
NCP flags, RPI units, the 32-byte payload layout, why `kMaxSaneRttMs`
filters startup artifacts (see below), and the fixed percentile/jitter
formula definitions used in the results table.

**Caveat on what "RTT" measures**: the benchmark refreshes the O2T payload's
timestamp continuously (roughly once per millisecond, driven by
`ConnectionManager::handleConnections()`'s polling loop), but EIPScanner's
`IOConnection` only actually transmits once per RPI window
(`IOConnection::notifyTick`, `src/IOConnection.cpp:94-142`). So the reported
RTT is **end-to-end data-freshness latency** (transmit-scheduling delay +
true network/processing round trip), not raw wire latency. The `min` column
across all RPIs (consistently ~1.4-3.6 ms) is the closest proxy for true
network RTT, since it's the case where the payload happened to be refreshed
right before a transmit tick fired.

### Why 10 ms is the practical floor

OpENer's POSIX build has no explicit min/max RPI validation in the
connection manager — any requested RPI is accepted
(`source/src/cip/cipconnectionobject.c:166-182`). Instead, RPI is silently
quantized to the platform timer-tick resolution
(`ConnectionObjectGetRequestedPacketInterval()`,
`cipconnectionobject.c:445-457`). That tick is defined as:

```c
// source/src/ports/POSIX/sample_application/opener_user_conf.h:191
static const MilliSeconds kOpenerTimerTickInMilliSeconds = 10;
```

`NetworkHandlerProcessCyclic()` (`source/src/ports/generic_networkhandler.c`)
only invokes `ManageConnections()` — which drives all Class 1 I/O production
— once per elapsed 10 ms tick, regardless of the RPI requested by the
scanner. So no cyclic rate faster than ~10 ms (100 Hz) is achievable on this
build, no matter what RPI EIPScanner asks for.

## Results

RPI sweep, 8 seconds per run, EIPScanner (hp6z4) → OpENer (this host):

| RPI requested | Cycles received | Reception ratio | Achieved rate | RTT min / p50 / p99 / max | Cycle jitter (mean abs) |
|---|---|---|---|---|---|
| 10 ms  | 788/800 | 98.5%  | 98.5 Hz | 1.40 / 6.69 / 11.73 / 11.74 ms  | 0.39 ms |
| 20 ms  | 401/400 | 100.2% | 50.1 Hz | 1.40 / 11.67 / 23.08 / 185.5 ms | 1.17 ms |
| 50 ms  | 161/160 | 100.6% | 20.1 Hz | 1.45 / 31.02 / 166.1 / 206.8 ms | 2.05 ms |
| 100 ms | 81/80   | 101.2% | 10.1 Hz | 3.61 / 52.25 / 295.7 / 295.7 ms | 3.77 ms |

(Reception ratio occasionally exceeds 100% because the fixed test duration
doesn't align perfectly to whole RPI cycles.)

### Observations

- **No packet loss** observed at any tested RPI.
- **Cycle timing is accurate**: measured inter-arrival interval tracks the
  requested RPI closely (e.g. 10.15 ms avg at RPI=10ms), with jitter growing
  roughly linearly with RPI.
- **10 ms confirmed as the practical floor**, consistent with the
  `kOpenerTimerTickInMilliSeconds` analysis above.
- **True network RTT stays low and roughly constant** (~1.4-3.6 ms `min`)
  across all RPIs; the growth in avg/p99/max RTT with RPI reflects
  transmit-scheduling latency inherent to a slower cyclic rate, not network
  degradation.
- At its fastest usable RPI (10 ms), this OpENer POSIX build sustains
  ~98.5 Hz cyclic I/O with sub-2 ms true round-trip latency and zero packet
  loss over a real two-host network path.

## Expected impact of the proposed DPDK refactor

This benchmark's SOCKET-path numbers above are the baseline that
[`DPDK_IO_DATAPATH_DESIGN.md`](DPDK_IO_DATAPATH_DESIGN.md)'s proposed
refactor should be compared against (its own §9 Testing plan calls for
exactly that: rerun this same benchmark against `OPENER_IO_DATAPATH=SOCKET`
and `=DPDK` builds and diff the distributions). Since the refactor only
replaces the RX/TX transport under `HandleReceivedConnectedData()` /
`SendUdpData()` — none of the CIP/assembly/connection-manager logic this
benchmark exercises changes — the realistic expectations are:

**Expected to improve:**
- **Jitter / tail latency (p99, max)** — the real target. `min` RTT above
  stays a tight ~1.4-3.6 ms across every RPI (close to the true floor), but
  `avg`/`p99`/`max` spread out badly as RPI grows (e.g. 100 ms RPI: p50=52 ms,
  p99/max=296 ms). Much of that spread is kernel scheduling variance
  (interrupt coalescing, `select()` wake-up granularity, syscall/copy
  overhead) that a DPDK poll-mode driver on an isolated core should remove.
- **Best-case single-packet latency** — should drop below the ~1.4 ms floor
  measured here, since kernel network-stack traversal, context switches, and
  `recvfrom`/`sendto` syscall overhead disappear from the datapath.

**Not expected to improve on its own:**
- **The 10 ms cyclic-rate floor** — set by
  `kOpenerTimerTickInMilliSeconds=10` gating `ManageConnections()`, a config
  constant unrelated to the kernel socket stack. DPDK alone doesn't touch
  it; going faster than 10 ms also needs the open question in the design
  doc's §6 (moving T2O production onto the DPDK thread's own tight poll
  loop) resolved.
- **Packet loss / reception ratio** — already ~100% on the socket path in
  every run above, so there's no headroom to reclaim there.
- **Explicit messaging or discovery** — explicitly out of scope for the
  refactor, stays on kernel sockets either way.

No DPDK-path numbers exist yet — the refactor is design-only
(`DPDK_IO_DATAPATH_DESIGN.md` status: *Proposal / not yet implemented*).
This section will be replaced with a real before/after comparison once it's
built and rerun with `BenchmarkExample.cpp`.

## Reproducing

1. Build OpENer for POSIX (`bin/posix/setup_posix.sh && make`) and run the
   sample app on one host: `./src/ports/POSIX/OpENer <interface>`.
2. On a **second** host, build EIPScanner with examples enabled:
   ```bash
   git clone https://github.com/nimbuscontrols/EIPScanner.git
   cd EIPScanner && mkdir build && cd build
   cmake -DEXAMPLE_ENABLED=ON ..
   cmake --build .
   ```
3. Copy [`BenchmarkExample.cpp`](BenchmarkExample.cpp) into that repo's
   `examples/` directory, add it as a CMake target alongside the existing
   examples in `examples/CMakeLists.txt`:
   ```cmake
   add_executable(benchmark_example BenchmarkExample.cpp)
   target_link_libraries(benchmark_example EIPScanner)
   ```
   then rebuild.
4. Run: `./examples/benchmark_example <opener_ip> <rpi_us> <duration_s>`,
   e.g. `./examples/benchmark_example 192.168.1.154 10000 8` for a 10 ms RPI,
   8-second run.

## Source

This document and [`BenchmarkExample.cpp`](BenchmarkExample.cpp) are
tracked in this repo and pushed to
[`github.com/zwu888/OpENer`](https://github.com/zwu888/OpENer), branch
`master`.

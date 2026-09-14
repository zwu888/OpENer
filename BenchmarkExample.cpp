// I/O messaging (Class 1) latency/throughput benchmark against an EtherNet/IP
// adapter (OpENer). Embeds a send timestamp + sequence number in the O2T
// payload; the adapter mirrors O2T -> T2O (OpENer's sample app, assembly
// 150 -> 100), so the T2O receive callback can compute round-trip latency
// using a single (local) monotonic clock -- no cross-host clock sync needed.
//
// Usage: benchmark_example <target_ip> <rpi_us> <duration_s> [--io-datapath=socket|dpdk]
//
// --io-datapath is a REPORTING LABEL, not a remote control: this is an
// EIPScanner-side client and has no way to select OpENer's I/O backend over
// the wire. OpENer's proposed --io-datapath flag (DPDK_IO_DATAPATH_DESIGN.md
// §5a) is a separate, server-side startup option -- set it there when you
// start OpENer, then pass the matching value here so the two runs this
// tool produces (one per backend) are self-labeled in their output, per
// the parity-check methodology in DPDK_IO_DATAPATH_DESIGN.md §9.

#include <cstring>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <cip/connectionManager/NetworkConnectionParams.h>
#include "SessionInfo.h"
#include "ConnectionManager.h"
#include "utils/Logger.h"

using namespace eipScanner::cip;
using eipScanner::SessionInfo;
using eipScanner::ConnectionManager;
using eipScanner::cip::connectionManager::ConnectionParameters;
using eipScanner::cip::connectionManager::NetworkConnectionParams;
using eipScanner::utils::Logger;
using eipScanner::utils::LogLevel;
using Clock = std::chrono::steady_clock;

namespace {
std::vector<double> g_rttMs;
std::vector<Clock::time_point> g_arrivals;
uint32_t g_received = 0;
uint32_t g_excludedStartup = 0;
// Fixed threshold, not derived from any RPI in this run: OpENer echoes one
// all-zero payload from connection setup before this program's first real
// timestamp is on the wire, which decodes as an RTT of "now minus
// steady_clock's epoch" -- typically hours (system uptime), always far
// above any plausible LAN RTT at the RPIs this benchmark exercises
// (milliseconds to low hundreds of ms). Anything past this line is that
// startup artifact, not a slow cycle.
constexpr double kMaxSaneRttMs = 2000.0;
}

int main(int argc, char **argv) {
  Logger::setLogLevel(LogLevel::ERROR);

  // --io-datapath=<value> may appear anywhere; everything else is
  // positional (target_ip, rpi_us, duration_s), in order, same as before.
  std::string ioDatapath = "socket";
  std::vector<std::string> positional;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    constexpr char kFlag[] = "--io-datapath=";
    if (arg.rfind(kFlag, 0) == 0) {
      ioDatapath = arg.substr(sizeof(kFlag) - 1);
    } else {
      positional.push_back(arg);
    }
  }
  if (ioDatapath != "socket" && ioDatapath != "dpdk") {
    std::fprintf(stderr,
        "warning: --io-datapath=%s is not \"socket\" or \"dpdk\" -- this "
        "tool only records it as a label, so any value is accepted, but "
        "check for a typo if that wasn't intended\n", ioDatapath.c_str());
  }

  std::string targetIp = positional.size() > 0 ? positional[0] : "192.168.1.154";
  uint32_t rpiUs = positional.size() > 1 ? static_cast<uint32_t>(std::stoul(positional[1])) : 10000;
  double durationS = positional.size() > 2 ? std::stod(positional[2]) : 10.0;

  auto si = std::make_shared<SessionInfo>(targetIp, 0xAF12);

  ConnectionManager connectionManager;
  ConnectionParameters parameters;
  // EPath: class 0x04 (Assembly), config instance 151, O2T instance 150,
  // T2O instance 100 -- fixed by OpENer's POSIX sample app
  // (DEMO_APP_{CONFIG,OUTPUT,INPUT}_ASSEMBLY_NUM), not scanner-chosen.
  parameters.connectionPath = {0x20, 0x04, 0x24, 151, 0x2C, 150, 0x2C, 100};
  parameters.o2tRealTimeFormat = true;
  // Arbitrary but fixed originator identity -- deterministic across runs so
  // repeated benchmark invocations are directly comparable in adapter logs.
  parameters.originatorVendorId = 342;
  parameters.originatorSerialNumber = 0x12345;
  // Both directions: Point-to-Point (not multicast) + scheduled priority,
  // with a fixed 32-byte payload size matching the sample app's assemblies.
  parameters.t2oNetworkConnectionParams |= NetworkConnectionParams::P2P;
  parameters.t2oNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
  parameters.t2oNetworkConnectionParams |= 32;
  parameters.o2tNetworkConnectionParams |= NetworkConnectionParams::P2P;
  parameters.o2tNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
  parameters.o2tNetworkConnectionParams |= 32;
  // RPI is in microseconds on the wire; this is the sole benchmark variable.
  parameters.o2tRPI = rpiUs;
  parameters.t2oRPI = rpiUs;
  parameters.transportTypeTrigger |= NetworkConnectionParams::CLASS1;

  auto io = connectionManager.forwardOpen(si, parameters);
  auto ptr = io.lock();
  if (!ptr) {
    std::fprintf(stderr, "ForwardOpen failed\n");
    return 1;
  }

  // Fixed 32-byte payload layout, deterministic across every run:
  //   bytes [0, 8)  -- send-time steady_clock timestamp, nanoseconds
  //   bytes [8, 12) -- monotonically increasing sequence number
  //   bytes [12,32) -- zero padding, unused
  uint32_t seq = 0;
  auto makePayload = [&seq]() {
    std::vector<uint8_t> payload(32, 0);
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    std::memcpy(payload.data(), &ns, 8);
    std::memcpy(payload.data() + 8, &seq, 4);
    seq++;
    return payload;
  };

  // Single-threaded callback: EIPScanner invokes this synchronously from
  // inside connectionManager.handleConnections() below, on the same thread
  // -- no locking needed around the g_* accumulators.
  ptr->setReceiveDataListener([](auto /*header*/, auto /*seqCount*/, const std::vector<uint8_t> &data) {
    auto now = Clock::now();
    if (data.size() < 12) return;  // short/malformed frame, ignore
    uint64_t sentNs;
    std::memcpy(&sentNs, data.data(), 8);
    uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    double rttMs = static_cast<double>(nowNs - sentNs) / 1e6;
    if (rttMs < 0 || rttMs > kMaxSaneRttMs) {
      g_excludedStartup++;
      return;
    }
    g_rttMs.push_back(rttMs);
    g_arrivals.push_back(now);
    g_received++;
  });

  // Seed a real timestamped payload before the loop so the connection's
  // first production cycle never echoes a zero/uninitialized payload.
  ptr->setDataToSend(makePayload());

  auto start = Clock::now();
  auto deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(durationS));

  while (connectionManager.hasOpenConnections() && Clock::now() < deadline) {
    // Refresh the payload every loop iteration (~1ms cadence, set by the
    // handleConnections() timeout below) so the timestamp embedded at
    // actual transmit time is always as fresh as possible. IOConnection
    // only transmits once per RPI window internally, so the measured RTT
    // is end-to-end data-freshness latency (transmit-scheduling delay +
    // true network RTT), not raw wire latency -- see EIPSCANNER_BENCHMARK.md.
    ptr->setDataToSend(makePayload());
    connectionManager.handleConnections(std::chrono::milliseconds(1));
  }

  // EIPScanner scales its own client-side inactivity watchdog to the
  // requested RPI. If that RPI is below what the adapter can actually
  // sustain (OpENer's floor is ~10ms, see EIPSCANNER_BENCHMARK.md), the
  // watchdog fires and closes the connection client-side -- hasOpenConnections()
  // goes false -- before the deadline. Calling forwardClose() on an
  // already-closed connection asserts inside EIPScanner, so skip it and
  // report the early closure instead of crashing.
  bool closedEarly = !connectionManager.hasOpenConnections();
  if (!closedEarly) {
    connectionManager.forwardClose(si, io);
  } else {
    std::fprintf(stderr,
        "connection closed early by EIPScanner's own inactivity watchdog "
        "-- requested_rpi_us=%u is likely below the adapter's sustainable "
        "floor (skipping forwardClose on an already-dead connection)\n",
        rpiUs);
  }

  if (g_rttMs.empty()) {
    std::printf("target=%s rpi_us=%u duration_s=%.1f io_datapath=%s closed_early=%s received=0 (no data received)\n",
                targetIp.c_str(), rpiUs, durationS, ioDatapath.c_str(), closedEarly ? "true" : "false");
    return 0;
  }

  // Nearest-rank percentiles (index = size * p / 100, clamped) -- a fixed,
  // deterministic definition so results are directly comparable run to run
  // without depending on an interpolation method.
  std::vector<double> sorted = g_rttMs;
  std::sort(sorted.begin(), sorted.end());
  double sum = 0;
  for (double v : sorted) sum += v;
  double avg = sum / sorted.size();
  double p50 = sorted[sorted.size() * 50 / 100];
  double p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
  double min = sorted.front();
  double max = sorted.back();

  double sqSum = 0;
  for (double v : sorted) sqSum += (v - avg) * (v - avg);
  double stddev = std::sqrt(sqSum / sorted.size());

  // Jitter here is mean absolute difference between consecutive cycle
  // intervals (RFC 3550-style, not stddev) -- a fixed formula so this
  // number means the same thing across every RPI tested.
  std::vector<double> intervalsMs;
  for (size_t i = 1; i < g_arrivals.size(); i++) {
    intervalsMs.push_back(std::chrono::duration<double, std::milli>(g_arrivals[i] - g_arrivals[i - 1]).count());
  }
  double intervalAvg = 0, intervalJitter = 0;
  if (!intervalsMs.empty()) {
    double isum = 0;
    for (double v : intervalsMs) isum += v;
    intervalAvg = isum / intervalsMs.size();
    double ijsum = 0;
    for (size_t i = 1; i < intervalsMs.size(); i++) ijsum += std::fabs(intervalsMs[i] - intervalsMs[i - 1]);
    if (intervalsMs.size() > 1) intervalJitter = ijsum / (intervalsMs.size() - 1);
  }

  double actualDurationS = std::chrono::duration<double>(Clock::now() - start).count();
  double achievedRateHz = g_received / actualDurationS;
  double expectedCycles = actualDurationS * 1e6 / rpiUs;
  double receptionRatioPct = 100.0 * g_received / expectedCycles;

  std::printf("target=%s requested_rpi_us=%u duration_s=%.1f io_datapath=%s closed_early=%s\n",
              targetIp.c_str(), rpiUs, durationS, ioDatapath.c_str(), closedEarly ? "true" : "false");
  std::printf("cycles_received=%u expected_cycles=%.0f reception_ratio_pct=%.1f achieved_rate_hz=%.2f excluded_startup_frames=%u\n",
              g_received, expectedCycles, receptionRatioPct, achievedRateHz, g_excludedStartup);
  std::printf("rtt_ms: min=%.3f avg=%.3f p50=%.3f p99=%.3f max=%.3f stddev=%.3f\n",
              min, avg, p50, p99, max, stddev);
  std::printf("cycle_interval_ms: avg=%.3f mean_abs_jitter=%.3f\n", intervalAvg, intervalJitter);

  return 0;
}

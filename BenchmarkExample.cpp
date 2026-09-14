// I/O messaging (Class 1) latency/throughput benchmark against an EtherNet/IP
// adapter (OpENer). Embeds a send timestamp + sequence number in the O2T
// payload; the adapter mirrors O2T -> T2O (OpENer's sample app, assembly
// 150 -> 100), so the T2O receive callback can compute round-trip latency
// using a single (local) monotonic clock -- no cross-host clock sync needed.
//
// Usage: benchmark_example <target_ip> <rpi_us> <duration_s>

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
constexpr double kMaxSaneRttMs = 2000.0;  // filters pre-init zero-payload echoes
}

int main(int argc, char **argv) {
  Logger::setLogLevel(LogLevel::ERROR);

  std::string targetIp = argc > 1 ? argv[1] : "192.168.1.154";
  uint32_t rpiUs = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 10000;
  double durationS = argc > 3 ? std::stod(argv[3]) : 10.0;

  auto si = std::make_shared<SessionInfo>(targetIp, 0xAF12);

  ConnectionManager connectionManager;
  ConnectionParameters parameters;
  parameters.connectionPath = {0x20, 0x04, 0x24, 151, 0x2C, 150, 0x2C, 100};
  parameters.o2tRealTimeFormat = true;
  parameters.originatorVendorId = 342;
  parameters.originatorSerialNumber = 0x12345;
  parameters.t2oNetworkConnectionParams |= NetworkConnectionParams::P2P;
  parameters.t2oNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
  parameters.t2oNetworkConnectionParams |= 32;
  parameters.o2tNetworkConnectionParams |= NetworkConnectionParams::P2P;
  parameters.o2tNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
  parameters.o2tNetworkConnectionParams |= 32;
  parameters.o2tRPI = rpiUs;
  parameters.t2oRPI = rpiUs;
  parameters.transportTypeTrigger |= NetworkConnectionParams::CLASS1;

  auto io = connectionManager.forwardOpen(si, parameters);
  auto ptr = io.lock();
  if (!ptr) {
    std::fprintf(stderr, "ForwardOpen failed\n");
    return 1;
  }

  uint32_t seq = 0;
  auto makePayload = [&seq]() {
    std::vector<uint8_t> payload(32, 0);
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    std::memcpy(payload.data(), &ns, 8);
    std::memcpy(payload.data() + 8, &seq, 4);
    seq++;
    return payload;
  };

  ptr->setReceiveDataListener([](auto /*header*/, auto /*seqCount*/, const std::vector<uint8_t> &data) {
    auto now = Clock::now();
    if (data.size() < 12) return;
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
    ptr->setDataToSend(makePayload());
    connectionManager.handleConnections(std::chrono::milliseconds(1));
  }

  connectionManager.forwardClose(si, io);

  if (g_rttMs.empty()) {
    std::printf("target=%s rpi_us=%u duration_s=%.1f received=0 (no data received)\n",
                targetIp.c_str(), rpiUs, durationS);
    return 0;
  }

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

  std::printf("target=%s requested_rpi_us=%u duration_s=%.1f\n", targetIp.c_str(), rpiUs, durationS);
  std::printf("cycles_received=%u expected_cycles=%.0f reception_ratio_pct=%.1f achieved_rate_hz=%.2f excluded_startup_frames=%u\n",
              g_received, expectedCycles, receptionRatioPct, achievedRateHz, g_excludedStartup);
  std::printf("rtt_ms: min=%.3f avg=%.3f p50=%.3f p99=%.3f max=%.3f stddev=%.3f\n",
              min, avg, p50, p99, max, stddev);
  std::printf("cycle_interval_ms: avg=%.3f mean_abs_jitter=%.3f\n", intervalAvg, intervalJitter);

  return 0;
}

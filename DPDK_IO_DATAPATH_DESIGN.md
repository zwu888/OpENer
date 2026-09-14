# Design: DPDK-Accelerated I/O Datapath for OpENer

Status: **Proposal / not yet implemented**
Scope agreed with stakeholder: accelerate only the cyclic Class 0/1 I/O
messaging datapath with DPDK; explicit (TCP) messaging and UDP discovery
stay on the kernel socket stack.

## 1. Goals / Non-Goals

**Goals**

- Remove kernel-stack and `select()`-loop jitter from the cyclic I/O
  (Class 0/1, UDP port 2222) send/receive path for latency- and
  throughput-sensitive adapters.
- Reuse all existing CIP logic unmodified: CPF/sequence-count/run-idle
  framing (`cipioconnection.c`), assembly data handling
  (`cipassembly.c`), and the connection state machine
  (`cipconnectionmanager.c`, `cipconnectionobject.c`).
- Land as an additive, compile-time-selectable backend, not a fork of the
  POSIX port — explicit messaging, discovery, and all non-Linux ports keep
  working exactly as today when the feature is off.

**Non-Goals (initial version)**

- Moving TCP explicit messaging (registration, ForwardOpen/Close,
  Get/Set_Attribute) to DPDK. That needs a full user-space TCP/IP stack
  (F-Stack, mTCP, …) and is a separate, much larger effort.
- Multicast I/O connections. IGMP membership handling is done by the
  kernel today; a DPDK datapath bypasses it. First version supports
  Point-to-Point I/O connections only (this is also what OpENer's sample
  app / EDS `Connection1` "Exclusive Owner" uses).
- Multi-queue / RSS scaling. A single adapter typically serves a handful
  of concurrent I/O connections at low packet rates (RPI in the
  millisecond range) — one RX queue and one TX queue on one dedicated
  core is enough.

## 2. Why this is a bigger question than "swap sockets for DPDK"

A DPDK poll-mode driver (PMD) takes **exclusive ownership of a NIC** away
from the kernel. Anything else that needs that NIC (SSH to the box, the
explicit-messaging TCP session, ARP, ICMP) stops working through the
normal network stack once it's DPDK-bound. Two deployment shapes handle
this differently, and the design needs to name which one it targets:

| Option | How it works | Tradeoffs |
|---|---|---|
| **A. Dedicated NIC** | Explicit messaging + discovery stay on one NIC via the kernel; a *second*, otherwise-unused NIC (or SR-IOV VF) is bound to a DPDK PMD (`vfio-pci`/`uio_pci_generic`) purely for I/O traffic. | Simplest to implement and reason about. Requires two physically separate L2-reachable NICs/VFs, which many "small adapter" deployments won't have. |
| **B. AF_XDP (shared NIC)** | One NIC stays under the kernel driver; an XDP program in the kernel redirects only UDP-port-2222 traffic into an `AF_XDP` socket, which DPDK's `net_af_xdp` PMD consumes in userspace. Everything else (ARP, ICMP, TCP 44818, discovery) flows through the kernel exactly as today. | No dedicated NIC needed; ARP/routing/multicast stay handled by the kernel for free. Lower ceiling than a fully dedicated PMD (still crosses into kernel once for the XDP redirect), but that's the right tradeoff for a device that also needs to keep talking TCP on the same wire. |

**Recommendation:** build the transport-abstraction seam (Section 5) so
it doesn't care which of the two is underneath, but implement and test
against **Option B (AF_XDP)** first. It's deployable on the same single
NIC as everything else — which matches how the existing OpENer sample
app is actually used — and defers the ARP/routing reimplementation that
Option A would force onto the application. Option A becomes a drop-in
alternative later for users who do have a spare NIC/VF and want the
absolute lowest jitter.

This machine (used for interop testing in `EIPSCANNER_TESTING.md`) has
DPDK 24.11 + hugepages already configured, an idle NIC (`0000:09:00.0`,
no kernel driver bound) usable for Option A experiments, and its active
NICs (`eno1`, `enp21s0np0`) intentionally excluded from any binding — the
box's own network access depends on them.

## 3. Network topology

Concrete topology for both options, using the hosts already in play for
interop/benchmark testing (`EIPSCANNER_TESTING.md`,
`EIPSCANNER_BENCHMARK.md`): the OpENer adapter at `192.168.1.154` and the
EIPScanner test host "hp6z4" at `192.168.1.151`, both on the same
`192.168.1.0/24` LAN segment today.

A visual side-by-side of both topologies, the kernel-hop comparison behind
the jitter-ceiling tradeoff, and a datasheet-style spec comparison is
published at
**[I/O Datapath Fork](https://claude.ai/code/artifact/fdab37cd-5ac4-4123-87b0-daec3f8efe63)**.

### Option B — AF_XDP, single shared NIC (recommended first target)

No topology change from what's tested today — one NIC, one cable, one
switch port. The split happens inside the OpENer host, in software:

```
                    192.168.1.0/24 LAN (existing switch)
                                 │
              ┌──────────────────┴──────────────────┐
              │                                      │
   ┌──────────┴───────────┐              ┌───────────┴──────────┐
   │  OpENer host           │              │  hp6z4 (EIPScanner)   │
   │  eno1  192.168.1.154    │              │  192.168.1.151          │
   │  (single NIC, kernel-    │              │  (ordinary kernel NIC,  │
   │   owned, unchanged)       │              │   no DPDK needed here)   │
   │                            │              └───────────────────────┘
   │  ┌──────────────────────┐  │
   │  │ Kernel network stack  │  │ ◀── TCP :44818  (explicit messaging)
   │  │                        │  │ ◀── UDP :44818  (discovery, ARP, ICMP, SSH)
   │  └───────────┬────────────┘  │
   │              │ XDP program redirects
   │              │ UDP dst-port 2222 only
   │  ┌───────────┴────────────┐  │
   │  │ AF_XDP socket            │  │ ◀── UDP :2222  (Class 0/1 cyclic I/O)
   │  │  → net_af_xdp DPDK PMD    │  │
   │  │  → dpdk_io_datapath.c     │  │
   │  │    (Thread B, isolated    │  │
   │  │     core, RX/TX poll loop)│  │
   │  └────────────────────────────┘  │
   └────────────────────────────────────┘
```

Everything not matching the XDP redirect filter (ARP, ICMP, TCP 44818, SSH,
UDP 44818 discovery) falls through to the kernel exactly as it does on the
current all-socket build — this is what makes Option B deployable on the
same single-NIC box already in use for interop testing, with no cabling or
switch-port changes.

#### How TCP and I/O traffic share one NIC under Option B

This is not DPDK taking ownership of the NIC the way Option A's `vfio-pci`
binding does. The NIC stays bound to its normal kernel driver (`i40e` for
the X722, `mlx5_core` for the ConnectX-4) the entire time. What changes is
a small eBPF/XDP program loaded into that driver's RX hook, which decides
per-packet which of two paths a frame takes — *before* the kernel's normal
IP stack ever sees it:

```
                    NIC (kernel driver, e.g. i40e), unchanged
                              │
                    RX ring, XDP hook (native driver mode)
                              │
              ┌────────────────────────────────────┐
              │  XDP program (eBPF, loaded once):    │
              │    if (udp && dst_port == 2222)      │
              │        → bpf_redirect_map(XSKMAP)    │  → AF_XDP socket → net_af_xdp
              │    else                              │     PMD → dpdk_io_datapath.c
              │        → XDP_PASS                    │
              └────────────────────────────────────┘
                     │                    │
        (everything else:          (only UDP:2222,
         TCP:44818, ARP,            Class 0/1 I/O)
         ICMP, UDP:44818
         discovery, SSH)
                     │
       normal kernel network stack, unchanged
       (generic_networkhandler.c's select() loop,
        CheckAndHandleTcpListenerSocket, etc.)
```

Key mechanics:

- **`XDP_PASS` is the default and does nothing special** — a frame that
  doesn't match the filter continues exactly as it does on a plain kernel
  build today. `CheckAndHandleTcpListenerSocket`, `HandleDataOnTcpSocket`,
  `CheckAndHandleUdpUnicastSocket` (discovery) all keep working unmodified
  because their packets simply never enter the redirect branch.
- **`XDP_REDIRECT` diverts the frame at the driver level**, before
  `netif_receive_skb()` — the kernel's UDP/IP stack never allocates an
  `sk_buff` for it or delivers it to a bound socket. This means
  `CheckAndHandleConsumingUdpSocket()` (today's socket-based UDP:2222
  reader) simply stops receiving anything once the XDP program is loaded —
  it becomes dead code under `OPENER_IO_DATAPATH=DPDK`, replaced by a poll
  into `dpdk_io_datapath.c` (§6) rather than deleted outright, so the
  SOCKET build path stays untouched.
- **The AF_XDP socket (`xsk`) is what DPDK's `net_af_xdp` PMD wraps** —
  `rte_eth_rx_burst`/`tx_burst` in `dpdk_io_datapath.c` (Thread B, §6)
  operate against this socket's shared-memory rings (`umem`), not against a
  raw NIC queue. From the CIP-layer code's point of view nothing changes —
  still just "poll for RX, build a TX mbuf" — but the plumbing underneath
  is a kernel-mediated zero-copy socket, not a PMD with hardware DMA rings.
- **Filter granularity**: the simplest version matches UDP dst-port 2222
  alone (static). A more correct refinement would only redirect traffic
  from a source IP with an *active* Class 1 connection — updated into a BPF
  map by `dpdk_io_datapath.c` on each `ForwardOpen`/`ForwardClose` — so a
  stray UDP:2222 packet from an unrelated host before any connection exists
  falls through to `XDP_PASS` instead of vanishing into a redirect with
  nothing listening on the DPDK side.
- **TX side is symmetric but separate**: OpENer's TCP responses (explicit
  messaging) go out via the normal `sendto()`/socket path exactly as today.
  T2O UDP:2222 packets go out via the AF_XDP socket's TX ring, with
  `dpdk_io_datapath.c` hand-building the Ethernet/IP/UDP headers itself
  (§7 — DPDK doesn't do sockets-style send). Both TX paths ultimately hit
  the same physical NIC TX hardware as two independent software paths — no
  coordination needed since UDP:2222 and TCP:44818 never contend for the
  same socket or buffer.
- **Single L2/L3 identity, no ARP duplication**: one NIC, one kernel-owned
  IP/MAC — ARP requests get `XDP_PASS`'d and answered by the kernel exactly
  as today. This is why `dpdk_arp.c` in the proposed module layout (§5) is
  scoped to Option A only; Option B never needs its own ARP responder.
- **Driver requirement**: this needs *native* (driver-mode) XDP support,
  not just generic/SKB-mode XDP, to get the zero-copy AF_XDP path DPDK's
  PMD expects. Both NICs on this test box qualify — `i40e` (X722) and
  `mlx5_core` (ConnectX-4) both support native XDP — so this is
  implementable here without new hardware.

### Option A — dedicated NIC, separate L2 segment

Requires a genuinely separate NIC (or SR-IOV VF) on **both** ends — the
scanner/PLC side also needs a second port on the dedicated segment, since a
DPDK-bound NIC no longer speaks ARP or answers on any other protocol:

```
        management LAN (192.168.1.0/24, existing switch)
                          │
   ┌──────────────────────┴──────────────────────┐
   │                                              │
┌──┴──────────────────────┐              ┌────────┴──────────────┐
│ OpENer host               │              │ hp6z4 (mgmt / SSH)      │
│ eno1  192.168.1.154        │              │ 192.168.1.151             │
│ (kernel: TCP:44818,         │              └────────────────────────┘
│  UDP:44818, SSH, ARP, ICMP) │
└──┬──────────────────────────┘
   │  second, otherwise-idle NIC — no kernel driver bound
┌──┴──────────────────────┐     dedicated I/O-only L2 segment      ┌────────────────────────┐
│ 0000:09:00.0 (X722)       │ ════════════════════════════════════▶│ Scanner/PLC I/O NIC      │
│ DPDK PMD (vfio-pci)        │   direct cable, or its own switch/    │ (separate port from its   │
│ dpdk_io_datapath.c          │   VLAN — UDP :2222 only, no ARP/       │  own management NIC)      │
│ (Thread B, isolated core)    │   DHCP/SSH needed on this segment      │                           │
└────────────────────────────┘                                    └────────────────────────┘
```

On this test machine, `0000:09:00.0` (an idle Intel X722 port, confirmed
unbound via `dpdk-devbind.py --status`) is the concrete candidate for this
role — see §2. Note this option isn't testable against hp6z4 as configured
today, since hp6z4 only has its single management NIC on the shared LAN; it
would need its own second, DPDK- or otherwise dedicated-port to become a
real Option-A peer.

## 4. Current architecture (for reference)

```
                 select() loop, generic_networkhandler.c
                 ┌─────────────────────────────────────────┐
TCP :44818  ───▶ │ CheckAndHandleTcpListenerSocket / Data   │──▶ encap.c ──▶ MessageRouter ──▶ CIP objects
UDP :44818  ───▶ │ CheckAndHandleUdp{Unicast,Broadcast}Sock │  (explicit messaging + discovery)
UDP :2222   ───▶ │ CheckAndHandleConsumingUdpSocket         │──▶ HandleReceivedConnectedData()  (cipconnectionmanager.c)
                 │                                          │        └─▶ connection_object->connection_receive_data_function
                 │                                          │             = HandleReceivedIoConnectionData (cipioconnection.c)
UDP :2222  ◀──── │ SendUdpData()  ◀── SendConnectedData() ◀─┤             (strips seq count / run-idle header, size-checks,
                 │                    (cipioconnection.c)   │              writes CipByteArray, calls AfterAssemblyDataReceived)
                 └─────────────────────────────────────────┘
                            │
                 every kOpenerTimerTickInMilliSeconds:
                 ManageConnections() → per-connection notifyTick-equivalent
                 → BeforeAssemblyDataSend() → SendConnectedData() for due connections
```

Everything from "CPF/sequence/run-idle framing" down to the assembly
object is transport-agnostic already — `HandleReceivedConnectedData()`
takes a raw payload + `sockaddr_in`, and `SendUdpData()` takes a
`sockaddr_in` + fully-framed payload. **This is the seam.** Confirmed by
reading the actual source (not assumed):

- `EipStatus SendUdpData(const struct sockaddr_in *const address, ENIPMessage *const message)` —
  declared in `opener_api.h`, implemented once in
  `generic_networkhandler.c`, called from `cipioconnection.c:908`.
- `EipStatus HandleReceivedConnectedData(const EipUint8 *const data, int data_length, struct sockaddr_in *const from_address)` —
  declared in `opener_api.h`, implemented in `cipconnectionmanager.c`,
  called from `generic_networkhandler.c:1110` after a `recvfrom()`.

Neither function's *signature* mentions sockets — only the two
implementations do. That means a DPDK backend can replace just those two
implementations (and the RX polling loop that used to be
`CheckAndHandleConsumingUdpSocket`), without touching `cipioconnection.c`,
`cipconnectionmanager.c`, or anything above them.

## 5. Proposed module layout

```
source/src/ports/DPDK/
  dpdk_io_datapath.c / .h     -- new module, built only when OPENER_IO_DATAPATH=DPDK
  dpdk_arp.c / .h             -- minimal ARP responder + resolver (Option A only;
                                  not needed for Option B, kernel owns ARP)
CMakeLists.txt                -- new OPENER_IO_DATAPATH option (SOCKET default | DPDK)
```

`generic_networkhandler.c` keeps `CheckAndHandleTcpListenerSocket`,
`HandleDataOnTcpSocket`, `CheckAndHandleUdpUnicastSocket`, and
`CheckAndHandleUdpGlobalBroadcastSocket` exactly as-is — those stay on
kernel sockets in both options. Only these two pieces get an
`#ifdef`/link-time swap:

- `SendUdpData()` — when `OPENER_IO_DATAPATH=DPDK`, this symbol is
  provided by `dpdk_io_datapath.c` instead of `generic_networkhandler.c`,
  and it enqueues a pre-built mbuf onto a TX ring instead of calling
  `sendto()`.
- The consuming-socket poll (`CheckAndHandleConsumingUdpSocket`) is
  replaced by a call into `dpdk_io_datapath.c`'s RX drain function from
  the same cyclic tick, which calls the *same*
  `HandleReceivedConnectedData()` the socket path calls today.

No new CIP-layer code, no change to assembly handling, no change to
`ManageConnections()`'s cadence.

## 6. Threading & timing model

DPDK poll-mode RX (`rte_eth_rx_burst`) is a tight spin loop — it cannot
share a thread with the `select()`-based explicit-messaging loop, which
blocks. Proposed model:

- **Thread A (existing)**: the current `NetworkHandlerProcessCyclic()`
  loop, unchanged — TCP explicit messaging, discovery, and the
  `kOpenerTimerTickInMilliSeconds` tick that drives `ManageConnections()`
  and `BeforeAssemblyDataSend()`.
- **Thread B (new)**: pinned to one isolated core, runs the DPDK RX/TX
  poll loop. On RX, it builds the `sockaddr_in` + payload exactly as
  `CheckAndHandleConsumingUdpSocket` does today and calls
  `HandleReceivedConnectedData()` directly.
- **Cross-thread data**: `HandleReceivedConnectedData()` →
  `HandleReceivedIoConnectionData()` → `NotifyAssemblyConnectedDataReceived()`
  writes straight into the assembly's `CipByteArray`, same as it does
  from the socket path today — no new synchronization needed there, since
  it's the exact same call chain, just invoked from thread B instead of
  the select() thread. What *does* need a small critical section (a
  spinlock or `rte_ring` SPSC handoff, TBD during implementation) is
  anything thread B and thread A both touch outside that call chain —
  namely the connection object list iteration in
  `CheckAndHandleConsumingUdpSocket`'s replacement, and
  `SendUdpData`/TX-ring access if `SendConnectedData()` can ever be
  invoked from thread A (it can — `BeforeAssemblyDataSend` fires from the
  `ManageConnections` tick on thread A). This needs one of:
  - an SPSC `rte_ring` from thread A → thread B carrying "connection X is
    due to send, here's the current assembly snapshot", with thread B
    doing the actual `rte_eth_tx_burst`, or
  - moving connection timer/tick management onto thread B entirely
    (simplest, but means thread B owns `ManageConnections()` for I/O
    connections specifically, which is a small refactor of who calls
    what — flagged as an open question for the implementation phase, not
    settled by this doc).

## 7. Packet construction

mbuf layout for a T2O (produced) packet is just today's `SendUdpData`
payload wrapped in Ethernet/IP/UDP headers built by hand (`rte_ether_hdr`
+ `rte_ipv4_hdr` + `rte_udp_hdr` prepended via `rte_pktmbuf_prepend`) —
DPDK does not give you a sockets-style UDP send; the application builds
every header itself. Concretely:

- Destination MAC: Option B — the kernel's ARP table already has it
  (same NIC, same L2 domain as everything else); the AF_XDP PMD path can
  read it via a small helper (or the design can require the O2T
  `SessionInfo`/ForwardOpen caller to have already ARPed, e.g. by the TCP
  explicit-messaging exchange having just happened over the same link).
  Option A needs `dpdk_arp.c` to maintain its own tiny ARP cache.
- Source MAC/IP: the DPDK port's own address (Option A) or the shared
  NIC's existing address (Option B).
- Checksums: offload to the NIC (`RTE_MBUF_F_TX_IP_CKSUM` /
  `..._UDP_CKSUM`) where the PMD supports it; software fallback
  otherwise. UDP checksum can also legally be `0` (IPv4 UDP allows
  disabling it) if offload isn't available and CPU cost matters more
  than end-to-end integrity — flagged as an implementation-time choice,
  not a correctness requirement either way for a LAN-local control link.

## 8. Build integration

```cmake
# source/CMakeLists.txt (new option, POSIX platform only)
set(OPENER_IO_DATAPATH "SOCKET" CACHE STRING "I/O (Class 0/1) datapath backend: SOCKET or DPDK")
set_property(CACHE OPENER_IO_DATAPATH PROPERTY STRINGS SOCKET DPDK)

if(OPENER_IO_DATAPATH STREQUAL "DPDK")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
  add_definitions(-DOPENER_IO_DATAPATH_DPDK)
  target_link_libraries(OpENer PRIVATE PkgConfig::DPDK)
  # dpdk_io_datapath.c replaces generic_networkhandler.c's SendUdpData
  # and the consuming-UDP-socket branch of NetworkHandlerProcessCyclic.
endif()
```

`pkg-config libdpdk` already resolves correctly on this machine
(`libdpdk-dev` 24.11.4 is installed), so this is a real, testable option
here, not speculative.

## 9. Testing plan (once implemented)

1. **Unit-level parity check**: point EIPScanner's `implicit_messaging`
   example (already used in `EIPSCANNER_TESTING.md`) at an
   `OPENER_IO_DATAPATH=SOCKET` build and an `OPENER_IO_DATAPATH=DPDK`
   build in turn, from the *same* client, and diff the received T2O
   byte sequences — they must be identical. This directly reuses the
   pattern-mirroring test already built (`test_io_verify.cpp` /
   `test_io_verify2.cpp` in that session's scratchpad).
2. **Latency/jitter comparison**: capture T2O packet inter-arrival time
   distribution under both backends at a fixed RPI, to quantify the
   actual benefit before calling the migration worthwhile for a given
   deployment.
3. **Same same-host caveat as before**: if the DPDK-bound NIC and the
   test client are on the same physical box, they must be on genuinely
   separate L2 segments (separate NIC + cabling, or separate VMs/netns) —
   the UDP-port-2222 wildcard-bind collision documented in
   `EIPSCANNER_TESTING.md` applies here too, and DPDK's exclusive NIC
   ownership doesn't change that if the *test client* is still a normal
   socket app on the same host reusing the same port.

## 10. Open questions for implementation phase

- Final choice between Option A and B (recommend starting with B, see
  §2).
- Where `ManageConnections()`/timer ownership for I/O connections ends up
  living (thread A vs thread B, §6) — affects whether any `rte_ring`
  handoff is needed at all.
- Whether to support graceful fallback (run on `OPENER_IO_DATAPATH=SOCKET`
  automatically if DPDK EAL init fails at startup, e.g. no hugepages
  configured) or fail hard. Recommend fail hard initially — silent
  fallback to a 10x-worse-jitter datapath is a worse failure mode for a
  control system than refusing to start.

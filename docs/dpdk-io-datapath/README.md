# DPDK I/O Datapath — Overview

This folder is the entry point for the proposal to accelerate OpENer's
cyclic Class 0/1 I/O messaging with DPDK. It exists to hold diagrams; the
written rationale and open questions live in the two root-level documents
this links to.

- **Full design & rationale:** [`../../DPDK_IO_DATAPATH_DESIGN.md`](../../DPDK_IO_DATAPATH_DESIGN.md)
- **Interop test findings this builds on:** [`../../EIPSCANNER_TESTING.md`](../../EIPSCANNER_TESTING.md)
- Status: **proposal, not yet implemented**

## Current architecture

Everything — TCP explicit messaging, UDP discovery, and UDP Class 0/1 I/O —
goes through one `select()` loop on one thread.

```mermaid
flowchart LR
    subgraph NIC["Kernel network stack (one NIC)"]
        TCP["TCP :44818"]
        UDPD["UDP :44818\n(discovery)"]
        UDPIO["UDP :2222\n(I/O)"]
    end

    subgraph Loop["generic_networkhandler.c — single select() thread"]
        SEL["NetworkHandlerProcessCyclic()"]
        TICK["every kOpenerTimerTickInMilliSeconds:\nManageConnections()"]
    end

    subgraph CIP["CIP core (unchanged either way)"]
        HRCD["HandleReceivedConnectedData()\ncipconnectionmanager.c"]
        HRIO["HandleReceivedIoConnectionData()\ncipioconnection.c"]
        SCD["SendConnectedData()\ncipioconnection.c"]
        ASM["Assembly object\ncipassembly.c"]
    end

    TCP --> SEL
    UDPD --> SEL
    UDPIO --> SEL
    SEL --> HRCD --> HRIO --> ASM
    TICK --> SCD -->|SendUdpData\nsendto| UDPIO
```

## Proposed hybrid architecture (Option B: shared NIC via AF_XDP)

Only the UDP :2222 path moves. TCP explicit messaging and discovery are
untouched — same thread, same code, same kernel sockets. The two
functions already transport-agnostic today (verified in the design doc)
become the seam where a DPDK/AF_XDP backend plugs in.

```mermaid
flowchart LR
    subgraph Kernel["Kernel network stack (same NIC)"]
        TCP["TCP :44818"]
        UDPD["UDP :44818 (discovery)"]
        ARP["ARP / ICMP / routing"]
        XDP["XDP program:\nredirect UDP:2222 → AF_XDP socket"]
    end

    subgraph ThreadA["Thread A — existing select() loop (unchanged)"]
        SEL["NetworkHandlerProcessCyclic()"]
    end

    subgraph ThreadB["Thread B — new, pinned core, DPDK/AF_XDP poll loop"]
        RX["rte_eth_rx_burst()"]
        TX["rte_eth_tx_burst()"]
    end

    subgraph CIP["CIP core — byte-for-byte unchanged"]
        HRCD["HandleReceivedConnectedData()"]
        HRIO["HandleReceivedIoConnectionData()"]
        SCD["SendConnectedData()"]
        ASM["Assembly object"]
        SUD["SendUdpData()\n— DPDK impl instead of sendto()"]
    end

    TCP --> SEL
    UDPD --> SEL
    XDP -->|AF_XDP PMD| RX
    RX --> HRCD --> HRIO --> ASM
    SCD --> SUD --> TX -->|AF_XDP PMD| XDP
```

## RX/TX sequence (per §5 of the design doc)

```mermaid
sequenceDiagram
    participant NIC as NIC / AF_XDP
    participant B as Thread B (DPDK poll loop)
    participant CIP as CIP core (HandleReceivedConnectedData → assembly)
    participant A as Thread A (select loop / ManageConnections tick)

    NIC->>B: mbuf (O2T datagram)
    B->>CIP: HandleReceivedConnectedData(payload, len, from_addr)
    CIP->>CIP: strip seq count + run/idle header, size check
    CIP->>CIP: write CipByteArray, AfterAssemblyDataReceived()

    A->>A: ManageConnections() tick fires
    A->>CIP: BeforeAssemblyDataSend()
    CIP->>CIP: SendConnectedData() builds framed payload
    Note over A,B: Open question (design doc §9):<br/>does A hand off to B via rte_ring,<br/>or does B own the I/O connection timers directly?
    CIP->>B: SendUdpData() → DPDK TX path
    B->>NIC: rte_eth_tx_burst(mbuf)
```

## Reading order

1. [`../../EIPSCANNER_TESTING.md`](../../EIPSCANNER_TESTING.md) — why this
   matters: the interop testing that exercised the exact I/O datapath
   these diagrams show, and the same-host UDP port collision it
   surfaced (relevant to how any DPDK version gets tested too).
2. [`../../DPDK_IO_DATAPATH_DESIGN.md`](../../DPDK_IO_DATAPATH_DESIGN.md) —
   full write-up: goals/non-goals, Option A vs B tradeoffs, the exact
   seam functions with file/line references, threading model, build
   integration, and open questions.
3. This file — the diagrams, kept separate so the design doc stays
   readable as text and these can be updated independently as the design
   evolves.

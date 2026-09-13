# Interop Testing: OpENer vs. EIPScanner

This document records a manual interoperability test session between **OpENer**
(this repo, commit `d8820ef`, v2.3.0) and
[**EIPScanner**](https://github.com/nimbuscontrols/EIPScanner) (commit `12c89a5`),
a C++ EtherNet/IP scanner library. It covers what was tested, what passed
immediately, one non-obvious bug that showed up during I/O testing (and turned
out to be a test-setup artifact, not a defect in either project), and how to
reproduce the tests.

## Environment

- Host: Debian 13 "trixie", Linux 6.12, glibc 2.41
- OpENer built via `bin/posix/setup_posix.sh` (POSIX port, static build)
- EIPScanner built via its own `cmake && cmake --build .` in `EIPScanner/build`
- Both projects were already cloned/built on the test machine at
  `~/git/OpENer` and `~/git/EIPScanner`

## Summary

| Feature                                   | Result |
|--------------------------------------------|--------|
| UDP discovery (`List_Identity`)             | ✅ Pass |
| Explicit messaging — Identity Object read   | ✅ Pass |
| Explicit messaging — generic Get/Set attribute | ✅ Pass |
| Implicit (Class 1) I/O messaging            | ✅ Pass (after fixing test setup — see below) |

## 1. Discovery (UDP `List_Identity`)

EIPScanner's `DiscoveryManager` broadcast a `List_Identity` request; OpENer
replied and was correctly identified:

```
Discovered device: OpENer PC with address 192.168.1.154:44818
```

## 2. Explicit messaging

- `IdentityObject` (class 0x01, instance 1) — read Vendor ID, Device Type,
  Product Code, Revision, Status, Serial Number, Product Name. All fields
  decoded correctly (`OpENer PC`, VendorId=1, ProductCode=65001, Revision
  2.3, ...).
- Generic `MessageRouter::sendRequest`:
  - `GET_ATTRIBUTE_SINGLE` on Identity Vendor ID → `1`
  - `SET_ATTRIBUTE_SINGLE` write of 10 bytes to Assembly instance 151
    (OpENer's sample-app config assembly) → success

Both round-tripped over the TCP explicit-messaging session with no issues.

## 3. Implicit (Class 1) I/O messaging

OpENer's POSIX sample app (`sampleapplication.c`) wires up three assemblies
for its demo I/O connection:

- **151** – config assembly
- **150** – output assembly (O2T, consumed by OpENer)
- **100** – input assembly (T2O, produced by OpENer)

and mirrors output data straight to the input assembly in
`AfterAssemblyDataReceived()`, which makes it a convenient loopback test: any
byte pattern written to instance 150 should reappear on instance 100.

### First attempt: false failure

Opening a Class 1 exclusive-owner connection worked (`ForwardOpen` succeeded,
cyclic data flowed both ways), but a known test pattern written to the output
assembly never showed up on the input side — explicit reads of assembly 150
kept coming back all zero, and the scanner logged spurious
`Received data from unknown connection T2O_ID=...` errors.

**Root cause:** both OpENer and EIPScanner were running as separate processes
*on the same host*, and both bind their I/O-messaging UDP socket to the
EtherNet/IP-mandated port using a wildcard address, `0.0.0.0:2222`
(confirmed with `ss -uan`, which showed two distinct sockets on that exact
address). With two same-host sockets sharing that address:port, the kernel
delivers each inbound datagram to only one of them, non-deterministically.
The practical effect:

- EIPScanner's own transmitted O2T packets sometimes got looped straight
  back into EIPScanner's own receiving socket instead of reaching OpENer
  (hence the "unknown connection" errors — it was seeing its own O2T
  connection ID where it expected a T2O one).
- OpENer's socket never actually received the real O2T traffic, so its
  output assembly never updated.

This is **not a bug in OpENer or EIPScanner** — it's an artifact of testing
both ends of an EtherNet/IP conversation from the same IP address, which
doesn't happen in real deployments (adapter and scanner are always separate
devices). The fixed UDP port 2222 from the CIP spec is exactly the kind of
thing that collides when you try to shortcut a two-device test onto one box.

### Fix: separate network namespaces

Isolating OpENer in its own Linux network namespace (a Docker container on a
user-defined bridge network) gives it its own independent socket table, so
its `0.0.0.0:2222` bind no longer collides with the host's:

```bash
docker network create --subnet=172.30.0.0/24 claude-eip-test
docker run -d --name opener-test --network claude-eip-test --ip 172.30.0.10 \
  -v /home/rwu/git/OpENer/bin/posix:/opt/opener:ro \
  debian:trixie-slim sleep infinity
docker exec -d opener-test /opt/opener/src/ports/POSIX/OpENer eth0
```

EIPScanner then ran unmodified on the host, targeting the container's IP
(`172.30.0.10`).

### Result after the fix

```
--- BEFORE opening I/O connection ---
Output Assembly (inst 150): 00 00 00 00 ... (32 bytes)
Input  Assembly (inst 100): 00 00 00 00 ... (32 bytes)

Open IO connection O2T_ID=... T2O_ID=... SerialNumber 1
Open UDP socket to send data to 172.30.0.10:2222

--- AFTER 2s of I/O with pattern sent on O2T ---
Output Assembly (inst 150): a0 a1 a2 a3 ... bf
Input  Assembly (inst 100): a0 a1 a2 a3 ... bf   <- correctly mirrored
```

A follow-up run with a live T2O listener confirmed the mirrored pattern was
received correctly on **every one of 20 cycles** at a 200 ms RPI:

```
Packets received: 20  Pattern echoed back correctly: YES
```

This confirms OpENer's Class 1 connection setup, run/idle header handling
(`OPENER_CONSUMED_DATA_HAS_RUN_IDLE_HEADER=ON` in this build), sequence
counting, and assembly data path all interoperate correctly with
EIPScanner's implicit-messaging implementation.

## Reproducing

1. Build OpENer for POSIX (`bin/posix/setup_posix.sh && make`).
2. Build EIPScanner (`mkdir build && cd build && cmake .. && cmake --build .`).
3. Run OpENer inside an isolated network namespace (own container/VM, or a
   second physical/virtual host) rather than on the same IP as the test
   client — this avoids the port 2222 collision described above.
4. Use EIPScanner's `examples/` (`discovery_example`,
   `identity_object_example`, `explicit_messaging`,
   `implicit_messaging`) as a starting point; the stock examples hardcode
   IPs (`172.28.x.x` / `127.0.0.1`), so point them at wherever OpENer is
   actually listening.
5. For I/O testing, OpENer's sample app assemblies 151/150/100
   (config/output/input) mirror output → input, which makes round-trip data
   verification straightforward.

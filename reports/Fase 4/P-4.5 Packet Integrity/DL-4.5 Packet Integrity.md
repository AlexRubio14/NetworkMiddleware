# DL-4.5 — CRC32 Packet Integrity & Scalability Gauntlet

**Date:** 2026-03-23
**Branch:** `P-4.5-Packet-Integrity`
**PR:** #13

---

## Why this step exists

After P-4.4 introduced parallel snapshot serialization, a question remained open for the TFG: can the delta-compression pipeline be trusted end-to-end? A single-bit corruption in a delta packet could silently produce a wrong hero position on every client — and with VLE encoding, a flipped bit can cascade across the entire varint. CRC32 closes that gap.

The second motivation was data for the Memoria. P-4.3 proved the middleware fits a 100Hz loop with 47 clients at 1.1% tick budget. But the Split-Phase job system from P-4.4 was never benchmarked against a sequential baseline under maximum load. P-4.5 provides that comparison.

---

## Handling the handoff design flaws

Gemini's handoff had three issues I caught before writing a line of code:

**1. CRC inside PacketHeader.**
The handoff proposed adding a `crc` field to `PacketHeader`. This can't work: the header is serialized first, then the payload is appended. When you write the header you don't yet know the payload bytes, so there's nothing to checksum. The correct design is a *trailer* — append the CRC after the full payload is assembled. The wire format becomes `[Header][Payload][CRC32]`.

I also considered whether to make CRC opt-in (a header flag like the existing `flags` byte). I decided against it: opt-in integrity defeats the purpose of integrity. If you ever want to skip the CRC you can just not check it; if you need it and forgot to set the flag you have no protection. A mandatory trailer is simpler and harder to misuse.

**2. "1 thread vs N threads" comparison.**
The handoff framed the benchmark as "JobSystem(1) vs JobSystem(N)". This is blocked by `kMinThreads = 2` — `JobSystem(1)` silently constructs with 2 threads. The meaningful comparison is not thread count but dispatch path: does the snapshot go through the Split-Phase pipeline or does the main thread serialize and send it directly? A `--sequential` flag in `main.cpp` switches paths at runtime, keeping the same binary for both benchmark arms.

**3. "CRC prevents bit-flipping from multithreading."**
This justification in the handoff was wrong. The Job System doesn't cause bit-flipping — workers write into isolated per-task buffers and the main thread only touches them after `sync.wait()`. CRC protects against: (a) network-level corruption in UDP transit, (b) serialization bugs in the delta/VLE pipeline that produce different bytes on sender vs receiver. Those are both real risks worth defending against.

---

## The cascade problem: 4 test files needed simultaneous updates

The hardest part of this step wasn't the CRC implementation — it was the test infrastructure.

Before P-4.5, every test that needed to inject a handshake packet just called `w.GetCompressedData()` directly and passed it to `MockTransport::InjectPacket()`. After P-4.5, `NetworkManager::Update()` verifies CRC on every received packet and discards anything that fails. That means every injected packet now needs a valid CRC trailer, and every BitReader that reads back a `sentPackets` entry needs to strip the 4-byte CRC before parsing.

Four test files had their own handshake helpers:
- `NetworkManagerTests.cpp`
- `SessionRecoveryTests.cpp`
- `JobSystemTests.cpp`
- `GameWorldTests.cpp`

These four files could not be updated one at a time — if I committed the receive-side CRC check before updating all helpers, every test that injected a packet without CRC would start failing. I updated all four files in the same working set before running the test suite.

The discovery order was:
1. First run: `GameWorldTests.cpp::SendSnapshot_ContainsTickID` crashed with `back()` on empty vector. Root cause: `DoHandshake` in that file wasn't using `WithCRC()`, so `nm.Update()` discarded all injected packets, leaving `sentPackets` empty.
2. Second run: `ProfilerTests::kFullSyncBytesPerClient_Is19` failed because I had already updated the constant to 23 but hadn't renamed the test.
3. Third run: `GameWorld.ForEachEstablished_*` tests failed — the manual Input packet injections in those tests also needed `WithCRC()`.
4. Fourth run: 190/190 pass.

The lesson: any test that touches `MockTransport::InjectPacket()` is affected by a receive-side CRC check. Future integrity changes (e.g. adding sequence number validation) will hit the same four files.

---

## CRC32 implementation details

The algorithm is the standard IEEE 802.3 CRC32:
- Polynomial: `0xEDB88320` (reflected form of `0x04C11DB7`)
- Initial value: `0xFFFFFFFF`
- Final XOR: `0xFFFFFFFF`
- Result for `"123456789"`: `0xCBF43926`

I used a `constexpr` 256-entry lookup table generated at compile time. This means the table is in the binary's read-only data segment — no runtime initialization, no static-init ordering issues, and the compiler can potentially constant-fold known-input checksums.

The empty-input case deserves a note: `initial XOR final = 0xFFFFFFFF ^ 0xFFFFFFFF = 0`. So `ComputeCRC32(nullptr, 0)` returns `0`, which is what the test `EmptyBuffer_ReturnsZero` asserts. This is the standard behavior.

---

## The `SendRaw()` / `SendBytes()` chokepoint

Before P-4.5, there were 5 direct `m_transport->Send()` calls in `NetworkManager.cpp` and 4 in `BotClient.cpp`. Each was its own send path with its own `RecordBytesSent()` call. This is a maintenance hazard: a future developer adding a new packet type could easily forget both the CRC and the profiler call.

After P-4.5 there is exactly one path out of each class:

```
NetworkManager → SendRaw() → append CRC → transport->Send() → RecordBytesSent()
BotClient      → SendBytes() → append CRC → transport->Send()
```

`RecordBytesSent()` moved inside `SendRaw()` as a side effect — the byte count now includes the 4-byte trailer, matching actual wire size. This is intentional: the profiler should measure what actually traverses the network, not just the payload.

---

## Scalability Gauntlet design

The benchmark script (`run_final_benchmark.sh`) is deliberately simple:

1. Build once in Release with `BUILD_TESTS=OFF`.
2. Apply tc netem: 100ms delay + 5% loss on loopback.
3. Sequential run: `./NetServer --sequential`, 100 bots, 60 seconds.
4. Parallel run: `./NetServer`, same config.
5. Parse last `[PROFILER]` line from each server log.
6. Print side-by-side table + save `benchmarks/results/final_<ts>_<hash>.md`.

The CRC Err column in the output is expected to be ~0 on loopback (tc netem can corrupt packets if you add a `corrupt` rule, but by default it only adds delay and loss). The column exists to detect hardware or kernel-level corruption in production deployments, not for the benchmark itself.

The JobSystem is always constructed even in sequential mode. Workers sleep on the condition variable — I measured ~0 CPU overhead at idle. The alternative (don't construct the JobSystem in sequential mode) would add a conditional to the game loop and complicate the `MaybeScale()` / `GetStealCount()` shutdown path. Not worth it.

---

## What I would do differently

The `WithCRC()` / `StripCRC()` helpers are duplicated across four test files. They're 10 lines each and do the same thing. A `tests/TestUtils.h` shared header would eliminate the duplication. I didn't add it because the plan said "prefer editing existing files to creating new ones" and the four files are already in the same CMake target. If a fifth test file ever needs handshake helpers, I'll extract it then.

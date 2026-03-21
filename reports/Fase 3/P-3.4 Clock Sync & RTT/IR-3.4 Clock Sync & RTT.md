---
type: implementation-report
proposal: P-3.4
date: 2026-03-21
status: pending-gemini-validation
---

# Implementation Report — P-3.4 Clock Synchronization & RTT Estimation

## Summary

Replaced the hardcoded `kResendInterval = 100ms` from P-3.3 with a dynamic RTT-based interval (RTT×1.5, floor 30ms). Added per-client RTT estimation via Exponential Moving Average (α=0.1) using the existing global sequence ACK mechanism. Added server-side clock offset calculation as the foundation for Phase 5 Predictive AI. Added stale Unreliable packet filtering (`m_lastProcessedSeq`) to prevent out-of-order delivery of position/input data.

---

## Files Modified

| File | Change |
|------|--------|
| `Shared/Network/PacketHeader.h` | Added `#include <chrono>` + `static CurrentTimeMs()` helper |
| `Core/RemoteClient.h` | Added `RTTContext` struct + 3 new fields to `RemoteClient` |
| `Core/NetworkManager.cpp` | 4 change sites: `Send()`, `ProcessAcks()`, `ResendPendingPackets()`, `Update()` |

No new files. No CMakeLists changes.

---

## Design Decisions Implemented

### 1. `CurrentTimeMs()` — static helper on PacketHeader

```cpp
static uint32_t CurrentTimeMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}
```

Uses `steady_clock` (monotonic, not wall-clock) truncated to 32 bits. Both server and Unreal client must use this method to fill `header.timestamp`. The 32-bit field wraps every ~49.7 days — irrelevant for session durations.

Added to `PacketHeader.h` alongside `IsAcked()`. No wire format change: the 32-bit `timestamp` field already existed in P-3.1, previously written as 0.

### 2. `RTTContext` struct — per-client RTT state

```cpp
struct RTTContext {
    float    rttEMA      = 100.0f;  // Smoothed RTT in ms (initialized to 100ms baseline)
    int      sampleCount = 0;
    float    clockOffset = 0.0f;    // ServerTime - (ClientTime + RTT/2), in ms
    std::map<uint16_t, std::chrono::steady_clock::time_point> sentTimes;
};
```

Initialized to `rttEMA = 100ms` — matches the P-3.3 hardcoded value, so the first resend interval (150ms) is deliberately more conservative than before. Converges to real RTT after the first few ACKs.

### 3. `Send()` — timestamp + sentTimes registration

Two additions:

```cpp
header.timestamp = Shared::PacketHeader::CurrentTimeMs();  // real value (was 0)
// ...
client.m_rtt.sentTimes[usedSeq] = std::chrono::steady_clock::now();  // after Send()
```

`usedSeq` is captured before `AdvanceLocal()` — it is the sequence number actually written into the header and therefore the key the remote will ACK.

### 4. `ProcessAcks()` — RTT sampling + clockOffset

```cpp
const auto sentIt = client.m_rtt.sentTimes.find(header.ack);
if (sentIt != client.m_rtt.sentTimes.end()) {
    const float rawRTT = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - sentIt->second).count();

    constexpr float kAlpha = 0.1f;
    client.m_rtt.rttEMA = kAlpha * rawRTT + (1.0f - kAlpha) * client.m_rtt.rttEMA;
    ++client.m_rtt.sampleCount;

    const float serverNow = static_cast<float>(Shared::PacketHeader::CurrentTimeMs());
    client.m_rtt.clockOffset =
        serverNow - (static_cast<float>(header.timestamp) + client.m_rtt.rttEMA / 2.0f);

    client.m_rtt.sentTimes.erase(sentIt);
}
```

**Karn's algorithm**: Not implemented. Retransmitted packet RTT samples are accepted as-is. The EMA (α=0.1) provides sufficient smoothing for MVP. Revisit in Phase 3.5 if needed (add `firstSentTime` to `PendingPacket`).

**clockOffset dependency**: Valid only once the Unreal ActorComponent writes `PacketHeader::CurrentTimeMs()` into `header.timestamp`. Until then, `clockOffset` accumulates an offset vs. `timestamp=0` and should be ignored. `sampleCount` can be used to gate reads.

### 5. `ResendPendingPackets()` — adaptive interval

```cpp
const auto dynamicInterval = std::chrono::milliseconds(
    std::max(30LL, static_cast<long long>(client.m_rtt.rttEMA * 1.5f))
);
if (now - pending.lastSentTime < dynamicInterval) continue;
```

Computed once per client per `ResendPendingPackets()` call — all pending packets for the same client share the same interval. Floor of 30ms prevents retransmit storms on LAN connections.

| RTT | Resend interval |
|-----|----------------|
| Initial (100ms EMA) | 150ms |
| LAN (≈20ms) | 30ms (floor) |
| WAN Europe (≈60ms) | 90ms |
| WAN Transoceanic (≈150ms) | 225ms |

### 6. `m_lastProcessedSeq` — Unreliable stale-packet filter

```cpp
// New fields in RemoteClient:
uint16_t m_lastProcessedSeq            = 0;
bool     m_lastProcessedSeqInitialized = false;
```

Applied in `Update()` default case, before `m_onDataReceived`, for `Snapshot`, `Input`, and `Heartbeat` only:

```cpp
if (isUnreliableChannel && client.m_lastProcessedSeqInitialized) {
    const int16_t diff = static_cast<int16_t>(
        header.sequence - client.m_lastProcessedSeq);
    if (diff <= 0) { break; }  // stale — exits switch, skips delivery
}
if (isUnreliableChannel) {
    client.m_lastProcessedSeq            = header.sequence;
    client.m_lastProcessedSeqInitialized = true;
}
```

Uses `int16_t` modular comparison (same pattern as P-3.3) to handle sequence wrap-around at 65535 correctly.

**Complement to P-3.3 duplicate detection**: P-3.3 filters exact duplicates and packets outside the 32-packet window. This filter additionally discards slightly-out-of-order arrivals (e.g., seq=5 after seq=6) that P-3.3 would have delivered. For position/input streams, in-order delivery is mandatory.

**`Reliable`/`ReliableUnordered` excluded**: These channels have their own ordering guarantees (receive buffer and retransmit). Applying this filter to them would break reliability semantics.

### 7. `sentTimes` 2-second cleanup

```cpp
const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(2);
std::erase_if(client.m_rtt.sentTimes, [&cutoff](const auto& e) {
    return e.second < cutoff;
});
```

Runs after every successfully processed packet (not on stale discards or duplicates, which `break` early out of the switch). Prevents unbounded map growth under persistent ACK loss. At typical game rates (60Hz), entries that survive 2 seconds represent ~120 unacknowledged packets — well past the `kMaxRetries=10` Link Loss threshold, so a client in this state would already be disconnected.

---

## Wire Format

No changes. The `timestamp` field (32 bits, position bits 68–99 in the header) already existed from P-3.1. Previously written as 0; now written with `CurrentTimeMs()`.

---

## Build Verification

```text
cmake --build cmake-build-debug
[100%] Built target NetServer  ← clean, no warnings
```

---

## Open Questions for Gemini

| Question | Context |
|----------|---------|
| Karn's algorithm | Should retransmitted packets be excluded from RTT sampling in Phase 3.4, or defer to Phase 3.5? Current implementation: accepted (EMA smooths outliers). |
| `clockOffset` gating | Should `NetworkManager` expose a `GetRTT(endpoint)` / `GetClockOffset(endpoint)` API for Phase 4 (Brain), or is internal access via `RemoteClient` sufficient? |
| `sentTimes` for unreliable packets | We record `sentTimes` for ALL sent packets, including Unreliable (Snapshot at 60Hz × 10 clients = 600 entries/s). ACKs arrive frequently so entries are consumed quickly. Is this acceptable, or should we only track one packet per client per RTT window? |

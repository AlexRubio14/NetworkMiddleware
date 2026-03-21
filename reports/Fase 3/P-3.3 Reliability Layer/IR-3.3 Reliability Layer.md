---
type: implementation-report
proposal: P-3.3
date: 2026-03-21
status: gemini-validated
---

# Implementation Report — P-3.3 Reliability Layer (UDP-R)

## Summary

Implemented the three-channel reliability system on top of the authenticated connections from P-3.2. The NetworkManager now supports guaranteed delivery (with and without ordering) for critical game events, while unreliable channels remain untouched for low-latency data.

---

## Files Modified

| File | Change |
|------|--------|
| `Shared/Network/PacketHeader.h` | Added `IsAcked()` inline helper |
| `Core/RemoteClient.h` | Added `PendingPacket` struct + 4 reliability fields |
| `Core/NetworkManager.h` | Added `Send()`, `SetClientDisconnectedCallback()`, 5 private helpers, 2 constants, new callback typedef |
| `Core/NetworkManager.cpp` | Implemented all P-3.3 methods; updated `Update()` |

No new files. No CMakeLists changes.

---

## Design Decisions Implemented

### 1. `IsAcked()` — modular arithmetic in header

```cpp
bool IsAcked(uint16_t seq) const {
    const int16_t diff = static_cast<int16_t>(ack - seq);
    if (diff == 0) return true;
    if (diff > 0 && diff <= 32) return (ack_bits >> (diff - 1)) & 1u;
    return false;
}
```

Uses `int16_t` cast for wrap-around correctness (same pattern as `SequenceContext::RecordReceived`). Window of 32 packets matches the 32-bit `ack_bits` field.

### 2. `PendingPacket` stores payload-only (post-header bytes)

The header is rebuilt on every resend with fresh `ack`/`ack_bits`, piggybacking the latest ACK state for free. This gives retransmissions useful work beyond just retransmitting — each retry implicitly sends updated acknowledgements to the remote.

### 3. Three-channel routing in `Update()` default case

```text
RecordReceived() → ProcessAcks() → route by type:
  Reliable         → HandleReliableOrdered()
  everything else  → m_onDataReceived immediately
```

Unreliable packets (Snapshot, Input, Heartbeat, ReliableUnordered) bypass the ordered buffer entirely — no head-of-line blocking.

### 4. `Send()` API shape

```cpp
void Send(const Shared::EndPoint& to,
          const std::vector<uint8_t>& payload,
          Shared::PacketType channel);
```

Single method for all three channels. Channel parameter determines whether a `PendingPacket` is stored and whether a 16-bit `reliableSeq` is prepended.

### 5. `HandleReliableOrdered` signature includes `totalBits`

`BitReader` does not expose remaining bits. `totalBits = buffer->size() * 8` is passed from `Update()` to compute `payloadBytes = (totalBits - 116) / 8` (100 header + 16 reliableSeq).

### 6. Full header preserved in buffered delivery — `BufferedPacket`

**Corrected per Gemini validation.** The original design used a synthetic header (type=Reliable, all other fields zero). Rejected because the `timestamp` field in the header is required by the Unreal client for animation interpolation (Phase 6). A packet buffered out-of-order must deliver the same timestamp as if it had arrived in order.

```cpp
struct BufferedPacket {
    Shared::PacketHeader header;   // full original header (timestamp preserved)
    std::vector<uint8_t> payload;
};
```

Both direct and buffered delivery paths pass the original `PacketHeader` to `m_onDataReceived`. In-order packets use the header from the current `Update()` call. Buffered packets carry it stored in `m_reliableReceiveBuffer`.

### 7. Duplicate detection via global sequence number

**Added per Gemini validation.** Each `RemoteClient` now tracks `m_seqInitialized` (false until first game packet received). On every packet in the `default` case, the global `header.sequence` is checked against `seqContext.remoteAck + ackBits` BEFORE `RecordReceived()`:

```cpp
const bool isDuplicate = client.m_seqInitialized && [&]() -> bool {
    const int16_t diff = static_cast<int16_t>(client.seqContext.remoteAck - header.sequence);
    if (diff < 0)   return false;          // newer than remoteAck → new packet
    if (diff == 0)  return true;           // exact match → duplicate
    if (diff <= 32) return (client.seqContext.ackBits >> (diff - 1)) & 1u;
    return true;                           // outside 32-packet window → discard
}();
```

If duplicate: `ProcessAcks()` still runs (the ACK information in the header is valid), but payload delivery is skipped. Applies to all channels — Reliable Ordered has a second line of defence via `reliableSeq`; `ReliableUnordered` events like deaths/purchases cannot fire twice.

### 8. Modular comparison for Reliable Ordered receive

```cpp
static_cast<int16_t>(reliableSeq - client.m_nextExpectedReliableSeq) > 0
```

Handles wrap-around at 65535 correctly (same int16_t cast pattern).

### 9. `ResendPendingPackets()` disconnect flow

Disconnection during retransmit iteration is deferred to avoid invalidating the `m_establishedClients` iterator. Endpoints to disconnect are collected, then `DisconnectClient()` is called after the loop.

---

## Wire Format — Reliable Ordered Packet

```text
[PacketHeader: 100 bits (13 bytes)]
[reliableSeq:  16 bits             ]  ← Reliable Ordered only
[payload:      N bytes             ]
```

`reliableSeq` is per-client, per-channel (not the global header `sequence`). It drives the receive buffer ordering.

---

## Constants Added

| Constant | Value | Purpose |
|----------|-------|---------|
| `kMaxRetries` | 10 | Retransmit attempts before Link Loss |
| `kResendInterval` | 100ms | Fixed until RTT is available (P-3.4) |

---

## Build Verification

```text
cmake --build cmake-build-debug
[100%] Built target NetServer  ← clean, no warnings
```

---

## Gemini Validation — Applied Corrections

| Question | Decision | Applied |
|----------|----------|---------|
| Synthetic header | Not acceptable — timestamp needed for Phase 6 interpolation. Use `BufferedPacket{header, payload}` | ✅ |
| ReliableUnordered duplicates | Mandatory detection using global seq window (seqContext + m_seqInitialized) | ✅ |
| kResendInterval = 100ms | Adequate for MVP. Will become RTT×1.25 in Phase 3.4 | No change needed |

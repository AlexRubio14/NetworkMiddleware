# IR-4.5 — CRC32 Packet Integrity & Scalability Gauntlet

**Phase:** Fase 4 — Simulation Infrastructure & Stress Test
**Step:** P-4.5 — Packet Integrity & Final Scalability Report
**Branch:** `P-4.5-Packet-Integrity`
**Date:** 2026-03-23
**Status:** ✅ Complete — 190/190 tests, 0 regressions

---

## Summary

P-4.5 closes Fase 4 with two deliverables:

1. **CRC32 packet integrity** — every outgoing packet gets a 4-byte IEEE 802.3 CRC32 trailer; every incoming packet is verified and discarded on mismatch. Protects the delta-compression serialization pipeline against corruption in transit.

2. **Scalability Gauntlet** — `scripts/run_final_benchmark.sh` drives two 60-second runs at maximum load (100 bots, 100ms/5% loss): Sequential baseline (`SendSnapshot()` on main thread) vs Parallel (Split-Phase + Job System workers), producing a side-by-side markdown report.

---

## Design Decisions

### CRC as wire trailer, not PacketHeader field

The handoff originally proposed storing CRC inside `PacketHeader`. This is architecturally incorrect: `PacketHeader` is serialized _before_ the payload, so it cannot cover payload bytes. The implemented wire format is:

```text
[Header: 13 bytes] [Payload: N bytes] [CRC32: 4 bytes]
```

CRC is appended at the single `SendRaw()` chokepoint and stripped/verified at the top of the receive loop before any `BitReader` construction.

### Single chokepoint pattern

Both `NetworkManager::SendRaw()` and `BotClient::SendBytes()` are the sole outgoing paths. All 5 `m_transport->Send()` calls in `NetworkManager.cpp` and all 4 in `BotClient.cpp` were replaced. This eliminates the risk of a future developer bypassing CRC on a new send site.

`RecordBytesSent()` was moved inside `SendRaw()` — the byte count now includes the 4-byte CRC trailer, matching the actual wire size.

### kFullSyncBytesPerClient: 19 → 23

Every packet on the wire is now 4 bytes larger. The delta efficiency constant was updated to keep the profiler calibrated: `23 = 19 bytes payload + 4 bytes CRC32`.

### Sequential vs Parallel benchmark design

`kMinThreads = 2` prevents `JobSystem(1)` from giving a true single-threaded baseline. The meaningful comparison is:

- **Sequential**: `manager.SendSnapshot()` called per client on the main thread (no job system involvement).
- **Parallel**: Split-Phase pipeline — `SerializeSnapshotFor()` dispatched to workers via `std::latch`, then `CommitAndSendSnapshot()` on main thread.

The `--sequential` flag switches modes at runtime. The `JobSystem` is always constructed but stays idle in sequential mode (workers sleep on the condition variable, ~0 CPU overhead).

---

## Files Modified / Created

| File | Action | Notes |
|------|--------|-------|
| `Shared/Serialization/CRC32.h` | **Create** | Header-only, constexpr table, IEEE 802.3 |
| `Shared/CMakeLists.txt` | Modify | Lists CRC32.h |
| `Core/NetworkProfiler.h/.cpp` | Modify | `crcErrors`, `IncrementCRCErrors()`, `kFullSyncBytesPerClient=23` |
| `Core/NetworkManager.h` | Modify | `SendRaw()` private method |
| `Core/NetworkManager.cpp` | Modify | `SendRaw()` impl, CRC verify in `Update()`, 5 send sites |
| `Core/BotClient.h` | Modify | `SendBytes()` private method |
| `Core/BotClient.cpp` | Modify | `SendBytes()` impl, CRC verify in `Update()`, 4 send sites |
| `Server/main.cpp` | Modify | `int main(int argc, char* argv[])`, `--sequential` flag |
| `tests/Core/CRC32Tests.cpp` | **Create** | 6 tests — pins IEEE 802.3 algorithm |
| `tests/Core/NetworkManagerTests.cpp` | Modify | `WithCRC`/`StripCRC` helpers, +3 CRC tests, all helpers fixed |
| `tests/Core/SessionRecoveryTests.cpp` | Modify | `WithCRC`/`StripCRC`, all 3 helpers + 5 readbacks fixed |
| `tests/Core/BotIntegrationTests.cpp` | Modify | +1 `BotClient_CorruptedPacket_Discarded` test |
| `tests/Core/GameWorldTests.cpp` | Modify | `WithCRC`/`StripCRC`, helpers + 3 input injections fixed |
| `tests/Core/JobSystemTests.cpp` | Modify | `WithCRC`/`StripCRC`, helpers fixed |
| `tests/Core/ProfilerTests.cpp` | Modify | `kFullSyncBytesPerClient_Is23` (was `_Is19`) |
| `tests/CMakeLists.txt` | Modify | Adds `Core/CRC32Tests.cpp` |
| `scripts/run_final_benchmark.sh` | **Create** | Sequential vs Parallel Gauntlet |

---

## Test Results

| Suite | Before | After | Delta |
|-------|--------|-------|-------|
| CRC32 | 0 | 6 | +6 |
| NetworkManager | ~35 | ~38 | +3 |
| BotIntegration | 4 | 5 | +1 |
| All others | unchanged | unchanged | 0 |
| **Total** | **180** | **190** | **+10** |

All 190 tests pass. 0 regressions. Build time: ~44ms on Windows/MSVC.

---

## Key Test Cases

**`CRC32.KnownVector_IEEE802_3`** — CRC32(`"123456789"`) == `0xCBF43926`. Pins polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Any accidental change to the algorithm breaks this immediately.

**`NetworkManager.Receive_BitFlip_Discarded`** — Injects a valid packet, flips 1 bit, verifies the data callback is NOT fired AND `profilerSnapshot.crcErrors == 1`.

**`NetworkManager.Receive_ValidCRC_Accepted`** — Same packet without bit flip → callback fires, `crcErrors == 0`.

**`BotIntegration.BotClient_CorruptedPacket_Discarded`** — Post-handshake, a Heartbeat packet with all-zeros CRC trailer is injected into the bot's receive queue. `bot.GetState()` must remain `Established` (packet silently discarded).

---

## Issues Flagged Before Implementation

Three design flaws in Gemini's handoff were flagged and corrected:

1. **CRC in PacketHeader** → changed to wire trailer (cannot cover payload if placed in header)
2. **1-thread vs N-thread comparison** → changed to Sequential vs Parallel (blocked by `kMinThreads=2`)
3. **"Bit-flipping from multithreading" justification** → corrected (CRC protects against network corruption and serialization bugs, not threading bugs)

---

## Benchmark Script

`scripts/run_final_benchmark.sh` runs:

1. Sequential run: `./NetServer --sequential` — all snapshots serialized + sent on main thread
2. Parallel run: `./NetServer` — Split-Phase, Job System workers in Phase A

Both: 100 bots, 100ms / 5% packet loss (tc netem on lo), 60 seconds each.

Output table columns: `Mode | Connected | Avg Tick | Budget% | Out | In | CRC Err | Delta Eff.`

Results auto-saved to `benchmarks/results/final_<timestamp>_<hash>.md`.

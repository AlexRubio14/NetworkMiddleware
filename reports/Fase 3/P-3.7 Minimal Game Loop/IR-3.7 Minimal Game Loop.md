# IR-3.7 — Minimal Game Loop

**Date:** 2026-03-23
**Branch:** Session-Recovery
**Tests:** 156/156 passing (+16 GameWorld, +1 SendInput regression, updated 2 BotIntegration)
**Phase:** P-3.7 Minimal Game Loop

---

## Summary

Implements the first authoritative game state in the server. Closes the gap identified in P-4.3 benchmarks: server now processes client inputs, advances hero positions, and sends delta-compressed snapshots every tick. The pipeline is:

```
Update() [drain UDP, buffer Input packets]
  → ForEachEstablished [apply inputs to GameWorld]
  → GameWorld::Tick [advance simulation]
  → ForEachEstablished [send delta snapshots]
  → ++tickID
```

---

## Files Modified

### `Shared/Gameplay/BaseHero.h`
- Added `const Data::HeroState& GetState() const` — read-only access for `GameWorld::GetHeroState()`.

### `Core/RemoteClient.h`
- Added `#include "../Shared/Network/InputPackets.h"` and `<optional>`.
- Added `std::optional<Shared::InputPayload> pendingInput` — buffered input for the current tick.
- Added `uint16_t m_lastClientAckedServerSeq` + `bool m_lastClientAckedServerSeqValid` — tracks the last server seq confirmed by the client (from `header.ack` of incoming packets), used by `SendSnapshot()` to find the correct delta baseline.

### `Core/NetworkManager.h`
- Added `#include "../Shared/Network/InputPackets.h"` and `"../Shared/Data/HeroState.h"`.
- Added `SendSnapshot(const EndPoint&, const HeroState&, uint32_t tickID)`.
- Added `ForEachEstablished(std::function<void(uint16_t, const EndPoint&, const InputPayload*)>)`.

### `Core/NetworkManager.cpp`
- Added `#include "../Shared/Network/InputPackets.h"` and `"../Shared/Data/Network/HeroSerializer.h"`.
- **Input interception** in `Update()` default branch: `PacketType::Input` packets now set `client.pendingInput = InputPayload::Read(reader)` instead of firing `m_onDataReceived`. This is the authoritative model — the game loop reads inputs via `ForEachEstablished`.
- **`ProcessAcks`**: After clearing reliable sents, saves `header.ack` into `client.m_lastClientAckedServerSeq` for delta baseline selection.
- **`SendSnapshot`**: Builds `[tickID:32][serialized HeroState]` payload. Uses `GetBaseline(m_lastClientAckedServerSeq)` for delta; falls back to full `Serialize()` if no confirmed baseline exists. Records the snapshot with `RecordSnapshot(usedSeq, state)` before calling `Send()`.
- **`ForEachEstablished`**: Iterates non-zombie established clients, calls callback with `(networkID, endpoint, inputPtr)`, clears `pendingInput` after each callback.

### `Core/CMakeLists.txt`
- Added `GameWorld.h` and `GameWorld.cpp` to `MiddlewareCore`.

### `Server/main.cpp`
- Added `#include "../Core/GameWorld.h"`.
- Instantiates `GameWorld gameWorld` and `uint32_t tickID = 0`.
- `SetClientConnectedCallback` → calls `gameWorld.AddHero(id)`.
- `SetClientDisconnectedCallback` → calls `gameWorld.RemoveHero(id)`.
- Removed the old `SetDataCallback` (Input is now intercepted by `NetworkManager`).
- Game loop updated with the 5-step pipeline described above.

---

## Files Created

### `Core/GameWorld.h` / `Core/GameWorld.cpp`
Authoritative simulation container. Key constants:

| Constant | Value | Meaning |
|---|---|---|
| `kMoveSpeed` | 100 f/s | Max displacement per second |
| `kMapBound` | ±500 units | Playable area limit |
| `kSpeedTolerance` | 5 f/s | Reserved for future speed checks |

Anti-cheat model: **client sends normalized direction [-1,1]**, server computes displacement `= clamp(dirX, -1,1) × kMoveSpeed × dt`, then clamps result to `±kMapBound`. There is no client-sent position — teleport / speed-hack attacks are structurally impossible.

### `tests/Core/GameWorldTests.cpp`
16 tests across 3 categories:

**GameWorld unit tests (12):**
- `HeroAdded_StartsAtOrigin` — spawn position is (0, 0)
- `AddHero_IdempotentOnDuplicateID` — duplicate AddHero doesn't crash
- `RemoveHero_ReturnsNullAfterRemoval` — GetHeroState returns nullptr after removal
- `RemoveHero_NonExistentID_NoCrash` — silent on unknown ID
- `GetHeroState_UnknownID_ReturnsNull`
- `ApplyInput_MovesHeroExactly` — 10 ticks at (1,0), dt=0.01 → x=10.0 exactly
- `ApplyInput_DiagonalMovement` — 1 tick at (1,1) → (1.0, 1.0)
- `AntiCheat_InputClamped_OverNormalized` — dirX=999 treated as 1.0, displacement=1.0
- `AntiCheat_ClampsToBounds` — 600 ticks pushing right → clamped at kMapBound
- `AntiCheat_ClampsNegativeBounds` — same, negative axis
- `ApplyInput_ZeroInput_DoesNotMove`
- `Tick_DoesNotCrash` — stub is safe

**SendSnapshot / TickID integration (4):**
- `SendSnapshot_ContainsTickID` — full handshake + SendSnapshot(42) → first 32 payload bits = 42
- `SendSnapshot_TickIDIncrements` — 5 consecutive calls with tick=0..4 verified
- `ForEachEstablished_ReceivesBufferedInput` — bot sends Input, server buffers it, ForEachEstablished delivers it with correct dirX/dirY
- `ForEachEstablished_InputClearedAfterCallback` — second ForEachEstablished call returns nullptr

---

## BotIntegration Test Updates

Two tests that verified Input packets reach `m_onDataReceived` were updated to reflect the P-3.7 behavioural change:

| Old test | New test | Reason |
|---|---|---|
| `SendInput_ServerFiresDataCallback` | `SendInput_ServerBuffersInputViaPendingInput` | Input no longer fires data callback |
| `InputSequence_StrictlyIncreasing` | `InputSequence_StrictlyIncreasing` (rewritten) | Delivery verified via ForEachEstablished |

Added `SendInput_DataCallbackNotFiredForInputPackets` as a regression guard.

---

## Design Decisions

### Why intercept Input inside NetworkManager, not in main.cpp?

`main.cpp` doesn't see individual packet types — it gets a fully parsed callback. Intercepting inside `NetworkManager::Update()` is consistent with how `Heartbeat` packets are already short-circuited (line 140-141 before this change), keeps `main.cpp` clean, and means `ForEachEstablished` can be called at the exact right moment in the game loop.

### Why `RecordSnapshot` before `Send()`?

`Send()` calls `seqContext.AdvanceLocal()`. `usedSeq = client.seqContext.localSequence` is captured before that call. Recording before `Send()` ensures the history entry uses the same seq that will appear in the packet header, regardless of what happens inside `Send()`.

### Why full-state fallback when no confirmed baseline?

On the first tick, `m_lastClientAckedServerSeqValid == false` — no server packet has been ACKed yet. Sending a delta against `seq=0` would be wrong (there's no snapshot at seq=0). Full `Serialize()` is the safe, correct fallback.

---

## Wire Format (Snapshot Payload)

```
[tickID: 32 bits][HeroState bits from HeroSerializer::SerializeDelta or ::Serialize]
```

- Full state path: `32 + 149 = 181 bits (~23 bytes)`
- Delta no-change path: `32 + 38 = 70 bits (~9 bytes)`

---

## Test Results

```
[==========] 156 tests from 14 test suites ran.
[  PASSED  ] 156 tests.
```

Previous: 139 tests (P-4.3). Delta: +17 tests (+16 new GameWorld, +1 new BotIntegration regression guard).

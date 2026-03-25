# IR-5.3+5.4 — Server-Side Lag Compensation & Network LOD

**Branch:** `P-5.3-Lag-Compensation`
**Date:** 2026-03-25
**Tests:** 223 / 223 passing (14 new)

---

## What was implemented

### P-5.3 — Server-Side Lag Compensation (Rewind)

When a client fires an ability at tick T, the packet arrives at the server at tick T + RTT/2. Without lag compensation the server would check the hit against current positions (tick T + RTT/2), which consistently misses targets that the client could see correctly. P-5.3 rewinds all entity positions to tick T and validates the hit against those historical positions, eliminating the latency-induced miss penalty.

### P-5.4 — Network LOD / AI Replication Prioritizer

P-5.1 culled invisible entities (FOW). P-5.4 goes further: even among visible entities, not all are equally important at every tick. An ally fighting next to you is critical information; a minion at the far end of the visible map is not. The `PriorityEvaluator` assigns each entity a replication tier (0–2) that controls how frequently it appears in snapshot batches: Tier 0 every tick, Tier 1 every other tick, Tier 2 every fifth tick.

---

## New files

### `Core/HitValidator.h`

Header-only hit-geometry helper. No dependencies, no state.

```cpp
inline bool CheckHit(float atkX, float atkY,
                      float tgtX, float tgtY, float range);
```

Uses squared-distance comparison (`dx²+dy² <= range²`) to avoid a `sqrt` call. Called from `main.cpp` when an ability input is received.

---

### `Brain/PriorityEvaluator.h` / `Brain/PriorityEvaluator.cpp`

Stateless tier evaluator. Brain-internal types only — no dependency on `MiddlewareShared`. `main.cpp` converts `HeroState` → `EvaluationTarget` at the module boundary, following the same pattern as `KalmanPredictor`.

**Types:**

| Type | Fields |
|------|--------|
| `EvaluationTarget` | `entityID`, `teamID`, `x`, `y` |
| `EntityRelevance` | `entityID`, `tier` (0, 1, or 2) |

**Tuning constants:**

| Constant | Value | Purpose |
|----------|-------|---------|
| `kBaseWeight` | 1.0f | Baseline interest for any visible entity |
| `kCombatBonus` | 4.0f | Multiplier when entity is in combat (total = 5×) |
| `kCombatRadius` | 200.0f | Proximity proxy for "in combat" — any opposing entity within this distance |
| `kTier0Min` | 1/150 ≈ 0.00667 | Interest threshold for Tier 0 (100Hz) |
| `kTier1Min` | 1/300 ≈ 0.00333 | Interest threshold for Tier 1 (50Hz) |

**Algorithm (per observer):**

Pass 1 — inCombat flag for each entity: O(N²) sweep over all pairs; entity is "in combat" if any opposing-team entity is within `kCombatRadius`. For the TFG scale (≤100 clients) this is negligible (10,000 distance checks = ~microseconds).

Pass 2 — tier assignment:
```
own hero:         → Tier 0 (always)
interest = (kBaseWeight + kCombatBonus × inCombat) / max(dist, 1.0f)
interest ≥ kTier0Min  → Tier 0
interest ≥ kTier1Min  → Tier 1
otherwise         → Tier 2
```

**Why proximity proxy instead of a `stateFlags` combat bit:** Adding `0x08=InCombat` to `HeroState` would require bot clients and the game simulation to maintain and update that flag, introducing state management logic that doesn't exist yet. The proximity proxy is fully computed from positions that are already available, requires no wire-format change, and is deterministic — all observers compute the same result for the same world state.

---

### `tests/Brain/PriorityEvaluatorTests.cpp`

8 new tests:

| Test | Validates |
|------|-----------|
| `OwnHero_AlwaysTier0` | Own entity is Tier 0 regardless of position |
| `NearbyEnemy_Tier0` | Enemy at 50u from observer → Tier 0 |
| `FarNonCombat_Tier2` | Ally at 400u, no enemies within kCombatRadius → Tier 2 |
| `CombatProximityBoostsTier` | Ally at 350u but within 50u of enemy → interest boosted, Tier < 2 |
| `Tier1Range` | Enemy at 250u (outside kCombatRadius) → Tier 1 |
| `ShouldSend_Tier0_EveryTick` | Tier 0 send gate passes for all tickIDs |
| `ShouldSend_Tier1_EvenTicks` | Tier 1 passes on even ticks only |
| `ShouldSend_Tier2_Every5th` | Tier 2 passes on ticks where tickID % 5 == 0 |

**Test fix during development:** `Tier1Range` initially placed the entity at exactly `kCombatRadius = 200u` from the observer. Because the inCombat check uses `<=`, the entity was considered in combat (interest = 5/200 = 0.025 → Tier 0). Corrected to 250u (interest = 1/250 = 0.004 → Tier 1 as intended).

---

## Modified files

### `Shared/Network/InputPackets.h`

Wire format extended from 24 to 40 bits:

| Field | Bits | Change |
|-------|------|--------|
| `dirX` | 8 | unchanged |
| `dirY` | 8 | unchanged |
| `buttons` | 8 | unchanged |
| `clientTickID` | 16 | **NEW** — P-5.3 lag compensation |

`kBitCount` updated from 24 to 40. `Write()` and `Read()` updated to serialize/deserialize `clientTickID` as a 16-bit field. 16 bits covers 65,535 ticks (~10 minutes at 100Hz) before wrap-around.

**Design decision — client-stamped tickID vs RTT inference:** Stamping the fire tick in the packet is the canonical approach (used by all commercial titles). RTT inference was considered to avoid the wire-format change but rejected: it introduces ~2-tick imprecision under jitter, and modifying `InputPayload` serialization is contained (3 files: header, BotClient, tests).

---

### `Core/BotClient.h` / `.cpp`

Added `uint16_t m_localTick = 0` (private). Incremented in `SendInput()` via post-increment:

```cpp
Shared::InputPayload{dirX, dirY, buttons, m_localTick++}.Write(w);
```

The `m_localTick` counter is monotone and independent of the server's `tickID`. The server maps `clientTickID` to a rewind slot using the circular buffer modulus; approximate alignment with server ticks is sufficient because the rewind window (200ms = 20 ticks) absorbs client-server tick rate differences.

---

### `Core/GameWorld.h` / `.cpp`

**New struct `RewindEntry`** (defined outside the class, in the `Core` namespace):

```cpp
struct RewindEntry {
    float    x      = 0.0f;
    float    y      = 0.0f;
    uint32_t tickID = 0;
    bool     valid  = false;
};
```

**New constant:** `static constexpr size_t kRewindSlots = 32` — 320ms at 100Hz, providing margin beyond the 200ms rewind limit without significant memory cost (32 × N_entities × 12 bytes).

**New private member:** `std::unordered_map<uint32_t, std::array<RewindEntry, kRewindSlots>> m_rewindHistory` — per-entity circular buffer indexed by `tickID % kRewindSlots`.

**New public methods:**

| Method | Description |
|--------|-------------|
| `RecordTick(uint32_t tickID)` | Snapshots position of all heroes into their slot. Called once per tick after `Tick()`. |
| `GetStateAtTick(uint32_t entityID, uint32_t tickID)` | Returns the stored entry if `entry.valid && entry.tickID == tickID`, else nullptr. The tickID check prevents returning a stale entry from a wrap-around collision. |

**Lifecycle integration:** `AddHero` emplaces an empty `std::array<RewindEntry, kRewindSlots>` for the entity; `RemoveHero` erases it. No dangling entries.

---

### `Brain/CMakeLists.txt`

Added `PriorityEvaluator.cpp` and `PriorityEvaluator.h` to the `Brain` static library.

---

### `Server/main.cpp`

**New constants in game loop scope:**
- `kMaxRewindTicks = 20` (200ms rewind limit)
- `kAbilityRange = 150.0f` (world units, stub for ability hit radius)

**Step 2 — P-5.3 lag compensation path:**

When `input->buttons` has any ability bit set:
1. Compute `fireTick` from `clientTickID`, clamped: `fireTick` must be within `[tickID - kMaxRewindTicks, tickID - 1]`.
2. Retrieve attacker's position at `fireTick` via `GetStateAtTick` (falls back to current position if the slot is evicted — tolerable edge case at startup).
3. For each other entity: retrieve its rewound position; call `CheckHit(atkX, atkY, tgtX, tgtY, kAbilityRange)`.
4. Log confirmed hits via `Logger::Info`.

No damage system exists yet — hit validation is a proof-of-concept log event. The full damage pipeline is deferred to a future phase.

**Step 3b — `gameWorld.RecordTick(tickID)`** inserted immediately after `gameWorld.Tick(kFixedDt)`, before the snapshot phase.

**Phase 0b — P-5.4 priority evaluation:**

After `SpatialGrid::MarkVision()` (Phase 0) and before the gather loop:

1. Build `allTargets: std::vector<EvaluationTarget>` from `gameWorld.ForEachHero()`.
2. Resolve `teamID` per entity from `entityTeams` map populated by a `ForEachEstablished` pass.
3. Per observer: call `priorityEvaluator.Evaluate(obsID, obsX, obsY, obsTeam, allTargets)` → `std::vector<EntityRelevance>`.
4. Build a `tierMap: std::unordered_map<uint32_t, uint8_t>` from the result.

**Gather loop** — now gated on FOW visibility AND tier:

```cpp
const bool send = (tier == 0) || (tier == 1 && tickID % 2 == 0) || (tier == 2 && tickID % 5 == 0);
if (send) snapshots.push_back({ep, st, {}});
```

Phase A and Phase B are unchanged — they operate on whatever task vector the gather loop produces.

**`Brain::PriorityEvaluator priorityEvaluator`** added to composition root alongside `KalmanPredictor` and `SpatialGrid`.

---

### `tests/Core/GameWorldTests.cpp`

6 new rewind tests:

| Test | Validates |
|------|-----------|
| `Rewind_RecordAndRetrieve` | `RecordTick` + `GetStateAtTick` round-trip for a moved hero |
| `Rewind_WrongTickReturnsNull` | Slot overwritten after `kRewindSlots` ticks → old tickID returns nullptr |
| `Rewind_UnknownEntityReturnsNull` | Unregistered entity always returns nullptr |
| `Rewind_RemoveHeroClearsHistory` | `RemoveHero` erases the rewind buffer |
| `Rewind_MultipleEntitiesTrackedIndependently` | Two heroes tracked in separate buffers |
| `Rewind_InputPayload_kBitCount` | Wire format assertion: `InputPayload::kBitCount == 40` |

---

## Test results

```
[==========] 223 tests from 20 test suites ran.
[  PASSED  ] 223 tests.
```

14 new tests, 0 regressions.

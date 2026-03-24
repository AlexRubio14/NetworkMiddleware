# DL-5.1 — Spatial Hashing & Shared Fog of War

**Date:** 2026-03-24
**Branch:** `P-5.1-Spatial-Hashing-FOW`
**PR:** #14

---

## Why this step exists

P-4.4 proved the job system can serialize snapshots in parallel. But every tick it was serializing every client's state for every other client — regardless of whether the other client could actually see them. In a MOBA, roughly 70% of the map is in fog at any given moment. Sending invisible enemy positions wastes bandwidth and leaks information the client is not supposed to have.

P-5.1 introduces the server-side interest management layer: a 20×20 spatial grid that gates snapshot generation on visibility. If a cell is not lit by the observing team, no snapshot packet is created for that entity — not a compressed one, not a "hero disappeared" packet, nothing. The bandwidth saving is proportional to the fraction of the map in fog.

The secondary motivation is correctness for the TFG's Memoria: the protocol now reflects how real netcode works. Commercial middlewares (Photon Bolt, UE Replication) all implement interest management; this is the first step toward parity.

---

## Resolving the handoff blockers

Gemini's revised handoff (round 2) was significantly cleaner than the first draft and resolved three architectural blockers I had flagged:

**B-1: Team system.**
The first handoff was vague about where `teamID` should live. The revised design is precise: `RemoteClient::teamID` stores the server-side assignment for the connection; assignment happens once during `HandleChallengeResponse` using `m_establishedClients.size() % 2` (round-robin Blue/Red by arrival order). No wire format changes needed — the team is authoritative server state, never sent to clients.

I deliberately did not add `teamID` to `ViegoEntity` or `HeroState`. The snapshot pipeline only needs to know the *observing* client's team (to query the bitset) and the entity's world position (to look up its cell). Both are already available without extending the entity model.

**B-2: One snapshot packet per visible entity.**
The previous architecture sent one packet per client (their own state only). P-5.1 expands this to one packet per (client, visible entity) pair. The snapshot loop now calls `gameWorld.ForEachHero()` for each client and skips entities whose cell is dark. This is the cheapest possible encoding: zero bytes for invisible entities.

**B-3: Thread-safety invariant.**
The grid is rebuilt on the main thread between `gameWorld.Tick()` and the `std::latch` creation. Workers only read the grid during Phase A. Since workers are dispatched *after* the grid is fully marked, and the latch creation provides the happens-before edge, no mutex is needed on `IsCellVisible()`. This preserves the P-4.4 threading contract.

---

## Implementation decisions

### Grid dimensions and the "master variable"

I could have hard-coded `VISION_CELL_RADIUS = 4` directly in `SpatialGrid.h`. Instead, `GameplayConstants.h` derives it:

```cpp
inline constexpr int VISION_CELL_RADIUS =
    static_cast<int>(DEFAULT_VISION_RANGE / GRID_CELL_SIZE) + 1;  // 150/50 + 1 = 4
```

The +1 is a safety margin: without it, a hero standing at the exact boundary between two cells would have a vision "edge" that flickers one cell short. Adding one cell of radius costs 9% more bits per MarkVision call (square side goes from 8 to 9) and eliminates all boundary artifacts.

`DEFAULT_VISION_RANGE` is the only constant a designer needs to touch. All downstream values (`VISION_CELL_RADIUS`, and in future a `REDUCED_VISION_RANGE` for bush mechanics) update automatically.

### `std::bitset<400>` vs `std::vector<bool>`

`std::bitset<N>` is the right tool here: the size is a compile-time constant (20×20), the operations we need (`set`, `test`, `reset`) are single-instruction on modern CPUs (BMI2), and `reset()` on a 400-bit bitset compiles to ~7 64-bit stores — faster than zeroing a `bool[400]`. The only limitation is fixed size, which is fine because the grid dimensions are architectural constants, not configuration.

### `MarkVision` — square vs circle

The handoff was silent on whether the vision shape should be a circle or a square. I implemented a square (axis-aligned bounding box of ±VISION_CELL_RADIUS cells). Reasons:

1. **Simpler bounds check** — one pair of comparisons per cell, no floating-point distance.
2. **Conservative** — a square contains the inscribed circle, so no cell that a circle would light is missed.
3. **Standard for competitive titles** — Dota 2 and LoL use cell-based square approximations at the grid level.

If a future step needs circular vision (e.g. for stealth units), the check is `(dr*dr + dc*dc) <= VISION_CELL_RADIUS*VISION_CELL_RADIUS`. The switch would be local to `MarkVision`.

### Avoiding a third `ForEachEstablished` signature

The grid rebuild loop needs each client's team ID. I considered extending the `ForEachEstablished` callback to a 4-argument form `(id, ep, input, teamID)`. That would have broken 6 existing test call sites.

Instead, I added `GetClientTeamID(const EndPoint& ep) const` as a const lookup on `m_establishedClients`. The grid rebuild loop calls it explicitly:

```cpp
spatialGrid.MarkVision(state->x, state->y, manager.GetClientTeamID(ep));
```

This keeps the existing callback signature stable, and the lookup is O(log N) on a `std::map` — negligible at 100 clients.

### Multi-entity snapshot pipeline

The old gather loop sent one snapshot per client. The new loop is:

```cpp
spatialGrid.Clear();
// Phase 0: mark vision for all connected heroes
manager.ForEachEstablished([&](uint16_t id, const EndPoint& ep, const InputPayload*) {
    if (const auto* state = gameWorld.GetHeroState(id))
        spatialGrid.MarkVision(state->x, state->y, manager.GetClientTeamID(ep));
});

// Gather: one task per (client, visible entity)
manager.ForEachEstablished([&](uint16_t, const EndPoint& ep, const InputPayload*) {
    const uint8_t team = manager.GetClientTeamID(ep);
    gameWorld.ForEachHero([&](uint32_t, const Data::HeroState& state) {
        if (spatialGrid.IsCellVisible(state.x, state.y, team))
            snapshots.push_back({ep, state, {}});
    });
});
```

The `std::latch` and Phase A/B dispatch are unchanged — they don't care whether the task vector has 1 entry or 100, just that every entry gets serialized before Phase B starts.

---

## Bandwidth model

With N clients split evenly between two teams, and vision covering ~30% of the map:

| Without FOW | With FOW |
|-------------|----------|
| N² snapshot tasks | N² × 0.30 ≈ 0.3N² |

At N=10 (5v5 MOBA): 100 → 30 snapshot tasks per tick. At 100 Hz with 23 bytes each: ~5.5 kbps saved per tick → ~550 kbps per second, cutting snapshot bandwidth from ~1.84 Mbps to ~0.55 Mbps for a real 5v5 game.

---

## What changed in the code

| File | Change |
|------|--------|
| `Shared/GameplayConstants.h` | **NEW** — centralized MAP/grid/vision constants |
| `Core/SpatialGrid.h/.cpp` | **NEW** — 20×20 grid, bitset[2] team visibility |
| `tests/Core/SpatialGridTests.cpp` | **NEW** — 14 tests |
| `Core/RemoteClient.h` | `uint8_t teamID = 0` |
| `Core/NetworkManager.h/.cpp` | `GetClientTeamID()`, teamID assignment at handshake |
| `Core/GameWorld.h/.cpp` | `ForEachHero(callback)` |
| `Server/main.cpp` | Phase 0 grid rebuild + FOW-filtered multi-entity gather |
| `Core/CMakeLists.txt` | SpatialGrid added |
| `Shared/CMakeLists.txt` | GameplayConstants.h added |
| `tests/CMakeLists.txt` | SpatialGridTests.cpp added |

---

## Test results

```
[==========] 204 tests from 18 test suites ran.
[  PASSED  ] 204 tests.
```

14 new tests, 0 regressions. The three mandatory tests from the handoff — `Test_Grid_Abstraction`, `Test_FogOfWar_Culling`, `Test_Team_Vision_Merge`, `Test_Boundary_Clamping` — all pass.

---

## What I would do differently

The two `ForEachEstablished` calls in the gather phase (one for marking, one for tasks) traverse the client map twice per tick. At 100 clients this is O(100) + O(100) — imperceptible. If the client count scaled to 1000+, a single-pass version that marks and gathers simultaneously would be worth considering. For this TFG's scope, clarity outweighs micro-optimization.

`GameplayConstants.h` does not yet replace `kMapBound` in `GameWorld.h` or `MAP_MIN/MAX` in `HeroSerializer.h`. Those are isolated uses that would require touching the serialization tests. I left them in place — the constants have identical values and the migration is a clean-up for a future PR, not a correctness issue.

# IR-5.1 — Spatial Hashing & Shared Fog of War

**Branch:** P-5.1-Spatial-Hashing-FOW
**Date:** 2026-03-24
**Tests:** 204 / 204 passing (14 new)

---

## What was implemented

### P-5.1 Spatial Hash Grid + Team-based FOW Interest Management

The server now maintains a 20×20 spatial grid that drives per-tick Fog of War culling. Only entities visible to a client's team generate snapshot packets — invisible entities are silently skipped, reducing bandwidth proportionally to the fraction of the map in fog.

---

## New files

### `Shared/GameplayConstants.h`
Centralised constants namespace `NetworkMiddleware::Shared::GameplayConfig`:

| Constant | Value | Derived from |
|----------|-------|--------------|
| `MAP_MIN` | −500.0f | — |
| `MAP_MAX` | 500.0f | — |
| `GRID_CELL_SIZE` | 50.0f | — |
| `DEFAULT_VISION_RANGE` | 150.0f | **master variable** |
| `GRID_COLS` / `GRID_ROWS` | 20 | (MAP_MAX−MAP_MIN) / GRID_CELL_SIZE |
| `VISION_CELL_RADIUS` | 4 | floor(150/50)+1 |
| `kMaxTeams` | 2 | — |

Tuning vision range requires only changing `DEFAULT_VISION_RANGE`; `VISION_CELL_RADIUS` updates automatically.

### `Core/SpatialGrid.h` / `Core/SpatialGrid.cpp`
20×20 grid (400 cells) covering ±500 world units. Internal state: two `std::bitset<400>` (one per team).

**API:**

| Method | Description |
|--------|-------------|
| `Clear()` | Reset both bitsets. Main thread, start of tick. |
| `MarkVision(x, y, teamID)` | Set all cells in a VISION_CELL_RADIUS square around (x,y) for teamID. |
| `IsCellVisible(x, y, teamID)` | Query — read-only, safe from worker threads during Phase A. |
| `GetCellIndex(x, y)` | Exposed for tests. |

All positions are boundary-clamped before indexing — no out-of-bounds access possible.

**Threading contract** (B-3 from handoff):
- Main thread calls `Clear()` + `MarkVision()` **before** the `std::latch` is created.
- Worker threads call `IsCellVisible()` **only during Phase A** (grid is read-only from this point).
- No mutex required: the latch creation provides the necessary happens-before relationship.

### `tests/Core/SpatialGridTests.cpp`
14 new tests covering all three handoff requirements plus edge cases:

| Test | Validates |
|------|-----------|
| `Test_Grid_Abstraction` | VISION_CELL_RADIUS = 4, GRID_COLS = 20 |
| `Test_FogOfWar_Culling` | Enemy at (400,400) invisible to team 0 at origin |
| `Test_Team_Vision_Merge` | Two allies at ±250 — both zones visible, gap between them is not |
| `Test_Boundary_Clamping` | MarkVision(500,500) — no crash, cell visible |
| `BoundaryClamp_NegativeOvershoot` | Positions beyond map limits — no UB |
| `GetCellIndex_Origin/Min/Max` | Index mapping correctness |
| `Clear_ResetsAllVisibility` | After Clear, both teams see nothing |
| `TeamVisibility_IsIsolated` | Team 0 vision does not bleed into team 1 |
| `InvalidTeamID_ReturnsFalse` | Out-of-range teamID silently ignored |
| `FogOfWar_EnemyJustOutsideRadius` | col=17 → outside [6,14] → invisible |
| `FogOfWar_EnemyJustInsideRadius` | col=13 → inside [6,14] → visible |
| `TeamVision_AllyOverlapMerges` | Overlapping allies — union covers gap cell |

---

## Modified files

### `Core/RemoteClient.h`
Added `uint8_t teamID = 0` in the P-5.1 section. Default 0 (Blue) until assigned.

### `Core/NetworkManager.h` / `.cpp`
- `GetClientTeamID(const EndPoint& ep) const` — returns `client.teamID` or 0 if not found. Used by main loop to look up team without changing `ForEachEstablished` signature.
- `HandleChallengeResponse`: assigns `newClient.teamID = m_establishedClients.size() % 2` before emplace → round-robin Blue/Red balance.

### `Core/GameWorld.h` / `.cpp`
Added `ForEachHero(callback)` const — iterates `m_heroes` and invokes callback with `(networkID, heroState)`. Required for the multi-entity snapshot pipeline.

### `Server/main.cpp`
Game loop step 4 now has two sub-phases before the existing Phase A/B:

```
Phase 0: Clear() + MarkVision() for all connected heroes
Gather:  ForEachHero × ForEachEstablished with IsCellVisible() filter
Phase A: parallel SerializeSnapshotFor() (grid read-only)
Phase B: sequential CommitAndSendSnapshot()
```

The snapshot task vector is now `(clients × visible_entities)` rather than `(clients × 1)`. With 70% of the map in fog (typical MOBA), this cuts snapshot count by ~70%.

### CMakeLists files
- `Core/CMakeLists.txt`: added `SpatialGrid.h` / `SpatialGrid.cpp`
- `Shared/CMakeLists.txt`: added `GameplayConstants.h`
- `tests/CMakeLists.txt`: added `Core/SpatialGridTests.cpp`

---

## Test results

```
[==========] 204 tests from 18 test suites ran.
[  PASSED  ] 204 tests.
```

No regressions. 14 new SpatialGrid tests all green.

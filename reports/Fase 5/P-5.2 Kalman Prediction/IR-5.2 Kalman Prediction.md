# IR-5.2 — Silent Server-Side Kalman Prediction

**Branch:** P-5.1-Spatial-Hashing-FOW
**Date:** 2026-03-24
**Tests:** 209 / 209 passing (5 new)

---

## What was implemented

### P-5.2 Brain::KalmanPredictor — Silent server-side prediction using a 4-state constant-velocity Kalman filter

When a client's input packet fails to arrive for a tick, the server previously had no movement to apply for that entity. P-5.2 eliminates this gap by synthesizing a plausible `InputPayload` from the Kalman filter's velocity estimate, keeping `GameWorld` simulation smooth without any wire-format changes. The feature is entirely server-internal: no new packet types, no client-side awareness.

---

## New files

### `Brain/KalmanPredictor.h` / `Brain/KalmanPredictor.cpp`

4-state constant-velocity Kalman filter operating at the server tick rate (100 Hz, dt = 0.01 s).

**State vector:** `x = [px, py, vx, vy]ᵀ`

**Matrices:**

| Matrix | Value | Purpose |
|--------|-------|---------|
| `F` (4×4) | CV model with dt | State transition |
| `H` (2×4) | Position-only rows | Observation (observe [px,py]) |
| `Q` (4×4) | diag(0.001, 0.001, 5.0, 5.0) | Process noise |
| `R` (2×2) | 0.015 × I₂ | Measurement noise (≈ 16-bit quantization step) |
| `P₀` (4×4) | I₄ | Initial covariance |

`Q_vel = 5.0` (much larger than `Q_pos = 0.001`) keeps the Kalman gain for velocity high enough that a 180° direction reversal is reflected in the velocity estimate within ~5–8 ticks, matching typical MOBA reaction time (100–300 ms). The asymmetry between position and velocity noise is the key tuning decision: a lower `Q_vel` would cause the predictor to lag behind abrupt direction changes.

**API:**

| Method | Description |
|--------|-------------|
| `AddEntity(id, x, y)` | Register entity at spawn (idempotent). |
| `RemoveEntity(id)` | Deregister on disconnect. |
| `HasEntity(id)` | Query registry. |
| `Tick(id, x, y, dirX, dirY)` | Real input arrived — predict + update. Returns real direction unchanged. |
| `Predict(id, x, y)` | No input this tick — predict only. Returns synthesized `PredictedInput`. |

`SynthesizeFromVelocity` normalises `[vx, vy]` to a unit direction; returns `{0,0}` if the entity is stationary (speed < 1e-6).

`mat22_inverse` uses an explicit determinant formula and falls back to predict-only if `|det| < 1e-10` (degenerate covariance guard).

All matrix operations are zero-allocation fixed-size template helpers in an anonymous namespace (no heap, no BLAS dependency).

**Brain module has no dependency on MiddlewareShared.** `PredictedInput` is a Brain-internal struct; `main.cpp` converts it to `InputPayload` at the boundary.

---

## Modified files

### `Brain/CMakeLists.txt`
Added `KalmanPredictor.cpp` and `KalmanPredictor.h` to the `Brain` static library target.

### `Server/main.cpp`

**Step 2 of the game loop** was rewritten to incorporate prediction:

```
For each established client:
  z_k = GameWorld::GetHeroState(id) → [x, y]   // authoritative position BEFORE this tick's input
  if (real input arrived):
      kalmanPredictor.Tick(id, x, y, dir.x, dir.y)   // predict + update
      toApply = real InputPayload
  else:
      pred = kalmanPredictor.Predict(id, x, y)        // predict only (extrapolate velocity)
      toApply = InputPayload{pred.dirX, pred.dirY, 0}
  GameWorld::ApplyInput(id, toApply, dt)
```

`KalmanPredictor kalmanPredictor` added to composition root alongside `SpatialGrid spatialGrid`.

**Lifecycle hooks** (no new callbacks, extended existing ones):
- `SetClientConnectedCallback`: calls `kalmanPredictor.AddEntity(id, 0.0f, 0.0f)`
- `SetClientDisconnectedCallback`: calls `kalmanPredictor.RemoveEntity(id)`

No changes to wire format, packet types, `NetworkManager`, `RemoteClient`, or any Core/Shared module.

### `tests/CMakeLists.txt`
Added `Brain/KalmanTests.cpp` to `MiddlewareTests`.

---

## New tests — `tests/Brain/KalmanTests.cpp`

5 tests covering the three design requirements from the handoff plus two robustness cases:

| Test | What it validates |
|------|-------------------|
| `Test_Kalman_Linear_Track` | After 20 warm-up ticks in +X, first prediction tick produces `dirX > 0.9`. Filter locks onto velocity. |
| `Test_Kalman_PacketLoss_Recovery` | After warm-up, 3 consecutive prediction ticks produce cumulative position error < 0.5 units vs. ideal straight-line. |
| `Test_Kalman_DirectionChange_Tracking` | After 20 +X ticks + 10 -X ticks with real input, synthetic prediction has `dot(pred, (-1,0)) > 0` — filter tracks the reversal. |
| `Test_Kalman_AddRemove_Idempotent` | Double `AddEntity` is a no-op; `RemoveEntity` twice is safe. |
| `Test_Kalman_UnknownEntity_ReturnsZero` | `Predict` on unregistered entity returns `{0,0}` without crashing. |

---

## Design decisions

**No wire-format change.** The prediction is entirely internal to the server. Clients receive their normal snapshots; they cannot detect whether a given tick's movement came from a real input or a synthesized one.

**Brain dep-free from MiddlewareShared.** Keeping `PredictedInput` as a Brain-internal type and converting at `main.cpp` enforces the module boundary: Brain has no dependency on `MiddlewareShared` and can be independently tested with plain GTest (no transport, no serialization).

**Position as observation, not as state correction.** The authoritative `GameWorld` position is fed as the Kalman measurement `z_k = [x, y]`, so the filter stays anchored to the ground truth. This prevents drift accumulation over long packet loss bursts.

**Predict-only on loss (no artificial update).** On a missing-input tick, `Predict` advances the state without assimilating any measurement. Committing the predicted state directly (copying `xPred → s.x`, `pPred → s.P`) is correct: the next real-input tick will correct via the update step.

---

## Test results

```
[==========] 209 tests from 19 test suites ran.
[  PASSED  ] 209 tests.
```

No regressions. 5 new KalmanTests all green.

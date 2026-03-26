# IR-5.x — Benchmark Regression Fixes

**Branch:** `P-5.x-regression-fixes`
**Date:** 2026-03-25
**Tests:** 230 / 230 passing (7 new)

---

## What was implemented

The P-5.x regression benchmark (commit `cb62ad3`) revealed two bugs silently introduced by P-5.1 that corrupted the snapshot pipeline for all subsequent phases.

### Bug 1 — Multi-entity delta baseline corruption

**Root cause:** `RemoteClient` was designed in P-3.5 with the implicit invariant that exactly one entity was sent per client per tick. The snapshot history (`m_history`) stored one `HeroState` per sequence number, and `GetBaseline(lastAckedSeq)` retrieved it.

P-5.1 broke this invariant by adding multi-entity snapshots (one `SnapshotTask` per visible entity per client). Each entity consumed a separate sequence number. When the client ACKed the last one, `GetBaseline(lastAckedSeq)` returned the state of whichever entity happened to own that sequence — the correct baseline for exactly one entity, and wrong for all others. The remaining N-1 entities fell back to full sync every tick.

**Result before fix:** Delta Efficiency 0–15% in all scenarios (should be 60–80% under real movement).

**Fix:** Replace the single-state history with a batch-per-seq history (`BatchEntry`). `RecordBatch(seq, entities)` stores all entity states sent in one tick. `ProcessAckedSeq(seq)` promotes confirmed entities into `m_entityBaselines[entityID]`. `GetEntityBaseline(entityID)` performs a direct lookup — independent of sequence numbers entirely.

The ACK window in `NetworkManager::ProcessInput` now calls `ProcessAckedSeq` for both `header.ack` and all 32 bits set in `header.ack_bits`, ensuring baselines are promoted even when multiple ACKs arrive in the same packet.

### Bug 2 — O(clients × entities) UDP sends per tick

**Root cause:** The gather loop built one `SnapshotTask` per (client, entity) pair. With 46 clients × 46 visible entities in the Zerg scenario, Phase B called `CommitAndSendSnapshot` 2116 times per tick — one UDP `send()` per call.

On Linux each `send()` costs ~1–3µs. On WSL2 the syscall crosses the hypervisor boundary: ~15–18µs each. 2116 × 17µs = **35.9ms per tick** — three times the 10ms budget.

The Job System (P-4.4) could not help: Phase B is intentionally serial (writes to `RemoteClient::m_history`). Parallelizing it would introduce race conditions.

**Fix:** Batch all visible entity states for one client into a single `SnapshotTask`. The gather loop now builds one task per client (`task.states` = all visible entities). Phase A serializes the batch with `SerializeBatchSnapshotFor`; Phase B calls `CommitAndSendBatchSnapshot` once per client — O(clients) `send()` calls regardless of entity count.

**Wire format change:** `[tickID:32][count:8][entity_0 bits][entity_1 bits]...[entity_N-1 bits]`

The `SendSnapshot` single-entity convenience path now delegates to the batch API so the wire format is identical across both code paths.

---

## Modified files

### `Core/RemoteClient.h` / `Core/RemoteClient.cpp`

**Removed:** `SnapshotEntry` struct, `RecordSnapshot()`, `GetBaseline()`, `m_lastClientAckedServerSeq`, `m_lastClientAckedServerSeqValid`.

**Added:**

| Symbol | Description |
|--------|-------------|
| `BatchEntry` | `seq`, `valid`, `vector<pair<entityID, HeroState>>` |
| `m_history[64]` | Circular buffer of `BatchEntry` (same 64-slot size) |
| `m_entityBaselines` | `unordered_map<uint32_t, HeroState>` — last ACKed state per entity |
| `RecordBatch(seq, entities)` | Writes all entity states to the history slot |
| `ProcessAckedSeq(seq)` | Promotes a confirmed batch into `m_entityBaselines` |
| `GetEntityBaseline(entityID)` | Returns a pointer to the confirmed state, or `nullptr` |

### `Core/NetworkManager.h` / `Core/NetworkManager.cpp`

**Removed:** Call to `client.GetBaseline(lastAckedSeq)` in `SerializeSnapshot`. Direct writes to `m_lastClientAckedServerSeq` in `ProcessInput`.

**Added:**

| Symbol | Description |
|--------|-------------|
| `SerializeBatchSnapshot(client, states, tickID)` | Private const — builds multi-entity wire buffer |
| `SerializeBatchSnapshotFor(ep, states, tickID)` | Public const — Phase A entry point for worker threads |
| `CommitAndSendBatchSnapshot(ep, states, payload)` | Public — Phase B: records batch, sends one packet |

`SerializeSnapshot` now calls `client.GetEntityBaseline(state.networkID)` instead of `client.GetBaseline(lastAckedSeq)`.

`ProcessInput` now calls `client.ProcessAckedSeq(header.ack)` and iterates `header.ack_bits` to process the full 32-seq ACK window.

`SendSnapshot` (single-entity path) delegates to `SerializeBatchSnapshot` + `CommitAndSendBatchSnapshot`.

### `Server/main.cpp`

`SnapshotTask` changed from `{ep, HeroState state, buffer}` to `{ep, vector<HeroState> states, buffer}`. The gather loop builds one task per client (instead of per entity). Phase A calls `SerializeBatchSnapshotFor`; Phase B calls `CommitAndSendBatchSnapshot`. Latch size is now O(clients), not O(clients × entities).

---

## New tests (7)

| Test | What it guards |
|------|---------------|
| `GetEntityBaseline_NullBeforeAck` | Baseline is not exposed until the client ACKs the seq |
| `GetEntityBaseline_CorrectAfterAck` | Baseline matches the sent state after `ProcessAckedSeq` |
| `UnknownEntity_ReturnsNullptr` | `GetEntityBaseline` on an unseen entityID returns `nullptr` |
| `EntitiesHaveIndependentBaselines` | Two entities in the same batch get independent baselines (the core regression guard) |
| `EvictedSeq_DoesNotUpdateBaseline` | After slot eviction, `ProcessAckedSeq` on the old seq is a no-op |
| `BatchSnapshot_ContainsTickIDAndCount` | Wire format: first 32 bits = tickID, next 8 = entity count |
| `BatchSnapshot_DeltaBaselinePerEntity` | Without ACK, repeated sends are full-sync (same size) |

---

## Benchmark results (WSL2, Release, commit `cb62ad3`)

| Scenario | Full Loop | Budget | Delta Eff. | Out kbps | Connected |
|----------|-----------|--------|-----------|----------|-----------|
| A: Clean Lab (10 bots, 0ms/0%) | 0.24ms | **2.4%** | **69%** | 746.9 kbps | 10/10 |
| B: Real World (10 bots, 50ms/5%) | 0.21ms | **2.1%** | **64%** | 362.2 kbps | 7/10 |
| C: Stress/Zerg (50 bots, 100ms/2%) | 1.62ms | **16.2%** | **74%** | 13936.2 kbps | 46/50 |

**Phase B send calls per tick:** 2116 → 46 in Scenario C (fixed with batch).
**Delta Efficiency:** 0–15% → 64–74% (fixed with per-entity baselines).
**Full Loop Scenario C:** 39.5ms → 1.62ms.

Note: bandwidth increase vs P-4.3 is expected — P-4.3 had no active game simulation (bots not moving, near-empty snapshots). P-5.x sends real HeroState snapshots every tick; the comparison is not apples-to-apples. Per-client bandwidth in Scenario C is ~302 kbps — well within consumer fiber capacity.

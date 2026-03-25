# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Engine-agnostic network middleware for MOBA games — a C++20 bachelor's thesis (TFG). Authoritative dedicated server targeting Linux, validated via a Visual Debugger (Unreal Engine ActorComponent plugin). Not a game itself. Goal: outperform commercial middlewares (Photon Bolt, Mirror, UE Replication) in bandwidth efficiency and latency.

**Current status:** Phases 1–5.4 complete (2026-03-25). P-5.3 adds Server-Side Lag Compensation: InputPayload extended to 40 bits (+ clientTickID:16), GameWorld::RecordTick/GetStateAtTick circular rewind buffer (32 slots / 320ms), HitValidator::CheckHit geometry helper. P-5.4 adds Network LOD: Brain::PriorityEvaluator assigns Tier 0/1/2 per (observer, entity) pair using interest=(1+4×inCombat)/distance; inCombat by proximity (kCombatRadius=200u); Tier 0=100Hz, Tier 1=50Hz, Tier 2=20Hz; Phase 0b in main loop before gather. 223/223 tests passing.

**Validated benchmark results (P-4.3, WSL2, Release):**
- Tick budget: **1.1%** (0.11ms / 10ms) with 47 clients under degraded network (100ms / 2% loss)
- Delta Efficiency: **99%** — the middleware sends 1% of what a full-sync system would
- NOTE: These numbers will change now that Snapshot packets are sent each tick (expect Out ~20-40 kbps, Delta Efficiency ~60-80% under real game load)
- 223/223 tests passing (Windows/MSVC)

## Build Commands

**Prerequisites:** CMake 3.20+, C++20 compiler, SFML (network + system components).

```bash
# Unit tests (Windows — uses cmake-build-test/)
powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1

# Unit tests (Linux/WSL2)
bash scripts/run_tests.sh

# Stress benchmark (WSL2 only — requires sudo for tc netem)
bash scripts/run_stress_test.sh               # full build + 3 scenarios
bash scripts/run_stress_test.sh --skip-build  # reuse last build

# P-4.5 Scalability Gauntlet (WSL2 only — Sequential vs Parallel, 100 bots)
bash scripts/run_final_benchmark.sh               # full build + 2 scenarios
bash scripts/run_final_benchmark.sh --skip-build  # reuse last build

# Manual build (Windows, from repo root)
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --config Debug
```

## Module Architecture

Six CMake production targets (4 static libraries + 2 executables), plus MiddlewareTests when BUILD_TESTS=ON:

```
MiddlewareShared  (no deps)
    ↑
MiddlewareCore    ← MiddlewareShared
MiddlewareTransport ← MiddlewareShared + sfml-network
Brain             (standalone, no shared dep)
    ↑
NetServer (exe)   ← MiddlewareCore + MiddlewareTransport + Brain + Threads
HeadlessBot (exe) ← MiddlewareCore + MiddlewareTransport + Threads

Tests (BUILD_TESTS=ON, skips SFML targets):
MiddlewareTests   ← MiddlewareCore + MiddlewareShared + GTest
```

**MiddlewareShared** — Everything shared between server and Unreal client:
- `Serialization/BitWriter` / `BitReader`: LSB-first block-based bit packing. No Protobuf/FlatBuffers — manual bit operations for maximum compression.
- `Data/Network/NetworkOptimizer`: Quantization (float → 16-bit int, 1.53cm precision, MAP ±500) and VLE/Base-128 varints. `ZigZagEncode`/`ZigZagDecode` for negative deltas.
- `Data/Network/HeroSerializer`: Serialize/deserialize `HeroState` using dirty bits (full sync) or delta protocol (delta compression). Full sync: 149 bits.
- `Gameplay/BaseHero`: Abstract MOBA hero with `SetNetworkVar<T>(member, newValue, bit)` template that auto-sets dirty bits. `dirtyMask` starts at `0xFFFFFFFF` for first tick.
- `Network/PacketHeader`: 104-bit header (sequence, ack, ack_bits bitmask, type, flags, timestamp).
- `Log/Logger`: Async producer-consumer logger (`std::jthread` + mutex queue). Channels: Core, Transport, Brain, General.

**MiddlewareTransport** — Only layer that knows SFML exists. `SFMLTransport` implements `ITransport`. `TransportFactory::Create(TransportType)` is the only entry point.

**MiddlewareCore** — Protocol implementation:
- `NetworkManager`: while-drains UDP per tick, buffers `Input` packets as `pendingInput`. Exposes `SendSnapshot()` and `ForEachEstablished()` for game loop integration (P-3.7).
- `RemoteClient`: per-client state — handshake, RTT/EMA, reliable send queue (`PendingPacket`), snapshot history (64-slot circular buffer for delta baselines), session keepalive, `pendingInput` (P-3.7), `m_lastClientAckedServerSeq` for delta baseline selection (P-3.7).
- `GameWorld`: authoritative simulation container. Owns `ViegoEntity` map. `ApplyInput()` clamps direction + position; `Tick()` placeholder for physics. `GetHeroState()` for snapshot reads. (P-3.7)
- `BotClient`: headless client state machine (Disconnected → Connecting → Challenging → Established). Lives in Core so tests can use it with MockTransport.
- `NetworkProfiler`: thread-safe telemetry (`std::atomic`). Reports every 5s: tick time, bandwidth in/out, retransmissions, delta efficiency.

**Brain** — AI layer (Fase 5+). `BrainManager` orchestrates `NeuralProcessor` and `BehaviorTree` behind `IDataProcessor` / `IBehaviorEngine` interfaces. Currently stubs.

## Protocol Stack (implemented)

```
P-3.1  PacketHeader (104 bits) — sequence, ack, ack_bits, type, flags, timestamp
P-3.2  Connection Handshake — ConnectionRequest → Challenge → ChallengeResponse → Accepted
P-3.3  Reliability Layer — PendingPacket queue, adaptive resend (RTT×1.5), kMaxRetries=10
P-3.4  Clock Sync & RTT — EMA (α=0.1), clockOffset, adaptive resend interval
P-3.5  Delta Compression — ZigZag+VLE, SnapshotHistory (64 slots), SerializeDelta
P-3.6  Session Recovery — heartbeats (1s), zombie state (10s timeout), reconnection token (120s)
P-3.7  Minimal Game Loop — GameWorld (authoritative), Input→GameWorld→Snapshot pipeline, 100Hz fixed-dt, anti-cheat clamping, tickID prefix for lag compensation
P-4.4  Dynamic Job System — WorkStealingQueue (mutex-per-thread, LIFO/FIFO), JobSystem (round-robin dispatch, work-stealing workers, MaybeScale EMA α=0.1 with 5s hysteresis), Split-Phase snapshots (SerializeSnapshotFor + std::latch + CommitAndSendSnapshot)
P-4.5  CRC32 Packet Integrity — IEEE 802.3 (0xEDB88320), constexpr lookup table, 4-byte trailer on every outgoing packet, verify+discard on receive; NetworkProfiler::crcErrors counter; --sequential server flag for Scalability Gauntlet benchmark
P-5.1  Spatial Hashing & FOW — 20×20 SpatialGrid (50 u/cell), std::bitset<400>[2] team visibility, MarkVision per hero each tick, IsCellVisible culls snapshot tasks; GameplayConstants.h (MAP_MIN/MAX, VISION_CELL_RADIUS=4); round-robin teamID in RemoteClient; multi-entity snapshot pipeline (clients × visible entities)
P-5.2  Silent Kalman Prediction — Brain::KalmanPredictor, 4-state CV model [x,y,vx,vy], F/H/Q/R matrices, predict+update cycle in main.cpp step 2; synthesizes InputPayload on missing input ticks; no wire-format change; PredictedInput type keeps Brain dep-free from Shared; Q_vel=5.0 for MOBA direction-change responsiveness
P-5.3  Server-Side Lag Compensation — InputPayload extended 24→40 bits (clientTickID:16); GameWorld::RecordTick/GetStateAtTick per-entity circular rewind buffer (kRewindSlots=32, 320ms window); HitValidator::CheckHit header-only geometry; main.cpp rewinds target to clientTickID (clamped to kMaxRewindTicks=20) on ability input and logs hit validation
P-5.4  Network LOD / AI Replication — Brain::PriorityEvaluator assigns Tier 0/1/2 per (observer, entity); interest=(kBaseWeight+kCombatBonus×inCombat)/dist, inCombat=proximity proxy (kCombatRadius=200u); Tier 0=100Hz, Tier 1=50Hz (even ticks), Tier 2=20Hz (every 5th); Phase 0b in main loop computes tiers before gather; FOW filter applied first; no Phase A/B changes
```

## Key Interfaces

| Interface | Purpose |
|-----------|---------|
| `ITransport` | `Initialize`, `Receive`, `Send`, `Close` |
| `INetworkable` | `Serialize(BitWriter&)`, `Unserialize(BitReader&)`, `GetDirtyMask`, `ClearDirtyMask`, `GetNetworkID` |
| `IHero` (extends INetworkable) | Full hero API: position, health/mana, level, abilities, status flags |

`BaseHero` implements `IHero` generically. `ViegoEntity` (hero type ID 66) is the first concrete hero.

## Wire Formats

**Full sync packet:** 149 bits (~19 bytes) per client
- `networkID`: 32 bits, `heroTypeID`: 16 bits
- `x`, `y`: 16 bits each (quantized, MAP ±500m, 1.53cm precision)
- `health`, `maxHealth`, `mana`, `maxMana`: VLE (base-128)
- `level`: 5 bits, `experience`: VLE
- `stateFlags`: 8 bits (0x01=Dead, 0x02=Stunned, 0x04=Rooted)

**Delta packet (no changes):** 38 bits (networkID + 6 dirty flags = 0)

**InputPayload:** 24 bits (dirX: 8b, dirY: 8b, buttons: 8b)

## Critical Design Rules

- **No singletons.** Composition root lives in `main.cpp`.
- **Transport isolation.** No code outside `MiddlewareTransport` may include SFML headers.
- **Dirty bit protocol.** Always use `SetNetworkVar<T>()` in `BaseHero` subclasses — never set `m_state` fields directly.
- **Endianness-neutral.** Bitwise operators are endian-independent. No `SwapEndian` calls.
- **Namespace:** `NetworkMiddleware` throughout.
- **No implementation in headers** except templates and constexpr.
- **TDD always.** Every new feature gets tests before or alongside implementation. 180 tests are the regression suite — never regress.
- **Tick loop:** each cycle must complete in < 10ms (100Hz target). Flag any blocking calls inside the tick loop.
- **sf::IpAddress(uint32_t)** expects big-endian (network byte order). ParseIpv4 returns big-endian.

## Testing Infrastructure

```bash
# Run all 180 unit tests
powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1   # Windows
bash scripts/run_tests.sh                                         # Linux/WSL2
bash scripts/run_tests.sh --coverage                             # + lcov HTML report
```

Test suites: BitWriterReader, PacketHeader, SequenceContext, NetworkOptimizer, HeroSerializer, DeltaCompression, NetworkManager, SessionRecovery, BotIntegration, NetworkProfiler, GameWorld.

## Benchmark Infrastructure

```bash
bash scripts/run_stress_test.sh   # WSL2 — builds, runs 3 scenarios, saves results
```

Results auto-saved to `benchmarks/results/<date>_<hash>.md`.
Metric definitions in `benchmarks/README.md`.
Key result: **1.1% tick budget** with 47 clients under 100ms/2% loss.

## Workflow

1. Gemini designs the proposal (handoff document)
2. Claude validates the handoff (flags issues before implementation)
3. Gemini addresses feedback
4. Claude implements with TDD
5. Claude writes IR automatically after implementation
6. Gemini validates IR
7. Claude creates PR → CI → merge to develop
8. Claude updates CLAUDE.md (current status, test count, benchmark results) — no need to ask
9. After entire phase: Claude writes phase-level DL

**Branch convention:** `feature-name` → `develop` → `main` (at milestones).
Never commit directly to `develop` or `main`.

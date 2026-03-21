# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Engine-agnostic network middleware for MOBA games — a C++20 bachelor's thesis (TFG). Authoritative dedicated server targeting Linux, validated via a Visual Debugger (Unreal Engine ActorComponent plugin). Not a game itself. Goal: outperform commercial middlewares (Photon Bolt, Mirror, UE Replication) in bandwidth efficiency and latency.

**Current status:** Phases 1 & 2 complete (2026-03-18). Phase 3 (Netcode Core & Session Protocol) is the active sprint.

## Build Commands

**Prerequisites:** CMake 3.20+, C++20 compiler, SFML (network + system components).

```bash
# Configure and build (from repo root)
mkdir cmake-build-debug && cd cmake-build-debug
cmake ..
cmake --build . --config Debug

# Run the server executable (integration/validation test)
./Server/NetServer           # Linux/WSL
./Server/Debug/NetServer.exe # Windows
```

There are no unit tests — `Server/main.cpp` serves as the integration validation (serialization round-trip with `assert()`). Validated results: 145-bit full sync packet, 1.75cm quantization error, 60-70% bandwidth reduction.

## Module Architecture

Five CMake static libraries + one executable, dependency order:

```
MiddlewareShared  (no deps)
    ↑
MiddlewareCore    ← MiddlewareShared
MiddlewareTransport ← MiddlewareShared + sfml-network
Brain             (standalone, no shared dep)
    ↑
NetServer (exe)   ← all four libraries + Threads
```

**MiddlewareShared** — Everything shared between server and Unreal client:
- `Serialization/BitWriter` / `BitReader`: LSB-first block-based bit packing. Core constraint: no Protobuf/FlatBuffers — manual bit operations for maximum compression.
- `Data/Network/NetworkOptimizer`: Quantization (float → 14-bit int, 1.75cm precision, MAP ±500) and VLE/Base-128 varints for health/mana/XP.
- `Data/Network/HeroSerializer`: Serialize/deserialize `HeroState` using dirty bits — only changed fields are written.
- `Gameplay/BaseHero`: Abstract MOBA hero with `SetNetworkVar<T>(member, newValue, bit)` template that auto-sets dirty bits. `dirtyMask` starts at `0xFFFFFFFF` for first tick (full state sync).
- `Log/Logger`: Async producer-consumer logger (`std::jthread` + mutex queue). Channels: Core, Transport, Brain. Hex/binary packet inspection built in.

**MiddlewareTransport** — Only layer that knows SFML exists. `SFMLTransport` implements `ITransport`. `TransportFactory::Create(TransportType)` is the only entry point — never instantiate transports directly.

**MiddlewareCore** — `NetworkManager` polls transport and fires `std::function` callbacks with raw `std::vector<uint8_t>` + `EndPoint`. Zero coupling to Brain. `PacketManager` is a stub (Phase 3).

**Brain** — AI layer (Phase 4+). `BrainManager` orchestrates `NeuralProcessor` (analysis) and `BehaviorTree` (decisions) behind `IDataProcessor` / `IBehaviorEngine` interfaces.

## Key Interfaces

| Interface | Purpose |
|-----------|---------|
| `ITransport` | `Initialize`, `Receive`, `Send`, `Close` |
| `INetworkable` | `Serialize(BitWriter&)`, `Unserialize(BitReader&)`, `GetDirtyMask`, `ClearDirtyMask`, `GetNetworkID` |
| `IHero` (extends INetworkable) | Full hero API: position, health/mana, level, abilities, status flags |

`BaseHero` implements `IHero` generically. `ViegoEntity` (hero type ID 66) is the first concrete hero.

## Data Flow

```
SFMLTransport (UDP)
    ↓ raw bytes (std::vector<uint8_t>)
NetworkManager::Update() → OnDataReceivedCallback
    ↓ lambda in main.cpp
Brain (analyze → decide)
```

## Critical Design Rules

- **No singletons.** Composition root lives in `main.cpp`.
- **Transport isolation.** No code outside `MiddlewareTransport` may include SFML headers.
- **Dirty bit protocol.** Always use `SetNetworkVar<T>()` in `BaseHero` subclasses — never set `m_state` fields directly.
- **Endianness-neutral.** Bitwise operators (`<<`, `>>`, `&`, `|`) are endian-independent by design. No `SwapEndian` calls.
- **Namespace:** `NetworkMiddleware` throughout.

## HeroState Wire Format Reference

Full sync packet: ~145 bits (~18 bytes). Fields serialized via `HeroSerializer`:
- `networkID`: 32 bits
- `heroTypeID`: 16 bits
- `x`, `y`: 14 bits each (quantized, MAP ±500m, 1.75cm precision)
- `health`, `maxHealth`, `mana`, `maxMana`: VLE (variable, base-128)
- `level`: 5 bits
- `experience`: VLE
- `stateFlags`: 8 bits (0x01=Dead, 0x02=Stunned, 0x04=Rooted)

## Phase 3 Roadmap (Active Sprint)

- **3.1 Packet Topology:** Header design (Sequence, Ack, Type)
- **3.2 Connection Handshake:** Hello → Challenge → Welcome (prevents packet injection); `RemoteClient` class per player
- **3.3 Reliability Layer (UDP-R):**
  - `Unreliable`: position/movement (Predictive AI compensates losses)
  - `Reliable Ordered`: item purchases, abilities, level-up
  - `Reliable Unordered`: deaths, chat
- **3.4 Clock Synchronization:** RTT calculation + Tick alignment client↔server (prerequisite for Predictive AI)
- **3.5 Delta Compression & Zig-Zag:** Circular snapshot buffer (Baselines, 128-256 slots), Zig-Zag encoding for negative deltas
- **3.6 [PENDING DESIGN] Session Recovery:** Heartbeats, timeouts, reconnection tokens

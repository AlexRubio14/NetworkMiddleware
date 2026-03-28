# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Engine-agnostic network middleware for MOBA games — C++20 bachelor's thesis (TFG). Authoritative dedicated server targeting Linux. Goal: outperform Photon Bolt / Mirror / UE Replication in bandwidth efficiency and latency.

**Current status:** Phases 1–6.2 complete (2026-03-28). 236/236 tests passing (Windows/MSVC).
P-6.1: RawUDPTransport — POSIX sockets + sendmmsg batch dispatch (Linux).
P-6.2: AsyncSendDispatcher — sendmmsg moved off the game loop via jthread + CV.
CD pipeline active (GitHub Actions → GitHub Releases + Docker GHCR).

## Build Commands

```bash
# Unit tests
powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1   # Windows
bash scripts/run_tests.sh                                         # Linux/WSL2
bash scripts/run_tests.sh --coverage                             # + lcov HTML report

# Stress benchmark (WSL2 only — requires sudo for tc netem)
bash scripts/run_stress_test.sh
bash scripts/run_final_benchmark.sh   # P-4.5 Scalability Gauntlet

# Manual build (Windows)
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --config Debug
```

## Critical Design Rules

- **No singletons.** Composition root lives in `main.cpp`.
- **Transport isolation.** No code outside `MiddlewareTransport` may include SFML headers.
- **Dirty bit protocol.** Always use `SetNetworkVar<T>()` in `BaseHero` subclasses — never set `m_state` fields directly.
- **Endianness-neutral.** Bitwise operators are endian-independent. No `SwapEndian` calls.
- **Namespace:** `NetworkMiddleware` throughout.
- **No implementation in headers** except templates and constexpr.
- **TDD always.** Every new feature gets tests before or alongside implementation. 231 tests are the regression suite — never regress.
- **Tick loop:** each cycle must complete in < 10ms (100Hz target). Flag any blocking calls inside the tick loop.
- **sf::IpAddress(uint32_t)** expects big-endian (network byte order). ParseIpv4 returns big-endian.

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

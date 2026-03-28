// NetworkMiddleware — P-5.4 Server
//
// Authoritative dedicated server with a 100 Hz game loop.
// P-4.4 adds the Dynamic Work-Stealing Job System and the Split-Phase
// snapshot pipeline:
//
//   Phase A (parallel): Job System serializes each client's batch snapshot
//                       concurrently using SerializeBatchSnapshotFor().
//   Phase B (main):     CommitAndSendBatchSnapshot() records history and
//                       transmits one packet per client sequentially.
//
// P-4.5 adds CRC32 packet integrity (trailer on every outgoing packet,
// verify+discard on every incoming packet) and a --sequential flag for
// the Scalability Gauntlet benchmark:
//   --sequential: disable Split-Phase; serialize + send inline per client.
//
// P-5.2 adds Silent Server-Side Kalman Prediction (Brain::KalmanPredictor).
// When a client's input packet is missing for a tick, the predictor
// synthesizes a plausible direction from the constant-velocity model,
// keeping GameWorld smooth without changing the wire format.
//
// P-5.3 adds Server-Side Lag Compensation (Rewind).
// InputPayload now carries clientTickID (16-bit). When an ability button is
// pressed, the server rewinds target positions to clientTickID (clamped to
// kMaxRewindTicks=20 ticks / 200ms) and performs a hit check against those
// historical positions using HitValidator::CheckHit.
//
// P-5.4 adds Network LOD / AI Replication Prioritizer (Brain::PriorityEvaluator).
// Phase 0b computes entity relevance tiers per observer before the gather loop.
// Tier 0 entities are sent every tick (100Hz); Tier 1 every 2nd tick (50Hz);
// Tier 2 every 5th tick (20Hz).  FOW filtering (P-5.1) is applied first so
// the prioritizer only scores visible entities.
//
// Dynamic scaling: MaybeScale() checks the EMA tick time every 1s and
// grows/shrinks the pool between kMinThreads and hardware_concurrency-1.
//
// Usage:
//   ./NetServer              (listens on UDP :7777, parallel mode)
//   ./NetServer --sequential (sequential snapshot dispatch — benchmark)
//   SERVER_PORT=9999 ./NetServer

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "../Brain/KalmanPredictor.h"
#include "../Brain/PriorityEvaluator.h"
#include "../Core/GameWorld.h"
#include "../Core/HitValidator.h"
#include "../Core/JobSystem.h"
#include "../Core/AsyncSendDispatcher.h"
#include "../Core/NetworkManager.h"
#include "../Core/SpatialGrid.h"
#include "../Core/VisibilityTracker.h"
#include "../Shared/Log/Logger.h"
#include "../Shared/Network/InputPackets.h"
#include "../Shared/NetworkAddress.h"
#include "../Shared/TransportType.h"
#include "../Transport/TransportFactory.h"

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Transport;

// ─── Graceful shutdown ────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void OnSignal(int) {
    g_running.store(false, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    // ── Parse flags ───────────────────────────────────────────────────────────
    bool parallelMode = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--sequential")
            parallelMode = false;
    }

    Logger::Start();
    Logger::Banner(parallelMode
        ? "NetServer — P-5.4 Parallel Mode (Lag Comp + Network LOD)"
        : "NetServer — P-5.4 Sequential Mode (benchmark baseline)");

    // ── Transport ─────────────────────────────────────────────────────────────

    uint16_t port = 7777;
    if (const char* portEnv = std::getenv("SERVER_PORT")) {
        try {
            const int parsed = std::stoi(portEnv);
            if (parsed < 0 || parsed > 65535) {
                Logger::Log(LogLevel::Error, LogChannel::Transport,
                    std::format("SERVER_PORT out of range [0,65535]: {} — aborting", portEnv));
                Logger::Stop();
                return 1;
            }
            port = static_cast<uint16_t>(parsed);
        } catch (const std::exception& e) {
            Logger::Log(LogLevel::Error, LogChannel::Transport,
                std::format("SERVER_PORT is not a valid integer '{}': {} — aborting", portEnv, e.what()));
            Logger::Stop();
            return 1;
        }
    }

    // P-6.1: Use native POSIX sockets on Linux (sendmmsg — 1 syscall per tick).
    // P-6.2: Wrap with AsyncSendDispatcher — sendmmsg runs off the game loop.
    // Windows keeps SFML for local dev / CI (no dispatcher).
    auto transport = []() -> std::shared_ptr<Shared::ITransport> {
#ifdef __linux__
        return TransportFactory::Create(TransportType::NATIVE_LINUX);
#else
        return TransportFactory::Create(TransportType::SFML);
#endif
    }();
    if (!transport->Initialize(port)) {
        Logger::Log(LogLevel::Error, LogChannel::Transport,
            std::format("Failed to bind UDP socket on port {} — aborting", port));
        Logger::Stop();
        return 1;
    }

    Logger::Log(LogLevel::Success, LogChannel::Transport,
        std::format("Listening on UDP :{}", port));

    // ── NetworkManager + GameWorld ────────────────────────────────────────────

#ifdef __linux__
    auto dispatcher = std::make_unique<Core::AsyncSendDispatcher>(transport);
    NetworkManager              manager(transport, std::move(dispatcher));
#else
    NetworkManager              manager(transport);
#endif
    GameWorld                   gameWorld;
    SpatialGrid                 spatialGrid;
    Brain::KalmanPredictor      kalmanPredictor;
    uint32_t                    tickID = 0;

    manager.SetClientConnectedCallback(
        [&gameWorld, &kalmanPredictor](uint16_t id, const EndPoint& ep) {
            Logger::Log(LogLevel::Success, LogChannel::Core,
                std::format("CLIENT CONNECTED   NetworkID={}  ep={}", id, ep.ToString()));
            gameWorld.AddHero(id);
            kalmanPredictor.AddEntity(id, 0.0f, 0.0f);
        });

    manager.SetClientDisconnectedCallback(
        [&gameWorld, &kalmanPredictor, &visTracker](uint16_t id, const EndPoint& ep) {
            Logger::Log(LogLevel::Warning, LogChannel::Core,
                std::format("CLIENT DISCONNECTED  NetworkID={}  ep={}", id, ep.ToString()));
            gameWorld.RemoveHero(id);
            kalmanPredictor.RemoveEntity(id);
            visTracker.RemoveClient(id);  // P-6.3: clean up visibility state
        });

    // ── P-4.4 Job System ──────────────────────────────────────────────────────
    // Start with kMinThreads (2). MaybeScale() grows/shrinks based on EMA load.
    // Logger callback injected here so JobSystem stays free of global-state deps.

    JobSystem jobSystem(JobSystem::kMinThreads, [](const std::string& msg) {
        Logger::Log(LogLevel::Info, LogChannel::Core, msg);
    });

    Logger::Log(LogLevel::Info, LogChannel::Core,
        std::format("[JobSystem] Started with {} worker threads (max=hardware_concurrency-1)",
            jobSystem.GetThreadCount()));

    Logger::Log(LogLevel::Info, LogChannel::Core,
        parallelMode
            ? "[Snapshot] Mode: PARALLEL (Split-Phase + Job System)"
            : "[Snapshot] Mode: SEQUENTIAL (main-thread only — benchmark baseline)");

    // ── P-5.4 Priority Evaluator ──────────────────────────────────────────────
    Brain::PriorityEvaluator priorityEvaluator;

    // ── P-6.3 Visibility Tracker ──────────────────────────────────────────────
    Core::VisibilityTracker visTracker;

    // ── P-5.3 Lag Compensation constants ─────────────────────────────────────
    static constexpr uint32_t kMaxRewindTicks  = 20;    // 200ms at 100Hz
    static constexpr float    kAbilityRange    = 150.0f; // world units

    // ── 100 Hz game loop ──────────────────────────────────────────────────────
    // Budget: 10ms per tick.
    //
    // Each tick:
    //   1. manager.Update()           — drain UDP, handshakes, ACKs, buffer inputs
    //   2. ForEachEstablished         — apply inputs (Kalman pred on miss) + lag comp
    //   3. gameWorld.Tick(dt)         — advance simulation
    //   3b. gameWorld.RecordTick()    — snapshot positions for rewind (P-5.3)
    //   4a. Phase 0: rebuild FOW grid — SpatialGrid::Clear + MarkVision
    //   4b. Phase 0b: PriorityEvaluator — tier per (observer, entity) (P-5.4)
    //   4c. Collect snapshot tasks    — gather (ep, HeroState) pairs filtered by FOW+tier
    //   4d. Phase A: dispatch to pool — parallel SerializeSnapshotFor()
    //   4e. Phase B: commit + send    — sequential CommitAndSendSnapshot()
    //   5. ++tickID                   — monotone counter for lag compensation
    //   6. jobSystem.MaybeScale()     — dynamic thread count adjustment
    //   7. sleep_until(nextTick)      — yield remaining budget

    constexpr auto  kTickInterval = std::chrono::microseconds(10'000);  // 100 Hz
    constexpr float kFixedDt      = 0.01f;                              // seconds

    // P-5.x Batch snapshot task: one entry per client, holds ALL visible entity
    // states for that observer.  Phase A serializes the entire batch into one
    // wire buffer; Phase B sends it in a single UDP packet.
    // This reduces send() calls from O(clients × entities) to O(clients) per tick,
    // eliminating the WSL2 syscall overhead that caused Scenario C's 39.5ms full loop.
    struct SnapshotTask {
        EndPoint                        ep;
        uint16_t                        obsID  = 0;  // P-6.3: observer networkID for VisibilityTracker
        std::vector<Data::HeroState>    states;   // all visible entities for this observer
        std::vector<uint8_t>            buffer;   // output filled by the worker job
    };

    // Full-loop EMA for JobSystem scaling.
    // NetworkProfiler::recentAvgTickMs only covers NetworkManager::Update(), which
    // excludes GameWorld::Tick(), Phase A serialisation and Phase B send.  Feeding
    // that partial metric to MaybeScale() could keep the pool undersized while the
    // outer tick is already over budget.  We maintain a separate EMA here that
    // covers the entire work window (steps 1-6, before sleep_until).
    float loopEmaMs   = 0.0f;
    constexpr float kLoopAlpha = 0.1f;

    // Persistent per-tick scratch storage — allocated once, cleared each tick.
    // Avoids heap churn in the hot path (gather loop runs 100× per second).
    std::vector<Brain::EvaluationTarget> allTargets;
    std::unordered_map<uint32_t, uint8_t> entityTeams;
    std::vector<SnapshotTask> snapshots;

    // Phase 0b pre-compute buffers — persistent to avoid heap allocation per tick.
    std::vector<bool>                                        inCombatFlags;
    std::unordered_map<uint32_t, size_t>                    entityIdx;
    std::unordered_map<uint32_t, std::vector<uint8_t>>      tierByObs;

    auto nextTick = std::chrono::steady_clock::now();

    Logger::Log(LogLevel::Info, LogChannel::Core,
        "Game loop starting at 100 Hz (10ms tick budget). Ctrl+C to stop.");

    while (g_running.load(std::memory_order_relaxed)) {
        const auto tickStart = std::chrono::steady_clock::now();

        // 1. Network I/O
        manager.Update();

        // 2. Apply buffered inputs — with P-5.2 Kalman prediction for missing ticks.
        //    P-5.3 Lag Compensation: if ability buttons are set, rewind target
        //    positions to clientTickID and validate hit against historical positions.
        //
        // z_k = GetHeroState(id)->[x,y]: authoritative position BEFORE this tick's
        // input.  The Kalman filter observes this position on real-input ticks to
        // refine its velocity estimate; on prediction ticks it extrapolates velocity
        // to synthesize a plausible InputPayload (silent, no wire-format change).
        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& /*ep*/, const InputPayload* input) {
                const auto* state = gameWorld.GetHeroState(id);
                if (!state) return;

                InputPayload toApply;
                if (input) {
                    // Real input arrived: update Kalman filter, use real input.
                    kalmanPredictor.Tick(id, state->x, state->y,
                                         input->dirX, input->dirY);
                    toApply = *input;

                    // P-5.3: Lag compensation — validate ability hits against rewound positions.
                    const bool abilityFired = (input->buttons & (
                        Shared::kAbility1 | Shared::kAbility2 |
                        Shared::kAbility3 | Shared::kAbility4 | Shared::kAttack)) != 0;

                    if (abilityFired) {
                        // clientTickID is the low 16 bits of the server tickID echoed back
                        // by BotClient from the last received Snapshot packet.  We compute
                        // the delta with wrap-safe uint16_t subtraction so the comparison
                        // works correctly across the uint16_t wrap-around boundary (~10 min).
                        // fireTick is within [tickID - kMaxRewindTicks, tickID] — note: exact
                        // same-tick match is valid; GetStateAtTick will return the entry
                        // recorded after Tick() but before snapshot dispatch this very tick.
                        //
                        // Wrap-around note: when clientTickID wraps (after ~65k inputs), delta
                        // may momentarily exceed kMaxRewindTicks; the clamp safely falls back to
                        // tickID - kMaxRewindTicks for that single input, degrading accuracy
                        // only during the wrap transition (one input packet in ~10 minutes).
                        const uint16_t tickID16 = static_cast<uint16_t>(tickID);
                        const uint16_t delta    = tickID16 - input->clientTickID; // wrap-safe
                        const uint32_t fireTick = (delta <= kMaxRewindTicks)
                            ? (tickID - static_cast<uint32_t>(delta))
                            : (tickID >= kMaxRewindTicks ? tickID - kMaxRewindTicks : 0u);

                        // Attacker position at fire tick (for range check origin).
                        const auto* atkHist = gameWorld.GetStateAtTick(id, fireTick);
                        const float atkX = atkHist ? atkHist->x : state->x;
                        const float atkY = atkHist ? atkHist->y : state->y;

                        // Check all other entities at the fire tick.
                        gameWorld.ForEachHero([&](uint32_t tgtID, const Data::HeroState& /*cur*/) {
                            if (tgtID == id) return;
                            const auto* tgtHist = gameWorld.GetStateAtTick(tgtID, fireTick);
                            if (!tgtHist) return;
                            if (CheckHit(atkX, atkY, tgtHist->x, tgtHist->y, kAbilityRange)) {
                                Logger::Log(LogLevel::Info, LogChannel::Core,
                                    std::format("[LagComp] HIT  attacker={} target={} fireTick={}",
                                        id, tgtID, fireTick));
                            }
                        });
                    }
                } else {
                    // No input this tick: synthesize from Kalman prediction.
                    const auto pred = kalmanPredictor.Predict(id, state->x, state->y);
                    toApply = InputPayload{pred.dirX, pred.dirY, 0, 0};
                }
                gameWorld.ApplyInput(id, toApply, kFixedDt);
            });

        // 3. Advance simulation
        gameWorld.Tick(kFixedDt);

        // 3b. P-5.3 — Record tick for rewind history (after Tick, before snapshots).
        gameWorld.RecordTick(tickID);

        // 4. Send snapshots — Split-Phase (P-4.4) + FOW (P-5.1) + Network LOD (P-5.4)
        //
        // Phase 0  (main thread): rebuild SpatialGrid — Clear + MarkVision.
        // Phase 0b (main thread): PriorityEvaluator — compute tier per (obs, entity).
        // Phase A  (workers):     Serialize each task concurrently.
        // Phase B  (main thread): Commit history + send.

        // Phase 0 — rebuild FOW grid
        spatialGrid.Clear();
        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& ep, const InputPayload* /*input*/) {
                const auto* st = gameWorld.GetHeroState(id);
                if (st)
                    spatialGrid.MarkVision(st->x, st->y, manager.GetClientTeamID(ep));
            });

        // Phase 0b — compute replication tiers per observer (P-5.4).
        // Reuse persistent allTargets / entityTeams — clear instead of reallocate.
        allTargets.clear();
        gameWorld.ForEachHero([&](uint32_t eid, const Data::HeroState& st) {
            Brain::EvaluationTarget t;
            t.entityID = eid;
            t.teamID   = 0;
            t.x        = st.x;
            t.y        = st.y;
            allTargets.push_back(t);
        });

        // Map entityID → teamID (derived from RemoteClient, resolved via EndPoint).
        entityTeams.clear();
        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& ep, const InputPayload* /*input*/) {
                entityTeams[id] = manager.GetClientTeamID(ep);
            });
        for (auto& t : allTargets) {
            auto it = entityTeams.find(t.entityID);
            if (it != entityTeams.end()) t.teamID = it->second;
        }

        // Phase 0b (cont.) — pre-compute inCombat[] ONCE, then tiers per observer.
        // O(N²) instead of the previous O(N³) (was: Evaluate() called N times, each O(N²)).
        //
        // inCombatFlags[i] = true if allTargets[i] is within kCombatRadius of any enemy.
        // entityIdx maps entityID → index in allTargets for O(1) tier lookups in gather.
        // tierByObs maps obsID → tier[i] for each entity i in allTargets.
        inCombatFlags = priorityEvaluator.ComputeInCombat(allTargets);

        entityIdx.clear();
        entityIdx.reserve(allTargets.size());
        for (size_t i = 0; i < allTargets.size(); ++i)
            entityIdx[allTargets[i].entityID] = i;

        tierByObs.clear();
        tierByObs.reserve(manager.GetEstablishedCount());
        manager.ForEachEstablished(
            [&](uint16_t obsID, const EndPoint& /*ep*/, const InputPayload* /*input*/) {
                const auto* obsSt = gameWorld.GetHeroState(obsID);
                if (!obsSt) return;
                const auto relevance = priorityEvaluator.Evaluate(
                    obsID, obsSt->x, obsSt->y, allTargets, inCombatFlags);
                std::vector<uint8_t> tiers(allTargets.size(), 2u);
                for (const auto& r : relevance) {
                    auto it = entityIdx.find(r.entityID);
                    if (it != entityIdx.end()) tiers[it->second] = r.tier;
                }
                tierByObs[obsID] = std::move(tiers);
            });

        // Tier helper: should we send this entity to this observer this tick?
        auto shouldSend = [&](uint8_t tier) -> bool {
            if (tier == 0) return true;
            if (tier == 1) return (tickID % 2) == 0;
            return (tickID % 5) == 0;  // tier 2
        };

        // Gather snapshot tasks — one entry per client, all visible entities batched.
        // P-5.x: one UDP packet per client per tick (batch) instead of one per entity.
        // This reduces Phase B send() calls from O(clients × entities) to O(clients).
        // Tier lookup is O(1): tierByObs[obsID][entityIdx[eid]].
        // Reuse persistent snapshots vector — clear instead of reallocate.
        snapshots.clear();
        manager.ForEachEstablished(
            [&](uint16_t obsID, const EndPoint& ep, const InputPayload* /*input*/) {
                const uint8_t obsTeam = manager.GetClientTeamID(ep);
                const auto* obsSt     = gameWorld.GetHeroState(obsID);
                if (!obsSt) return;

                const auto obsIt = tierByObs.find(obsID);
                if (obsIt == tierByObs.end()) return;
                const std::vector<uint8_t>& tiers = obsIt->second;

                SnapshotTask task;
                task.ep    = ep;
                task.obsID = obsID;
                gameWorld.ForEachHero([&](uint32_t eid, const Data::HeroState& st) {
                    if (!spatialGrid.IsCellVisible(st.x, st.y, obsTeam)) return;
                    const auto idxIt = entityIdx.find(eid);
                    const uint8_t tier = (idxIt != entityIdx.end()) ? tiers[idxIt->second] : 2u;
                    if (shouldSend(tier))
                        task.states.push_back(st);
                });
                if (!task.states.empty())
                    snapshots.push_back(std::move(task));
            });

        // RecordEntitySnapshotsSent is now called inside CommitAndSendBatchSnapshot
        // so the count is always accurate regardless of code path.

        // P-6.2: 30Hz snapshot rate — simulate at 100Hz, send every 3rd tick.
        // Reduces bandwidth ~3x (100Hz → 30Hz out). Phase A+B are skipped on
        // the other 2 ticks; the gather loop above still runs at 100Hz so FOW
        // and LOD tier data remain fresh.
        static constexpr int kSnapshotEveryNTicks = 3;
        const bool sendThisTick = (tickID % kSnapshotEveryNTicks == 0);

        if (!snapshots.empty() && sendThisTick) {
            // P-6.3 — Re-entry baseline eviction (main thread, before Phase A).
            // When an entity transitions invisible→visible for a client, its
            // m_entityBaselines entry may be stale (ACK loss on the return path
            // means the server's confirmed baseline lags the client's local copy).
            // Evicting forces SerializeBatchSnapshotFor to emit a full state,
            // guaranteeing the client resyncs correctly on re-entry.
            for (auto& task : snapshots) {
                std::vector<uint32_t> nowVisible;
                nowVisible.reserve(task.states.size());
                for (const auto& st : task.states)
                    nowVisible.push_back(st.networkID);
                const auto reentrants = visTracker.UpdateAndGetReentrants(task.obsID, nowVisible);
                for (const uint32_t eid : reentrants)
                    manager.EvictEntityBaseline(task.ep, eid);
            }

            if (parallelMode) {
                // Phase A — parallel serialization (one job per client, not per entity).
                // The latch size is now O(clients), not O(clients × entities).
                // sync.wait() is an unbounded barrier; we poll try_wait() against the
                // tick deadline to detect over-budget serialization early.
                std::latch sync(static_cast<std::ptrdiff_t>(snapshots.size()));

                for (auto& task : snapshots) {
                    jobSystem.Execute([&manager, &task, tickID, &sync]() {
                        task.buffer = manager.SerializeBatchSnapshotFor(task.ep, task.states, tickID);
                        sync.count_down();
                    });
                }

                // Deadline-aware drain: detect over-budget without stranding tasks.
                const auto phaseADeadline = nextTick + kTickInterval;
                bool phaseAOverBudget = false;
                while (!sync.try_wait()) {
                    if (std::chrono::steady_clock::now() >= phaseADeadline) {
                        phaseAOverBudget = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (phaseAOverBudget) {
                    Logger::Log(LogLevel::Warning, LogChannel::Core,
                        "Snapshot Phase A exceeded tick budget — waiting for worker drain");
                    sync.wait();  // correctness: must drain before touching task.buffer
                }

                // Phase B — one send per client (was one send per entity before batching).
                for (auto& task : snapshots) {
                    manager.CommitAndSendBatchSnapshot(task.ep, task.states, task.buffer);
                }
            } else {
                // Sequential baseline — serialize + send inline, one batch per client.
                for (auto& task : snapshots) {
                    task.buffer = manager.SerializeBatchSnapshotFor(task.ep, task.states, tickID);
                    manager.CommitAndSendBatchSnapshot(task.ep, task.states, task.buffer);
                }
            }
        }

        // P-6.2: Signal the async send thread (or flush synchronously on SFML).
        // On Linux: returns immediately — sendmmsg runs off the game loop.
        // On Windows / fallback: synchronous Flush() as before.
        if (sendThisTick)
            manager.FlushTransport();

        // 5. Advance tick counter
        ++tickID;

        // 6. Full-loop EMA — covers all work steps 1-5 before sleep.
        const auto tickWorkEnd = std::chrono::steady_clock::now();
        const float fullTickMs = std::chrono::duration<float, std::milli>(
            tickWorkEnd - tickStart).count();
        loopEmaMs = kLoopAlpha * fullTickMs + (1.0f - kLoopAlpha) * loopEmaMs;

        // Record full-loop time in profiler so it appears in MaybeReport output.
        manager.RecordFullTick(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                tickWorkEnd - tickStart).count()));

        nextTick += kTickInterval;

        // If the tick ran over budget, reset to avoid catch-up spin.
        const auto now = std::chrono::steady_clock::now();
        if (nextTick < now) {
            Logger::Log(LogLevel::Warning, LogChannel::Core,
                std::format("Tick over budget by {:.2f}ms — resetting schedule",
                    std::chrono::duration<float, std::milli>(now - nextTick).count()));
            nextTick = now;
        }

        // 7. Dynamic thread-pool scaling using full-loop EMA (steps 1-5).
        //    This signal includes GameWorld::Tick + Phase A + Phase B, giving
        //    the scaler an accurate view of total CPU pressure per tick.
        jobSystem.MaybeScale(loopEmaMs);

        std::this_thread::sleep_until(nextTick);
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────

    Logger::Log(LogLevel::Info, LogChannel::Core,
        std::format("Shutdown. Connected clients at exit: {}  Steals: {}",
            manager.GetEstablishedCount(),
            jobSystem.GetStealCount()));

    transport->Close();
    Logger::Sync();
    Logger::Stop();
    return 0;
}

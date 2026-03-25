// NetworkMiddleware — P-5.4 Server
//
// Authoritative dedicated server with a 100 Hz game loop.
// P-4.4 adds the Dynamic Work-Stealing Job System and the Split-Phase
// snapshot pipeline:
//
//   Phase A (parallel): Job System serializes each client's snapshot
//                       concurrently using SerializeSnapshotFor().
//   Phase B (main):     CommitAndSendSnapshot() records history and
//                       transmits pre-built payloads sequentially.
//
// P-4.5 adds CRC32 packet integrity (trailer on every outgoing packet,
// verify+discard on every incoming packet) and a --sequential flag for
// the Scalability Gauntlet benchmark:
//   --sequential: disable Split-Phase; dispatch snapshots on the main
//                 thread (SendSnapshot), Job System stays idle.
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
#include "../Core/NetworkManager.h"
#include "../Core/SpatialGrid.h"
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

    auto transport = TransportFactory::Create(TransportType::SFML);
    if (!transport->Initialize(port)) {
        Logger::Log(LogLevel::Error, LogChannel::Transport,
            std::format("Failed to bind UDP socket on port {} — aborting", port));
        Logger::Stop();
        return 1;
    }

    Logger::Log(LogLevel::Success, LogChannel::Transport,
        std::format("Listening on UDP :{}", port));

    // ── NetworkManager + GameWorld ────────────────────────────────────────────

    NetworkManager              manager(transport);
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
        [&gameWorld, &kalmanPredictor](uint16_t id, const EndPoint& ep) {
            Logger::Log(LogLevel::Warning, LogChannel::Core,
                std::format("CLIENT DISCONNECTED  NetworkID={}  ep={}", id, ep.ToString()));
            gameWorld.RemoveHero(id);
            kalmanPredictor.RemoveEntity(id);
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

    // Snapshot task: holds endpoint, a value-copy of HeroState, and the output buffer.
    // Value copy ensures workers read stable data while main thread is at latch::wait().
    struct SnapshotTask {
        EndPoint              ep;
        Data::HeroState       state;   // copy — workers are read-only on this
        std::vector<uint8_t>  buffer;  // output filled by the worker job
    };

    // Full-loop EMA for JobSystem scaling.
    // NetworkProfiler::recentAvgTickMs only covers NetworkManager::Update(), which
    // excludes GameWorld::Tick(), Phase A serialisation and Phase B send.  Feeding
    // that partial metric to MaybeScale() could keep the pool undersized while the
    // outer tick is already over budget.  We maintain a separate EMA here that
    // covers the entire work window (steps 1-6, before sleep_until).
    float loopEmaMs   = 0.0f;
    constexpr float kLoopAlpha = 0.1f;

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
                        // Clamp fireTick to the valid rewind window.
                        const uint32_t fireTick =
                            (tickID >= kMaxRewindTicks &&
                             input->clientTickID <= static_cast<uint16_t>(tickID) &&
                             (tickID - input->clientTickID) <= kMaxRewindTicks)
                            ? static_cast<uint32_t>(input->clientTickID)
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
        // Build global entity list once; evaluate per observer.
        std::vector<Brain::EvaluationTarget> allTargets;
        gameWorld.ForEachHero([&](uint32_t eid, const Data::HeroState& st) {
            Brain::EvaluationTarget t;
            t.entityID = eid;
            t.teamID   = 0; // resolved below per-observer; set placeholder
            t.x        = st.x;
            t.y        = st.y;
            allTargets.push_back(t);
        });

        // Map entityID → teamID for the evaluator (team is a per-client property).
        // We build the target list with correct teamIDs derived from RemoteClient.
        // Since ForEachEstablished iterates by EndPoint, we use GetClientTeamID.
        // For entities not in the client map (should not happen), teamID stays 0.
        std::unordered_map<uint32_t, uint8_t> entityTeams;
        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& ep, const InputPayload* /*input*/) {
                entityTeams[id] = manager.GetClientTeamID(ep);
            });
        for (auto& t : allTargets) {
            auto it = entityTeams.find(t.entityID);
            if (it != entityTeams.end()) t.teamID = it->second;
        }

        // Tier helper: should we send this entity to this observer this tick?
        auto shouldSend = [&](uint8_t tier) -> bool {
            if (tier == 0) return true;
            if (tier == 1) return (tickID % 2) == 0;
            return (tickID % 5) == 0;  // tier 2
        };

        // Gather snapshot tasks: FOW-visible + tier-filtered.
        std::vector<SnapshotTask> snapshots;
        manager.ForEachEstablished(
            [&](uint16_t obsID, const EndPoint& ep, const InputPayload* /*input*/) {
                const uint8_t obsTeam = manager.GetClientTeamID(ep);
                const auto* obsSt     = gameWorld.GetHeroState(obsID);
                if (!obsSt) return;

                // Evaluate tiers for this observer.
                const auto relevance = priorityEvaluator.Evaluate(
                    obsID, obsSt->x, obsSt->y, obsTeam, allTargets);

                // Build a quick entityID → tier lookup.
                std::unordered_map<uint32_t, uint8_t> tierMap;
                for (const auto& r : relevance)
                    tierMap[r.entityID] = r.tier;

                gameWorld.ForEachHero([&](uint32_t eid, const Data::HeroState& st) {
                    if (!spatialGrid.IsCellVisible(st.x, st.y, obsTeam)) return;
                    const uint8_t tier = tierMap.count(eid) ? tierMap.at(eid) : 2u;
                    if (shouldSend(tier))
                        snapshots.push_back({ep, st, {}});
                });
            });

        if (!snapshots.empty()) {
            if (parallelMode) {
                // Phase A — parallel serialization
                // sync.wait() is an unbounded barrier; we poll try_wait() against the
                // tick deadline to detect over-budget serialization early.  We always
                // drain fully before entering Phase B — accessing task.buffer before
                // all jobs complete would be UB (workers still write into the vector).
                std::latch sync(static_cast<std::ptrdiff_t>(snapshots.size()));

                for (auto& task : snapshots) {
                    jobSystem.Execute([&manager, &task, tickID, &sync]() {
                        task.buffer = manager.SerializeSnapshotFor(task.ep, task.state, tickID);
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

                // Phase B — sequential commit + send
                for (auto& task : snapshots) {
                    manager.CommitAndSendSnapshot(task.ep, task.state, task.buffer);
                }
            } else {
                // Sequential baseline — main thread serializes and sends all snapshots.
                // Job System stays idle (workers sleep on condvar, ~0 CPU overhead).
                for (auto& task : snapshots) {
                    manager.SendSnapshot(task.ep, task.state, tickID);
                }
            }
        }

        // 5. Advance tick counter
        ++tickID;

        // 6. Full-loop EMA — covers all work steps 1-5 before sleep.
        const float fullTickMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - tickStart).count();
        loopEmaMs = kLoopAlpha * fullTickMs + (1.0f - kLoopAlpha) * loopEmaMs;

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

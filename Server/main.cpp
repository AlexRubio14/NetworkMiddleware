// NetworkMiddleware — P-4.5 Server
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

#include "../Core/NetworkManager.h"
#include "../Core/GameWorld.h"
#include "../Core/JobSystem.h"
#include "../Shared/Log/Logger.h"
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
        ? "NetServer — P-4.5 Parallel Mode (Split-Phase Job System)"
        : "NetServer — P-4.5 Sequential Mode (benchmark baseline)");

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

    NetworkManager manager(transport);
    GameWorld gameWorld;
    uint32_t tickID = 0;

    manager.SetClientConnectedCallback([&gameWorld](uint16_t id, const EndPoint& ep) {
        Logger::Log(LogLevel::Success, LogChannel::Core,
            std::format("CLIENT CONNECTED   NetworkID={}  ep={}", id, ep.ToString()));
        gameWorld.AddHero(id);
    });

    manager.SetClientDisconnectedCallback([&gameWorld](uint16_t id, const EndPoint& ep) {
        Logger::Log(LogLevel::Warning, LogChannel::Core,
            std::format("CLIENT DISCONNECTED  NetworkID={}  ep={}", id, ep.ToString()));
        gameWorld.RemoveHero(id);
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

    // ── 100 Hz game loop ──────────────────────────────────────────────────────
    // Budget: 10ms per tick.
    //
    // Each tick:
    //   1. manager.Update()           — drain UDP, handshakes, ACKs, buffer inputs
    //   2. ForEachEstablished         — apply buffered inputs to GameWorld
    //   3. gameWorld.Tick(dt)         — advance simulation
    //   4a. Collect snapshot tasks    — gather (ep, HeroState) pairs
    //   4b. Phase A: dispatch to pool — parallel SerializeSnapshotFor()
    //   4c. Phase B: commit + send    — sequential CommitAndSendSnapshot()
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

        // 2. Apply buffered inputs
        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& /*ep*/, const InputPayload* input) {
                if (input)
                    gameWorld.ApplyInput(id, *input, kFixedDt);
            });

        // 3. Advance simulation
        gameWorld.Tick(kFixedDt);

        // 4. Send snapshots — Split-Phase (P-4.4)
        //
        // Phase A: each job serializes one client's snapshot into a local buffer.
        //   Workers access RemoteClient read-only (baseline + acked seq).
        //   Main thread is blocked at sync.wait() — no concurrent map mutations.
        //
        // Phase B: main thread records history and sends buffers over the socket.
        //   Sequential: SFML socket + RemoteClient writes are single-threaded.

        std::vector<SnapshotTask> snapshots;
        snapshots.reserve(manager.GetEstablishedCount());

        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& ep, const InputPayload* /*input*/) {
                const auto* state = gameWorld.GetHeroState(id);
                if (state)
                    snapshots.push_back({ep, *state, {}});
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

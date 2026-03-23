// NetworkMiddleware — P-3.7 Server
//
// Authoritative dedicated server with a 100 Hz game loop (Minimal Game Loop).
// Integrates the first real game state: Input → GameWorld → Snapshot pipeline.
//
// Usage:
//   ./NetServer              (listens on UDP :7777)
//   SERVER_PORT=9999 ./NetServer
//
// Profiler output (every 5s via Logger):
//   [PROFILER] Clients: 10 | Avg Tick: 1.2ms | Out: 14kbps | Retries: 2 | Delta Efficiency: 74%

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <thread>

#include "../Core/NetworkManager.h"
#include "../Core/GameWorld.h"
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

int main() {
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    Logger::Start();
    Logger::Banner("NetServer — P-4.3 Stress Test");

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

    // ── NetworkManager ────────────────────────────────────────────────────────

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

    // Input packets are now intercepted by NetworkManager and buffered as pendingInput.
    // The game loop reads them via ForEachEstablished(). No data callback needed.

    // ── 100 Hz game loop ──────────────────────────────────────────────────────
    // Budget: 10ms per tick.
    //
    // Each tick:
    //   1. manager.Update()         — drain UDP, process handshakes/ACKs, buffer inputs
    //   2. ForEachEstablished (1st) — apply buffered inputs to GameWorld, clear inputs
    //   3. gameWorld.Tick(dt)       — advance simulation (placeholder for Fase 5)
    //   4. ForEachEstablished (2nd) — send delta snapshots to each client
    //   5. ++tickID                 — monotone tick counter for lag compensation
    //   6. sleep_until(nextTick)    — yield remaining budget
    //
    // Over-budget clamp: if the tick runs long, reset nextTick to avoid
    // a catch-up spin that would starve the OS scheduler.

    constexpr auto  kTickInterval = std::chrono::microseconds(10'000);  // 100 Hz
    constexpr float kFixedDt      = 0.01f;                              // seconds

    auto nextTick = std::chrono::steady_clock::now();

    Logger::Log(LogLevel::Info, LogChannel::Core,
        "Game loop starting at 100 Hz (10ms tick budget). Ctrl+C to stop.");

    while (g_running.load(std::memory_order_relaxed)) {
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

        // 4. Send snapshots
        manager.ForEachEstablished(
            [&](uint16_t id, const EndPoint& ep, const InputPayload* /*input*/) {
                const auto* state = gameWorld.GetHeroState(id);
                if (state)
                    manager.SendSnapshot(ep, *state, tickID);
            });

        // 5. Advance tick counter
        ++tickID;

        nextTick += kTickInterval;

        // If the tick ran over budget, reset to avoid catch-up spin.
        const auto now = std::chrono::steady_clock::now();
        if (nextTick < now) {
            Logger::Log(LogLevel::Warning, LogChannel::Core,
                std::format("Tick over budget by {:.2f}ms — resetting schedule",
                    std::chrono::duration<float, std::milli>(now - nextTick).count()));
            nextTick = now;
        }

        std::this_thread::sleep_until(nextTick);
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────

    Logger::Log(LogLevel::Info, LogChannel::Core,
        std::format("Shutdown. Connected clients at exit: {}",
            manager.GetEstablishedCount()));

    transport->Close();
    Logger::Sync();
    Logger::Stop();
    return 0;
}

// NetworkMiddleware — P-4.3 Server
//
// Production-ready dedicated server with a 100 Hz game loop.
// Replaces the Phase 3 visual demo (kept in git history / IR-3.x).
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
#include <thread>

#include "../Core/NetworkManager.h"
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

    const char* portEnv = std::getenv("SERVER_PORT");
    const uint16_t port = portEnv
        ? static_cast<uint16_t>(std::stoi(portEnv))
        : 7777;

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

    manager.SetClientConnectedCallback([](uint16_t id, const EndPoint& ep) {
        Logger::Log(LogLevel::Success, LogChannel::Core,
            std::format("CLIENT CONNECTED   NetworkID={}  ep={}", id, ep.ToString()));
    });

    manager.SetClientDisconnectedCallback([](uint16_t id, const EndPoint& ep) {
        Logger::Log(LogLevel::Warning, LogChannel::Core,
            std::format("CLIENT DISCONNECTED  NetworkID={}  ep={}", id, ep.ToString()));
    });

    manager.SetDataCallback([](const PacketHeader& h, BitReader&, const EndPoint& ep) {
        Logger::Log(LogLevel::Debug, LogChannel::Core,
            std::format("INPUT seq={}  from={}", h.sequence, ep.ToString()));
    });

    // ── 100 Hz game loop ──────────────────────────────────────────────────────
    // Budget: 10ms per tick. NetworkManager::Update() drains the full UDP buffer
    // in a while loop (P-4.3 fix), so all accumulated packets are processed
    // before sleep_until yields the thread.

    constexpr auto kTickInterval = std::chrono::microseconds(10'000);  // 100 Hz
    auto nextTick = std::chrono::steady_clock::now();

    Logger::Log(LogLevel::Info, LogChannel::Core,
        "Game loop starting at 100 Hz (10ms tick budget). Ctrl+C to stop.");

    while (g_running.load(std::memory_order_relaxed)) {
        manager.Update();

        nextTick += kTickInterval;
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

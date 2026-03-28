// HeadlessBot — P-4.1/4.2 Stress-test client.
//
// Connects to the NetServer, completes the full handshake, then sends
// InputPayload packets at ~60 Hz to generate realistic network load.
//
// Configuration (env vars):
//   SERVER_HOST  — Dotted-decimal IPv4 of the server  (default: 127.0.0.1)
//   SERVER_PORT  — UDP port of the server             (default: 7777)
//
// Docker Compose usage:
//   docker-compose up --scale bot=10
//
// Note: docker-compose.yml uses network_mode: host (Linux).
// On Docker Desktop for Windows the flag is silently ignored and bridge
// networking is used instead — change SERVER_HOST to the server container IP.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "../Core/BotClient.h"
#include "../Shared/Log/Logger.h"
#include "../Shared/NetworkAddress.h"
#include "../Shared/TransportType.h"
#include "../Transport/TransportFactory.h"

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Transport;

// Parse "A.B.C.D" into a uint32_t in network byte order (big-endian).
// sf::IpAddress(uint32_t) and sf::IpAddress::toInteger() both use network
// byte order, so EndPoints used with SFMLTransport must match.
// Note: test-only EndPoints (MockTransport) are opaque keys — unaffected.
//
// Validates: exactly 4 octets, each in [0, 255]. Returns 0 on error (caller
// must treat 0.0.0.0 as invalid for a server address).
static bool ParseIpv4(const std::string& ip, uint32_t& out) {
    uint32_t result = 0;
    int shift = 24;  // MSB first: octet A in bits 24-31 (network byte order)
    int octetCount = 0;
    std::istringstream ss(ip);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (octetCount >= 4)
            return false;  // more than 4 octets
        int octet;
        try {
            octet = std::stoi(token);
        } catch (...) {
            return false;  // non-numeric
        }
        if (octet < 0 || octet > 255)
            return false;  // out of range
        result |= (static_cast<uint32_t>(octet) << shift);
        shift -= 8;
        ++octetCount;
    }
    if (octetCount != 4)
        return false;  // fewer than 4 octets
    out = result;
    return true;
}

int main() {
    Logger::Start();

    const char* hostEnv = std::getenv("SERVER_HOST");
    const char* portEnv = std::getenv("SERVER_PORT");
    const std::string serverHost = hostEnv ? hostEnv : "127.0.0.1";

    uint16_t serverPort = 7777;
    if (portEnv) {
        try {
            const int parsed = std::stoi(portEnv);
            if (parsed < 0 || parsed > 65535) {
                Logger::Log(LogLevel::Error, LogChannel::Core,
                    std::format("SERVER_PORT out of range [0,65535]: {} — aborting", portEnv));
                Logger::Stop();
                return 1;
            }
            serverPort = static_cast<uint16_t>(parsed);
        } catch (const std::exception& e) {
            Logger::Log(LogLevel::Error, LogChannel::Core,
                std::format("SERVER_PORT is not a valid integer '{}': {} — aborting", portEnv, e.what()));
            Logger::Stop();
            return 1;
        }
    }

    uint32_t serverIp = 0;
    if (!ParseIpv4(serverHost, serverIp)) {
        Logger::Log(LogLevel::Error, LogChannel::Core,
            std::format("SERVER_HOST '{}' is not a valid dotted-decimal IPv4 — aborting", serverHost));
        Logger::Stop();
        return 1;
    }

    Logger::Banner("HeadlessBot — P-4.1/4.2");
    Logger::Log(LogLevel::Info, LogChannel::General,
        std::format("Target server: {}:{}", serverHost, serverPort));

    auto transport = TransportFactory::Create(TransportType::SFML);
    if (!transport->Initialize(0)) {  // port 0 = OS assigns an ephemeral port
        Logger::Log(LogLevel::Error, LogChannel::Transport,
            "Failed to bind UDP socket — aborting");
        Logger::Stop();
        return 1;
    }

    const EndPoint serverEp{serverIp, serverPort};
    BotClient bot(transport, serverEp);
    bot.Connect();

    // Wait up to 5 seconds for the handshake to complete.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bot.GetState() == BotClient::State::Connecting ||
           bot.GetState() == BotClient::State::Challenging) {
        bot.Update();
        if (std::chrono::steady_clock::now() > deadline) {
            Logger::Log(LogLevel::Error, LogChannel::Core,
                "Handshake timeout — is the server running?");
            Logger::Stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (bot.GetState() != BotClient::State::Established) {
        Logger::Log(LogLevel::Error, LogChannel::Core,
            "Connection denied or failed");
        Logger::Stop();
        return 1;
    }

    Logger::Log(LogLevel::Success, LogChannel::Core,
        std::format("Established — NetworkID={}  token={:#018x}",
            bot.GetNetworkID(), bot.GetReconnectionToken()));

    // ── Roam zone config (FOW benchmark support) ─────────────────────────────
    // BOT_ROAM_RADIUS > 0: bot dead-reckons its position from the inputs it
    // sends (server moves heroes at kMoveSpeed=100 u/s). When the estimated
    // position exceeds zone_center ± roam_radius, the bot steers back toward
    // the zone center instead of picking a random direction.  This keeps bots
    // anchored to their spawn zone so the FOW benchmark can isolate the
    // filtering effect (two groups far apart stay far apart).
    //
    //   BOT_ROAM_RADIUS    — max wander distance from zone center (0 = free, default)
    //   BOT_ZONE_CENTER_X  — zone center X (default: 0)
    //   BOT_ZONE_CENTER_Y  — zone center Y (default: 0)
    const char* roamRadiusEnv = std::getenv("BOT_ROAM_RADIUS");
    const char* zoneCxEnv     = std::getenv("BOT_ZONE_CENTER_X");
    const char* zoneCyEnv     = std::getenv("BOT_ZONE_CENTER_Y");

    const float roamRadius = roamRadiusEnv ? std::stof(roamRadiusEnv) : 0.0f;
    const float zoneCx     = zoneCxEnv     ? std::stof(zoneCxEnv)     : 0.0f;
    const float zoneCy     = zoneCyEnv     ? std::stof(zoneCyEnv)     : 0.0f;

    // Dead-reckoned position — starts at zone center (approximate; the server
    // assigns the actual spawn, but the bot can't know it until it receives a
    // snapshot).  Error stays bounded because we correct toward zone center
    // whenever the estimate drifts beyond roamRadius.
    float estX = zoneCx;
    float estY = zoneCy;

    if (roamRadius > 0.0f) {
        Logger::Log(LogLevel::Info, LogChannel::General,
            std::format("Roam zone: center=({:.0f},{:.0f})  radius={:.0f}u",
                zoneCx, zoneCy, roamRadius));
    }

    // ── 60 Hz input loop — Chaos mode (P-4.3) ────────────────────────────────
    // Direction changes every 0.5s to maximize delta compression stress:
    // each direction flip produces a large delta vs the previous state,
    // exercising the snapshot buffer and forcing frequent full-field updates.
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> dir(-1.0f, 1.0f);
    std::uniform_int_distribution<int>    btn(0, 0xFF);

    constexpr auto  kTickInterval  = std::chrono::microseconds(16'667);  // ~60 Hz
    constexpr auto  kChaosInterval = std::chrono::milliseconds(500);     // flip every 0.5s
    constexpr float kMoveSpeed     = 100.0f;                             // units/s (must match server)
    constexpr float kDt            = 1.0f / 60.0f;                      // seconds per tick
    constexpr float kMapBound      = 500.0f;

    float chaosDirX = dir(rng);
    float chaosDirY = dir(rng);

    auto nextTick            = std::chrono::steady_clock::now();
    auto nextDirectionChange = nextTick + kChaosInterval;

    while (bot.GetState() == BotClient::State::Established) {
        const auto now = std::chrono::steady_clock::now();

        // ── Dead reckoning — update estimated position ────────────────────────
        // Normalise the direction vector the same way the server does before
        // multiplying by kMoveSpeed (zero-vector → no movement).
        const float len = std::sqrt(chaosDirX * chaosDirX + chaosDirY * chaosDirY);
        if (len > 0.0f) {
            estX += (chaosDirX / len) * kMoveSpeed * kDt;
            estY += (chaosDirY / len) * kMoveSpeed * kDt;
            estX = std::clamp(estX, -kMapBound, kMapBound);
            estY = std::clamp(estY, -kMapBound, kMapBound);
        }

        // ── Chaos: pick a new direction every 0.5s ────────────────────────────
        if (now >= nextDirectionChange) {
            if (roamRadius > 0.0f) {
                // If outside the roam radius, steer back toward zone center.
                const float dx   = estX - zoneCx;
                const float dy   = estY - zoneCy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > roamRadius) {
                    chaosDirX = -dx / dist;
                    chaosDirY = -dy / dist;
                } else {
                    chaosDirX = dir(rng);
                    chaosDirY = dir(rng);
                }
            } else {
                chaosDirX = dir(rng);
                chaosDirY = dir(rng);
            }
            nextDirectionChange = now + kChaosInterval;
        }

        bot.Update();
        bot.SendInput(chaosDirX, chaosDirY, static_cast<uint8_t>(btn(rng)));

        nextTick += kTickInterval;
        // If the process was descheduled and nextTick fell behind wall-clock, reset
        // it to avoid a burst of back-to-back sends that skews the traffic profile.
        if (nextTick < now) nextTick = now;
        std::this_thread::sleep_until(nextTick);
    }

    bot.Disconnect();
    Logger::Log(LogLevel::Info, LogChannel::General, "HeadlessBot disconnected cleanly");
    Logger::Stop();
    return 0;
}

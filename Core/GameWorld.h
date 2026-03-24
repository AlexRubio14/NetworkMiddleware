#pragma once
#include "../Shared/Data/HeroState.h"
#include "../Shared/Network/InputPackets.h"
#include "../Shared/Gameplay/ViegoEntity.h"
#include <functional>
#include <memory>
#include <unordered_map>

namespace NetworkMiddleware::Core {

    // Authoritative simulation container for P-3.7 Minimal Game Loop.
    //
    // GameWorld owns the server-side hero entities. Each tick the server:
    //   1. Calls ApplyInput() for each client that sent an Input packet.
    //   2. Calls Tick() to advance physics / timers (stub for P-3.7).
    //   3. Reads GetHeroState() and sends delta Snapshots.
    //
    // Anti-cheat model: client sends NORMALIZED direction (-1..1). Server
    // computes the actual displacement using kMoveSpeed × dt, then clamps
    // the resulting position to ±kMapBound. There is no position received
    // from the client — relay / teleport attacks are impossible.
    class GameWorld {
    public:
        static constexpr float kMoveSpeed      = 100.0f;  // units per second
        static constexpr float kMapBound        = 500.0f;  // ±X and ±Y limit
        static constexpr float kSpeedTolerance  = 5.0f;   // reserved for future anti-cheat checks

        // Spawn a ViegoEntity at origin for the given networkID.
        // Silently ignores duplicate IDs (idempotent — safe to call from
        // OnClientConnected even if the client reconnects without full cleanup).
        void AddHero(uint32_t networkID);

        // Remove the hero with the given networkID (called on disconnect).
        void RemoveHero(uint32_t networkID);

        // Apply normalized input from a client for one simulation step.
        // dirX/dirY are clamped to [-1, 1] before computing displacement.
        // The resulting position is clamped to [-kMapBound, kMapBound].
        void ApplyInput(uint32_t networkID, const Shared::InputPayload& input, float dt);

        // Advance the simulation by dt seconds (placeholder for future physics).
        void Tick(float dt);

        // Returns the current HeroState for the given networkID, or nullptr
        // if the hero does not exist.
        const Shared::Data::HeroState* GetHeroState(uint32_t networkID) const;

        // Iterate all heroes, invoking callback(networkID, heroState) for each.
        // Used by the snapshot pipeline to send per-entity packets to each client.
        void ForEachHero(
            std::function<void(uint32_t, const Shared::Data::HeroState&)> callback) const;

    private:
        std::unordered_map<uint32_t, std::unique_ptr<Shared::Gameplay::ViegoEntity>> m_heroes;
    };

}  // namespace NetworkMiddleware::Core

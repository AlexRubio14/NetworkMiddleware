#include "GameWorld.h"
#include <algorithm>
#include <cmath>

namespace NetworkMiddleware::Core {

    void GameWorld::AddHero(uint32_t networkID) {
        if (m_heroes.contains(networkID))
            return;
        m_heroes.emplace(networkID,
            std::make_unique<Shared::Gameplay::ViegoEntity>(networkID));
        m_rewindHistory.emplace(networkID, std::array<RewindEntry, kRewindSlots>{});
    }

    void GameWorld::RemoveHero(uint32_t networkID) {
        m_heroes.erase(networkID);
        m_rewindHistory.erase(networkID);
    }

    void GameWorld::ApplyInput(uint32_t networkID,
                               const Shared::InputPayload& input,
                               float dt) {
        auto it = m_heroes.find(networkID);
        if (it == m_heroes.end())
            return;

        auto& hero = *it->second;

        // Clamp direction to [-1, 1] — anti-cheat: reject any over-normalised vector.
        const float dx = std::clamp(input.dirX, -1.0f, 1.0f);
        const float dy = std::clamp(input.dirY, -1.0f, 1.0f);

        // Authoritative displacement
        const float newX = std::clamp(hero.GetX() + dx * kMoveSpeed * dt,
                                      -kMapBound, kMapBound);
        const float newY = std::clamp(hero.GetY() + dy * kMoveSpeed * dt,
                                      -kMapBound, kMapBound);

        // Speed-hack gate: reject if displacement exceeds kSpeedTolerance per tick.
        // Catches misconfigured dt (dt >> kTickDt) and future movement-ability bugs.
        // Normal 100 Hz: max ≈ 1.41 units (diagonal) — well within the 5-unit limit.
        const float moveDx = newX - hero.GetX();
        const float moveDy = newY - hero.GetY();
        if (moveDx * moveDx + moveDy * moveDy > kSpeedTolerance * kSpeedTolerance)
            return;  // Silent reject

        hero.SetPosition(newX, newY);
    }

    void GameWorld::Tick(float /*dt*/) {
        // Placeholder for future physics / ability timers (Fase 5+).
    }

    const Shared::Data::HeroState* GameWorld::GetHeroState(uint32_t networkID) const {
        const auto it = m_heroes.find(networkID);
        if (it == m_heroes.end())
            return nullptr;
        return &it->second->GetState();
    }

    void GameWorld::ForEachHero(
        std::function<void(uint32_t, const Shared::Data::HeroState&)> callback) const
    {
        for (const auto& [id, hero] : m_heroes)
            callback(id, hero->GetState());
    }

    // P-5.3 ──────────────────────────────────────────────────────────────────

    void GameWorld::RecordTick(uint32_t tickID) {
        const size_t slot = tickID % kRewindSlots;
        for (const auto& [id, hero] : m_heroes) {
            auto it = m_rewindHistory.find(id);
            if (it == m_rewindHistory.end()) continue;
            auto& entry  = it->second[slot];
            entry.x      = hero->GetX();
            entry.y      = hero->GetY();
            entry.tickID = tickID;
            entry.valid  = true;
        }
    }

    const RewindEntry* GameWorld::GetStateAtTick(uint32_t entityID, uint32_t tickID) const {
        const auto it = m_rewindHistory.find(entityID);
        if (it == m_rewindHistory.end()) return nullptr;

        const auto& entry = it->second[tickID % kRewindSlots];
        if (!entry.valid || entry.tickID != tickID) return nullptr;
        return &entry;
    }

}  // namespace NetworkMiddleware::Core

#include "BaseHero.h"

namespace NetworkMiddleware::Shared::Gameplay {

    uint32_t BaseHero::GetDirtyMask()  const { return m_state.dirtyMask; }
    void     BaseHero::ClearDirtyMask()      { m_state.dirtyMask = 0; }
    uint32_t BaseHero::GetNetworkID()  const { return m_state.networkID; }
    float    BaseHero::GetX()          const { return m_state.x; }
    float    BaseHero::GetY()          const { return m_state.y; }
    uint32_t BaseHero::GetLevel()      const { return m_state.level; }
    float    BaseHero::GetExperience() const { return m_state.experience; }
    bool     BaseHero::IsDead()        const { return m_state.health <= 0; }
    uint16_t BaseHero::GetHeroTypeID() const { return m_state.heroTypeID; }
    const Data::HeroState& BaseHero::GetState() const { return m_state; }

    BaseHero::BaseHero(uint32_t netID, uint16_t typeID) {
        m_state.networkID = netID;
        m_state.heroTypeID = typeID;

        // Force a full synchronization on creation by setting all bits to 1
        m_state.dirtyMask = 0xFFFFFFFF;

        // Initialize default values
        m_state.level = 1;
        m_state.experience = 0.0f;
        m_state.health = 100.0f;
        m_state.maxHealth = 100.0f;
        m_state.mana = 50.0f;
        m_state.maxMana = 50.0f;
    }

    void BaseHero::SetPosition(float x, float y) {
        // Position is handled as a single dirty bit for X and Y combined
        if (m_state.x != x || m_state.y != y) {
            m_state.x = x;
            m_state.y = y;
            m_state.dirtyMask |= (1 << static_cast<uint32_t>(HeroDirtyBits::Position));
        }
    }

    void BaseHero::Heal(float amount) {
        float newHealth = m_state.health + amount;
        if (newHealth > m_state.maxHealth) {
            newHealth = m_state.maxHealth;
        }

        SetNetworkVar(m_state.health, newHealth, HeroDirtyBits::Health);
    }

    void BaseHero::TakeDamage(float amount) {
        float newHealth = m_state.health - amount;
        if (newHealth < 0) {
            newHealth = 0;
        }

        SetNetworkVar(m_state.health, newHealth, HeroDirtyBits::Health);
    }

    void BaseHero::RestoreMana(float amount) {
        float newMana = m_state.mana + amount;
        if (newMana > m_state.maxMana) {
            newMana = m_state.maxMana;
        }

        SetNetworkVar(m_state.mana, newMana, HeroDirtyBits::Mana);
    }

    void BaseHero::UseMana(float amount) {
        if (m_state.mana >= amount) {
            float newMana = m_state.mana - amount;
            SetNetworkVar(m_state.mana, newMana, HeroDirtyBits::Mana);
        }
    }

    // --- Inside BaseHero.cpp ---

    void BaseHero::AddExperience(float amount) {
        SetNetworkVar(m_state.experience, m_state.experience + amount, HeroDirtyBits::Experience);

        // Simple logic: if XP > 1000, level up (just for testing)
        if (m_state.experience >= 1000.0f) {
            LevelUp();
            m_state.experience = 0.0f;
        }
    }

    void BaseHero::LevelUp() {
        uint32_t newLevel = m_state.level + 1;
        SetNetworkVar(m_state.level, newLevel, HeroDirtyBits::Level);

        // Riot Style: Increasing Max HP on level up
        float newMaxHP = m_state.maxHealth + 90.0f;
        SetNetworkVar(m_state.maxHealth, newMaxHP, HeroDirtyBits::MaxHealth);
    }

    bool BaseHero::IsStunned() const {
        // Check if bit 1 (Stun) is active in our bit-field stateFlags
        // bit 0 = Dead, bit 1 = Stunned
        return (m_state.stateFlags & (1 << 1)) != 0;
    }
}
#pragma once
#include "IHero.h"
#include "HeroDirtyBits.h"
#include "../Data/HeroState.h"

namespace NetworkMiddleware::Shared::Gameplay {

    /**
     * Base class for all Heroes.
     * Handles the "Dirty Bit" logic and state management in a generic way.
     */
    class BaseHero : public IHero
    {
    protected:
        Data::HeroState m_state;

        /**
         * Template to update a network variable and mark it as dirty if it changed.
         * Must remain in the header for the compiler to generate code for different types.
         */
        template<typename T>
        void SetNetworkVar(T& member, const T& newValue, HeroDirtyBits bit) {
            if (member != newValue) {
                member = newValue;
                m_state.dirtyMask |= (1 << static_cast<uint32_t>(bit));
            }
        }

    public:
        BaseHero(uint32_t netID, uint16_t typeID);
        ~BaseHero() override = default;

        // --- INetworkable Implementation ---
        uint32_t GetDirtyMask() const override;
        void ClearDirtyMask() override;
        uint32_t GetNetworkID() const override;

        // These will be implemented by the Serializer later
        void Serialize(BitWriter& writer) const override = 0;
        void Unserialize(BitReader& reader) override = 0;

        // --- IHero Gameplay Implementation ---
        void SetPosition(float x, float y) override;
        void Heal(float amount) override;
        void TakeDamage(float amount) override;
        void UseMana(float amount) override;
        void RestoreMana(float amount);
        void AddExperience(float amount) override;
        void LevelUp() override;

        float GetX() const override;
        float GetY() const override;
        uint32_t GetLevel() const override;
        float GetExperience() const override;
        bool IsDead() const override;
        bool IsStunned() const override;

        // Identity
        std::string GetHeroName() const override = 0;
        uint16_t GetHeroTypeID() const override;

        // Direct state access for GameWorld (read-only, P-3.7)
        const Data::HeroState& GetState() const;
    };
}
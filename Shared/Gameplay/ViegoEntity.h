#pragma once
#include "BaseHero.h"

namespace NetworkMiddleware::Shared::Gameplay {

    /**
     * Specific implementation of the hero Viego.
     * Focuses on gameplay logic, delegating networking to HeroSerializer.
     */
    class ViegoEntity : public BaseHero {
    public:
        /**
         * Constructor for Viego.
         * @param netID Unique network identifier.
         */
        explicit ViegoEntity(uint32_t netID);
        ~ViegoEntity() override = default;

        // --- Identity ---
        std::string GetHeroName() const override { return "Viego"; }

        // --- INetworkable Implementation ---
        // We delegate the bit-packing to the specialized Serializer
        void Serialize(BitWriter& writer) const override;
        void Unserialize(BitReader& reader) override;

        // --- IHero Gameplay Implementation ---
        void CastAbility(int slot) override;
        void LevelUpAbility(int slot) override;
        float GetAbilityCooldown(int slot) const override;

    private:
        // Viego-specific logic constants
        static constexpr uint16_t VIEGO_TYPE_ID = 66;
    };
}
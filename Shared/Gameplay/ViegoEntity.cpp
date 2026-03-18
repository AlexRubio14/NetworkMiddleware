#include "ViegoEntity.h"
#include "../Data/Network/HeroSerializer.h"
#include <iostream> // For debug logging

namespace NetworkMiddleware::Shared::Gameplay {

    ViegoEntity::ViegoEntity(uint32_t netID) 
        : BaseHero(netID, VIEGO_TYPE_ID) 
    {
        // Viego starts with specific base stats
        m_state.health = 630.0f;
        m_state.maxHealth = 630.0f;
        m_state.mana = 0.0f; // Viego uses no mana in LoL, but we have the field
        m_state.maxMana = 0.0f;
    }

    void ViegoEntity::Serialize(BitWriter& writer) const {
        // High-level call to our optimized static serializer
        Network::HeroSerializer::Serialize(m_state, writer);
    }

    void ViegoEntity::Unserialize(BitReader& reader) {
        // High-level call to restore state from bits
        Network::HeroSerializer::Deserialize(m_state, reader);
    }

    void ViegoEntity::CastAbility(int slot) {
        // Here you would implement the logic for:
        // Slot 1: Blade of the Ruined King (Q)
        // Slot 2: Spectral Maw (W)
        // Slot 3: Harrowed Path (E)
        // Slot 4: Heartbreaker (R)
        
        std::cout << "Viego casting ability in slot: " << slot << std::endl;
        
        // Example: Casting Q might cost nothing but trigger a cooldown or animation state
        // m_state.stateFlags |= (1 << 2); // Set 'isCasting' bit
    }

    void ViegoEntity::LevelUpAbility(int slot) {
        // Logic to increase ability damage/scaling
    }

    float ViegoEntity::GetAbilityCooldown(int slot) const {
        // Return CD based on level and items
        return 0.0f; 
    }
}

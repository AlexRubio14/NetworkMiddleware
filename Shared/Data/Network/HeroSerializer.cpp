#include "HeroSerializer.h"
#include "NetworkOptimizer.h"
#include "../Gameplay/HeroDirtyBits.h"

namespace NetworkMiddleware::Shared::Network {

    void HeroSerializer::Serialize(const Data::HeroState& state, BitWriter& writer) {
        // 1. Write the Identity (Always needed so the client knows which hero this is)
        writer.WriteBits(state.networkID, 32);
        
        // 2. Write the Dirty Mask
        writer.WriteBits(state.dirtyMask, 32);

        // 3. Selective Serialization based on the mask
        
        // Position (Quantized 14 bits x 2)
        if (state.dirtyMask & (1 << (uint32_t)HeroDirtyBits::Position)) {
            uint32_t qX = NetworkOptimizer::QuantizeFloat(state.x, MAP_MIN, MAP_MAX, POS_BITS);
            uint32_t qY = NetworkOptimizer::QuantizeFloat(state.y, MAP_MIN, MAP_MAX, POS_BITS);
            writer.WriteBits(qX, POS_BITS);
            writer.WriteBits(qY, POS_BITS);
        }

        // Health (VLE)
        if (state.dirtyMask & (1 << (uint32_t)HeroDirtyBits::Health)) {
            NetworkOptimizer::WriteVLE(writer, static_cast<uint32_t>(state.health));
        }

        // Max Health (VLE - rarely changes)
        if (state.dirtyMask & (1 << (uint32_t)HeroDirtyBits::MaxHealth)) {
            NetworkOptimizer::WriteVLE(writer, static_cast<uint32_t>(state.maxHealth));
        }

        // Mana (VLE)
        if (state.dirtyMask & (1 << (uint32_t)HeroDirtyBits::Mana)) {
            NetworkOptimizer::WriteVLE(writer, static_cast<uint32_t>(state.mana));
        }

        // Level (Fixed 5 bits is enough for lvl 1-31)
        if (state.dirtyMask & (1 << (uint32_t)HeroDirtyBits::Level)) {
            writer.WriteBits(state.level, 5);
        }

        // State Flags (8 bits)
        if (state.dirtyMask & (1 << (uint32_t)HeroDirtyBits::StateFlags)) {
            writer.WriteBits(state.stateFlags, 8);
        }
    }

    void HeroSerializer::Deserialize(Data::HeroState& outState, BitReader& reader) {
        // 1. Read Identity
        outState.networkID = reader.ReadBits(32);

        // 2. Read the Mask to know what's coming
        uint32_t mask = reader.ReadBits(32);

        // 3. Decompress only the fields present in the packet
        
        if (mask & (1 << (uint32_t)HeroDirtyBits::Position)) {
            uint32_t qX = reader.ReadBits(POS_BITS);
            uint32_t qY = reader.ReadBits(POS_BITS);
            outState.x = NetworkOptimizer::DequantizeFloat(qX, MAP_MIN, MAP_MAX, POS_BITS);
            outState.y = NetworkOptimizer::DequantizeFloat(qY, MAP_MIN, MAP_MAX, POS_BITS);
        }

        if (mask & (1 << (uint32_t)HeroDirtyBits::Health)) {
            outState.health = static_cast<float>(NetworkOptimizer::ReadVLE(reader));
        }

        if (mask & (1 << (uint32_t)HeroDirtyBits::MaxHealth)) {
            outState.maxHealth = static_cast<float>(NetworkOptimizer::ReadVLE(reader));
        }

        if (mask & (1 << (uint32_t)HeroDirtyBits::Mana)) {
            outState.mana = static_cast<float>(NetworkOptimizer::ReadVLE(reader));
        }

        if (mask & (1 << (uint32_t)HeroDirtyBits::Level)) {
            outState.level = reader.ReadBits(5);
        }

        if (mask & (1 << (uint32_t)HeroDirtyBits::StateFlags)) {
            outState.stateFlags = static_cast<uint8_t>(reader.ReadBits(8));
        }
    }
}
#include "HeroSerializer.h"
#include "NetworkOptimizer.h"
#include "../Gameplay/HeroDirtyBits.h"
#include <cstdint>

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

    void HeroSerializer::SerializeDelta(const Data::HeroState& current,
                                        const Data::HeroState& baseline,
                                        BitWriter& writer) {
        // Identity (always present so the receiver knows which hero this is)
        writer.WriteBits(current.networkID, 32);

        // Position — compare in quantized space to avoid precision drift
        const uint32_t qXc = NetworkOptimizer::QuantizeFloat(current.x,  MAP_MIN, MAP_MAX, POS_BITS);
        const uint32_t qYc = NetworkOptimizer::QuantizeFloat(current.y,  MAP_MIN, MAP_MAX, POS_BITS);
        const uint32_t qXb = NetworkOptimizer::QuantizeFloat(baseline.x, MAP_MIN, MAP_MAX, POS_BITS);
        const uint32_t qYb = NetworkOptimizer::QuantizeFloat(baseline.y, MAP_MIN, MAP_MAX, POS_BITS);
        const int32_t  dQx = static_cast<int32_t>(qXc) - static_cast<int32_t>(qXb);
        const int32_t  dQy = static_cast<int32_t>(qYc) - static_cast<int32_t>(qYb);
        const bool posChanged = (dQx != 0 || dQy != 0);
        writer.WriteBits(posChanged ? 1u : 0u, 1);
        if (posChanged) {
            NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dQx));
            NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dQy));
        }

        // Health
        const int32_t dHealth = static_cast<int32_t>(current.health) - static_cast<int32_t>(baseline.health);
        writer.WriteBits(dHealth != 0 ? 1u : 0u, 1);
        if (dHealth != 0) NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dHealth));

        // MaxHealth
        const int32_t dMaxHealth = static_cast<int32_t>(current.maxHealth) - static_cast<int32_t>(baseline.maxHealth);
        writer.WriteBits(dMaxHealth != 0 ? 1u : 0u, 1);
        if (dMaxHealth != 0) NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dMaxHealth));

        // Mana
        const int32_t dMana = static_cast<int32_t>(current.mana) - static_cast<int32_t>(baseline.mana);
        writer.WriteBits(dMana != 0 ? 1u : 0u, 1);
        if (dMana != 0) NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dMana));

        // Level
        const int32_t dLevel = static_cast<int32_t>(current.level) - static_cast<int32_t>(baseline.level);
        writer.WriteBits(dLevel != 0 ? 1u : 0u, 1);
        if (dLevel != 0) NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dLevel));

        // StateFlags — bitmask, write raw if changed
        const bool flagsChanged = (current.stateFlags != baseline.stateFlags);
        writer.WriteBits(flagsChanged ? 1u : 0u, 1);
        if (flagsChanged) writer.WriteBits(current.stateFlags, 8);
    }

    void HeroSerializer::DeserializeDelta(Data::HeroState& outState,
                                          const Data::HeroState& baseline,
                                          BitReader& reader) {
        outState = baseline; // start from baseline, apply deltas on top

        outState.networkID = reader.ReadBits(32);

        // Position
        if (reader.ReadBits(1)) {
            const uint32_t qXb   = NetworkOptimizer::QuantizeFloat(baseline.x, MAP_MIN, MAP_MAX, POS_BITS);
            const uint32_t qYb   = NetworkOptimizer::QuantizeFloat(baseline.y, MAP_MIN, MAP_MAX, POS_BITS);
            const int32_t  dQx   = NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ReadVLE(reader));
            const int32_t  dQy   = NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ReadVLE(reader));
            outState.x = NetworkOptimizer::DequantizeFloat(static_cast<uint32_t>(static_cast<int32_t>(qXb) + dQx), MAP_MIN, MAP_MAX, POS_BITS);
            outState.y = NetworkOptimizer::DequantizeFloat(static_cast<uint32_t>(static_cast<int32_t>(qYb) + dQy), MAP_MIN, MAP_MAX, POS_BITS);
        }

        // Health
        if (reader.ReadBits(1))
            outState.health = baseline.health + static_cast<float>(NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ReadVLE(reader)));

        // MaxHealth
        if (reader.ReadBits(1))
            outState.maxHealth = baseline.maxHealth + static_cast<float>(NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ReadVLE(reader)));

        // Mana
        if (reader.ReadBits(1))
            outState.mana = baseline.mana + static_cast<float>(NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ReadVLE(reader)));

        // Level
        if (reader.ReadBits(1))
            outState.level = static_cast<uint32_t>(static_cast<int32_t>(baseline.level) + NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ReadVLE(reader)));

        // StateFlags
        if (reader.ReadBits(1))
            outState.stateFlags = static_cast<uint8_t>(reader.ReadBits(8));
    }
}
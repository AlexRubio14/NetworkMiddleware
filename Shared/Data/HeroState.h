#pragma once
#include <cstdint>

namespace NetworkMiddleware::Shared::Data {

    /**
     * HeroState: A Plain Old Data (POD) structure representing the
     * current snapshot of a Hero's attributes.
     * * This structure is designed for memory alignment and cache efficiency.
     * The Serializer will read this data to perform bit-packing.
     */
    struct HeroState {
        // --- Network Synchronization ---
        // 32-bit mask to track which fields have changed (Dirty Bits)
        uint32_t dirtyMask = 0;

        uint32_t networkID = 0;

        uint16_t heroTypeID = 0;

        // --- Transformation ---
        // Stored as floats for high-precision server-side calculations.
        // Will be quantized to 14 bits during serialization.
        float x = 0.0f;
        float y = 0.0f;

        // --- Vitals ---
        // 14-bit precision targets on wire.
        float health = 0.0f;
        float maxHealth = 0.0f;
        float mana = 0.0f;
        float maxMana = 0.0f;

        // --- Progression ---
        uint32_t level = 1;
        float experience = 0.0f;

        // --- State Flags ---
        // Bit-field: 0x01 = Dead, 0x02 = Stunned, 0x04 = Rooted, etc.
        uint8_t stateFlags = 0;
    };
}
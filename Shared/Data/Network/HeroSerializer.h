#pragma once
#include "../Data/HeroState.h"
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"

namespace NetworkMiddleware::Shared::Network {

    /**
     * Static class responsible for packing and unpacking HeroState data.
     * It uses the NetworkOptimizer to achieve maximum bit efficiency.
     */
    class HeroSerializer {
    public:
        // Main entry point for sending a hero's delta update
        static void Serialize(const Data::HeroState& state, BitWriter& writer);

        // Main entry point for receiving and updating a hero's state
        static void Deserialize(Data::HeroState& outState, BitReader& reader);

    private:
        // World boundaries for quantization (consistent between Server & Client)
        static constexpr float MAP_MIN = -500.0f;
        static constexpr float MAP_MAX = 500.0f;
        static constexpr int POS_BITS = 14; // 6cm precision
    };
}
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
        // Full-state serialization (dirty-mask based, ~145 bits for a full sync)
        static void Serialize(const Data::HeroState& state, BitWriter& writer);
        static void Deserialize(Data::HeroState& outState, BitReader& reader);

        // Delta serialization (P-3.5): encodes only fields that changed vs baseline.
        // Wire format: networkID(32) + 6 inline dirty bits, each followed by
        // ZigZag-VLE delta if set. Typically 38–90 bits vs 145 for full sync.
        static void SerializeDelta(const Data::HeroState& current,
                                   const Data::HeroState& baseline,
                                   BitWriter& writer);

        // Applies the delta from reader on top of baseline and writes into outState.
        static void DeserializeDelta(Data::HeroState& outState,
                                     const Data::HeroState& baseline,
                                     BitReader& reader);

    private:
        static constexpr float MAP_MIN  = -500.0f;
        static constexpr float MAP_MAX  =  500.0f;
        static constexpr int   POS_BITS = 16; // 1.53cm precision over 1000m
    };
}
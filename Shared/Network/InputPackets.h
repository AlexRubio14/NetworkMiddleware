#pragma once
#include <cstdint>
#include "../Serialization/BitWriter.h"
#include "../Serialization/BitReader.h"
#include "../Data/Network/NetworkOptimizer.h"

// Wire format for client input commands (P-4.1/4.2).
// Sent at ~60 Hz by the HeadlessBot (and in future by the Unreal client).
// Total: 24 bits — small enough to fit in a single UDP packet with the header.

namespace NetworkMiddleware::Shared {

    // Bitmask values for InputPayload::buttons.
    enum InputButtons : uint8_t {
        kAbility1 = 0x01,
        kAbility2 = 0x02,
        kAbility3 = 0x04,
        kAbility4 = 0x08,  // Ultimate
        kAttack   = 0x10,
        // 0x20, 0x40, 0x80 reserved
    };

    struct InputPayload {
        float   dirX    = 0.0f;  // Movement direction X, normalized [-1.0, 1.0]
        float   dirY    = 0.0f;  // Movement direction Y, normalized [-1.0, 1.0]
        uint8_t buttons = 0;     // Action bitmask (see InputButtons above)

        static constexpr uint32_t kBitCount = 24;  // 8 + 8 + 8

        void Write(BitWriter& writer) const {
            using Opt = Network::NetworkOptimizer;
            const uint32_t qX = Opt::QuantizeFloat(dirX, -1.0f, 1.0f, 8);
            const uint32_t qY = Opt::QuantizeFloat(dirY, -1.0f, 1.0f, 8);
            writer.WriteBits(qX, 8);
            writer.WriteBits(qY, 8);
            writer.WriteBits(buttons, 8);
        }

        static InputPayload Read(BitReader& reader) {
            using Opt = Network::NetworkOptimizer;
            InputPayload p;
            p.dirX    = Opt::DequantizeFloat(reader.ReadBits(8), -1.0f, 1.0f, 8);
            p.dirY    = Opt::DequantizeFloat(reader.ReadBits(8), -1.0f, 1.0f, 8);
            p.buttons = static_cast<uint8_t>(reader.ReadBits(8));
            return p;
        }
    };

}  // namespace NetworkMiddleware::Shared

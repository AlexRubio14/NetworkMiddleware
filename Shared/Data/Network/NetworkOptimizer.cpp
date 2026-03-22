#include "NetworkOptimizer.h"
#include <algorithm>
#include <cmath>

namespace NetworkMiddleware::Shared::Network {

    uint32_t NetworkOptimizer::QuantizeFloat(float value, float min, float max, int bits) {
        // 1. Clamp value to ensure it's within range
        value = std::max(min, std::min(max, value));

        // 2. Calculate the precision scale based on number of bits (2^bits - 1)
        uint32_t maxQuantizedValue = (1u << bits) - 1;

        // 3. Normalize the value to [0, 1]
        float normalized = (value - min) / (max - min);

        // 4. Scale to the bit range and round
        return static_cast<uint32_t>(std::round(normalized * maxQuantizedValue));
    }

    float NetworkOptimizer::DequantizeFloat(uint32_t quantizedValue, float min, float max, int bits) {
        uint32_t maxQuantizedValue = (1u << bits) - 1;

        // 1. Convert back to [0, 1] range
        float normalized = static_cast<float>(quantizedValue) / static_cast<float>(maxQuantizedValue);

        // 2. Remap to original range [min, max]
        return min + (normalized * (max - min));
    }

    void NetworkOptimizer::WriteVLE(BitWriter& writer, uint32_t value) {
        // While there are more than 7 bits of data left...
        while (value >= 0x80) { // 0x80 is 128 (1000 0000)
            // Write the lower 7 bits + set the 8th bit to 1 (Continuation bit)
            writer.WriteBits((value & 0x7F) | 0x80, 8);
            value >>= 7; // Shift right by 7 bits to process the next chunk
        }
        // Write the last 7 bits (the 8th bit will be 0, signaling the end)
        writer.WriteBits(value & 0x7F, 8);
    }

    uint32_t NetworkOptimizer::ReadVLE(BitReader& reader) {
        uint32_t value = 0;
        uint32_t shift = 0;
        uint32_t b;

        do {
            b = reader.ReadBits(8);
            // Take the lower 7 bits and shift them to their position
            value |= (b & 0x7F) << shift;
            shift += 7;

            // Safety check: if we exceed 35 bits, something is wrong with the stream
            if (shift > 35) return 0;

        } while (b & 0x80); // Keep reading if the 8th bit is 1

        return value;
    }

    bool NetworkOptimizer::IsLittleEndian() {
        uint32_t test = 1;
        return *(uint8_t*)&test == 1;
    }

    uint32_t NetworkOptimizer::SwapEndian(uint32_t value) {
        return ((value >> 24) & 0xff) |
                   ((value << 8) & 0xff0000) |
                   ((value >> 8) & 0xff00) |
                   ((value << 24) & 0xff000000);
    }

    uint32_t NetworkOptimizer::ZigZagEncode(int32_t n) {
        // Portable: avoids implementation-defined signed right-shift.
        // n < 0 produces the all-ones mask (0xFFFFFFFF), n >= 0 produces 0.
        return (static_cast<uint32_t>(n) << 1) ^ (n < 0 ? ~0u : 0u);
    }

    int32_t NetworkOptimizer::ZigZagDecode(uint32_t n) {
        return static_cast<int32_t>((n >> 1) ^ (~(n & 1) + 1));
    }
}

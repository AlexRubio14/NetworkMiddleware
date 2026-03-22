#pragma once
#include <cstdint>
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"

namespace NetworkMiddleware::Shared::Network {
    /**
         * Static utility class for network data optimization.
         * Includes quantization for floats and Variable Length Encoding (VLE).
         */
    class NetworkOptimizer {
    public:

        // --- Quantization ---

        /**
         * Compresses a float into a fixed-bit integer within a specific range.
         * Formula: ScaledValue = (Value - Min) / (Max - Min) * (2^bits - 1)
         */
        static uint32_t QuantizeFloat(float value, float min, float max, int bits);

        /**
         * Restores a float from a quantized integer.
         */
        static float DequantizeFloat(uint32_t quantizedValue, float min, float max, int bits);


        /**
         * Writes a 32-bit integer using 7-bit chunks.
         * The 8th bit of each byte is a "continuation bit".
         * 0-127 -> 8 bits
         * 128-16383 -> 16 bits
         */
        static void WriteVLE(BitWriter& writer, uint32_t value);

        /**
         * Reads a VLE-encoded integer from the stream.
         */
        static uint32_t ReadVLE(BitReader& reader);

        static bool IsLittleEndian();

        /**
         * Swaps the byte order of a 32-bit integer.
         */
        static uint32_t SwapEndian(uint32_t value);

        // --- Zig-Zag Encoding ---

        /**
         * Maps signed integers to unsigned so that small absolute values
         * (positive or negative) produce small unsigned values → efficient VLE.
         * Formula: (n << 1) ^ (n >> 31)
         */
        static uint32_t ZigZagEncode(int32_t n);

        /**
         * Reverses ZigZagEncode.
         * Formula: (n >> 1) ^ -(n & 1)
         */
        static int32_t ZigZagDecode(uint32_t n);
    };
}
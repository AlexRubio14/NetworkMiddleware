#pragma once
#include <vector>
#include <cstdint>

 namespace NetworkMiddleware::Shared
 {
    class BitWriter
    {
    private:
        std::vector<uint8_t> m_buffer;
        size_t m_bitHead; //current Bit position

    public:
        BitWriter(size_t initialCapacity = 1500); // Default MTU size

        // Writes a specific number of bits of a value into the buffer
        void WriteBits(uint32_t value, uint32_t numBits);

        // Finalizes the buffer and returns it (padding with 0s if needed)
        std::vector<uint8_t> GetCompressedData() const;

        const std::vector<uint8_t>& GetRawBuffer() const;
    };
 }
#pragma once
#include <vector>
#include <cstdint>

namespace NetworkMiddleware::Shared
{
    class BitReader
    {
    private:
        const std::vector<uint8_t>& m_buffer;
        size_t m_bitHead;

    public:
        BitReader(const std::vector<uint8_t>& data);

        // Reads n bits from the current head position
        uint32_t ReadBits(uint32_t numBits);
    };
}
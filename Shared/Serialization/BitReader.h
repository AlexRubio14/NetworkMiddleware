#pragma once
#include <vector>
#include <cstdint>

namespace NetworkMiddleware::Shared {
    class BitReader {
    private:
        const std::vector<uint8_t> m_buffer; // Store a copy or use a reference
        size_t m_bitHead;
        size_t m_totalBits; // Store the limit

    public:
        // Update constructor to accept bit count
        BitReader(const std::vector<uint8_t>& data, size_t totalBits);

        uint32_t ReadBits(uint32_t numBits);
    };
}
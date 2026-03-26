#include "BitReader.h"
#include <algorithm>

namespace NetworkMiddleware::Shared
{
    BitReader::BitReader(const std::vector<uint8_t>& data, size_t totalBits)
    : m_buffer(data), m_bitHead(0), m_totalBits(totalBits) {};

    uint32_t BitReader::ReadBits(uint32_t numBits)
    {
        uint32_t value = 0;
        uint32_t bitsReadSoFar = 0;

        while (numBits > 0)
        {
            // Stop at m_totalBits to avoid consuming tail-padding zeros written by
            // BitWriter::GetCompressedData() when the payload is not byte-aligned.
            if (m_bitHead >= m_totalBits)
                return value;

            size_t byteIndex = m_bitHead >> 3;
            size_t bitOffset = m_bitHead & 7;

            uint32_t bitsAvailableInByte = 8 - (uint32_t)bitOffset;
            // Also limit bitsToRead so we never step past m_totalBits.
            const uint32_t bitsRemaining = static_cast<uint32_t>(m_totalBits - m_bitHead);
            uint32_t bitsToRead = std::min({numBits, bitsAvailableInByte, bitsRemaining});

            // Create mask and extract bits from current byte
            if (byteIndex >= m_buffer.size())
                return value;   // truncated packet — return what we have so far

            uint32_t mask = (1U << bitsToRead) - 1;
            uint32_t extractedBits = (m_buffer[byteIndex] >> bitOffset) & mask;

            // Place extracted bits into the resulting value at the correct position
            value |= (extractedBits << bitsReadSoFar);

            // Advance
            m_bitHead += bitsToRead;
            bitsReadSoFar += bitsToRead;
            numBits -= bitsToRead;
        }

        return value;
    }
}
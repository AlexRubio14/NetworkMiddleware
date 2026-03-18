#include "BitReader.h"

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
            size_t byteIndex = m_bitHead >> 3;
            size_t bitOffset = m_bitHead & 7;

            uint32_t bitsAvailableInByte = 8 - (uint32_t)bitOffset;
            uint32_t bitsToRead = (numBits < bitsAvailableInByte) ? numBits : bitsAvailableInByte;

            // Create mask and extract bits from current byte
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
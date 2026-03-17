#include "BitReader.h"

namespace NetworkMiddleware::Shared
{
    BitReader::BitReader(const std::vector<uint8_t>& data)
        : m_buffer(data), m_bitHead(0) {}

    uint32_t BitReader::ReadBits(uint32_t numBits)
    {
        uint32_t value = 0;

        for (uint32_t i = 0; i < numBits; ++i)
        {
            size_t byteIndex = m_bitHead / 8;
            size_t bitOffset = m_bitHead % 8;

            if (byteIndex < m_buffer.size())
            {
                // Extract the bit and set it in the result value
                if (m_buffer[byteIndex] & (1U << bitOffset))
                {
                    value |= (1U << i);
                }
            }

            m_bitHead++;
        }

        return value;
    }
}
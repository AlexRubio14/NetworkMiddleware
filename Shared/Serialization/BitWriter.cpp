#include "BitWriter.h"
#include <algorithm>

#include "Data/Network/NetworkOptimizer.h"

namespace NetworkMiddleware::Shared {

     BitWriter::BitWriter(size_t initialCapacity)
         : m_bitHead(0)
     {
         // reserve() prepares memory without increasing the vector's size
         m_buffer.reserve(initialCapacity);
         // Start with the first byte ready
         m_buffer.push_back(0);
     }

     void BitWriter::WriteBits(uint32_t value, uint32_t numBits)
     {
         if (numBits < 32)
             value &= (1U << numBits) - 1;

         while (numBits > 0)
         {
             size_t byteIndex = m_bitHead >> 3;   // Equivalent a / 8
             size_t bitOffset = m_bitHead & 7;    // Equivalent a % 8

             // Grow buffer
             if (byteIndex >= m_buffer.size())
                 m_buffer.push_back(0);

             // How many bits can we fit in the current byte?
             uint32_t bitsFreeInByte = 8 - (uint32_t)bitOffset;
             uint32_t bitsToWrite = (numBits < bitsFreeInByte) ? numBits : bitsFreeInByte;

             // Create a mask for the bits we are about to write
             uint32_t mask = (1U << bitsToWrite) - 1;
             uint32_t bitsToPutIn = value & mask;

             // "Pour" the bits into the byte at the correct offset
             m_buffer[byteIndex] |= (static_cast<uint8_t>(bitsToPutIn) << bitOffset);

             // Advance the state
             value >>= bitsToWrite;
             numBits -= bitsToWrite;
             m_bitHead += bitsToWrite;
         }
     }

     // Returns only the bytes that actually contain data
     std::vector<uint8_t> BitWriter::GetCompressedData() const
     {
         size_t bytesUsed = (m_bitHead + 7) / 8;
         return std::vector<uint8_t>(m_buffer.begin(), m_buffer.begin() + bytesUsed);
     }

     const std::vector<uint8_t>& BitWriter::GetRawBuffer() const
     {
         return m_buffer;
     }
 }

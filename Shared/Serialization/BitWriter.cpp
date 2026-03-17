#include "BitWriter.h"
#include <algorithm>

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
         // Safety mask to avoid overflow
         value &= (numBits == 32) ? 0xFFFFFFFF : (1U << numBits) - 1;

         for (uint32_t i = 0; i < numBits; ++i)
         {
             size_t byteIndex = m_bitHead / 8;
             size_t bitOffset = m_bitHead % 8;

             // Grow the buffer if we reach a new byte boundary
             if (byteIndex >= m_buffer.size())
             {
                 m_buffer.push_back(0);
             }

             // Set the bit if it's 1 in the input value
             if (value & (1U << i))
             {
                 m_buffer[byteIndex] |= (1U << bitOffset);
             }

             m_bitHead++;
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
#pragma once
// Shared/Serialization/CRC32.h — P-4.5 Packet Integrity
//
// Standard IEEE 802.3 CRC32 (Ethernet / ZIP / PNG polynomial).
// Lookup table generated at compile time; computation is ~1-2 cycles/byte.
//
// Convention used throughout this project:
//   initial value  = 0xFFFFFFFF
//   final XOR      = 0xFFFFFFFF   (standard)
//   empty input    → 0x00000000
//   "123456789"    → 0xCBF43926   (canonical IEEE test vector)
//
// Both sender and receiver call ComputeCRC32 on the same bytes, so any
// consistent algorithm (with or without final XOR) would work — but we follow
// the standard to allow cross-tool verification of packet captures.

#include <array>
#include <cstdint>
#include <vector>

namespace NetworkMiddleware::Shared {

namespace detail {

// Generates the 256-entry CRC32 lookup table at compile time.
// Polynomial 0xEDB88320 is the bit-reversed (LSB-first) form of 0x04C11DB7.
constexpr std::array<uint32_t, 256> MakeCRC32Table() noexcept {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}

inline constexpr auto kCRC32Table = MakeCRC32Table();

} // namespace detail

// Compute CRC32 over [data, data+length).
// Returns 0x00000000 for empty input (0xFFFFFFFF initial ^ 0xFFFFFFFF final).
inline uint32_t ComputeCRC32(const uint8_t* data, size_t length) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
        crc = detail::kCRC32Table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t ComputeCRC32(const std::vector<uint8_t>& data) noexcept {
    return ComputeCRC32(data.data(), data.size());
}

} // namespace NetworkMiddleware::Shared

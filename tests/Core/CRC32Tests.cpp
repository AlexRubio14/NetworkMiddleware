// P-4.5 Unit tests for Shared/Serialization/CRC32.h
//
// Pins the exact CRC32 algorithm used throughout the packet integrity layer so
// that any accidental change to the polynomial, initial value, or final XOR is
// immediately caught as a regression.

#include <gtest/gtest.h>
#include "Shared/Serialization/CRC32.h"

using namespace NetworkMiddleware::Shared;

// ─── CRC32 algorithm correctness ─────────────────────────────────────────────

// IEEE 802.3 canonical test vector: CRC32("123456789") == 0xCBF43926.
// This locks in: polynomial 0xEDB88320, initial 0xFFFFFFFF, final XOR 0xFFFFFFFF.
TEST(CRC32, KnownVector_IEEE802_3) {
    const std::vector<uint8_t> input = {
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39  // "123456789"
    };
    EXPECT_EQ(ComputeCRC32(input), 0xCBF43926u);
}

// Empty input: initial 0xFFFFFFFF XOR final 0xFFFFFFFF = 0x00000000.
TEST(CRC32, EmptyBuffer_ReturnsZero) {
    EXPECT_EQ(ComputeCRC32(nullptr, 0), 0u);
    EXPECT_EQ(ComputeCRC32(std::vector<uint8_t>{}), 0u);
}

// Flipping a single bit anywhere in the payload must change the CRC.
TEST(CRC32, SingleBitFlip_ChangesCRC) {
    std::vector<uint8_t> data(20, 0xAB);
    const uint32_t original = ComputeCRC32(data);

    data[7] ^= 0x01u;  // flip bit 0 of byte 7
    const uint32_t flipped = ComputeCRC32(data);

    EXPECT_NE(original, flipped);
}

// Same input always produces the same output (pure function, no side effects).
TEST(CRC32, Consistency_SameInputSameOutput) {
    const std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    EXPECT_EQ(ComputeCRC32(data), ComputeCRC32(data));
}

// The vector overload must produce the same result as the pointer overload.
TEST(CRC32, VectorOverload_MatchesPointerOverload) {
    const std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44, 0x55};
    EXPECT_EQ(ComputeCRC32(data), ComputeCRC32(data.data(), data.size()));
}

// CRC is sensitive to length: {0xAB} != {0xAB, 0x00}.
TEST(CRC32, LengthSensitive_DifferentLength_DifferentCRC) {
    const std::vector<uint8_t> a = {0xAB};
    const std::vector<uint8_t> b = {0xAB, 0x00};
    EXPECT_NE(ComputeCRC32(a), ComputeCRC32(b));
}

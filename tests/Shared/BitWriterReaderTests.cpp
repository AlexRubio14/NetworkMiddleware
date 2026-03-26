#include <gtest/gtest.h>
#include "Shared/Serialization/BitWriter.h"
#include "Shared/Serialization/BitReader.h"

using namespace NetworkMiddleware::Shared;

// ─── Bounds safety (truncated / malformed packet) ────────────────────────────
// A truncated packet must never trigger UB — BitReader returns partial data
// instead of accessing out-of-bounds memory.

TEST(BitWriterReader, ReadBeyondBuffer_ReturnsPartialNoUB) {
    // Buffer holds 8 bits (1 byte), but we ask for 16.
    // Must not crash, assert, or cause UB.
    const std::vector<uint8_t> buf = {0xFF};
    BitReader r(buf, 8);
    r.ReadBits(8);                    // consumes the only byte
    const uint32_t extra = r.ReadBits(8);  // reads past end
    EXPECT_EQ(extra, 0u);             // out-of-range bytes return 0
}

TEST(BitWriterReader, ReadFromEmptyBuffer_ReturnsZero) {
    const std::vector<uint8_t> empty;
    BitReader r(empty, 0);
    EXPECT_EQ(r.ReadBits(8), 0u);
}

TEST(BitWriterReader, ReadPartiallyTruncated_ReturnsAvailableBits) {
    // Buffer holds 1 byte (0xa5), ask for 16 bits in a single call.
    // BitReader reads the 8 available bits then hits the bounds guard.
    // Result: lower 8 bits = 0xa5, upper 8 bits = 0 (not accessed).
    const std::vector<uint8_t> buf = {0xa5};
    BitReader r(buf, 8);
    const uint32_t val = r.ReadBits(16);
    EXPECT_EQ(val, 0xa5u);
}

// ─── Single field round-trips ────────────────────────────────────────────────

TEST(BitWriterReader, WriteSingleOneBit) {
    BitWriter w;
    w.WriteBits(1, 1);
    auto data = w.GetCompressedData();
    BitReader r(data, 1);
    EXPECT_EQ(r.ReadBits(1), 1u);
}

TEST(BitWriterReader, WriteSingleZeroBit) {
    BitWriter w;
    w.WriteBits(0, 1);
    auto data = w.GetCompressedData();
    BitReader r(data, 1);
    EXPECT_EQ(r.ReadBits(1), 0u);
}

TEST(BitWriterReader, WriteFullByte) {
    BitWriter w;
    w.WriteBits(0xFF, 8);
    auto data = w.GetCompressedData();
    BitReader r(data, 8);
    EXPECT_EQ(r.ReadBits(8), 0xFFu);
}

TEST(BitWriterReader, Write16BitMax) {
    BitWriter w;
    w.WriteBits(0xFFFF, 16);
    auto data = w.GetCompressedData();
    BitReader r(data, 16);
    EXPECT_EQ(r.ReadBits(16), 0xFFFFu);
}

TEST(BitWriterReader, Write32Bits) {
    BitWriter w;
    w.WriteBits(0xDEADBEEF, 32);
    auto data = w.GetCompressedData();
    BitReader r(data, 32);
    EXPECT_EQ(r.ReadBits(32), 0xDEADBEEFu);
}

TEST(BitWriterReader, WriteZero32Bits) {
    BitWriter w;
    w.WriteBits(0, 32);
    auto data = w.GetCompressedData();
    BitReader r(data, 32);
    EXPECT_EQ(r.ReadBits(32), 0u);
}

// ─── Multi-field round-trips ─────────────────────────────────────────────────

TEST(BitWriterReader, WriteMultipleFieldsInSequence) {
    BitWriter w;
    w.WriteBits(42,   16);
    w.WriteBits(7,     4);
    w.WriteBits(255,   8);
    w.WriteBits(1,     1);
    auto data = w.GetCompressedData();

    BitReader r(data, 29);
    EXPECT_EQ(r.ReadBits(16),  42u);
    EXPECT_EQ(r.ReadBits(4),    7u);
    EXPECT_EQ(r.ReadBits(8),  255u);
    EXPECT_EQ(r.ReadBits(1),    1u);
}

TEST(BitWriterReader, WritePacketHeaderFields) {
    // Simulates writing the 104-bit header fields
    BitWriter w;
    w.WriteBits(0x1234, 16); // sequence
    w.WriteBits(0x5678, 16); // ack
    w.WriteBits(0xABCDABCD, 32); // ack_bits
    w.WriteBits(0x3,  4); // type
    w.WriteBits(0x1,  4); // flags
    w.WriteBits(0xCAFEBABE, 32); // timestamp
    auto data = w.GetCompressedData();
    ASSERT_EQ(data.size(), 13u); // 104 bits = 13 bytes

    BitReader r(data, 104);
    EXPECT_EQ(r.ReadBits(16), 0x1234u);
    EXPECT_EQ(r.ReadBits(16), 0x5678u);
    EXPECT_EQ(r.ReadBits(32), 0xABCDABCDu);
    EXPECT_EQ(r.ReadBits(4),  0x3u);
    EXPECT_EQ(r.ReadBits(4),  0x1u);
    EXPECT_EQ(r.ReadBits(32), 0xCAFEBABEu);
}

// ─── Buffer sizing ────────────────────────────────────────────────────────────

TEST(BitWriterReader, CompressedDataSize_ExactByte) {
    BitWriter w;
    w.WriteBits(0, 8);
    EXPECT_EQ(w.GetCompressedData().size(), 1u);
}

TEST(BitWriterReader, CompressedDataSize_CrossByteBoundary) {
    BitWriter w;
    w.WriteBits(0, 9); // 9 bits → 2 bytes
    EXPECT_EQ(w.GetCompressedData().size(), 2u);
}

TEST(BitWriterReader, CompressedDataSize_104Bits) {
    BitWriter w;
    for (int i = 0; i < 104; ++i) w.WriteBits(1, 1);
    EXPECT_EQ(w.GetCompressedData().size(), 13u);
}

// ─── LSB-first ordering ───────────────────────────────────────────────────────

TEST(BitWriterReader, LSBFirstByteOrdering) {
    // Write 8 individual bits: 1,1,0,0,1,1,0,1 (LSB first)
    // Expected byte: bit0=1 bit1=1 bit2=0 bit3=0 bit4=1 bit5=1 bit6=0 bit7=1 = 0b10110011 = 0xB3
    BitWriter w;
    w.WriteBits(1, 1);
    w.WriteBits(1, 1);
    w.WriteBits(0, 1);
    w.WriteBits(0, 1);
    w.WriteBits(1, 1);
    w.WriteBits(1, 1);
    w.WriteBits(0, 1);
    w.WriteBits(1, 1);
    auto data = w.GetCompressedData();
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], 0xB3u);
}
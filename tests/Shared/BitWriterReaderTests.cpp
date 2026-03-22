#include <gtest/gtest.h>
#include "Shared/Serialization/BitWriter.h"
#include "Shared/Serialization/BitReader.h"

using namespace NetworkMiddleware::Shared;

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
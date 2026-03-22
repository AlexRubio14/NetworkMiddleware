#include <gtest/gtest.h>
#include "Shared/Network/PacketHeader.h"

using namespace NetworkMiddleware::Shared;

// ─── Constants ────────────────────────────────────────────────────────────────

TEST(PacketHeader, BitCountIs104) {
    EXPECT_EQ(PacketHeader::kBitCount, 104u);
}

TEST(PacketHeader, ByteCountIs13) {
    EXPECT_EQ(PacketHeader::kByteCount, 13u);
}

// ─── Write/Read round-trip ────────────────────────────────────────────────────

TEST(PacketHeader, WriteReadRoundTrip_AllFields) {
    PacketHeader h;
    h.sequence  = 0x1234;
    h.ack       = 0x5678;
    h.ack_bits  = 0xDEADBEEF;
    h.type      = 0x3;
    h.flags     = 0x1;
    h.timestamp = 0xCAFEBABE;

    BitWriter w;
    h.Write(w);
    auto data = w.GetCompressedData();
    ASSERT_EQ(data.size(), 13u);

    BitReader r(data, data.size() * 8);
    PacketHeader h2 = PacketHeader::Read(r);

    EXPECT_EQ(h2.sequence,  h.sequence);
    EXPECT_EQ(h2.ack,       h.ack);
    EXPECT_EQ(h2.ack_bits,  h.ack_bits);
    EXPECT_EQ(h2.type,      h.type);
    EXPECT_EQ(h2.flags,     h.flags);
    EXPECT_EQ(h2.timestamp, h.timestamp);
}

TEST(PacketHeader, WriteReadRoundTrip_ZeroValues) {
    PacketHeader h; // all defaults = 0
    BitWriter w;
    h.Write(w);
    auto data = w.GetCompressedData();

    BitReader r(data, data.size() * 8);
    PacketHeader h2 = PacketHeader::Read(r);

    EXPECT_EQ(h2.sequence,  0u);
    EXPECT_EQ(h2.ack,       0u);
    EXPECT_EQ(h2.ack_bits,  0u);
    EXPECT_EQ(h2.type,      0u);
    EXPECT_EQ(h2.flags,     0u);
    EXPECT_EQ(h2.timestamp, 0u);
}

// ─── IsAcked ─────────────────────────────────────────────────────────────────

TEST(PacketHeader, IsAcked_DirectAck) {
    PacketHeader h;
    h.ack = 100;
    h.ack_bits = 0;
    EXPECT_TRUE(h.IsAcked(100));
}

TEST(PacketHeader, IsAcked_BitZeroSet) {
    PacketHeader h;
    h.ack = 100;
    h.ack_bits = 0b001; // bit0 → ack-1 = 99 confirmed
    EXPECT_TRUE(h.IsAcked(99));
}

TEST(PacketHeader, IsAcked_BitNotSet) {
    PacketHeader h;
    h.ack = 100;
    h.ack_bits = 0b001; // only bit0 set
    EXPECT_FALSE(h.IsAcked(98)); // diff=2, bit1 not set
}

TEST(PacketHeader, IsAcked_AllBitsSet) {
    PacketHeader h;
    h.ack = 100;
    h.ack_bits = 0xFFFFFFFF;
    for (uint16_t i = 68; i <= 99; ++i)
        EXPECT_TRUE(h.IsAcked(i)); // diff 1..32, all bits set
}

TEST(PacketHeader, IsAcked_OutsideWindow) {
    PacketHeader h;
    h.ack = 100;
    h.ack_bits = 0xFFFFFFFF;
    EXPECT_FALSE(h.IsAcked(67)); // diff=33 > 32
}

TEST(PacketHeader, IsAcked_NewerThanAck) {
    PacketHeader h;
    h.ack = 50;
    h.ack_bits = 0;
    EXPECT_FALSE(h.IsAcked(51)); // diff = int16_t(50-51) = -1 < 0
}

TEST(PacketHeader, IsAcked_WrapAround) {
    PacketHeader h;
    h.ack = 1;
    h.ack_bits = 0b10; // bit1 set → ack-2 = 65535 confirmed
    EXPECT_TRUE(h.IsAcked(65535)); // diff = int16_t(1-65535) = 2, bit1 → true

    h.ack_bits = 0;
    EXPECT_FALSE(h.IsAcked(65535)); // same diff, bit not set → false
}

// ─── CurrentTimeMs ────────────────────────────────────────────────────────────

TEST(PacketHeader, CurrentTimeMs_NonZero) {
    EXPECT_GT(PacketHeader::CurrentTimeMs(), 0u);
}

TEST(PacketHeader, CurrentTimeMs_Monotonic) {
    const uint32_t t1 = PacketHeader::CurrentTimeMs();
    const uint32_t t2 = PacketHeader::CurrentTimeMs();
    EXPECT_GE(t2, t1);
}
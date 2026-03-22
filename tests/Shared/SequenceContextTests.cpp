#include <gtest/gtest.h>
#include "Shared/Network/PacketHeader.h"

using namespace NetworkMiddleware::Shared;

// ─── AdvanceLocal ─────────────────────────────────────────────────────────────

TEST(SequenceContext, AdvanceLocal_Increments) {
    SequenceContext ctx;
    ctx.AdvanceLocal();
    EXPECT_EQ(ctx.localSequence, 1u);
}

TEST(SequenceContext, AdvanceLocal_WrapsAt65535) {
    SequenceContext ctx;
    ctx.localSequence = 65535;
    ctx.AdvanceLocal();
    EXPECT_EQ(ctx.localSequence, 0u);
}

// ─── RecordReceived — basic ───────────────────────────────────────────────────

TEST(SequenceContext, RecordReceived_FirstPacket) {
    SequenceContext ctx;
    ctx.RecordReceived(5);
    EXPECT_EQ(ctx.remoteAck, 5u);
}

TEST(SequenceContext, RecordReceived_NewerPacket_AdvancesAck) {
    SequenceContext ctx;
    ctx.RecordReceived(10);
    ctx.RecordReceived(11);
    EXPECT_EQ(ctx.remoteAck, 11u);
    // diff=1 → bit0 set (10 was received)
    EXPECT_TRUE((ctx.ackBits & 1u) != 0);
}

TEST(SequenceContext, RecordReceived_Duplicate_NoChange) {
    SequenceContext ctx;
    ctx.RecordReceived(10);
    const uint32_t bitsBefore = ctx.ackBits;
    ctx.RecordReceived(10);
    EXPECT_EQ(ctx.remoteAck, 10u);
    EXPECT_EQ(ctx.ackBits, bitsBefore);
}

TEST(SequenceContext, RecordReceived_OutOfOrder_Marksbit) {
    SequenceContext ctx;
    ctx.RecordReceived(10);
    ctx.RecordReceived(12); // skip 11
    EXPECT_EQ(ctx.remoteAck, 12u);

    ctx.RecordReceived(11); // arrives late
    EXPECT_EQ(ctx.remoteAck, 12u); // ack unchanged
    // diff = int16_t(11-12) = -1 → bit = 0, set bit0
    EXPECT_TRUE((ctx.ackBits & 1u) != 0);
}

TEST(SequenceContext, RecordReceived_MultiplePrior) {
    SequenceContext ctx;
    ctx.RecordReceived(0);
    ctx.RecordReceived(1);
    ctx.RecordReceived(2);
    ctx.RecordReceived(3);
    EXPECT_EQ(ctx.remoteAck, 3u);
    // bits for 2(diff=1,bit0), 1(diff=2,bit1), 0(diff=3,bit2)
    EXPECT_TRUE((ctx.ackBits & 0b111) == 0b111);
}

// ─── RecordReceived — wrap-around ────────────────────────────────────────────

TEST(SequenceContext, RecordReceived_WrapAround_ZeroAfter65535) {
    SequenceContext ctx;
    ctx.RecordReceived(65535);
    ctx.RecordReceived(0); // 0 is newer (diff = int16_t(0-65535) = 1 > 0)
    EXPECT_EQ(ctx.remoteAck, 0u);
    // bit0 should be set: 65535 was received (diff=1)
    EXPECT_TRUE((ctx.ackBits & 1u) != 0);
}

TEST(SequenceContext, RecordReceived_OldPacket_WrapAround) {
    SequenceContext ctx;
    ctx.RecordReceived(1);
    ctx.RecordReceived(65535); // arrives late, older: diff=int16_t(65535-1)=-2 → bit1
    EXPECT_EQ(ctx.remoteAck, 1u); // ack unchanged
    EXPECT_TRUE((ctx.ackBits & 0b10) != 0);
}

// ─── RecordReceived — large jump ─────────────────────────────────────────────

TEST(SequenceContext, RecordReceived_JumpBeyond32_ResetsBits) {
    SequenceContext ctx;
    ctx.RecordReceived(0);
    ctx.ackBits = 0xFFFFFFFF;
    ctx.RecordReceived(33); // diff=33 >= 32 → reset
    EXPECT_EQ(ctx.remoteAck, 33u);
    EXPECT_EQ(ctx.ackBits, 0u);
}

TEST(SequenceContext, RecordReceived_TooOld_Ignored) {
    SequenceContext ctx;
    ctx.RecordReceived(100);
    const uint32_t bitsBefore = ctx.ackBits;
    ctx.RecordReceived(67); // diff = int16_t(67-100) = -33 → bit=32 >= 32 → ignored
    EXPECT_EQ(ctx.remoteAck, 100u);
    EXPECT_EQ(ctx.ackBits, bitsBefore);
}
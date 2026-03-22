// P-4.3 NetworkProfiler unit tests.
// All tests are deterministic — no sleeps, no real clocks.

#include <gtest/gtest.h>
#include "Core/NetworkProfiler.h"

using namespace NetworkMiddleware::Core;

// ─── RecordBytesSent ─────────────────────────────────────────────────────────

TEST(NetworkProfiler, RecordBytesSent_AccumulatesCorrectly) {
    NetworkProfiler p;
    p.RecordBytesSent(100);
    p.RecordBytesSent(50);
    p.RecordBytesSent(25);
    const auto s = p.GetSnapshot(1);
    EXPECT_EQ(s.totalBytesSent, 175u);
}

// ─── RecordBytesReceived ─────────────────────────────────────────────────────

TEST(NetworkProfiler, RecordBytesReceived_AccumulatesCorrectly) {
    NetworkProfiler p;
    p.RecordBytesReceived(200);
    p.RecordBytesReceived(300);
    const auto s = p.GetSnapshot(1);
    EXPECT_EQ(s.totalBytesReceived, 500u);
}

// ─── IncrementRetransmissions ────────────────────────────────────────────────

TEST(NetworkProfiler, IncrementRetransmissions_CountsCorrectly) {
    NetworkProfiler p;
    for (int i = 0; i < 5; ++i)
        p.IncrementRetransmissions();
    const auto s = p.GetSnapshot(1);
    EXPECT_EQ(s.retransmissions, 5u);
}

// ─── RecordTick ───────────────────────────────────────────────────────────────

TEST(NetworkProfiler, RecordTick_AvgTimeCorrect) {
    NetworkProfiler p;
    p.RecordTick(100);
    p.RecordTick(200);
    p.RecordTick(300);  // avg = (100+200+300)/3 = 200µs
    const auto s = p.GetSnapshot(1);
    EXPECT_FLOAT_EQ(s.avgTickTimeUs, 200.0f);
}

TEST(NetworkProfiler, RecordTick_NoTicks_AvgIsZero) {
    NetworkProfiler p;
    const auto s = p.GetSnapshot(1);
    EXPECT_EQ(s.avgTickTimeUs, 0.0f);
}

// ─── DeltaEfficiency ─────────────────────────────────────────────────────────

// No ticks → 0 (no data to compute ratio).
TEST(NetworkProfiler, DeltaEfficiency_NoData_IsZero) {
    NetworkProfiler p;
    const auto s = p.GetSnapshot(10);
    EXPECT_EQ(s.deltaEfficiency, 0.0f);
}

// Sending exactly the theoretical full-sync size → efficiency = 0
// (we're not compressing anything).
TEST(NetworkProfiler, DeltaEfficiency_FullSyncGivesZeroEfficiency) {
    NetworkProfiler p;
    constexpr uint32_t kFull = NetworkProfiler::kFullSyncBytesPerClient;
    p.RecordBytesSent(kFull);  // 1 client × 19 bytes = 19 bytes/tick
    p.RecordTick(1000);
    const auto s = p.GetSnapshot(1);
    EXPECT_NEAR(s.deltaEfficiency, 0.0f, 0.001f);
}

// Sending half the theoretical size → efficiency = 50%
// (delta compression halved the bandwidth).
TEST(NetworkProfiler, DeltaEfficiency_HalfSentGivesFiftyPercent) {
    NetworkProfiler p;
    constexpr uint32_t kFull = NetworkProfiler::kFullSyncBytesPerClient;
    // 2 clients → theoretical = 38 bytes/tick.
    // We send only 19 bytes → efficiency = 1 - (19/38) = 0.5
    p.RecordBytesSent(kFull);
    p.RecordTick(1000);
    const auto s = p.GetSnapshot(2);
    EXPECT_NEAR(s.deltaEfficiency, 0.5f, 0.001f);
}

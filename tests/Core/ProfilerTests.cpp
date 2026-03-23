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

// ─── kFullSyncBytesPerClient ─────────────────────────────────────────────────
//
// Asserts the constant independently of any other test that uses it.
// A bad change to the constant would keep DeltaEfficiency tests green but fail here,
// catching protocol-size regressions. Value: 149 bits / 8 = 18.625 → ceiling = 19.
TEST(NetworkProfiler, kFullSyncBytesPerClient_Is19) {
    EXPECT_EQ(NetworkProfiler::kFullSyncBytesPerClient, 19u);
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

// ─── P-4.4: EMA reactive tick time ───────────────────────────────────────────

// Initial EMA is zero before any tick is recorded.
TEST(NetworkProfiler, EMA_InitiallyZero) {
    NetworkProfiler p;
    EXPECT_FLOAT_EQ(p.GetRecentAvgTickUs(), 0.0f);
}

// After a single tick of value V, EMA = α*V (since EMA_old = 0).
TEST(NetworkProfiler, EMA_FirstTick) {
    NetworkProfiler p;
    p.RecordTick(10000);  // 10ms
    // EMA = 0.1 * 10000 + 0.9 * 0 = 1000µs
    EXPECT_NEAR(p.GetRecentAvgTickUs(), 1000.0f, 1.0f);
}

// After 10 ticks of 10ms each, EMA must have risen above 5ms.
// Math: EMA_n = 10000 * (1 - 0.9^n).  At n=10: 10000*(1-0.349) ≈ 6513µs > 5000µs.
// This validates the "reacts within 10 iterations" requirement from the handoff.
TEST(NetworkProfiler, EMA_ReactsWithin10Ticks) {
    NetworkProfiler p;
    for (int i = 0; i < 10; ++i)
        p.RecordTick(10000);  // 10ms each
    EXPECT_GT(p.GetRecentAvgTickUs(), 5000.0f);
}

// EMA converges close to the steady-state value after 100 ticks.
// At n=100: EMA ≈ 10000*(1-0.9^100) ≈ 9999.97µs → within 1% of 10000µs.
TEST(NetworkProfiler, EMA_ConvergesAfter100Ticks) {
    NetworkProfiler p;
    for (int i = 0; i < 100; ++i)
        p.RecordTick(10000);
    EXPECT_NEAR(p.GetRecentAvgTickUs(), 10000.0f, 200.0f);  // within 2%
}

// GetSnapshot exposes recentAvgTickMs (µs → ms conversion).
TEST(NetworkProfiler, EMA_SnapshotExposesRecentMs) {
    NetworkProfiler p;
    for (int i = 0; i < 10; ++i)
        p.RecordTick(5000);  // 5ms
    const auto s = p.GetSnapshot(1);
    // recentAvgTickMs must be between 0 and 5ms after 10 ticks from zero.
    EXPECT_GT(s.recentAvgTickMs, 0.0f);
    EXPECT_LE(s.recentAvgTickMs, 5.0f);
}

// SetRecentAvgTickUsForTest bypasses RecordTick — used by scaling tests.
TEST(NetworkProfiler, EMA_TestHook_Override) {
    NetworkProfiler p;
    p.SetRecentAvgTickUsForTest(8000.0f);
    EXPECT_FLOAT_EQ(p.GetRecentAvgTickUs(), 8000.0f);
}

#include <gtest/gtest.h>
#include "Data/Network/NetworkOptimizer.h"
#include "Data/Network/HeroSerializer.h"
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"
#include "Data/HeroState.h"
#include "Gameplay/HeroDirtyBits.h"
#include "Core/RemoteClient.h"

using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Shared::Network;
using namespace NetworkMiddleware::Shared::Data;
using namespace NetworkMiddleware::Core;

// ─── ZigZag Encoding ─────────────────────────────────────────────────────────

TEST(ZigZag, Encode_Zero) {
    EXPECT_EQ(NetworkOptimizer::ZigZagEncode(0), 0u);
}

TEST(ZigZag, Encode_PositiveOne) {
    EXPECT_EQ(NetworkOptimizer::ZigZagEncode(1), 2u);
}

TEST(ZigZag, Encode_NegativeOne) {
    EXPECT_EQ(NetworkOptimizer::ZigZagEncode(-1), 1u);
}

TEST(ZigZag, Encode_PositiveTwo) {
    EXPECT_EQ(NetworkOptimizer::ZigZagEncode(2), 4u);
}

TEST(ZigZag, Encode_NegativeTwo) {
    EXPECT_EQ(NetworkOptimizer::ZigZagEncode(-2), 3u);
}

TEST(ZigZag, RoundTrip_VariousValues) {
    for (int32_t v : {0, 1, -1, 100, -100, 500, -500, 16383, -16383}) {
        EXPECT_EQ(NetworkOptimizer::ZigZagDecode(NetworkOptimizer::ZigZagEncode(v)), v)
            << "Failed for v=" << v;
    }
}

TEST(ZigZag, Encode_NegativeMakesSmallUnsigned) {
    // Small negative values should encode to small unsigned values → efficient VLE
    // -1 → 1,  -64 → 127  (both fit in 1 VLE byte)
    EXPECT_LT(NetworkOptimizer::ZigZagEncode(-64), 128u);
}

// ─── SerializeDelta / DeserializeDelta ───────────────────────────────────────

static HeroState MakeBaseline() {
    HeroState s;
    s.networkID  = 7u;
    s.x          = 100.0f;
    s.y          = -50.0f;
    s.health     = 300.0f;
    s.maxHealth  = 500.0f;
    s.mana       = 80.0f;
    s.level      = 10u;
    s.stateFlags = 0x00u;
    return s;
}

TEST(SerializeDelta, NoChanges_MinBitCount) {
    // networkID(32) + 6 one-bit flags all zero = 38 bits
    HeroState baseline = MakeBaseline();
    BitWriter w;
    HeroSerializer::SerializeDelta(baseline, baseline, w);
    EXPECT_EQ(w.GetBitCount(), 38u);
}

TEST(SerializeDelta, NoChanges_FewerBitsThanFullSync) {
    HeroState baseline = MakeBaseline();
    BitWriter wDelta, wFull;
    baseline.dirtyMask = 0xFFFFFFFF;
    HeroSerializer::SerializeDelta(baseline, baseline, wDelta);
    HeroSerializer::Serialize(baseline, wFull);
    EXPECT_LT(wDelta.GetBitCount(), wFull.GetBitCount());
}

TEST(SerializeDelta, HealthChanged_RoundTrip) {
    HeroState baseline = MakeBaseline();
    HeroState current  = baseline;
    current.health = 350.0f; // +50

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());

    HeroState result;
    HeroSerializer::DeserializeDelta(result, baseline, r);

    EXPECT_EQ(result.networkID, current.networkID);
    EXPECT_EQ(result.health,    current.health);
    EXPECT_EQ(result.x,         baseline.x); // unchanged
    EXPECT_EQ(result.mana,      baseline.mana);
}

TEST(SerializeDelta, NegativeHealthDelta_RoundTrip) {
    HeroState baseline = MakeBaseline();
    HeroState current  = baseline;
    current.health = 50.0f; // -250 (taking damage)

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());

    HeroState result;
    HeroSerializer::DeserializeDelta(result, baseline, r);

    EXPECT_EQ(result.health, current.health);
}

TEST(SerializeDelta, PositionChanged_RoundTrip) {
    HeroState baseline = MakeBaseline();
    HeroState current  = baseline;
    current.x = 105.0f;
    current.y = -55.0f;

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());

    HeroState result;
    HeroSerializer::DeserializeDelta(result, baseline, r);

    const float kMaxError = 1000.0f / ((1u << 16) - 1);
    EXPECT_NEAR(result.x, current.x, kMaxError);
    EXPECT_NEAR(result.y, current.y, kMaxError);
    EXPECT_EQ(result.health, baseline.health); // unchanged
}

TEST(SerializeDelta, PositionOnly_FewerBitsThanFullSync) {
    HeroState baseline = MakeBaseline();
    HeroState current  = baseline;
    current.x = 101.0f; // small move

    BitWriter wDelta, wFull;
    HeroSerializer::SerializeDelta(current, baseline, wDelta);
    current.dirtyMask = (1u << (uint32_t)HeroDirtyBits::Position);
    HeroSerializer::Serialize(current, wFull);

    EXPECT_LT(wDelta.GetBitCount(), wFull.GetBitCount());
}

TEST(SerializeDelta, StateFlags_RoundTrip) {
    HeroState baseline = MakeBaseline();
    HeroState current  = baseline;
    current.stateFlags = 0x03u; // Dead | Stunned

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());

    HeroState result;
    HeroSerializer::DeserializeDelta(result, baseline, r);

    EXPECT_EQ(result.stateFlags, 0x03u);
}

TEST(SerializeDelta, AllFieldsChanged_RoundTrip) {
    HeroState baseline = MakeBaseline();
    HeroState current;
    current.networkID  = baseline.networkID;
    current.x          = 200.0f;
    current.y          = 300.0f;
    current.health     = 100.0f;
    current.maxHealth  = 500.0f;
    current.mana       = 20.0f;
    current.level      = 11u;
    current.stateFlags = 0x01u;

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());

    HeroState result;
    HeroSerializer::DeserializeDelta(result, baseline, r);

    const float kMaxError = 1000.0f / ((1u << 16) - 1);
    EXPECT_NEAR(result.x,       current.x,       kMaxError);
    EXPECT_NEAR(result.y,       current.y,       kMaxError);
    EXPECT_EQ(result.health,    current.health);
    EXPECT_EQ(result.maxHealth, current.maxHealth);
    EXPECT_EQ(result.mana,      current.mana);
    EXPECT_EQ(result.level,     current.level);
    EXPECT_EQ(result.stateFlags,current.stateFlags);
}

TEST(SerializeDelta, AllFieldsChanged_FewerBitsThanFullSync) {
    HeroState baseline = MakeBaseline();
    HeroState current;
    current.networkID  = baseline.networkID;
    current.x          = 200.0f;
    current.y          = 300.0f;
    current.health     = 100.0f;   // small delta (-200)
    current.maxHealth  = 500.0f;   // no change
    current.mana       = 20.0f;    // small delta (-60)
    current.level      = 11u;      // +1
    current.stateFlags = 0x01u;

    BitWriter wDelta, wFull;
    HeroSerializer::SerializeDelta(current, baseline, wDelta);
    current.dirtyMask = 0xFFFFFFFF;
    HeroSerializer::Serialize(current, wFull);

    EXPECT_LT(wDelta.GetBitCount(), wFull.GetBitCount());
}

// ─── SnapshotHistory ─────────────────────────────────────────────────────────

TEST(SnapshotHistory, RecordAndRetrieve) {
    RemoteClient client;
    HeroState state = MakeBaseline();

    client.RecordSnapshot(10u, state);

    const HeroState* result = client.GetBaseline(10u);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->networkID, state.networkID);
    EXPECT_EQ(result->health,    state.health);
}

TEST(SnapshotHistory, UnknownSeq_ReturnsNullptr) {
    RemoteClient client;
    EXPECT_EQ(client.GetBaseline(42u), nullptr);
}

TEST(SnapshotHistory, StaleSeq_ReturnsNullptr) {
    // seq=0 and seq=64 map to the same slot — storing 64 evicts 0
    RemoteClient client;
    HeroState state = MakeBaseline();

    client.RecordSnapshot(0u, state);
    ASSERT_NE(client.GetBaseline(0u), nullptr); // present

    client.RecordSnapshot(64u, state);          // overwrites slot 0
    EXPECT_EQ(client.GetBaseline(0u), nullptr); // evicted
    EXPECT_NE(client.GetBaseline(64u), nullptr);
}

TEST(SnapshotHistory, NullptrBaseline_ImpliesFullSync) {
    // Caller contract: GetBaseline returning nullptr means force full sync.
    // This test documents the contract rather than testing implementation.
    RemoteClient client;
    EXPECT_EQ(client.GetBaseline(999u), nullptr);
    // Caller would detect nullptr and call HeroSerializer::Serialize (full sync)
}

// ─── 16-bit precision: 2cm movement detection ────────────────────────────────

TEST(SerializeDelta, TwoCmMove_DetectedAndSerialized) {
    // With 14 bits over 1000m: step ≈ 6.1cm → 2cm rounds to 0 → NOT detected.
    // With 16 bits over 1000m: step ≈ 1.53cm → 2cm rounds to ~1 step → detected.
    HeroState baseline = MakeBaseline(); // x = 100.0f
    HeroState current  = baseline;
    current.x = 100.02f; // +2 cm

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);

    // packet must be larger than the 38-bit no-change minimum
    EXPECT_GT(w.GetBitCount(), 38u);

    // round-trip must recover the position within 16-bit precision
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroState result;
    HeroSerializer::DeserializeDelta(result, baseline, r);

    const float kStep = 1000.0f / ((1u << 16) - 1); // ≈ 1.53 cm
    EXPECT_NEAR(result.x, current.x, kStep);
}

// ─── Identity validation ──────────────────────────────────────────────────────

TEST(SerializeDelta, Identity_Mismatch_DeserializeDelta_Rejected) {
    // Serialize a delta for hero 7.
    HeroState baseline = MakeBaseline(); // networkID = 7
    HeroState current  = baseline;
    current.health = 400.0f;

    BitWriter w;
    HeroSerializer::SerializeDelta(current, baseline, w);

    // Attempt to deserialize against a baseline for a different hero (ID 99).
    HeroState wrongBaseline  = MakeBaseline();
    wrongBaseline.networkID  = 99u;
    wrongBaseline.health     = 200.0f;

    HeroState result = wrongBaseline;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::DeserializeDelta(result, wrongBaseline, r);

    // Packet must be rejected: dirtyMask stays 0, no fields applied.
    EXPECT_EQ(result.dirtyMask, 0u);
    EXPECT_EQ(result.health, wrongBaseline.health);
}

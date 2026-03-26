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

// ─── SnapshotHistory — per-entity baselines (P-5.x) ─────────────────────────
//
// P-5.x changed from "one HeroState per seq" to "one batch per seq, per-entity
// baseline confirmed via ProcessAckedSeq()".  The fix eliminates the multi-entity
// delta corruption where GetBaseline(lastAckedSeq) returned the wrong entity's state.

TEST(SnapshotHistory, GetEntityBaseline_NullBeforeAck) {
    // Recording a batch does NOT immediately expose the baseline.
    // The client must ACK the seq first (ProcessAckedSeq).
    RemoteClient client;
    HeroState state = MakeBaseline();
    client.RecordBatch(10u, {{state.networkID, state}});

    EXPECT_EQ(client.GetEntityBaseline(state.networkID), nullptr);
}

TEST(SnapshotHistory, GetEntityBaseline_CorrectAfterAck) {
    RemoteClient client;
    HeroState state = MakeBaseline();
    client.RecordBatch(10u, {{state.networkID, state}});
    client.ProcessAckedSeq(10u);

    const HeroState* result = client.GetEntityBaseline(state.networkID);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->networkID, state.networkID);
    EXPECT_EQ(result->health,    state.health);
}

TEST(SnapshotHistory, UnknownEntity_ReturnsNullptr) {
    // No batch recorded — GetEntityBaseline always returns nullptr.
    RemoteClient client;
    EXPECT_EQ(client.GetEntityBaseline(42u), nullptr);
}

TEST(SnapshotHistory, EntitiesHaveIndependentBaselines) {
    // Bug this test guards against: previously GetBaseline(lastAckedSeq) returned
    // whichever entity owned that seq, causing entity A's delta to be computed
    // against entity B's state when multiple entities share a tick.
    RemoteClient client;
    HeroState stateA = MakeBaseline();
    stateA.networkID = 1; stateA.x = 100.0f; stateA.health = 200.0f;

    HeroState stateB = MakeBaseline();
    stateB.networkID = 2; stateB.x = 300.0f; stateB.health = 400.0f;

    // Both entities recorded in the same batch (same seq — one packet per client).
    client.RecordBatch(7u, {{stateA.networkID, stateA}, {stateB.networkID, stateB}});
    client.ProcessAckedSeq(7u);

    const HeroState* baseA = client.GetEntityBaseline(stateA.networkID);
    const HeroState* baseB = client.GetEntityBaseline(stateB.networkID);

    ASSERT_NE(baseA, nullptr);
    ASSERT_NE(baseB, nullptr);
    EXPECT_FLOAT_EQ(baseA->x,      stateA.x);      // entity A has its OWN baseline
    EXPECT_FLOAT_EQ(baseA->health, stateA.health);
    EXPECT_FLOAT_EQ(baseB->x,      stateB.x);      // entity B has its OWN baseline
    EXPECT_FLOAT_EQ(baseB->health, stateB.health);
}

TEST(SnapshotHistory, EvictedSeq_DoesNotUpdateBaseline) {
    // seq=0 and seq=64 map to the same history slot (0 % 64 == 64 % 64 == 0).
    // After seq=64 overwrites the slot, ProcessAckedSeq(0) must be a no-op.
    RemoteClient client;
    HeroState stateOld = MakeBaseline();
    stateOld.networkID = 5; stateOld.health = 100.0f;

    HeroState stateNew = MakeBaseline();
    stateNew.networkID = 5; stateNew.health = 999.0f;  // different value

    client.RecordBatch(0u,  {{stateOld.networkID, stateOld}});
    client.RecordBatch(64u, {{stateNew.networkID, stateNew}});  // evicts slot 0

    // ACK seq=64: baseline becomes stateNew.
    client.ProcessAckedSeq(64u);
    const HeroState* after64 = client.GetEntityBaseline(5u);
    ASSERT_NE(after64, nullptr);
    EXPECT_FLOAT_EQ(after64->health, stateNew.health);

    // ACK seq=0 now: slot holds seq=64, so the guard (entry.seq != seq) fires → no-op.
    client.ProcessAckedSeq(0u);
    const HeroState* afterStale = client.GetEntityBaseline(5u);
    ASSERT_NE(afterStale, nullptr);
    EXPECT_FLOAT_EQ(afterStale->health, stateNew.health);  // unchanged — still stateNew
}

TEST(SnapshotHistory, NullptrBaseline_ImpliesFullSync) {
    // Contract: GetEntityBaseline returning nullptr means force full sync.
    RemoteClient client;
    EXPECT_EQ(client.GetEntityBaseline(999u), nullptr);
    // Caller detects nullptr and calls HeroSerializer::Serialize (full sync).
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

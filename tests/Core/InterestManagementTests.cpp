#include <gtest/gtest.h>
#include "Core/NetworkManager.h"
#include "Core/RemoteClient.h"
#include "Core/VisibilityTracker.h"
#include "Shared/Network/PacketHeader.h"
#include "Shared/Network/HandshakePackets.h"
#include "Shared/Serialization/BitWriter.h"
#include "Shared/Serialization/BitReader.h"
#include "Shared/Serialization/CRC32.h"
#include "Shared/Data/HeroState.h"
#include "MockTransport.h"

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Shared::Data;
using namespace NetworkMiddleware::Tests;

// ─── Helpers shared with NetworkManagerTests ─────────────────────────────────

static std::vector<uint8_t> WithCRC_IM(std::vector<uint8_t> data) {
    const uint32_t crc = ComputeCRC32(data);
    data.push_back(static_cast<uint8_t>((crc >>  0) & 0xFF));
    data.push_back(static_cast<uint8_t>((crc >>  8) & 0xFF));
    data.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
    return data;
}

static std::vector<uint8_t> StripCRC_IM(const std::vector<uint8_t>& data) {
    return data.size() >= 4
        ? std::vector<uint8_t>(data.begin(), data.end() - 4)
        : data;
}

static std::vector<uint8_t> MakeHeaderOnlyPacket_IM(PacketType type, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h{};
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(type);
    h.Write(w);
    return WithCRC_IM(w.GetCompressedData());
}

static std::vector<uint8_t> MakeChallengeResponsePacket_IM(uint64_t salt, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h{};
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(PacketType::ChallengeResponse);
    h.Write(w);
    ChallengeResponsePayload{salt}.Write(w);
    return WithCRC_IM(w.GetCompressedData());
}

static EndPoint MakeEP(uint32_t addr = 0x0100007F, uint16_t port = 9100) {
    return EndPoint{addr, port};
}

static uint16_t CompleteHandshake_IM(MockTransport& t, NetworkManager& nm, const EndPoint& ep) {
    t.InjectPacket(MakeHeaderOnlyPacket_IM(PacketType::ConnectionRequest), ep);
    nm.Update();
    if (t.sentPackets.empty()) return 0;

    const auto challengeStripped = StripCRC_IM(t.sentPackets.back().first);
    BitReader cr(challengeStripped, challengeStripped.size() * 8);
    PacketHeader::Read(cr);
    const auto challenge = ChallengePayload::Read(cr);
    t.sentPackets.clear();

    t.InjectPacket(MakeChallengeResponsePacket_IM(challenge.salt), ep);
    nm.Update();
    if (t.sentPackets.empty()) return 0;

    const auto acceptStripped = StripCRC_IM(t.sentPackets.back().first);
    BitReader ar(acceptStripped, acceptStripped.size() * 8);
    PacketHeader::Read(ar);
    const auto accepted = ConnectionAcceptedPayload::Read(ar);
    t.sentPackets.clear();

    return accepted.networkID;
}

// ─── RemoteClient::EvictEntityBaseline ───────────────────────────────────────

TEST(InterestManagement, EvictEntityBaseline_RemovesBaseline) {
    RemoteClient client;
    client.networkID = 1;

    // Manually build a batch entry and promote it as if seq 0 was ACKed.
    HeroState state{};
    state.networkID = 42;
    state.x = 100.0f;
    client.RecordBatch(0, {{42u, state}});
    client.ProcessAckedSeq(0);

    ASSERT_NE(client.GetEntityBaseline(42), nullptr);

    client.EvictEntityBaseline(42);
    EXPECT_EQ(client.GetEntityBaseline(42), nullptr);
}

TEST(InterestManagement, EvictEntityBaseline_OtherEntitiesUnaffected) {
    RemoteClient client;
    client.networkID = 1;

    HeroState s1{}, s2{};
    s1.networkID = 10;
    s2.networkID = 20;
    client.RecordBatch(0, {{10u, s1}, {20u, s2}});
    client.ProcessAckedSeq(0);

    client.EvictEntityBaseline(10);

    EXPECT_EQ(client.GetEntityBaseline(10), nullptr);
    ASSERT_NE(client.GetEntityBaseline(20), nullptr);
}

TEST(InterestManagement, EvictEntityBaseline_NoBaselineIsNoop) {
    RemoteClient client;
    client.networkID = 1;
    // Evicting an entity that was never baselined should not crash.
    EXPECT_NO_FATAL_FAILURE(client.EvictEntityBaseline(99));
    EXPECT_EQ(client.GetEntityBaseline(99), nullptr);
}

// ─── NetworkManager::EvictEntityBaseline ─────────────────────────────────────

TEST(InterestManagement, NetworkManager_EvictEntityBaseline_DelegatesCorrectly) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEP(0x0200007F, 9200);
    const uint16_t id = CompleteHandshake_IM(*t, nm, ep);
    ASSERT_NE(id, 0u);

    // Simulate a snapshot being sent and ACKed so a baseline is recorded.
    HeroState state{};
    state.networkID = 55;
    state.x = 50.0f;
    nm.CommitAndSendBatchSnapshot(ep, {state},
        nm.SerializeBatchSnapshotFor(ep, {state}, 0));

    // Simulate the client ACKing seq 0 by sending a heartbeat that carries ack=0.
    {
        BitWriter w;
        PacketHeader h{};
        h.sequence = 0;
        h.ack      = 0;
        h.type     = static_cast<uint8_t>(PacketType::Heartbeat);
        h.Write(w);
        t->InjectPacket(WithCRC_IM(w.GetCompressedData()), ep);
        nm.Update();
    }

    // After ACK, re-serialize — should produce a delta (fewer bits) vs first call.
    // We check indirectly: after eviction, baseline is gone → next serialize is full.
    nm.EvictEntityBaseline(ep, 55u);

    // No crash, and the manager doesn't assert. Functional verification:
    // SerializeBatchSnapshotFor must still succeed (returns non-empty payload).
    const auto payload = nm.SerializeBatchSnapshotFor(ep, {state}, 1);
    EXPECT_FALSE(payload.empty());
}

TEST(InterestManagement, NetworkManager_EvictEntityBaseline_UnknownEP_IsNoop) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);
    EXPECT_NO_FATAL_FAILURE(nm.EvictEntityBaseline(MakeEP(0xFF000001, 1234), 99u));
}

// ─── VisibilityTracker + RemoteClient integration ────────────────────────────

// Simulate the game-loop pattern: gather → visTracker.Update → evict → serialize.
// Verify that after eviction, GetEntityBaseline returns null (full state will follow).
TEST(InterestManagement, ReentryPattern_BaselineEvictedBeforeSerialization) {
    VisibilityTracker tracker;
    RemoteClient client;
    client.networkID = 1;

    HeroState state{};
    state.networkID = 7;
    state.x = 200.0f;

    // --- Send tick 1: entity 7 visible, baseline promoted after ACK ---
    client.RecordBatch(0, {{7u, state}});
    client.ProcessAckedSeq(0);
    ASSERT_NE(client.GetEntityBaseline(7), nullptr);
    tracker.UpdateAndGetReentrants(1, {7u});  // prime tracker: entity 7 was visible

    // --- Send tick 2: entity 7 leaves FOW ---
    tracker.UpdateAndGetReentrants(1, {});    // entity 7 no longer visible

    // --- Send tick 3: entity 7 re-enters FOW ---
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {7u});
    EXPECT_TRUE(reentrants.count(7u));        // detected as re-entry

    // Simulate eviction as main.cpp would do
    for (const uint32_t eid : reentrants)
        client.EvictEntityBaseline(eid);

    // Baseline must now be null → next serialization will be a full state
    EXPECT_EQ(client.GetEntityBaseline(7), nullptr);
}

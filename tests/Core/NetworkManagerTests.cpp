#include <gtest/gtest.h>
#include "Core/NetworkManager.h"
#include "Shared/Network/HandshakePackets.h"
#include "MockTransport.h"

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Tests;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> MakeHeaderOnlyPacket(PacketType type, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h;
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(type);
    h.Write(w);
    return w.GetCompressedData();
}

static std::vector<uint8_t> MakeChallengeResponsePacket(uint64_t salt, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h;
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(PacketType::ChallengeResponse);
    h.Write(w);
    ChallengeResponsePayload{salt}.Write(w);
    return w.GetCompressedData();
}

static EndPoint MakeEndpoint(uint32_t addr = 0x0100007F, uint16_t port = 9000) {
    return EndPoint{addr, port};
}

// Completes the full handshake for a client. Returns assigned NetworkID.
static uint16_t CompleteHandshake(MockTransport& t, NetworkManager& nm, const EndPoint& ep) {
    // Step 1 — ConnectionRequest
    t.InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), ep);
    nm.Update();
    EXPECT_GE(t.sentPackets.size(), 1u);

    // Step 2 — Read challenge salt
    auto& challengePkt = t.sentPackets.back().first;
    BitReader cr(challengePkt, challengePkt.size() * 8);
    PacketHeader::Read(cr);
    const auto challenge = ChallengePayload::Read(cr);
    t.sentPackets.clear();

    // Step 3 — ChallengeResponse
    t.InjectPacket(MakeChallengeResponsePacket(challenge.salt), ep);
    nm.Update();
    EXPECT_GE(t.sentPackets.size(), 1u);

    // Step 4 — Read NetworkID
    auto& acceptPkt = t.sentPackets.back().first;
    BitReader ar(acceptPkt, acceptPkt.size() * 8);
    PacketHeader::Read(ar);
    const auto accepted = ConnectionAcceptedPayload::Read(ar);
    t.sentPackets.clear();

    return accepted.networkID;
}

// ─── Handshake ────────────────────────────────────────────────────────────────

TEST(NetworkManager, ConnectionRequest_SendsChallenge) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), MakeEndpoint());
    nm.Update();

    ASSERT_EQ(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets[0].first, t->sentPackets[0].first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::ConnectionChallenge);
}

TEST(NetworkManager, CorrectChallengeResponse_ClientEstablished) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool connected = false;
    nm.SetClientConnectedCallback([&](uint16_t, const EndPoint&) { connected = true; });

    CompleteHandshake(*t, nm, MakeEndpoint());

    EXPECT_TRUE(connected);
    EXPECT_EQ(nm.GetEstablishedCount(), 1u);
    EXPECT_EQ(nm.GetPendingCount(),     0u);
}

TEST(NetworkManager, WrongSalt_SendsDeniedAndRejectsClient) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), MakeEndpoint());
    nm.Update();
    t->sentPackets.clear();

    t->InjectPacket(MakeChallengeResponsePacket(0xBADBADBADBADBADull), MakeEndpoint());
    nm.Update();

    ASSERT_GE(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets[0].first, t->sentPackets[0].first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::ConnectionDenied);
    EXPECT_EQ(nm.GetEstablishedCount(), 0u);
}

TEST(NetworkManager, NetworkID_StartsAtOne) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const uint16_t id = CompleteHandshake(*t, nm, MakeEndpoint());
    EXPECT_EQ(id, 1u);
}

TEST(NetworkManager, TwoClients_GetDifferentNetworkIDs) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const uint16_t id1 = CompleteHandshake(*t, nm, MakeEndpoint(0x0100007F, 9000));
    const uint16_t id2 = CompleteHandshake(*t, nm, MakeEndpoint(0x0200007F, 9001));

    EXPECT_NE(id1, id2);
    EXPECT_EQ(nm.GetEstablishedCount(), 2u);
}

// ─── Game packet routing ──────────────────────────────────────────────────────

TEST(NetworkManager, GamePacket_FromUnknownClient_Ignored) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool received = false;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { received = true; });

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot), MakeEndpoint());
    nm.Update();

    EXPECT_FALSE(received);
}

TEST(NetworkManager, GamePacket_FromEstablishedClient_Delivered) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool received = false;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { received = true; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot, 1), ep);
    nm.Update();

    EXPECT_TRUE(received);
}

// ─── Duplicate detection ──────────────────────────────────────────────────────

TEST(NetworkManager, DuplicatePacket_DeliveredOnce) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    int deliveries = 0;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { ++deliveries; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    const auto pkt = MakeHeaderOnlyPacket(PacketType::Snapshot, 1);
    t->InjectPacket(pkt, ep);
    nm.Update();
    t->InjectPacket(pkt, ep); // exact duplicate
    nm.Update();

    EXPECT_EQ(deliveries, 1);
}

// ─── Stale Unreliable filter (P-3.4) ─────────────────────────────────────────

TEST(NetworkManager, StaleUnreliable_Dropped) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    int deliveries = 0;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { ++deliveries; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    // seq=5 arrives first, then seq=3 (older → stale)
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot, 5), ep);
    nm.Update();
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot, 3), ep);
    nm.Update();

    EXPECT_EQ(deliveries, 1); // only seq=5 delivered
}

TEST(NetworkManager, NewerUnreliable_Delivered) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    int deliveries = 0;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { ++deliveries; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot, 3), ep);
    nm.Update();
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot, 5), ep);
    nm.Update();

    EXPECT_EQ(deliveries, 2);
}

// ─── GetClientNetworkStats (P-3.4) ───────────────────────────────────────────

TEST(NetworkManager, GetClientNetworkStats_NulloptForUnknown) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    EXPECT_FALSE(nm.GetClientNetworkStats(MakeEndpoint()).has_value());
}

TEST(NetworkManager, GetClientNetworkStats_ValidForEstablished) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    const auto stats = nm.GetClientNetworkStats(ep);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->rtt, 0.0f);   // initialised at 100ms
    EXPECT_EQ(stats->sampleCount, 0); // no round-trips yet
}

// ─── Send ─────────────────────────────────────────────────────────────────────

TEST(NetworkManager, Send_Reliable_TransmitsPacket) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);
    t->sentPackets.clear();

    nm.Send(ep, {0x01, 0x02}, PacketType::Reliable);

    EXPECT_EQ(t->sentPackets.size(), 1u);
}

TEST(NetworkManager, Send_ToUnknownEndpoint_NoTransmit) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    nm.Send(MakeEndpoint(), {0x01}, PacketType::Reliable);

    EXPECT_TRUE(t->sentPackets.empty());
}

// ─── P-4.3: While-drain ───────────────────────────────────────────────────────

// A single Update() must drain ALL pending packets, not just one.
// This validates the if→while fix in NetworkManager::Update().
TEST(NetworkManager, WhileDrain_ProcessesAllPendingPacketsInOneUpdate) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    // Inject 3 ConnectionRequests from 3 distinct endpoints before any Update().
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), MakeEndpoint(0x0100007F, 9001));
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), MakeEndpoint(0x0200007F, 9002));
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), MakeEndpoint(0x0300007F, 9003));

    // Single Update() — all 3 must be processed.
    nm.Update();

    // All 3 should now be in pending (waiting for ChallengeResponse).
    EXPECT_EQ(nm.GetPendingCount(), 3u);
}

// ─── P-4.3: kMaxClients constant ─────────────────────────────────────────────

TEST(NetworkManager, kMaxClients_IsOneHundred) {
    EXPECT_EQ(NetworkManager::kMaxClients, 100u);
}

// ─── P-4.3: kMaxClients enforcement at ChallengeResponse ─────────────────────
//
// Scenario: server is at kMaxClients-1. Two clients both complete ConnectionRequest
// (server was not full at that point, so both got a Challenge). They both send
// their ChallengeResponse in the same Update() tick. Only the first should be
// accepted; the second must be denied even though it had a valid salt.
TEST(NetworkManager, ChallengeResponse_DeniedWhenServerFull) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    // Fill to kMaxClients - 1 using distinct endpoints.
    for (uint16_t i = 0; i < NetworkManager::kMaxClients - 1; ++i) {
        const EndPoint ep = MakeEndpoint(i + 1, static_cast<uint16_t>(9000 + i));
        CompleteHandshake(*t, nm, ep);
    }
    ASSERT_EQ(nm.GetEstablishedCount(), NetworkManager::kMaxClients - 1);

    // Two extra clients both send ConnectionRequest (server not yet full).
    const EndPoint ep1 = MakeEndpoint(0xFFFE, 8001);
    const EndPoint ep2 = MakeEndpoint(0xFFFF, 8002);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), ep1);
    nm.Update();
    auto& c1Pkt = t->sentPackets.back().first;
    BitReader cr1(c1Pkt, c1Pkt.size() * 8);
    PacketHeader::Read(cr1);
    const uint64_t salt1 = ChallengePayload::Read(cr1).salt;
    t->sentPackets.clear();

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), ep2);
    nm.Update();
    auto& c2Pkt = t->sentPackets.back().first;
    BitReader cr2(c2Pkt, c2Pkt.size() * 8);
    PacketHeader::Read(cr2);
    const uint64_t salt2 = ChallengePayload::Read(cr2).salt;
    t->sentPackets.clear();

    // Both respond in the same tick — server goes from kMaxClients-1 to kMaxClients
    // after the first, and must deny the second.
    t->InjectPacket(MakeChallengeResponsePacket(salt1), ep1);
    t->InjectPacket(MakeChallengeResponsePacket(salt2), ep2);
    nm.Update();

    // Exactly kMaxClients established — one of the two was denied.
    EXPECT_EQ(nm.GetEstablishedCount(), NetworkManager::kMaxClients);
    const bool ep1Ok = nm.GetClientNetworkStats(ep1).has_value();
    const bool ep2Ok = nm.GetClientNetworkStats(ep2).has_value();
    EXPECT_NE(ep1Ok, ep2Ok);  // exactly one accepted, one denied
}
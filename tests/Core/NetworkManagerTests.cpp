#include <gtest/gtest.h>
#include "Core/NetworkManager.h"
#include "Shared/Network/HandshakePackets.h"
#include "Shared/Serialization/CRC32.h"
#include "MockTransport.h"

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Tests;

// ─── CRC test helpers ─────────────────────────────────────────────────────────
// P-4.5: All outgoing packets now carry a 4-byte CRC32 trailer.
// WithCRC() appends it to injected packets; StripCRC() removes it when reading
// back sent packets before constructing a BitReader.

static std::vector<uint8_t> WithCRC(std::vector<uint8_t> data) {
    const uint32_t crc = ComputeCRC32(data);
    data.push_back(static_cast<uint8_t>((crc >>  0) & 0xFF));
    data.push_back(static_cast<uint8_t>((crc >>  8) & 0xFF));
    data.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
    return data;
}

static std::vector<uint8_t> StripCRC(const std::vector<uint8_t>& data) {
    return data.size() >= 4
        ? std::vector<uint8_t>(data.begin(), data.end() - 4)
        : data;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> MakeHeaderOnlyPacket(PacketType type, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h{};
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(type);
    h.Write(w);
    return WithCRC(w.GetCompressedData());
}

static std::vector<uint8_t> MakeChallengeResponsePacket(uint64_t salt, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h{};
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(PacketType::ChallengeResponse);
    h.Write(w);
    ChallengeResponsePayload{salt}.Write(w);
    return WithCRC(w.GetCompressedData());
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
    if (t.sentPackets.empty()) return 0;  // abort: no challenge sent

    // Step 2 — Read challenge salt (strip CRC trailer before parsing)
    const auto challengeStripped = StripCRC(t.sentPackets.back().first);
    BitReader cr(challengeStripped, challengeStripped.size() * 8);
    PacketHeader::Read(cr);
    const auto challenge = ChallengePayload::Read(cr);
    t.sentPackets.clear();

    // Step 3 — ChallengeResponse
    t.InjectPacket(MakeChallengeResponsePacket(challenge.salt), ep);
    nm.Update();
    EXPECT_GE(t.sentPackets.size(), 1u);
    if (t.sentPackets.empty()) return 0;  // abort: no accepted packet sent

    // Step 4 — Read NetworkID (strip CRC trailer before parsing)
    const auto acceptStripped = StripCRC(t.sentPackets.back().first);
    BitReader ar(acceptStripped, acceptStripped.size() * 8);
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
    const auto pkt0 = StripCRC(t->sentPackets[0].first);
    BitReader r(pkt0, pkt0.size() * 8);
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
    const auto deniedPkt = StripCRC(t->sentPackets[0].first);
    BitReader r(deniedPkt, deniedPkt.size() * 8);
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
    const auto c1Stripped = StripCRC(t->sentPackets.back().first);
    BitReader cr1(c1Stripped, c1Stripped.size() * 8);
    PacketHeader::Read(cr1);
    const uint64_t salt1 = ChallengePayload::Read(cr1).salt;
    t->sentPackets.clear();

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), ep2);
    nm.Update();
    const auto c2Stripped = StripCRC(t->sentPackets.back().first);
    BitReader cr2(c2Stripped, c2Stripped.size() * 8);
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

// ─── P-4.5 CRC32 Packet Integrity ────────────────────────────────────────────

// All packets sent by NetworkManager must carry a valid 4-byte CRC32 trailer.
TEST(NetworkManager, Send_AppendsCRC_TrailerValid) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);
    t->sentPackets.clear();

    nm.Send(ep, {0x01, 0x02, 0x03}, PacketType::Snapshot);

    ASSERT_EQ(t->sentPackets.size(), 1u);
    const auto& wire = t->sentPackets[0].first;
    ASSERT_GE(wire.size(), 4u);

    // Extract the last 4 bytes as little-endian CRC
    const size_t n = wire.size() - 4;
    const uint32_t trailerCRC =
          static_cast<uint32_t>(wire[n + 0])
        | static_cast<uint32_t>(wire[n + 1]) <<  8
        | static_cast<uint32_t>(wire[n + 2]) << 16
        | static_cast<uint32_t>(wire[n + 3]) << 24;

    // Compute CRC over the bytes preceding the trailer
    EXPECT_EQ(trailerCRC, ComputeCRC32(wire.data(), n));
}

// A received packet whose CRC has been tampered with must be silently discarded.
// The data callback must NOT fire, and the profiler must count 1 CRC error.
TEST(NetworkManager, Receive_BitFlip_Discarded) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool received = false;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { received = true; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    // Build a valid Snapshot packet (with CRC) then flip one bit in the payload
    auto pkt = MakeHeaderOnlyPacket(PacketType::Snapshot, 1);
    pkt[4] ^= 0x01u;  // tamper with a byte inside the header region

    t->InjectPacket(pkt, ep);
    nm.Update();

    EXPECT_FALSE(received);
    EXPECT_EQ(nm.GetProfilerSnapshot().crcErrors, 1u);
}

// A packet with a correct CRC must be accepted and delivered normally.
TEST(NetworkManager, Receive_ValidCRC_Accepted) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool received = false;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { received = true; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Snapshot, 1), ep);
    nm.Update();

    EXPECT_TRUE(received);
    EXPECT_EQ(nm.GetProfilerSnapshot().crcErrors, 0u);
}
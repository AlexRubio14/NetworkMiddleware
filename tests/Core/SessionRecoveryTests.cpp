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

static std::vector<uint8_t> MakeReconnectionRequestPacket(uint16_t oldNetworkID, uint64_t token, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h;
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(PacketType::ReconnectionRequest);
    h.Write(w);
    ReconnectionRequestPayload{oldNetworkID, token}.Write(w);
    return w.GetCompressedData();
}

static EndPoint MakeEndpoint(uint32_t addr = 0x0100007F, uint16_t port = 9000) {
    return EndPoint{addr, port};
}

struct HandshakeResult {
    uint16_t networkID;
    uint64_t token;
};

// Completes the full handshake and returns {networkID, reconnectionToken}.
static HandshakeResult CompleteHandshake(MockTransport& t, NetworkManager& nm, const EndPoint& ep) {
    t.InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), ep);
    nm.Update();

    auto& challengePkt = t.sentPackets.back().first;
    BitReader cr(challengePkt, challengePkt.size() * 8);
    PacketHeader::Read(cr);
    const auto challenge = ChallengePayload::Read(cr);
    t.sentPackets.clear();

    t.InjectPacket(MakeChallengeResponsePacket(challenge.salt), ep);
    nm.Update();

    auto& acceptPkt = t.sentPackets.back().first;
    BitReader ar(acceptPkt, acceptPkt.size() * 8);
    PacketHeader::Read(ar);
    const auto accepted = ConnectionAcceptedPayload::Read(ar);
    t.sentPackets.clear();

    return {accepted.networkID, accepted.reconnectionToken};
}

// ─── Heartbeat ───────────────────────────────────────────────────────────────

TEST(SessionRecovery, Heartbeat_FromEstablishedClient_NoDataCallback) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool received = false;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) { received = true; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    // Heartbeat is keepalive only — must not fire the data callback
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Heartbeat, 1), ep);
    nm.Update();

    EXPECT_FALSE(received);
}

TEST(SessionRecovery, HeartbeatSent_WhenNoOutgoingFor1s) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);
    t->sentPackets.clear();

    // Simulate 2 seconds of silence (no outgoing from server)
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(2));

    ASSERT_GE(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets.front().first, t->sentPackets.front().first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::Heartbeat);
}

// ─── Graceful Disconnect ──────────────────────────────────────────────────────

TEST(SessionRecovery, Disconnect_RemovesClient_FiresCallback) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool disconnected = false;
    nm.SetClientDisconnectedCallback([&](uint16_t, const EndPoint&) { disconnected = true; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Disconnect, 1), ep);
    nm.Update();

    EXPECT_TRUE(disconnected);
    EXPECT_EQ(nm.GetEstablishedCount(), 0u);
}

TEST(SessionRecovery, Disconnect_FromUnknownClient_Ignored) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool disconnected = false;
    nm.SetClientDisconnectedCallback([&](uint16_t, const EndPoint&) { disconnected = true; });

    // No handshake — Disconnect from unknown endpoint
    t->InjectPacket(MakeHeaderOnlyPacket(PacketType::Disconnect), MakeEndpoint());
    nm.Update();

    EXPECT_FALSE(disconnected);
    EXPECT_EQ(nm.GetEstablishedCount(), 0u);
}

// ─── Session Timeout & Zombie ────────────────────────────────────────────────

TEST(SessionRecovery, SessionTimeout_MarksClientAsZombie) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    // Simulate 11 seconds of silence (no incoming from client)
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(11));

    EXPECT_TRUE(nm.IsClientZombie(ep));
    EXPECT_EQ(nm.GetEstablishedCount(), 1u); // Still in map as zombie
}

TEST(SessionRecovery, NoTimeout_WithinSessionInterval) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    // 5 seconds — should not time out yet (kSessionTimeout = 10s)
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(5));

    EXPECT_FALSE(nm.IsClientZombie(ep));
}

TEST(SessionRecovery, ZombieExpiry_RemovesClient_FiresCallback) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    bool disconnected = false;
    nm.SetClientDisconnectedCallback([&](uint16_t, const EndPoint&) { disconnected = true; });

    const EndPoint ep = MakeEndpoint();
    CompleteHandshake(*t, nm, ep);

    const auto base = std::chrono::steady_clock::now();

    // Step 1: become zombie at t+11s
    nm.ProcessSessionKeepAlive(base + std::chrono::seconds(11));
    ASSERT_TRUE(nm.IsClientZombie(ep));

    // Step 2: zombie expires at t+11s+121s
    nm.ProcessSessionKeepAlive(base + std::chrono::seconds(11 + 121));

    EXPECT_TRUE(disconnected);
    EXPECT_EQ(nm.GetEstablishedCount(), 0u);
}

// ─── Reconnection ─────────────────────────────────────────────────────────────

TEST(SessionRecovery, Reconnect_ValidToken_EstablishedAtNewEndpoint) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint oldEp = MakeEndpoint(0x0100007F, 9000);
    const EndPoint newEp = MakeEndpoint(0x0200007F, 9001);

    auto [networkID, token] = CompleteHandshake(*t, nm, oldEp);

    // Force zombie
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(11));
    ASSERT_TRUE(nm.IsClientZombie(oldEp));

    // Reconnect from new endpoint
    t->InjectPacket(MakeReconnectionRequestPacket(networkID, token), newEp);
    nm.Update();

    EXPECT_EQ(nm.GetEstablishedCount(), 1u);
    EXPECT_FALSE(nm.IsClientZombie(newEp));
}

TEST(SessionRecovery, Reconnect_InvalidToken_Rejected) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint oldEp = MakeEndpoint(0x0100007F, 9000);
    const EndPoint newEp = MakeEndpoint(0x0200007F, 9001);

    auto [networkID, token] = CompleteHandshake(*t, nm, oldEp);

    // Force zombie
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(11));
    ASSERT_TRUE(nm.IsClientZombie(oldEp));

    // Reconnect with wrong token → rejected (ConnectionDenied)
    t->InjectPacket(MakeReconnectionRequestPacket(networkID, token ^ 0xDEADBEEFDEADBEEFull), newEp);
    nm.Update();

    ASSERT_GE(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets.front().first, t->sentPackets.front().first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::ConnectionDenied);

    // Still zombie at old endpoint; new endpoint not established
    EXPECT_TRUE(nm.IsClientZombie(oldEp));
}

TEST(SessionRecovery, Reconnect_NonZombieNetworkID_Rejected) {
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep    = MakeEndpoint(0x0100007F, 9000);
    const EndPoint newEp = MakeEndpoint(0x0200007F, 9001);

    auto [networkID, token] = CompleteHandshake(*t, nm, ep);

    // Client is NOT zombie — reconnection must be rejected
    t->InjectPacket(MakeReconnectionRequestPacket(networkID, token), newEp);
    nm.Update();

    ASSERT_GE(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets.front().first, t->sentPackets.front().first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::ConnectionDenied);

    EXPECT_EQ(nm.GetEstablishedCount(), 1u);
    // Confirm it is the ORIGINAL endpoint still active, not the new one
    EXPECT_TRUE(nm.GetClientNetworkStats(ep).has_value());
    EXPECT_FALSE(nm.GetClientNetworkStats(newEp).has_value());
}

TEST(SessionRecovery, Reconnect_SameEndpoint_Succeeds) {
    // Regression: a zombie client that hasn't changed IP/port must be able to
    // reconnect via ReconnectionRequest (same endpoint == just re-activating the slot).
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep = MakeEndpoint(0x0100007F, 9000);

    auto [networkID, token] = CompleteHandshake(*t, nm, ep);

    // Force zombie
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(11));
    ASSERT_TRUE(nm.IsClientZombie(ep));
    t->sentPackets.clear();

    // Reconnect from the SAME endpoint — must succeed
    t->InjectPacket(MakeReconnectionRequestPacket(networkID, token), ep);
    nm.Update();

    EXPECT_EQ(nm.GetEstablishedCount(), 1u);
    EXPECT_FALSE(nm.IsClientZombie(ep));

    // Must have sent ConnectionAccepted (not ConnectionDenied)
    ASSERT_GE(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets.front().first, t->sentPackets.front().first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::ConnectionAccepted);
}

TEST(SessionRecovery, Reconnect_OccupiedEndpoint_Rejected) {
    // If another active client already occupies the new endpoint,
    // the reconnection must be rejected without corrupting either session.
    auto t = std::make_shared<MockTransport>();
    NetworkManager nm(t);

    const EndPoint ep1 = MakeEndpoint(0x0100007F, 9000); // zombie
    const EndPoint ep2 = MakeEndpoint(0x0200007F, 9001); // active, occupies target endpoint

    auto [networkID, token] = CompleteHandshake(*t, nm, ep1);
    CompleteHandshake(*t, nm, ep2); // ep2 is now active

    // Make ep1 zombie
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(11));
    ASSERT_TRUE(nm.IsClientZombie(ep1));

    // Try to reconnect ep1 from ep2 — ep2 is already occupied
    t->InjectPacket(MakeReconnectionRequestPacket(networkID, token), ep2);
    nm.Update();

    ASSERT_GE(t->sentPackets.size(), 1u);
    BitReader r(t->sentPackets.front().first, t->sentPackets.front().first.size() * 8);
    const auto h = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(h.type), PacketType::ConnectionDenied);

    // ep1 zombie must still exist (not erased before validation)
    EXPECT_TRUE(nm.IsClientZombie(ep1));
    // ep2 active session must be unaffected
    EXPECT_TRUE(nm.GetClientNetworkStats(ep2).has_value());
    EXPECT_EQ(nm.GetEstablishedCount(), 2u);
}

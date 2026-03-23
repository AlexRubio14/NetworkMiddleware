#include <gtest/gtest.h>
#include "Core/GameWorld.h"
#include "Core/NetworkManager.h"
#include "Shared/Network/HandshakePackets.h"
#include "Shared/Network/InputPackets.h"
#include "Shared/Serialization/BitReader.h"
#include "Shared/Serialization/BitWriter.h"
#include "MockTransport.h"
#include <cmath>

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Tests;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static EndPoint MakeEp(uint16_t port = 9001) {
    return EndPoint{0x0100007F, port};
}

static std::vector<uint8_t> MakeHeaderOnly(PacketType type, uint16_t seq = 0,
                                            uint16_t ack = 0) {
    BitWriter w;
    PacketHeader h;
    h.sequence = seq;
    h.ack      = ack;
    h.type     = static_cast<uint8_t>(type);
    h.Write(w);
    return w.GetCompressedData();
}

static std::vector<uint8_t> MakeChallengeResponse(uint64_t salt, uint16_t seq = 0) {
    BitWriter w;
    PacketHeader h;
    h.sequence = seq;
    h.type     = static_cast<uint8_t>(PacketType::ChallengeResponse);
    h.Write(w);
    ChallengeResponsePayload{salt}.Write(w);
    return w.GetCompressedData();
}

// Completes full handshake and returns the assigned NetworkID.
static uint16_t DoHandshake(MockTransport& t, NetworkManager& nm, const EndPoint& ep) {
    t.InjectPacket(MakeHeaderOnly(PacketType::ConnectionRequest), ep);
    nm.Update();

    auto& cpkt = t.sentPackets.back().first;
    BitReader cr(cpkt, cpkt.size() * 8);
    PacketHeader::Read(cr);
    const auto challenge = ChallengePayload::Read(cr);
    t.sentPackets.clear();

    t.InjectPacket(MakeChallengeResponse(challenge.salt), ep);
    nm.Update();

    auto& apkt = t.sentPackets.back().first;
    BitReader ar(apkt, apkt.size() * 8);
    PacketHeader::Read(ar);
    const auto accepted = ConnectionAcceptedPayload::Read(ar);
    t.sentPackets.clear();

    return accepted.networkID;
}

// ─── GameWorld unit tests ─────────────────────────────────────────────────────

TEST(GameWorld, HeroAdded_StartsAtOrigin) {
    GameWorld gw;
    gw.AddHero(1);
    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_FLOAT_EQ(s->x, 0.0f);
    EXPECT_FLOAT_EQ(s->y, 0.0f);
}

TEST(GameWorld, AddHero_IdempotentOnDuplicateID) {
    GameWorld gw;
    gw.AddHero(1);
    gw.AddHero(1);  // must not crash or create a second entity
    EXPECT_NE(gw.GetHeroState(1), nullptr);
}

TEST(GameWorld, RemoveHero_ReturnsNullAfterRemoval) {
    GameWorld gw;
    gw.AddHero(2);
    gw.RemoveHero(2);
    EXPECT_EQ(gw.GetHeroState(2), nullptr);
}

TEST(GameWorld, RemoveHero_NonExistentID_NoCrash) {
    GameWorld gw;
    EXPECT_NO_THROW(gw.RemoveHero(999));
}

TEST(GameWorld, GetHeroState_UnknownID_ReturnsNull) {
    GameWorld gw;
    EXPECT_EQ(gw.GetHeroState(42), nullptr);
}

TEST(GameWorld, ApplyInput_MovesHeroExactly) {
    // 10 ticks at dirX=1, dirY=0, dt=0.01 → x = 10 × 100 × 0.01 = 10.0
    GameWorld gw;
    gw.AddHero(1);

    const InputPayload right{1.0f, 0.0f, 0};
    for (int i = 0; i < 10; ++i)
        gw.ApplyInput(1, right, 0.01f);

    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_NEAR(s->x, 10.0f, 0.001f);
    EXPECT_NEAR(s->y,  0.0f, 0.001f);
}

TEST(GameWorld, ApplyInput_DiagonalMovement) {
    // dirX=1, dirY=1, dt=0.01 → each axis = 100 × 0.01 = 1.0
    GameWorld gw;
    gw.AddHero(1);

    gw.ApplyInput(1, {1.0f, 1.0f, 0}, 0.01f);

    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_NEAR(s->x, 1.0f, 0.001f);
    EXPECT_NEAR(s->y, 1.0f, 0.001f);
}

TEST(GameWorld, AntiCheat_InputClamped_OverNormalized) {
    // dirX=999 must be treated as 1.0; displacement = 1 × 100 × 0.01 = 1.0 (not 999)
    GameWorld gw;
    gw.AddHero(1);
    gw.ApplyInput(1, {999.0f, 0.0f, 0}, 0.01f);

    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_NEAR(s->x, 1.0f, 0.001f);
}

TEST(GameWorld, AntiCheat_ClampsToBounds) {
    // Place hero near right boundary, push further right → must clamp to kMapBound
    GameWorld gw;
    gw.AddHero(1);

    // Move to 498 first (49.8 ticks at speed 100, dt=0.01 → 49.8 × 1.0 = 49.8...
    // simpler: just apply enough ticks to guarantee clamping)
    const InputPayload right{1.0f, 0.0f, 0};
    for (int i = 0; i < 600; ++i)   // 600 × 1.0 = 600 units > kMapBound=500
        gw.ApplyInput(1, right, 0.01f);

    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_LE(s->x, GameWorld::kMapBound);
    EXPECT_GE(s->x, -GameWorld::kMapBound);
}

TEST(GameWorld, AntiCheat_ClampsNegativeBounds) {
    GameWorld gw;
    gw.AddHero(1);

    const InputPayload left{-1.0f, 0.0f, 0};
    for (int i = 0; i < 600; ++i)
        gw.ApplyInput(1, left, 0.01f);

    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_GE(s->x, -GameWorld::kMapBound);
}

TEST(GameWorld, ApplyInput_ZeroInput_DoesNotMove) {
    GameWorld gw;
    gw.AddHero(1);
    gw.ApplyInput(1, {0.0f, 0.0f, 0}, 0.01f);

    const auto* s = gw.GetHeroState(1);
    ASSERT_NE(s, nullptr);
    EXPECT_FLOAT_EQ(s->x, 0.0f);
    EXPECT_FLOAT_EQ(s->y, 0.0f);
}

TEST(GameWorld, Tick_DoesNotCrash) {
    GameWorld gw;
    gw.AddHero(1);
    EXPECT_NO_THROW(gw.Tick(0.01f));
}

// ─── SendSnapshot / TickID integration tests ─────────────────────────────────

TEST(GameWorld, SendSnapshot_ContainsTickID) {
    // Full integration: connect a client, call SendSnapshot, parse the packet
    // and verify the first 32 bits equal the tickID we passed.

    auto transport = std::make_shared<MockTransport>();
    NetworkManager nm(transport);

    const EndPoint ep = MakeEp();
    const uint16_t netID = DoHandshake(*transport, nm, ep);

    // Build a HeroState for the snapshot
    Data::HeroState state;
    state.networkID = netID;
    state.dirtyMask = 0xFFFFFFFF;  // full sync on first send

    const uint32_t testTickID = 42;
    nm.SendSnapshot(ep, state, testTickID);
    ASSERT_FALSE(transport->sentPackets.empty());

    // Find the Snapshot packet (last sent)
    const auto& pkt = transport->sentPackets.back().first;
    BitReader r(pkt, pkt.size() * 8);
    const auto hdr = PacketHeader::Read(r);
    EXPECT_EQ(static_cast<PacketType>(hdr.type), PacketType::Snapshot);

    // First 32 bits of payload = tickID
    const uint32_t readTickID = r.ReadBits(32);
    EXPECT_EQ(readTickID, testTickID);
}

TEST(GameWorld, SendSnapshot_TickIDIncrements) {
    auto transport = std::make_shared<MockTransport>();
    NetworkManager nm(transport);

    const EndPoint ep = MakeEp(9002);
    const uint16_t netID = DoHandshake(*transport, nm, ep);

    Data::HeroState state;
    state.networkID = netID;
    state.dirtyMask = 0xFFFFFFFF;

    for (uint32_t tick = 0; tick < 5; ++tick) {
        transport->sentPackets.clear();
        nm.SendSnapshot(ep, state, tick);

        ASSERT_FALSE(transport->sentPackets.empty());
        const auto& pkt = transport->sentPackets.back().first;
        BitReader r(pkt, pkt.size() * 8);
        PacketHeader::Read(r);

        const uint32_t readTickID = r.ReadBits(32);
        EXPECT_EQ(readTickID, tick) << "Mismatch at tick=" << tick;
    }
}

TEST(GameWorld, ForEachEstablished_ReceivesBufferedInput) {
    auto transport = std::make_shared<MockTransport>();
    NetworkManager nm(transport);

    const EndPoint ep = MakeEp(9003);
    const uint16_t netID = DoHandshake(*transport, nm, ep);
    (void)netID;
    transport->sentPackets.clear();

    // Inject an Input packet
    BitWriter w;
    PacketHeader h;
    h.sequence = 1;
    h.type     = static_cast<uint8_t>(PacketType::Input);
    h.Write(w);
    InputPayload{0.5f, -0.5f, 0}.Write(w);
    transport->InjectPacket(w.GetCompressedData(), ep);

    nm.Update();  // Input is buffered, NOT delivered via data callback

    bool gotInput = false;
    nm.ForEachEstablished([&](uint16_t, const EndPoint&, const InputPayload* inp) {
        if (inp) {
            gotInput = true;
            EXPECT_NEAR(inp->dirX,  0.5f, 0.01f);
            EXPECT_NEAR(inp->dirY, -0.5f, 0.01f);
        }
    });

    EXPECT_TRUE(gotInput);
}

TEST(GameWorld, ForEachEstablished_InputClearedAfterCallback) {
    auto transport = std::make_shared<MockTransport>();
    NetworkManager nm(transport);

    const EndPoint ep = MakeEp(9004);
    DoHandshake(*transport, nm, ep);
    transport->sentPackets.clear();

    // Inject Input
    BitWriter w;
    PacketHeader h;
    h.sequence = 1;
    h.type     = static_cast<uint8_t>(PacketType::Input);
    h.Write(w);
    InputPayload{1.0f, 0.0f, 0}.Write(w);
    transport->InjectPacket(w.GetCompressedData(), ep);
    nm.Update();

    // First call consumes the input
    nm.ForEachEstablished([](uint16_t, const EndPoint&, const InputPayload*) {});

    // Second call (snapshot phase) must get nullptr input
    int nonNullCount = 0;
    nm.ForEachEstablished([&](uint16_t, const EndPoint&, const InputPayload* inp) {
        if (inp) ++nonNullCount;
    });

    EXPECT_EQ(nonNullCount, 0);
}

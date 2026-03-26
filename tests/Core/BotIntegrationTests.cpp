#include <gtest/gtest.h>
#include "Core/BotClient.h"
#include "Core/NetworkManager.h"
#include "Core/MockTransport.h"
#include "Shared/Network/HandshakePackets.h"
#include "Shared/Serialization/BitWriter.h"
#include "Shared/Serialization/CRC32.h"

using namespace NetworkMiddleware;
using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Tests;

// ─── Endpoints ───────────────────────────────────────────────────────────────

// The bot pretends to be at kBotEp; the server is at kServerEp.
static const EndPoint kServerEp{0x0100007F, 7777};
static const EndPoint kBotEp   {0x0200007F, 9000};

// ─── Routing helper ──────────────────────────────────────────────────────────
//
// Routes all packets pending in each MockTransport's sentPackets queue to the
// other side's incomingQueue.  Must be called after every Update() or send
// so the next side's Update() sees the packets.
//
//   Bot  → Server: inject into serverT with sender = kBotEp
//   Server → Bot:  inject into botT   with sender = kServerEp

static void Route(MockTransport& serverT, MockTransport& botT) {
    for (auto& [data, to] : botT.sentPackets)
        serverT.InjectPacket(data, kBotEp);
    botT.sentPackets.clear();

    for (auto& [data, to] : serverT.sentPackets)
        botT.InjectPacket(data, kServerEp);
    serverT.sentPackets.clear();
}

// ─── Handshake helper ────────────────────────────────────────────────────────
//
// Drives both state machines through the full 3-way handshake:
//   ConnectionRequest → ConnectionChallenge → ChallengeResponse → ConnectionAccepted
//
// After this call:
//   bot.GetState()           == BotClient::State::Established
//   nm.GetEstablishedCount() == 1

static void DoFullHandshake(MockTransport& serverT, NetworkManager& nm,
                             MockTransport& botT,   BotClient& bot) {
    // Bot sends ConnectionRequest
    bot.Connect();
    Route(serverT, botT);

    // Server processes ConnectionRequest, sends Challenge
    nm.Update();
    Route(serverT, botT);

    // Bot receives Challenge, sends ChallengeResponse
    bot.Update();
    Route(serverT, botT);

    // Server processes ChallengeResponse, sends ConnectionAccepted
    nm.Update();
    Route(serverT, botT);

    // Bot receives ConnectionAccepted → Established
    bot.Update();
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(BotIntegration, Handshake_BotReachesEstablished) {
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);

    EXPECT_EQ(bot.GetState(), BotClient::State::Established);
    EXPECT_EQ(nm.GetEstablishedCount(), 1u);
    EXPECT_NE(bot.GetNetworkID(), 0u);
    EXPECT_NE(bot.GetReconnectionToken(), 0u);
}

// P-3.7: Input packets are now buffered as pendingInput (consumed via ForEachEstablished)
// rather than fired through the data callback. These tests verify the new behaviour.

TEST(BotIntegration, SendInput_ServerBuffersInputViaPendingInput) {
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);

    constexpr int kInputCount = 10;
    int delivered = 0;

    for (int i = 0; i < kInputCount; ++i) {
        bot.SendInput(1.0f, 0.0f, InputButtons::kAttack);
        Route(*serverT, *botT);
        nm.Update();

        nm.ForEachEstablished([&](uint16_t, const EndPoint&, const InputPayload* inp) {
            if (inp) ++delivered;
        });

        Route(*serverT, *botT);
    }

    EXPECT_EQ(delivered, kInputCount);
}

TEST(BotIntegration, SendInput_DataCallbackNotFiredForInputPackets) {
    // Regression: Input packets must NOT reach m_onDataReceived anymore (P-3.7 change).
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);

    int callbackFired = 0;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) {
        ++callbackFired;
    });

    for (int i = 0; i < 5; ++i) {
        bot.SendInput(0.0f, 1.0f, InputButtons::kAbility1);
        Route(*serverT, *botT);
        nm.Update();
        Route(*serverT, *botT);
    }

    EXPECT_EQ(callbackFired, 0);
}

TEST(BotIntegration, SendInput_AllInputsDeliveredViaForEachEstablished) {
    // Verifies that every input sent by the bot is delivered exactly once via
    // ForEachEstablished (one per tick). The old name "InputSequence_StrictlyIncreasing"
    // was renamed because sequence ordering is no longer observable here — inputs are
    // buffered as pendingInput and the header is not re-exposed.
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);

    int deliveredCount = 0;
    for (int i = 0; i < 5; ++i) {
        bot.SendInput(0.0f, 1.0f, InputButtons::kAbility1);
        Route(*serverT, *botT);
        nm.Update();

        nm.ForEachEstablished([&](uint16_t, const EndPoint&, const InputPayload* inp) {
            if (inp) ++deliveredCount;
        });

        Route(*serverT, *botT);
    }

    EXPECT_EQ(deliveredCount, 5);
}

// ─── P-4.5: CRC32 integrity ───────────────────────────────────────────────────

// A packet with an all-zeros CRC trailer must be silently discarded by BotClient.
// The bot's state must remain Established (not crash, not disconnect).
TEST(BotIntegration, BotClient_CorruptedPacket_Discarded) {
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);
    ASSERT_EQ(bot.GetState(), BotClient::State::Established);

    // Build a valid Heartbeat packet body (header only, no payload).
    BitWriter w;
    PacketHeader h;
    h.sequence = 10;
    h.type     = static_cast<uint8_t>(PacketType::Heartbeat);
    h.Write(w);
    std::vector<uint8_t> pkt = w.GetCompressedData();

    // Append an all-zeros CRC trailer — this is NOT the real CRC.
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    // Inject the corrupted packet directly into the bot's receive queue.
    botT->InjectPacket(pkt, kServerEp);

    // BotClient::Update() must discard the packet and remain Established.
    bot.Update();

    EXPECT_EQ(bot.GetState(), BotClient::State::Established);
}

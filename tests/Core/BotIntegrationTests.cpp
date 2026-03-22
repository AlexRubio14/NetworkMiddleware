#include <gtest/gtest.h>
#include "Core/BotClient.h"
#include "Core/NetworkManager.h"
#include "Core/MockTransport.h"

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

TEST(BotIntegration, SendInput_ServerFiresDataCallback) {
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);

    int received = 0;
    nm.SetDataCallback([&](const PacketHeader&, BitReader&, const EndPoint&) {
        ++received;
    });

    constexpr int kInputCount = 10;
    for (int i = 0; i < kInputCount; ++i) {
        bot.SendInput(1.0f, 0.0f, InputButtons::kAttack);
        Route(*serverT, *botT);
        nm.Update();
        Route(*serverT, *botT);
    }

    EXPECT_EQ(received, kInputCount);
}

TEST(BotIntegration, InputSequence_StrictlyIncreasing) {
    auto serverT = std::make_shared<MockTransport>();
    auto botT    = std::make_shared<MockTransport>();
    NetworkManager nm(serverT);
    BotClient      bot(botT, kServerEp);

    DoFullHandshake(*serverT, nm, *botT, bot);

    std::vector<uint16_t> sequences;
    nm.SetDataCallback([&](const PacketHeader& h, BitReader&, const EndPoint&) {
        sequences.push_back(h.sequence);
    });

    for (int i = 0; i < 5; ++i) {
        bot.SendInput(0.0f, 1.0f, InputButtons::kAbility1);
        Route(*serverT, *botT);
        nm.Update();
        Route(*serverT, *botT);
    }

    ASSERT_EQ(sequences.size(), 5u);
    for (size_t i = 1; i < sequences.size(); ++i)
        EXPECT_GT(sequences[i], sequences[i - 1]);
}

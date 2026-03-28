#ifdef __linux__

#include <gtest/gtest.h>
#include "../../Transport/RawUDPTransport.h"
#include "../../Shared/NetworkAddress.h"

#include <thread>
#include <chrono>

using namespace NetworkMiddleware::Transport;
using namespace NetworkMiddleware::Shared;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static EndPoint Loopback(uint16_t port) {
    EndPoint ep;
    ep.address = 0x7F000001u;  // 127.0.0.1 in host byte order
    ep.port    = port;
    return ep;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(RawUDPTransport, InitializeBindsSocket) {
    RawUDPTransport t;
    EXPECT_NO_THROW(t.Initialize(0));  // port 0 = kernel assigns ephemeral port
}

TEST(RawUDPTransport, CloseIsIdempotent) {
    RawUDPTransport t;
    ASSERT_NO_THROW(t.Initialize(0));
    EXPECT_NO_THROW(t.Close());
    EXPECT_NO_THROW(t.Close());  // second call must not crash
}

TEST(RawUDPTransport, FlushEmptyQueueIsNoOp) {
    RawUDPTransport t;
    ASSERT_NO_THROW(t.Initialize(0));
    EXPECT_NO_THROW(t.Flush());  // empty queue — must not crash or assert
}

TEST(RawUDPTransport, SendAndReceiveLoopback) {
    // Sender on port A, receiver on port B.
    // Send() queues the message; Flush() dispatches via sendmmsg.
    // Receiver drains with Receive().

    RawUDPTransport sender;
    RawUDPTransport receiver;

    ASSERT_NO_THROW(sender.Initialize(0));    // ephemeral sender port
    ASSERT_NO_THROW(receiver.Initialize(19876));

    const std::vector<uint8_t> payload = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
    sender.Send(payload, Loopback(19876));
    sender.Flush();

    // Give the kernel a moment to route the loopback packet.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::vector<uint8_t> received;
    EndPoint senderEp;
    const bool ok = receiver.Receive(received, senderEp);

    ASSERT_TRUE(ok);
    EXPECT_EQ(received, payload);
    EXPECT_EQ(senderEp.address, 0x7F000001u);  // 127.0.0.1
}

TEST(RawUDPTransport, SendMultipleAndFlushOnce) {
    // Verifies that multiple Send() calls are all dispatched by a single Flush().
    RawUDPTransport sender;
    RawUDPTransport receiver;

    ASSERT_NO_THROW(sender.Initialize(0));
    ASSERT_NO_THROW(receiver.Initialize(19877));

    const std::vector<uint8_t> pkt1 = { 0x01 };
    const std::vector<uint8_t> pkt2 = { 0x02, 0x03 };
    const std::vector<uint8_t> pkt3 = { 0x04, 0x05, 0x06 };

    sender.Send(pkt1, Loopback(19877));
    sender.Send(pkt2, Loopback(19877));
    sender.Send(pkt3, Loopback(19877));
    sender.Flush();  // 1 sendmmsg call dispatches all 3

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    int received = 0;
    std::vector<uint8_t> buf;
    EndPoint ep;
    while (receiver.Receive(buf, ep)) ++received;

    EXPECT_EQ(received, 3);
}

TEST(RawUDPTransport, ReceiveReturnsFalseWhenEmpty) {
    RawUDPTransport t;
    ASSERT_NO_THROW(t.Initialize(0));

    std::vector<uint8_t> buf;
    EndPoint ep;
    EXPECT_FALSE(t.Receive(buf, ep));  // non-blocking: no packets → false immediately
}

#endif  // __linux__

#pragma once
#include "Shared/ITransport.h"
#include <queue>
#include <vector>
#include <utility>

namespace NetworkMiddleware::Tests {

    struct IncomingPacket {
        std::vector<uint8_t> data;
        Shared::EndPoint     sender;
    };

    // ITransport stub for unit tests.
    // Inject packets via InjectPacket(); capture sent packets in sentPackets.
    class MockTransport : public Shared::ITransport {
    public:
        std::queue<IncomingPacket>                                          incomingQueue;
        std::vector<std::pair<std::vector<uint8_t>, Shared::EndPoint>>     sentPackets;

        bool Initialize(uint16_t) override { return true; }
        void Close()              override {}

        bool Receive(std::vector<uint8_t>& buffer, Shared::EndPoint& sender) override {
            if (incomingQueue.empty()) return false;
            auto& pkt = incomingQueue.front();
            buffer = pkt.data;
            sender = pkt.sender;
            incomingQueue.pop();
            return true;
        }

        void Send(const std::vector<uint8_t>& buffer, const Shared::EndPoint& recipient) override {
            sentPackets.push_back({buffer, recipient});
        }

        void InjectPacket(const std::vector<uint8_t>& data, const Shared::EndPoint& sender) {
            incomingQueue.push({data, sender});
        }
    };

} // namespace NetworkMiddleware::Tests
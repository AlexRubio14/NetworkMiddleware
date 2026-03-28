#pragma once
#ifdef __linux__

#include "ITransport.h"
#include <netinet/in.h>
#include <unordered_map>
#include <vector>

namespace NetworkMiddleware::Transport {

    // P-6.1: Native Linux UDP transport using POSIX sockets.
    // Send() queues outgoing messages; Flush() dispatches them all via
    // sendmmsg — 1 syscall per tick instead of 1 per client.
    class RawUDPTransport : public Shared::ITransport {
    private:
        struct OutMessage {
            std::vector<uint8_t> buffer;
            sockaddr_in          addr{};
        };

        int                                           m_sockfd = -1;
        std::unordered_map<uint32_t, sockaddr_in>     m_addrCache;
        std::vector<OutMessage>                       m_sendQueue;

        static constexpr int kSendBufSize = 4 * 1024 * 1024;  // 4 MB
        static constexpr int kRecvBufSize = 4 * 1024 * 1024;  // 4 MB

        const sockaddr_in& GetOrBuildAddr(const Shared::EndPoint& ep);

    public:
        RawUDPTransport() = default;
        ~RawUDPTransport() override { Close(); }

        bool Initialize(uint16_t port) override;
        bool Receive(std::vector<uint8_t>& buffer, Shared::EndPoint& sender) override;
        void Send(const std::vector<uint8_t>& buffer, const Shared::EndPoint& recipient) override;
        void Flush() override;
        void Close() override;
    };

}  // namespace NetworkMiddleware::Transport

#endif  // __linux__

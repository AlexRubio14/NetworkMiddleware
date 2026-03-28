#pragma once
#ifdef __linux__

#include "ITransport.h"
#include <mutex>
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

        // Cache keyed by full EndPoint (address + port) so distinct clients on the
        // same IP (e.g. all bots on 127.0.0.1) never share a cached sockaddr_in.
        struct EndPointHash {
            std::size_t operator()(const Shared::EndPoint& ep) const noexcept {
                return std::hash<uint64_t>{}(
                    (static_cast<uint64_t>(ep.address) << 16) | ep.port);
            }
        };
        std::unordered_map<Shared::EndPoint, sockaddr_in, EndPointHash> m_addrCache;  // main thread only

        // P-6.2: Swap-buffer — producer (main) writes m_sendQueue, consumer
        // (send thread) atomically swaps and processes m_flushQueue.
        // m_queueMutex is held only during the O(1) swap, not during sendmmsg.
        std::vector<OutMessage>  m_sendQueue;
        std::vector<OutMessage>  m_flushQueue;
        std::mutex               m_queueMutex;

        static constexpr int kSendBufSize = 4 * 1024 * 1024;  // 4 MB
        static constexpr int kRecvBufSize = 4 * 1024 * 1024;  // 4 MB

        // Main-thread only — no mutex needed (m_addrCache is never touched by Flush).
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

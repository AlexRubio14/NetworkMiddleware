#ifdef __linux__

#include "RawUDPTransport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace NetworkMiddleware::Transport {

    // ── Initialize ────────────────────────────────────────────────────────────

    bool RawUDPTransport::Initialize(uint16_t port)
    {
        m_sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sockfd == -1)
            throw std::runtime_error("RawUDP: socket() failed: " + std::string(strerror(errno)));

        // Non-blocking: Receive() returns immediately when no data is available.
        if (::fcntl(m_sockfd, F_SETFL, O_NONBLOCK) == -1) {
            Close();
            throw std::runtime_error("RawUDP: fcntl(O_NONBLOCK) failed: " + std::string(strerror(errno)));
        }

        // Increase kernel send/receive buffers to absorb bursts at 100Hz x 49 clients.
        // Without this, the kernel may silently drop packets when the buffer saturates.
        const int sendBuf = kSendBufSize;
        const int recvBuf = kRecvBufSize;
        ::setsockopt(m_sockfd, SOL_SOCKET, SO_SNDBUF, &sendBuf, sizeof(sendBuf));
        ::setsockopt(m_sockfd, SOL_SOCKET, SO_RCVBUF, &recvBuf, sizeof(recvBuf));

        sockaddr_in serverAddr{};
        serverAddr.sin_family      = AF_INET;
        serverAddr.sin_port        = htons(port);
        serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(m_sockfd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
            Close();
            throw std::runtime_error("RawUDP: bind() on port " + std::to_string(port)
                                     + " failed: " + std::string(strerror(errno)));
        }

        return true;
    }

    // ── GetOrBuildAddr ────────────────────────────────────────────────────────
    // Cache sockaddr_in per client so we never re-build it on every Send().
    // EndPoint.address is stored in host byte order; htonl converts to network order.

    const sockaddr_in& RawUDPTransport::GetOrBuildAddr(const Shared::EndPoint& ep)
    {
        auto it = m_addrCache.find(ep);
        if (it != m_addrCache.end())
            return it->second;

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(ep.port);
        addr.sin_addr.s_addr = htonl(ep.address);

        auto [inserted, ok] = m_addrCache.emplace(ep, addr);
        (void)ok;
        return inserted->second;
    }

    // ── Send ─────────────────────────────────────────────────────────────────
    // P-6.2: Thread-safe. Holds m_queueMutex only during push (O(1)).
    // GetOrBuildAddr() accesses m_addrCache (main-thread only) before locking.

    void RawUDPTransport::Send(const std::vector<uint8_t>& buffer, const Shared::EndPoint& recipient)
    {
        if (m_sockfd == -1) return;
        const sockaddr_in& addr = GetOrBuildAddr(recipient);  // main thread — no lock needed
        std::lock_guard lock(m_queueMutex);
        m_sendQueue.push_back({ buffer, addr });
    }

    // ── Flush ─────────────────────────────────────────────────────────────────
    // P-6.1: 1 sendmmsg syscall for all queued packets.
    // P-6.2: Called by the AsyncSendDispatcher thread. Swap-buffer pattern:
    //   1. Lock → swap(m_sendQueue, m_flushQueue) → unlock  (O(1), minimal contention)
    //   2. sendmmsg from m_flushQueue (no lock held during syscall)
    //   3. clear m_flushQueue (keeps capacity — avoids reallocation next tick)

    void RawUDPTransport::Flush()
    {
        if (m_sockfd == -1) return;

        {
            std::lock_guard lock(m_queueMutex);
            if (m_sendQueue.empty()) return;
            std::swap(m_sendQueue, m_flushQueue);  // O(1) pointer swap
        }
        // Lock released — sendmmsg runs without contention.

        const size_t n = m_flushQueue.size();
        std::vector<mmsghdr> mmhdrs(n);
        std::vector<iovec>   iovecs(n);

        for (size_t i = 0; i < n; ++i) {
            auto& msg = m_flushQueue[i];
            iovecs[i].iov_base = msg.buffer.data();
            iovecs[i].iov_len  = msg.buffer.size();

            mmhdrs[i].msg_hdr.msg_name       = &msg.addr;
            mmhdrs[i].msg_hdr.msg_namelen    = sizeof(sockaddr_in);
            mmhdrs[i].msg_hdr.msg_iov        = &iovecs[i];
            mmhdrs[i].msg_hdr.msg_iovlen     = 1;
            mmhdrs[i].msg_hdr.msg_control    = nullptr;
            mmhdrs[i].msg_hdr.msg_controllen = 0;
            mmhdrs[i].msg_hdr.msg_flags      = 0;
            mmhdrs[i].msg_len                = 0;
        }

        ::sendmmsg(m_sockfd, mmhdrs.data(), static_cast<unsigned int>(n), 0);
        m_flushQueue.clear();  // retains capacity — no reallocation on next tick
    }

    // ── Receive ───────────────────────────────────────────────────────────────
    // Non-blocking recvfrom. Returns false immediately when no packets are
    // available (EAGAIN/EWOULDBLOCK), allowing the caller to drain the queue
    // in a loop without blocking the game tick.

    bool RawUDPTransport::Receive(std::vector<uint8_t>& buffer, Shared::EndPoint& sender)
    {
        sockaddr_in srcAddr{};
        socklen_t   addrLen = sizeof(srcAddr);
        buffer.resize(1500);

        const ssize_t bytes = ::recvfrom(
            m_sockfd, buffer.data(), 1500, 0,
            reinterpret_cast<sockaddr*>(&srcAddr), &addrLen);

        if (bytes <= 0) {
            buffer.clear();
            return false;  // EAGAIN/EWOULDBLOCK — no more packets this tick
        }

        buffer.resize(static_cast<size_t>(bytes));
        sender.address = ntohl(srcAddr.sin_addr.s_addr);
        sender.port    = ntohs(srcAddr.sin_port);
        return true;
    }

    // ── Close ─────────────────────────────────────────────────────────────────

    void RawUDPTransport::Close()
    {
        if (m_sockfd != -1) {
            ::close(m_sockfd);  // POSIX close(), not recursive Close()
            m_sockfd = -1;
        }
    }

}  // namespace NetworkMiddleware::Transport

#endif  // __linux__

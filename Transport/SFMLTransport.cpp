#include "SFMLTransport.h"

namespace NetworkMiddleware::Transport {
    bool SFMLTransport::Initialize(uint16_t port) {
        if (m_socket.bind(port) != sf::Socket::Done) {
            return false;
        }
        m_socket.setBlocking(false);
        return true;
    }

    void SFMLTransport::Send(const std::vector<uint8_t>& buffer, const Shared::EndPoint& recipient) {
        sf::IpAddress client(recipient.address);
        m_socket.send(buffer.data(), buffer.size(), client, recipient.port);
    }

    bool SFMLTransport::Receive(std::vector<uint8_t>& buffer, Shared::EndPoint& sender) {
        sf::IpAddress ip;
        unsigned short port;
        char data[1500];
        std::size_t received;
        
        if (m_socket.receive(data, sizeof(data), received, ip, port) == sf::Socket::Done) {
            buffer.assign(data, data + received);
            sender.address = ip.toInteger();
            sender.port = port;
            return true;
        }
        return false;
    }

    void SFMLTransport::Close() {
        m_socket.unbind();
    }
}

#pragma once
#include "../Shared/ITransport.h"
#include <SFML/Network.hpp>

namespace NetworkMiddleware::Transport {
    class SFMLTransport : public Shared::ITransport {
    private:
        sf::UdpSocket m_socket; 

    public:
        bool Initialize(uint16_t port) override;
        bool Receive(std::vector<uint8_t>& buffer, Shared::EndPoint& sender) override;
        void Send(const std::vector<uint8_t>& buffer, const Shared::EndPoint& recipient) override;
        void Close() override;
    };
}

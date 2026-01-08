#pragma once
#include "ITransport.h" // Heredamos de la interfaz
#include <SFML/Network.hpp>

namespace Middleware {
    class SFMLTransport : public ITransport {
    private:
        sf::UdpSocket m_socket; // El "motor" de red de SFML

    public:
        bool Initialize(uint16_t port) override;
        void Send(const std::vector<uint8_t>& data, const std::string& address, uint16_t port) override;
        bool Receive(std::vector<uint8_t>& outData, std::string& outAddress, uint16_t& outPort) override;
        void Close() override;
    };
}
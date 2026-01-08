#pragma once
#include "SFMLTransport.h"
#include <iostream>

namespace Middleware {
    bool SFMLTransport::Initialize(uint16_t port) {
        if (m_socket.bind(port) != sf::Socket::Done) {
            return false;
        }
        m_socket.setBlocking(false); // No bloqueamos el hilo principal
        return true;
    }

    void SFMLTransport::Send(const std::vector<uint8_t>& data, const std::string& address, uint16_t port) {
        sf::IpAddress recipient(address);
        m_socket.send(data.data(), data.size(), recipient, port);
    }

    bool SFMLTransport::Receive(std::vector<uint8_t>& outData, std::string& outAddress, uint16_t& outPort) {
        uint8_t buffer[1024]; // Buffer temporal
        std::size_t received;
        sf::IpAddress sender;

        if (m_socket.receive(buffer, sizeof(buffer), received, sender, outPort) == sf::Socket::Done) {
            outData.assign(buffer, buffer + received);
            outAddress = sender.toString();
            return true;
        }
        return false;
    }

    void SFMLTransport::Close() {
        m_socket.unbind();
    }
}
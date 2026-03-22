#pragma once
#include <string>
#include <cstdint>

namespace NetworkMiddleware::Shared {
    struct EndPoint {
        uint32_t address = 0;
        uint16_t port = 0;

        bool operator==(const EndPoint& other) const {
            return address == other.address && port == other.port;
        }

        bool operator<(const EndPoint& other) const {
            if (address != other.address) return address < other.address;
            return port < other.port;
        }

        // Cross-platform: manual byte extraction (network byte order, no arpa/inet.h needed).
        std::string ToString() const {
            return std::to_string( address        & 0xFF) + "." +
                   std::to_string((address >>  8) & 0xFF) + "." +
                   std::to_string((address >> 16) & 0xFF) + "." +
                   std::to_string((address >> 24) & 0xFF) + ":" +
                   std::to_string(port);
        }
    };
}
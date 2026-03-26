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

        // Returns "A.B.C.D:port". Bytes are extracted LSB-first from the host-byte-order
        // uint32_t stored in `address` — cross-platform, no arpa/inet.h needed.
        std::string ToString() const;
    };
}
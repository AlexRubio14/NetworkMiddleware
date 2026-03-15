#pragma once
#include <string>
#include <arpa/inet.h>
#include <cstdint>

namespace NetworkMiddleware::Shared {
    struct EndPoint {
        uint32_t address = 0;
        uint16_t port = 0;

        std::string ToString() const{
            struct in_addr ip_addr {};
            ip_addr.s_addr = address;
            char* ip_str = inet_ntoa(ip_addr);
            return (ip_str ? std::string(ip_str) : "0.0.0.0") + ":" + std::to_string(port);
        }
    };
}
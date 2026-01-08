#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace Middleware {
    class ITransport {
    public:
        virtual ~ITransport() = default;
        virtual bool Initialize(uint16_t port) = 0;
        virtual void Send(const std::vector<uint8_t>& data, const std::string& address, uint16_t port) = 0;
        virtual bool Receive(std::vector<uint8_t>& outData, std::string& outAddress, uint16_t& outPort) = 0;
        virtual void Close() = 0;
    };
}

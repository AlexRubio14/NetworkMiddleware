#pragma once
#include <vector>
#include "NetworkAddress.h"

namespace NetworkMiddleware::Shared {
    class ITransport {
    public:
        virtual ~ITransport() = default;
        virtual bool Initialize(uint16_t port) = 0;

        virtual bool Receive(std::vector<uint8_t>& buffer, EndPoint& sender) = 0;
        virtual void Send(const std::vector<uint8_t>& buffer, const EndPoint& recipient) = 0;
        virtual void Close() = 0;
    };
}

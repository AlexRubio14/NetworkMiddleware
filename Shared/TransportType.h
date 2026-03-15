#pragma once

namespace NetworkMiddleware::Shared {
    enum class TransportType {
        SFML,
        ASIO,
        NATIVE_LINUX
    };
}
#include "NetworkAddress.h"

namespace NetworkMiddleware::Shared {

std::string EndPoint::ToString() const {
    return std::to_string( address        & 0xFF) + "." +
           std::to_string((address >>  8) & 0xFF) + "." +
           std::to_string((address >> 16) & 0xFF) + "." +
           std::to_string((address >> 24) & 0xFF) + ":" +
           std::to_string(port);
}

} // namespace NetworkMiddleware::Shared

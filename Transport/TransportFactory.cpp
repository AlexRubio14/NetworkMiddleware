#include "TransportFactory.h"
#include "SFMLTransport.h"

#ifdef __linux__
#include "RawUDPTransport.h"
#endif

namespace NetworkMiddleware::Transport {
    std::shared_ptr<Shared::ITransport> TransportFactory::Create(Shared::TransportType type) {
        switch (type) {
            case Shared::TransportType::SFML:
                return std::make_shared<SFMLTransport>();
#ifdef __linux__
            case Shared::TransportType::NATIVE_LINUX:
                return std::make_shared<RawUDPTransport>();
#endif
            default:
                return nullptr;
        }
    }
}
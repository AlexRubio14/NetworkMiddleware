#include "TransportFactory.h"
#include "SFMLTransport.h"

namespace NetworkMiddleware::Transport {
    std::shared_ptr<Shared::ITransport> TransportFactory::Create(Shared::TransportType type) {
        switch (type) {
            case Shared::TransportType::SFML:
                return std::make_unique<SFMLTransport>();
            default:
                return nullptr;
        }
    }
}
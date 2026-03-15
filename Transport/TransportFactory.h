#pragma once
#include "ITransport.h"
#include <memory>
#include "../Shared/TransportType.h"

namespace NetworkMiddleware::Transport {

    class TransportFactory {
    public:
        static std::shared_ptr<Shared::ITransport> Create(Shared::TransportType type);
    };
}

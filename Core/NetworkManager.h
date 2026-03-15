#pragma once
#include "../Shared/ITransport.h"
#include <memory>
#include <functional>

namespace NetworkMiddleware::Core {

    using OnDataReceivedCallback = std::function<void(const std::vector<uint8_t>&, const Shared::EndPoint&)>;
    class NetworkManager {
    private:
        std::shared_ptr<Shared::ITransport> m_transport;
        OnDataReceivedCallback m_onDataReceived;

    public:
        explicit NetworkManager(std::shared_ptr<Shared::ITransport> transport);

        void SetDataCallback(OnDataReceivedCallback callback);
        void Update();
    };
}
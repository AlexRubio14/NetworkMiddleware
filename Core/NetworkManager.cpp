
#include "NetworkManager.h"

namespace NetworkMiddleware::Core {
    NetworkManager::NetworkManager(std::shared_ptr<Shared::ITransport> transport)
        : m_transport(std::move(transport)){}

    void NetworkManager::SetDataCallback(OnDataReceivedCallback callback)
    {
        m_onDataReceived = std::move(callback);
    }

    void NetworkManager::Update()
    {
        std::vector<uint8_t> buffer;
        Shared::EndPoint sender;

        if (m_transport->Receive(buffer, sender))
        {
            if (m_onDataReceived)
                m_onDataReceived(buffer, sender);
        }
    }
}


#include "NetworkManager.h"
#include "Log/Logger.h"

namespace NetworkMiddleware::Core {
    NetworkManager::NetworkManager(std::shared_ptr<Shared::ITransport> transport)
        : m_transport(std::move(transport)){}

    void NetworkManager::SetDataCallback(OnDataReceivedCallback callback)
    {
        m_onDataReceived = std::move(callback);
    }

    void NetworkManager::Update()
    {
        auto buffer = std::make_shared<std::vector<uint8_t>>();;
        Shared::EndPoint sender;

        if (m_transport->Receive(*buffer, sender))
        {
            Shared::Logger::LogPacket(Shared::LogChannel::Core, buffer);

            if (m_onDataReceived)
                m_onDataReceived(*buffer, sender);
        }
    }
}

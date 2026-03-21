#pragma once
#include "../Shared/ITransport.h"
#include "../Shared/Network/PacketHeader.h"
#include "../Shared/Network/PacketTypes.h"
#include "../Shared/Serialization/BitReader.h"
#include "RemoteClient.h"
#include <memory>
#include <functional>
#include <map>
#include <random>

namespace NetworkMiddleware::Core {

    // Callback para paquetes de juego de clientes ya establecidos.
    // El BitReader está posicionado en el bit 100 (inicio del payload).
    using OnDataReceivedCallback = std::function<void(
        const Shared::PacketHeader&,
        Shared::BitReader&,
        const Shared::EndPoint&
    )>;

    // Callback que se dispara cuando un cliente completa el handshake.
    using OnClientConnectedCallback = std::function<void(
        uint16_t networkID,
        const Shared::EndPoint&
    )>;

    class NetworkManager {
    public:
        static constexpr uint16_t         kMaxClients      = 10;
        static constexpr std::chrono::seconds kHandshakeTimeout{5};

    private:
        std::shared_ptr<Shared::ITransport>         m_transport;
        OnDataReceivedCallback                       m_onDataReceived;
        OnClientConnectedCallback                    m_onClientConnected;

        std::map<Shared::EndPoint, RemoteClient>     m_pendingClients;      // En handshake
        std::map<Shared::EndPoint, RemoteClient>     m_establishedClients;  // Conectados

        uint16_t                                     m_nextNetworkID = 1;
        std::mt19937_64                              m_rng{std::random_device{}()};

        void CheckTimeouts();
        void HandleConnectionRequest(const Shared::EndPoint& sender);
        void HandleChallengeResponse(Shared::BitReader& reader, const Shared::EndPoint& sender);
        void SendHeaderOnly(Shared::PacketType type, const Shared::EndPoint& to);
        void SendChallenge(const Shared::EndPoint& to, uint64_t salt);
        void SendConnectionAccepted(const Shared::EndPoint& to, uint16_t networkID);

    public:
        explicit NetworkManager(std::shared_ptr<Shared::ITransport> transport);

        void SetDataCallback(OnDataReceivedCallback callback);
        void SetClientConnectedCallback(OnClientConnectedCallback callback);
        void Update();

        size_t GetEstablishedCount() const { return m_establishedClients.size(); }
        size_t GetPendingCount()     const { return m_pendingClients.size(); }
    };
}

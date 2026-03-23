#pragma once
#include "../Shared/ITransport.h"
#include "../Shared/Network/PacketHeader.h"
#include "../Shared/Network/PacketTypes.h"
#include "../Shared/Network/InputPackets.h"
#include "../Shared/Serialization/BitReader.h"
#include "../Shared/Data/HeroState.h"
#include "RemoteClient.h"
#include "NetworkProfiler.h"
#include <memory>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include <random>
#include <chrono>

namespace NetworkMiddleware::Core {

    // Métricas de red por cliente, expuestas al módulo Brain (P-3.4).
    struct ClientNetworkStats {
        float    rtt         = 0.0f;  // RTT suavizado en ms (EMA, α=0.1)
        float    clockOffset = 0.0f;  // ServerNow - (ClientTime + RTT/2) en ms
        int      sampleCount = 0;     // Número de muestras de RTT tomadas
    };

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

    // Callback que se dispara cuando un cliente se desconecta por Link Loss (kMaxRetries agotados).
    using OnClientDisconnectedCallback = std::function<void(
        uint16_t networkID,
        const Shared::EndPoint&
    )>;

    class NetworkManager {
    public:
        static constexpr uint16_t             kMaxClients       = 100;
        static constexpr std::chrono::seconds kHandshakeTimeout{5};
        static constexpr uint8_t              kMaxRetries       = 10;
        static constexpr std::chrono::milliseconds kResendInterval{100};

        // P-3.6 Session Recovery timing constants (easy to change)
        static constexpr std::chrono::seconds kHeartbeatInterval{1};   // Send heartbeat after 1s of silence
        static constexpr std::chrono::seconds kSessionTimeout{10};     // Mark zombie after 10s no incoming
        static constexpr std::chrono::seconds kZombieDuration{120};    // Remove zombie after 120s

    private:
        std::shared_ptr<Shared::ITransport>         m_transport;
        OnDataReceivedCallback                       m_onDataReceived;
        OnClientConnectedCallback                    m_onClientConnected;
        OnClientDisconnectedCallback                 m_onClientDisconnected;
        NetworkProfiler                              m_profiler;

        std::map<Shared::EndPoint, RemoteClient>     m_pendingClients;      // En handshake
        std::map<Shared::EndPoint, RemoteClient>     m_establishedClients;  // Conectados

        uint16_t                                     m_nextNetworkID = 1;
        std::mt19937_64                              m_rng{std::random_device{}()};

        void CheckTimeouts();
        void HandleConnectionRequest(const Shared::EndPoint& sender);
        void HandleChallengeResponse(Shared::BitReader& reader, const Shared::EndPoint& sender);
        void SendHeaderOnly(Shared::PacketType type, const Shared::EndPoint& to);
        void SendChallenge(const Shared::EndPoint& to, uint64_t salt);


        // P-3.3 Reliability Layer
        void ProcessAcks(RemoteClient& client, const Shared::PacketHeader& header);
        void ResendPendingPackets();
        void DisconnectClient(const Shared::EndPoint& endpoint);
        void HandleReliableOrdered(Shared::BitReader& reader, RemoteClient& client,
                                   const Shared::EndPoint& sender, size_t totalBits,
                                   const Shared::PacketHeader& header);
        void DeliverBufferedReliable(RemoteClient& client, const Shared::EndPoint& sender);

        // P-3.6 Session Recovery
        void HandleDisconnect(const Shared::EndPoint& sender);
        void HandleReconnectionRequest(Shared::BitReader& reader, const Shared::EndPoint& sender);
        void SendConnectionAccepted(const Shared::EndPoint& to, uint16_t networkID, uint64_t token);

        // P-4.4 Split-Phase internal helper — const, no side effects.
        // Builds the wire payload for a snapshot without touching RemoteClient state.
        std::vector<uint8_t> SerializeSnapshot(const RemoteClient& client,
                                               const Shared::Data::HeroState& state,
                                               uint32_t tickID) const;

    public:
        explicit NetworkManager(std::shared_ptr<Shared::ITransport> transport);

        void SetDataCallback(OnDataReceivedCallback callback);
        void SetClientConnectedCallback(OnClientConnectedCallback callback);
        void SetClientDisconnectedCallback(OnClientDisconnectedCallback callback);

        // Envía un paquete al cliente `to`. El canal determina la garantía de entrega:
        //   Reliable         → entrega ordenada garantizada (con buffer de recepción)
        //   ReliableUnordered → entrega garantizada, sin orden (sin buffer)
        //   Cualquier otro   → sin garantías (fire and forget)
        void Send(const Shared::EndPoint& to, const std::vector<uint8_t>& payload,
                  Shared::PacketType channel);

        void Update();

        // P-3.6: Checks heartbeat / session timeout / zombie expiry for all clients.
        // Exposed with explicit `now` parameter for deterministic unit testing.
        // Update() calls this internally with steady_clock::now().
        void ProcessSessionKeepAlive(std::chrono::steady_clock::time_point now);

        // Devuelve las métricas de red del cliente (para Brain, P-3.4).
        // Retorna nullopt si el endpoint no corresponde a un cliente establecido.
        std::optional<ClientNetworkStats> GetClientNetworkStats(const Shared::EndPoint& ep) const;

        size_t GetEstablishedCount() const { return m_establishedClients.size(); }
        size_t GetPendingCount()     const { return m_pendingClients.size(); }

        // P-4.3: Access profiler snapshot (for embedding metrics in game loop).
        NetworkProfiler::Snapshot GetProfilerSnapshot() const;

        // Returns true if the client at `ep` is in zombie state (timed out, awaiting reconnection).
        bool IsClientZombie(const Shared::EndPoint& ep) const;

        // P-3.7 Game Loop

        // Send an authoritative snapshot to `to` using delta compression.
        // Selects the best available baseline from m_lastClientAckedServerSeq;
        // falls back to a full Serialize() if no valid baseline exists.
        // Records the sent snapshot for future delta baselines.
        // Single-threaded convenience path (wraps the two Split-Phase methods below).
        void SendSnapshot(const Shared::EndPoint& to,
                          const Shared::Data::HeroState& state,
                          uint32_t tickID);

        // P-4.4 Split-Phase API — see NetworkManager.cpp for threading contract.

        // Phase A (parallel-safe): serializes state into a wire buffer.
        // Read-only on all shared state; safe to call from Job System worker threads
        // while the main thread is blocked on std::latch::wait().
        // Returns an empty vector if `to` is not an established client.
        std::vector<uint8_t> SerializeSnapshotFor(const Shared::EndPoint& to,
                                                   const Shared::Data::HeroState& state,
                                                   uint32_t tickID) const;

        // Phase B (main-thread only): records the snapshot in history and sends
        // the pre-built payload over the transport.  Must be called after
        // std::latch::wait() guarantees that Phase A has completed.
        void CommitAndSendSnapshot(const Shared::EndPoint& to,
                                   const Shared::Data::HeroState& state,
                                   const std::vector<uint8_t>& payload);

        // Iterate all non-zombie established clients.
        // Callback receives: (networkID, endpoint, pendingInput or nullptr).
        // After the callback returns, pendingInput is cleared for that client.
        void ForEachEstablished(
            std::function<void(uint16_t, const Shared::EndPoint&, const Shared::InputPayload*)> callback);
    };
}

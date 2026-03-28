#pragma once
#include "../Shared/NetworkAddress.h"
#include "../Shared/Network/PacketHeader.h"
#include "../Shared/Network/InputPackets.h"
#include "../Shared/Data/HeroState.h"
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace NetworkMiddleware::Core {

    // Estado de estimación de RTT por cliente (P-3.4).
    // sentTimes mapea cada global sequence al instante en que fue enviado.
    // Cuando llega un ACK para esa sequence se calcula el RTT bruto y se actualiza el EMA.
    // Entries más viejas de 2 segundos se purgan en Update() para evitar crecimiento indefinido.
    struct RTTContext {
        float    rttEMA      = 100.0f;  // RTT suavizado en ms (EMA, α=0.1). Inicializado a 100ms.
        int      sampleCount = 0;       // Número de muestras de RTT tomadas
        float    clockOffset = 0.0f;    // ServerTime - (ClientTime + RTT/2) en ms. Requiere
                                        // que el cliente Unreal escriba su timestamp en el header.

        // seq global → instante de envío por parte de este servidor.
        std::map<uint16_t, std::chrono::steady_clock::time_point> sentTimes;
    };

    // Paquete Reliable Ordered almacenado fuera de orden, esperando que lleguen los anteriores.
    // Guarda el header completo (timestamp incluido) para que la entrega al game layer sea
    // idéntica a la de un paquete llegado en orden (necesario para interpolación en Fase 6).
    struct BufferedPacket {
        Shared::PacketHeader         header;
        std::vector<uint8_t>         payload;
    };

    // Payload de un paquete fiable pendiente de confirmación (ACK).
    // El NetworkManager lo guarda en m_reliableSents keyed por número de secuencia global.
    // Solo almacena los bytes DESPUÉS del header — el header se reconstruye en cada reenvío
    // para llevar el ack/ack_bits más reciente (piggybacking gratuito).
    struct PendingPacket {
        std::vector<uint8_t>                   payload;      // Bytes tras el header
        uint16_t                               sequence;     // Seq global (fijo en retransmisiones)
        std::chrono::steady_clock::time_point  lastSentTime;
        uint8_t                                retryCount  = 0;
        uint8_t                                channelType;  // PacketType como uint8_t
    };

    // Representa a un cliente conectado al servidor.
    // El NetworkManager mantiene dos colecciones de RemoteClient:
    //   - m_pendingClients:    enviado el Challenge, esperando ChallengeResponse
    //   - m_establishedClients: handshake completado, sesión activa
    struct RemoteClient {
        Shared::EndPoint                          endpoint;
        uint16_t                                  networkID     = 0;
        Shared::SequenceContext                   seqContext;
        uint64_t                                  challengeSalt = 0;
        std::chrono::steady_clock::time_point     challengeSentAt;

        RemoteClient() = default;

        RemoteClient(const Shared::EndPoint& ep, uint16_t id, uint64_t salt)
            : endpoint(ep)
            , networkID(id)
            , challengeSalt(salt)
            , challengeSentAt(std::chrono::steady_clock::now())
        {}

        // Devuelve true si el handshake lleva más de `timeout` sin completarse.
        bool IsTimedOut(std::chrono::seconds timeout) const {
            const auto elapsed = std::chrono::steady_clock::now() - challengeSentAt;
            return elapsed > timeout;
        }

        // --- P-3.3 Reliability Layer ---

        // Cola de envío fiable: clave = número de secuencia global.
        std::map<uint16_t, PendingPacket>         m_reliableSents;

        // Secuencia de envío Reliable Ordered (por canal, independiente del seq global).
        uint16_t                                  m_nextOutgoingReliableSeq = 0;

        // Estado de recepción Reliable Ordered.
        uint16_t                                  m_nextExpectedReliableSeq = 0;
        std::map<uint16_t, BufferedPacket>         m_reliableReceiveBuffer;  // Paquetes fuera de orden

        // Detección de duplicados: false hasta que llega el primer paquete de juego.
        // Evita que la comparación con remoteAck=0 del estado inicial genere falsos positivos.
        bool                                      m_seqInitialized = false;

        // --- P-3.4 Clock Sync & RTT ---

        // Estimación de RTT y cálculo de offset de reloj.
        RTTContext                                m_rtt;

        // Filtro de paquetes Unreliable fuera de orden (Snapshot, Input, Heartbeat).
        // Solo se entrega al game layer el paquete con la sequence más alta vista hasta ahora.
        // Usa comparación int16_t modular para manejar wrap-around en 65535.
        uint16_t                                  m_lastProcessedSeq            = 0;
        bool                                      m_lastProcessedSeqInitialized = false;

        // --- P-3.5 Snapshot History (Delta Compression baseline) ---
        //
        // P-5.x: history is keyed per-batch.  One tick = one batch packet per client,
        // containing N entity states.  ProcessAckedSeq() promotes a confirmed batch
        // into m_entityBaselines so SerializeSnapshot can delta-compress each entity
        // against the last state the client has actually received, not the last entity
        // that happened to share a sequence number.

        static constexpr size_t kHistorySize = 64; // ~640ms at 100 Hz

        struct BatchEntry {
            uint16_t seq   = 0;
            bool     valid = false;
            std::vector<std::pair<uint32_t, Shared::Data::HeroState>> entities;
        };

        std::array<BatchEntry, kHistorySize> m_history{};

        // Per-entity last-confirmed state (updated when client ACKs the seq).
        std::unordered_map<uint32_t, Shared::Data::HeroState> m_entityBaselines;

        // Records all entity states sent in a single batch packet (seq = localSequence).
        void RecordBatch(uint16_t seq,
                         const std::vector<std::pair<uint32_t, Shared::Data::HeroState>>& entities);

        // Called when the client's ACK confirms seq was received.
        // Promotes every entity in that batch into m_entityBaselines.
        void ProcessAckedSeq(uint16_t seq);

        // Returns the last confirmed state for entityID, or nullptr if not yet ACKed.
        // nullptr → caller must fall back to a full sync (Serialize).
        const Shared::Data::HeroState* GetEntityBaseline(uint32_t entityID) const;

        // P-6.3: Evicts the cached baseline for entityID so the next snapshot
        // sends a full state.  Call when an entity transitions invisible→visible
        // (FOW re-entry) to guarantee the client receives a correct full sync.
        void EvictEntityBaseline(uint32_t entityID);

        // --- P-3.6 Session Recovery ---

        // Random token issued in ConnectionAccepted; required for reconnection.
        uint64_t reconnectionToken = 0;

        // True when the session has timed out but the reconnection window is still open.
        // The client can present its token to resume the session from any endpoint.
        bool isZombie = false;

        // Timestamp of the last packet received from this client.
        // Default-initialized to epoch; always set explicitly before emplacing into m_establishedClients.
        std::chrono::steady_clock::time_point lastIncomingTime{};

        // Timestamp of the last packet sent to this client (used for heartbeat triggering).
        // Default-initialized to epoch; always set explicitly before emplacing into m_establishedClients.
        std::chrono::steady_clock::time_point lastOutgoingTime{};

        // When the client transitioned to zombie state (for expiry calculation).
        std::chrono::steady_clock::time_point zombieTime;

        // --- P-5.1 Team Assignment ---

        // Assigned during HandleChallengeResponse based on arrival order (round-robin).
        // 0 = Blue team, 1 = Red team. Used by SpatialGrid for FOW interest management.
        uint8_t teamID = 0;

        // --- P-3.7 Game Loop ---

        // Buffered input from the client for the current tick.
        // Set by NetworkManager when a PacketType::Input arrives; consumed and cleared
        // by ForEachEstablished() during the game loop's input phase.
        std::optional<Shared::InputPayload> pendingInput;

        // (Removed in P-5.x: delta baseline selection now uses per-entity m_entityBaselines
        //  updated via ProcessAckedSeq(), not a single global last-acked-seq.)
    };

}

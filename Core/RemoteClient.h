#pragma once
#include "../Shared/NetworkAddress.h"
#include "../Shared/Network/PacketHeader.h"
#include <chrono>
#include <map>
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
    };

}

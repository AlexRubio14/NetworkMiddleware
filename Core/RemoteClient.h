#pragma once
#include "../Shared/NetworkAddress.h"
#include "../Shared/Network/PacketHeader.h"
#include <chrono>
#include <map>
#include <vector>

namespace NetworkMiddleware::Core {

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
    };

}

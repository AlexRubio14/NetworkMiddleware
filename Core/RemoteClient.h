#pragma once
#include "../Shared/NetworkAddress.h"
#include "../Shared/Network/PacketHeader.h"
#include <chrono>

namespace NetworkMiddleware::Core {

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
    };

}

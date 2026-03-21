#pragma once
#include <cstdint>
#include "../Serialization/BitWriter.h"
#include "../Serialization/BitReader.h"

// Payloads de los paquetes de handshake (P-3.2).
// Todos van precedidos del PacketHeader de P-3.1.
// ConnectionRequest y ConnectionDenied no tienen payload propio.

namespace NetworkMiddleware::Shared {

    // SERVER → CLIENT: enviado en respuesta a ConnectionRequest.
    // Contiene un salt aleatorio que el cliente debe devolver para probar
    // que realmente recibe paquetes en esa IP/Puerto (anti-spoofing básico).
    struct ChallengePayload {
        uint64_t salt = 0;

        static constexpr uint32_t kBitCount = 64;  // 2 × 32 bits (BitWriter solo acepta uint32_t)

        void Write(BitWriter& writer) const {
            writer.WriteBits(static_cast<uint32_t>(salt & 0xFFFFFFFF), 32);         // low 32 bits
            writer.WriteBits(static_cast<uint32_t>((salt >> 32) & 0xFFFFFFFF), 32); // high 32 bits
        }

        static ChallengePayload Read(BitReader& reader) {
            ChallengePayload p;
            const uint32_t lo = reader.ReadBits(32);
            const uint32_t hi = reader.ReadBits(32);
            p.salt = (static_cast<uint64_t>(hi) << 32) | lo;
            return p;
        }
    };

    // CLIENT → SERVER: el cliente devuelve exactamente el mismo salt.
    // Si coincide con el almacenado, el servidor acepta la conexión.
    using ChallengeResponsePayload = ChallengePayload;

    // SERVER → CLIENT: la conexión ha sido aceptada.
    // Incluye el NetworkID único asignado a esta sesión.
    struct ConnectionAcceptedPayload {
        uint16_t networkID = 0;

        static constexpr uint32_t kBitCount = 16;

        void Write(BitWriter& writer) const {
            writer.WriteBits(networkID, 16);
        }

        static ConnectionAcceptedPayload Read(BitReader& reader) {
            ConnectionAcceptedPayload p;
            p.networkID = static_cast<uint16_t>(reader.ReadBits(16));
            return p;
        }
    };

}

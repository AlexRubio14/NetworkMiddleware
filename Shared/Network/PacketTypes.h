#pragma once
#include <cstdint>

namespace NetworkMiddleware::Shared {

    // Tipo de paquete — ocupa 4 bits en el wire format (valores 0–15).
    // Los 4 bits restantes del byte original son PacketFlags (ver abajo).
    enum class PacketType : uint8_t {
        Snapshot          = 0x1,  // Estado del héroe — posición, salud, flags (Unreliable)
        Input             = 0x2,  // Input del cliente — movimiento, habilidades (Unreliable)
        Reliable          = 0x3,  // Compras, level-up, habilidades lanzadas (Reliable Ordered)
        ReliableUnordered = 0x4,  // Muertes, chat (Reliable Unordered)
        Heartbeat         = 0x5,  // Keepalive de conexión
    };

    // Flags de control del paquete — 4 bits en el wire format.
    // Se usan principalmente en Fase 3.3 (Reliability Layer).
    enum class PacketFlags : uint8_t {
        None          = 0x0,
        IsRetransmit  = 0x1,  // Este paquete es un reenvío de uno perdido (Fase 3.3)
        IsFragment    = 0x2,  // Este paquete es un fragmento de uno mayor (Fase 3.3)
        // 0x4 y 0x8 reservados para fases futuras
    };

}

#pragma once
#include <cstdint>
#include "PacketTypes.h"
#include "../Serialization/BitWriter.h"
#include "../Serialization/BitReader.h"

namespace NetworkMiddleware::Shared {

    // Mantiene el estado de secuenciación de una conexión concreta.
    // El PacketManager tendrá uno de estos por cada RemoteClient (Fase 3.2).
    struct SequenceContext {
        uint16_t localSequence = 0;  // Próximo número de secuencia a enviar
        uint16_t remoteAck     = 0;  // Último seq recibido del remoto
        uint32_t ackBits       = 0;  // Bit N = ¿recibimos el paquete (remoteAck - N - 1)?

        // Registra la llegada de un paquete con número de secuencia 'receivedSeq'.
        // Actualiza remoteAck y ackBits con aritmética modular (wrap-around a 65535).
        void RecordReceived(uint16_t receivedSeq);

        // Incrementa localSequence (wraps 65535 → 0 automáticamente).
        void AdvanceLocal();
    };

    // Header de 100 bits que precede a todo paquete UDP.
    // Los 4 bits de relleno hasta completar 13 bytes los gestiona GetCompressedData().
    //
    // Wire format (LSB-first, igual que BitWriter):
    //   [sequence : 16][ack : 16][ack_bits : 32][type : 4][flags : 4][timestamp : 32]
    //
    struct PacketHeader {
        uint16_t sequence  = 0;                       // Número de secuencia de ESTE paquete
        uint16_t ack       = 0;                       // Último seq recibido del remoto
        uint32_t ack_bits  = 0;                       // Historial de 32 paquetes anteriores a 'ack'
        uint8_t  type      = 0;                       // PacketType (4 bits en wire)
        uint8_t  flags     = 0;                       // PacketFlags (4 bits en wire)
        uint32_t timestamp = 0;                       // Timestamp del servidor en ms (Clock Sync Fase 3.4)

        // 100 bits totales. GetCompressedData() rellena los 4 bits restantes hasta completar 13 bytes.
        static constexpr uint32_t kBitCount  = 16 + 16 + 32 + 4 + 4 + 32;  // = 100 bits
        static constexpr uint32_t kByteCount = (kBitCount + 7) / 8;         // = 13 bytes

        // Devuelve true si 'seq' está confirmado por el ack + ack_bits de este header.
        // Usa la misma aritmética modular int16_t que SequenceContext::RecordReceived.
        bool IsAcked(uint16_t seq) const {
            const int16_t diff = static_cast<int16_t>(ack - seq);
            if (diff == 0) return true;                        // ACK directo
            if (diff > 0 && diff <= 32)
                return (ack_bits >> (diff - 1)) & 1u;          // En la ventana del bitmask
            return false;
        }

        void Write(BitWriter& writer) const;
        static PacketHeader Read(BitReader& reader);
    };

}

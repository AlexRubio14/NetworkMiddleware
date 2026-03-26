#include "PacketHeader.h"

namespace NetworkMiddleware::Shared {

    // -------------------------------------------------------------------------
    // PacketHeader::Write
    // Serializa el header en orden de campos usando BitWriter.
    // Cada campo se escribe con exactamente los bits que necesita.
    // -------------------------------------------------------------------------
    void PacketHeader::Write(BitWriter& writer) const {
        writer.WriteBits(sequence, 16);
        writer.WriteBits(ack,      16);
        writer.WriteBits(ack_bits, 32);
        writer.WriteBits(type,      4);  // 4 bits: PacketType (0–15)
        writer.WriteBits(flags,     4);  // 4 bits: PacketFlags (IsRetransmit, IsFragment...)
        writer.WriteBits(timestamp, 32);
    }

    // -------------------------------------------------------------------------
    // PacketHeader::Read
    // Deserializa el header desde un BitReader ya posicionado al inicio del paquete.
    // Tras esta llamada, el reader queda posicionado en el bit 104 (inicio del payload).
    // -------------------------------------------------------------------------
    PacketHeader PacketHeader::Read(BitReader& reader) {
        PacketHeader h;
        h.sequence  = static_cast<uint16_t>(reader.ReadBits(16));
        h.ack       = static_cast<uint16_t>(reader.ReadBits(16));
        h.ack_bits  = reader.ReadBits(32);
        h.type      = static_cast<uint8_t>(reader.ReadBits(4));   // 4 bits: PacketType
        h.flags     = static_cast<uint8_t>(reader.ReadBits(4));   // 4 bits: PacketFlags
        h.timestamp = reader.ReadBits(32);
        return h;
    }

    // -------------------------------------------------------------------------
    // SequenceContext::RecordReceived
    //
    // Cuando llega un paquete con número de secuencia 'receivedSeq', actualizamos
    // remoteAck y ackBits para reflejar su llegada.
    //
    // Convención del bitmask:
    //   ackBits bit 0 = ¿llegó el paquete (remoteAck - 1)?
    //   ackBits bit 1 = ¿llegó el paquete (remoteAck - 2)?
    //   ...
    //   ackBits bit 31 = ¿llegó el paquete (remoteAck - 32)?
    //
    // El cast a int16_t hace la aritmética modular correcta para secuencias
    // que dan la vuelta (65535 → 0). Funciona mientras la diferencia esté
    // dentro de [-32768, 32767], lo que cubre el 50% del espacio de secuencias.
    // -------------------------------------------------------------------------
    void SequenceContext::RecordReceived(uint16_t receivedSeq) {
        const int16_t diff = static_cast<int16_t>(receivedSeq - remoteAck);

        if (diff == 0) {
            // Paquete duplicado — ignorar
            return;
        }

        if (diff > 0) {
            // Paquete más nuevo que el que teníamos como referencia.
            // Desplazamos ackBits hacia la izquierda 'diff' posiciones:
            //   - El antiguo remoteAck pasa a ser el bit (diff - 1), marcado como recibido.
            //   - Los huecos entre ambos quedan a 0 (paquetes no recibidos).
            if (diff < 32) {
                ackBits = (ackBits << diff) | (1u << (diff - 1));
            } else {
                // Salto mayor de 32 — toda la historia anterior queda fuera de ventana.
                ackBits = 0;
            }
            remoteAck = receivedSeq;
        } else {
            // Paquete más antiguo que llegó fuera de orden.
            // Calculamos a qué bit corresponde y lo marcamos.
            // bit = remoteAck - receivedSeq - 1
            const int bit = -diff - 1;
            if (bit < 32) {
                ackBits |= (1u << bit);
            }
            // Si bit >= 32, el paquete está fuera de la ventana — ignorar.
        }
    }

    // -------------------------------------------------------------------------
    // SequenceContext::AdvanceLocal
    // Incrementa el número de secuencia local. El overflow de uint16_t
    // (65535 → 0) es intencionado y correcto: la aritmética modular en
    // RecordReceived lo maneja sin problema.
    // -------------------------------------------------------------------------
    void SequenceContext::AdvanceLocal() {
        ++localSequence;
    }

}

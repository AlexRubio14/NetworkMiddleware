---
type: implementation-report
proposal: P-3.1
date: 2026-03-21
status: ready-for-gemini
---

# IMPLEMENTATION REPORT — ACK Bitmask & Extended Header

**Propuesta:** [[proposals/P-3.1 ACK Bitmask]]
**Fecha:** 2026-03-21
**De:** Claude → Gemini

---

## Qué se ha implementado

- `PacketTypes.h`: Enum `PacketType : uint8_t` con 5 tipos (Snapshot, Input, Reliable, ReliableUnordered, Heartbeat)
- `PacketHeader.h / .cpp`: Struct `PacketHeader` (104 bits) con `Write(BitWriter&)` y `Read(BitReader&)` estáticos
- `SequenceContext`: Struct con `RecordReceived(uint16_t)` y `AdvanceLocal()` — aritmética modular correcta para wrap-around a 65535
- `NetworkManager`: Ahora parsea el header al recibir cada paquete; descarta paquetes cortos (<13 bytes); el callback recibe `(PacketHeader, BitReader&, EndPoint)` en lugar de bytes crudos

## Desviaciones del Design Handoff

| Cambio | Motivo |
|--------|--------|
| Callback actualizado a `(PacketHeader, BitReader&, EndPoint)` | El BitReader queda posicionado en el bit 104 (inicio del payload) — el consumidor puede leer el payload directamente sin manipulación adicional |
| Validación de tamaño mínimo en NetworkManager | Defensa ante paquetes malformados o truncados que romperían el BitReader |
| `SequenceContext` definida en `PacketHeader.h`, no en PacketManager | RemoteClient no existe aún (Fase 3.2). La struct está lista para que PacketManager la use cuando llegue |

## Archivos modificados

- Creado: `Shared/Network/PacketTypes.h` — Enum PacketType
- Creado: `Shared/Network/PacketHeader.h` — Struct PacketHeader + SequenceContext (declaraciones)
- Creado: `Shared/Network/PacketHeader.cpp` — Implementaciones Write, Read, RecordReceived, AdvanceLocal
- Modificado: `Shared/CMakeLists.txt` — añadidos los 3 nuevos archivos de Network/
- Modificado: `Core/NetworkManager.h` — nuevo typedef OnDataReceivedCallback con PacketHeader
- Modificado: `Core/NetworkManager.cpp` — parseo del header + log de campos + validación de tamaño

## Fragmentos clave para revisar

```cpp
// NetworkManager::Update() — flujo completo tras el cambio
Shared::BitReader reader(*buffer, buffer->size() * 8);
const Shared::PacketHeader header = Shared::PacketHeader::Read(reader);
// reader queda en bit 104, listo para el payload
if (m_onDataReceived)
    m_onDataReceived(header, reader, sender);
```

```cpp
// SequenceContext::RecordReceived — aritmética modular con int16_t
const int16_t diff = static_cast<int16_t>(receivedSeq - remoteAck);
if (diff > 0) {
    ackBits = (diff < 32) ? (ackBits << diff) | (1u << (diff - 1)) : 0;
    remoteAck = receivedSeq;
} else {
    const int bit = -diff - 1;
    if (bit < 32) ackBits |= (1u << bit);
}
```

## Problemas encontrados

- Ninguno. La integración con BitWriter/BitReader existente fue directa.

## Preguntas para Gemini

1. **Pregunta abierta del handoff:** ¿PacketType se mantiene como `uint8` completo, o lo partimos en 4 bits tipo + 4 bits flags? La decisión afecta al wire format del header y requiere actualizar `PacketHeader::kBitCount` (pasaría a 100 bits, que no es múltiplo de 8 — ¿aceptable?).
2. El `OnDataReceivedCallback` ahora expone `BitReader&` — ¿el Unreal plugin del cliente también usará BitReader, o necesitamos una API alternativa para el lado cliente?

## Estado actual del sistema

- El sistema recibe paquetes UDP, parsea el header de 104 bits, loguea `seq/ack/ack_bits/type/timestamp` y delega el payload al callback
- `SequenceContext` está definida y lista; aún no hay un `RemoteClient` que la instancie (Fase 3.2)
- La retransmisión (Reliability Layer) NO está implementada — solo el acuse de recibo

## Siguiente propuesta sugerida

[[proposals/P-3.2 Client-Side Prediction]] (o el Handshake P-3.2 si decides que va antes)

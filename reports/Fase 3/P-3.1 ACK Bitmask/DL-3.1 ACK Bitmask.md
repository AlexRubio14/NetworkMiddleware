---
type: dev-log-alex
proposal: P-3.1
date: 2026-03-21
status: personal
---

# DEV LOG — ACK Bitmask & Extended Header

**Propuesta:** [[proposals/P-3.1 ACK Bitmask]]
**Fecha:** 2026-03-21

> **Nota:** Este documento describe la iteración 1 de P-3.1 (type: 8 bits).
> La iteración 2 partió el byte en `type(4) + flags(4)` — ver **DL-3.1b**.
> El formato de wire definitivo es el de DL-3.1b; los fragmentos de código aquí son históricos.

---

## ¿Qué problema resolvíamos?

Cuando dos ordenadores se mandan paquetes UDP, ninguno de los dos sabe si el paquete llegó. UDP es "dispara y olvida": lo mandas y ya. Si se pierde, no hay aviso.

Para un juego de red necesitamos saber qué llegó y qué no, porque hay cosas que **sí o sí tienen que llegar** (una habilidad lanzada, una muerte) y otras que no importa si se pierden (la posición del héroe, que se mandará de nuevo en el siguiente frame).

El primer paso para construir esa lógica es que cada paquete lleve consigo una "cabecera" que diga: *"yo soy el paquete número 47, y he recibido por tu parte hasta el 35, y además te confirmo que también me llegaron el 34, 33, 31 y 30"*. Eso es lo que hemos construido en esta propuesta.

---

## ¿Qué hemos construido?

- Un **enum de tipos de paquete** (`PacketType`) para que el servidor sepa de qué trata cada paquete nada más abrirlo
- Una **cabecera de 104 bits** (`PacketHeader`) que va al principio de cada paquete UDP
- Una **lógica de seguimiento de confirmaciones** (`SequenceContext`) que actualiza qué paquetes han llegado y cuáles no
- Integración en el `NetworkManager` para que parsee la cabecera automáticamente al recibir datos

---

## Cómo funciona — explicado paso a paso

### Paso 1 — El servidor manda un paquete

Antes de mandar cualquier cosa (estado del héroe, input, lo que sea), el servidor escribe primero la cabecera. La cabecera tiene 5 campos:

```
[ sequence (16 bits) | ack (16 bits) | ack_bits (32 bits) | type (8 bits) | timestamp (32 bits) ]
```

Total: 104 bits = 13 bytes. Siempre los mismos 13 bytes al principio, sin excepción.

### Paso 2 — El cliente recibe el paquete

El `NetworkManager` coge el paquete crudo de UDP y lo primero que hace es leer esos 13 bytes iniciales. Con ellos construye un `PacketHeader` y sabe:

- **sequence:** "este es el paquete número 47 que te mando"
- **ack:** "el último paquete tuyo que me llegó fue el 35"
- **ack_bits:** "y además me llegaron el 34, 33, 31, 30" (bitmask)
- **type:** "este paquete contiene estado del héroe (Snapshot)"
- **timestamp:** "lo mandé en el ms 3847 desde que arrancó el servidor"

### Paso 3 — El NetworkManager delega el resto

Tras leer la cabecera, el `NetworkManager` llama al callback con el header ya parseado y con el "lector de bits" posicionado justo después de la cabecera, en el payload real. El consumidor del callback puede leer la posición del héroe, su salud, etc., directamente sin preocuparse de la cabecera.

### Paso 4 — SequenceContext actualiza el bitmask

Cada conexión mantiene un `SequenceContext`. Cuando llega el paquete 47:

- Si 47 es más nuevo que el último que habíamos visto → actualizamos `remoteAck = 47` y desplazamos el bitmask
- Si 47 llegó fuera de orden (ya habíamos visto el 50) → marcamos el bit correspondiente sin cambiar `remoteAck`

---

## El código clave

### PacketHeader — escribir la cabecera

```cpp
void PacketHeader::Write(BitWriter& writer) const {
    writer.WriteBits(sequence, 16);  // número de este paquete
    writer.WriteBits(ack,      16);  // último paquete recibido del otro
    writer.WriteBits(ack_bits, 32);  // historial de 32 anteriores
    writer.WriteBits(type,      8);  // qué tipo de paquete es
    writer.WriteBits(timestamp, 32); // cuándo lo mandamos (ms)
}
```

`WriteBits(valor, numBits)` ya existía de la Fase 2. Escribe exactamente los bits que le pides, sin bytes de relleno innecesarios. Lo usamos aquí exactamente igual que para serializar la posición del héroe.

### PacketHeader — leer la cabecera

```cpp
PacketHeader PacketHeader::Read(BitReader& reader) {
    PacketHeader h;
    h.sequence  = static_cast<uint16_t>(reader.ReadBits(16));
    h.ack       = static_cast<uint16_t>(reader.ReadBits(16));
    h.ack_bits  = reader.ReadBits(32);
    h.type      = static_cast<uint8_t>(reader.ReadBits(8));
    h.timestamp = reader.ReadBits(32);
    return h;
}
```

`ReadBits` siempre devuelve `uint32_t`. El `static_cast` convierte al tipo exacto del campo. Importante: el `BitReader` avanza su posición interna automáticamente. Tras este Read, el reader está en el bit 104, apuntando al inicio del payload.

### SequenceContext::RecordReceived — el corazón del ACK Bitmask

```cpp
void SequenceContext::RecordReceived(uint16_t receivedSeq) {
    const int16_t diff = static_cast<int16_t>(receivedSeq - remoteAck);

    if (diff == 0) return; // duplicado

    if (diff > 0) {
        // Paquete más nuevo: desplazamos el bitmask y actualizamos remoteAck
        if (diff < 32) {
            ackBits = (ackBits << diff) | (1u << (diff - 1));
        } else {
            ackBits = 0; // salto demasiado grande, reseteamos
        }
        remoteAck = receivedSeq;
    } else {
        // Paquete antiguo que llegó tarde (fuera de orden)
        const int bit = -diff - 1;
        if (bit < 32) ackBits |= (1u << bit);
    }
}
```

Lo más importante de esta función es la primera línea:

```cpp
const int16_t diff = static_cast<int16_t>(receivedSeq - remoteAck);
```

Aquí hay un truco matemático para manejar el wrap-around de los números de secuencia.

### NetworkManager::Update — integración

```cpp
Shared::BitReader reader(*buffer, buffer->size() * 8);
const Shared::PacketHeader header = Shared::PacketHeader::Read(reader);

// reader está ahora en bit 104 — listo para el payload
if (m_onDataReceived)
    m_onDataReceived(header, reader, sender);
```

---

## Conceptos nuevos que aparecen aquí

| Concepto | Qué es | Por qué lo usamos |
|----------|--------|-------------------|
| **Número de secuencia** | Un contador que aumenta con cada paquete enviado (0, 1, 2, ... 65535, 0, 1, ...) | Permite al receptor saber si un paquete llegó fuera de orden o se perdió |
| **ACK (Acknowledgement)** | Confirmación de recepción: "he recibido hasta el paquete número X" | Sin ACKs no sabemos qué retransmitir |
| **ACK Bitmask** | Un número de 32 bits donde cada bit representa si un paquete anterior llegó o no | En lugar de confirmar de uno en uno, confirmamos 32 de golpe por paquete |
| **Wrap-around** | Cuando el número de secuencia llega a 65535 y vuelve a 0 | Los uint16_t tienen límite — hay que manejar ese salto sin confundirlo con pérdida de paquetes |
| **Aritmética modular int16_t** | El truco de castear `uint16_t - uint16_t` a `int16_t` para obtener la diferencia correcta incluso con wrap-around | Evita código complejo de comparación especial para el wrap-around |
| **Payload** | Los datos reales del paquete (posición, salud...) que van después del header | La cabecera es infraestructura; el payload es el contenido |

---

## Diagramas visuales

### Header wire format — 104 bits en wire (LSB-first)

```
 bit 0                                                                      bit 103
 ┌──────────────────┬──────────────────┬────────────────────────────────────┬──────────────────┬──────────────────────────────────────┐
 │    sequence      │       ack        │             ack_bits               │  type  │ flags   │            timestamp                 │
 │     16 bits      │     16 bits      │             32 bits                │ 4 bits │ 4 bits  │             32 bits                  │
 │   this packet    │  last recv'd     │  history: bit N = recv'd (ack-N-1) │pkt type│  flags  │  server time in ms (wraps ~49 days)  │
 └──────────────────┴──────────────────┴────────────────────────────────────┴──────────────────┴──────────────────────────────────────┘
  [0..15]            [16..31]           [32..63]                             [64..67] [68..71]   [72..103]

  Serialized with BitWriter (LSB-first block packing). Total = 13 bytes on wire.
  BitReader is positioned at bit 104 after Read() — ready for payload.
```

### ACK bitmask sliding window

```
  Scenario: remoteAck = 106, ack_bits = 0b00010110  (bits 1,2,4 set)

  seq:    ...  100  101  102  103  104  105  106
               │    │    │    │    │    │    │
  diff:        6    5    4    3    2    1    0  ← (remoteAck - seq)
  source:      ?  bit4  bit3 bit2 bit1 bit0  direct-ack
  ack_bits:       1    0    1    1    0
                  ✓    ✗    ✓    ✓    ✗    ✓
               ──────────────────────────────────────────
               OK  LOST  OK   OK  LOST  OK  ← IsAcked() result

  Window = 32 packets. Anything with diff > 32 → outside window → false.
```

### int16_t wrap-around trick — por qué funciona

```
  Normal case:
    seq=50, remoteAck=47  →  uint16_t diff = 50-47 = 3
                          →  int16_t cast = +3  → newer packet ✓

  Wrap-around case:
    seq=2, remoteAck=65534  →  uint16_t diff = 2-65534 = 4 (modular)
                            →  int16_t(4) = +4  → newer packet ✓  (CORRECT)

  Without int16_t cast:
    uint16_t diff = 4 → positive → also works here... but:

    seq=65534, remoteAck=2  →  uint16_t diff = 65534-2 = 65532
                            →  int16_t(65532) = -4  → older packet ✓  (CORRECT)
                            →  without cast: 65532 >> 32 → outside window (WRONG)

  Rule: always cast (a - b) to int16_t before comparing.
```

### SequenceContext::RecordReceived — flujo de decisión

```
  RecordReceived(receivedSeq)
         │
         ▼
  diff = int16_t(receivedSeq - remoteAck)
         │
         ├── diff == 0  ──────────────────► RETURN (duplicate, ignore)
         │
         ├── diff > 0  (newer packet)
         │      │
         │      ├── diff < 32  ─────────► shift ackBits left by diff
         │      │                         set bit (diff-1) = 1
         │      │                         remoteAck = receivedSeq
         │      │
         │      └── diff >= 32 ─────────► ackBits = 0  (history gap too large)
         │                                remoteAck = receivedSeq
         │
         └── diff < 0  (older, out-of-order)
                │
                bit = (-diff) - 1
                │
                ├── bit < 32  ──────────► ackBits |= (1 << bit)  (mark as received)
                └── bit >= 32 ──────────► ignore (too old, outside window)
```

## Cómo encaja en el sistema completo

```
SFMLTransport (UDP)
    ↓ std::vector<uint8_t> (bytes crudos)
NetworkManager::Update()
    ↓ crea BitReader
    ↓ PacketHeader::Read(reader)  ← NUEVO: parsea los primeros 104 bits
    ↓ loguea seq/ack/ack_bits/type/timestamp
    ↓ llama callback con (PacketHeader, BitReader posicionado en payload, EndPoint)
Brain / HeroSerializer  ← el callback lee el payload desde el reader
```

El `SequenceContext` (también NUEVO) vive en el `PacketManager` y será instanciado por `RemoteClient` en la Fase 3.2. Por ahora existe como estructura pero nadie la instancia aún.

---

## Decisiones de diseño que tomamos (y por qué)

| Decisión | Alternativa descartada | Motivo |
|----------|------------------------|--------|
| Callback recibe `BitReader&` en lugar de `vector<uint8_t>` | Pasar el vector completo y que el consumidor salte 13 bytes | Con BitReader el consumidor simplemente empieza a leer — no sabe ni que hay cabecera. Menos acoplamiento. |
| `SequenceContext` en `PacketHeader.h` (no en PacketManager) | Definirla directamente en PacketManager | RemoteClient no existe todavía (Fase 3.2). Poner la struct aquí la hace disponible sin crear dependencias prematuras. |
| Header de exactamente 104 bits (13 bytes, alineado a byte) | Header de 100 bits si PacketType fuera 4 bits | 13 bytes es múltiplo limpio de 8. Un header de 100 bits requeriría 13 bytes igualmente pero con 4 bits de relleno — más complejidad por 0 ahorro real. |
| Validación de tamaño mínimo en NetworkManager | Sin validación | Un paquete de 12 bytes haría que BitReader leyera fuera de límites → crash. Es defensa básica. |

---

## Qué podría salir mal (edge cases)

- **Paquete duplicado:** `diff == 0` → ignorado explícitamente con `return`
- **Salto de secuencia > 32:** El bitmask se resetea a 0 — toda la historia previa se descarta. Esto puede pasar si el cliente desconecta y reconecta (se manejará en Fase 3.2 con handshake)
- **Paquete muy antiguo (bit >= 32):** Fuera de ventana de 32 paquetes → se ignora silenciosamente. A 128Hz, 32 paquetes = ~250ms de historial, suficiente para ráfagas normales
- **Buffer < 13 bytes:** El NetworkManager lo descarta con log de Warning antes de crear el BitReader

---

## Qué aprender si quieres profundizar

- Fiedler, G. (2016). *Packet Acks* — explica exactamente este sistema de bitmask con ejemplos visuales: https://gafferongames.com/post/packet-acks/ (la base de nuestra implementación)
- Fiedler, G. (2016). *State Synchronization* — cómo los ACKs alimentan el sistema de Delta Compression que viene en Fase 3.5

---

## Estado del sistema tras esta implementación

**Funciona:**
- Cada paquete que llega es parseado: se extrae el header completo antes de procesar el payload
- El sistema detecta y descarta paquetes malformados (<13 bytes)
- `SequenceContext` puede trackear correctamente qué paquetes han llegado con wrap-around correcto
- El log muestra `seq/ack/ack_bits/type/timestamp` para cada paquete recibido

**Pendiente (próxima propuesta):**
- `RemoteClient` (Fase 3.2 — Handshake): instanciará un `SequenceContext` por cliente conectado
- Retransmisión (Fase 3.3 — Reliability Layer): usará los ACKs para decidir qué reenviar
- Clock Sync (Fase 3.4): usará el campo `timestamp` que ya incluimos aquí

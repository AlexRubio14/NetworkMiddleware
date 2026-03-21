---
type: implementation-report
proposal: P-3.2
date: 2026-03-21
status: ready-for-gemini
---

# IMPLEMENTATION REPORT — P-3.2 Connection Handshake

**Propuesta:** [[proposals/P-3.2 Client-Side Prediction]]
**Fecha:** 2026-03-21
**De:** Claude → Gemini

---

## Qué se ha implementado

- `HandshakePackets.h`: payloads para los 3 pasos con datos — `ChallengePayload` (uint64_t salt serializado en 2×32 bits), `ChallengeResponsePayload` (alias de ChallengePayload), `ConnectionAcceptedPayload` (uint16_t networkID)
- `RemoteClient.h`: struct con `EndPoint`, `networkID`, `SequenceContext`, `challengeSalt`, `challengeSentAt` + método `IsTimedOut(seconds)`
- `NetworkAddress.h`: añadidos `operator==` y `operator<` a `EndPoint` para uso como clave de `std::map`
- `PacketTypes.h`: tipos 0x6–0xA añadidos (ConnectionRequest, ConnectionChallenge, ChallengeResponse, ConnectionAccepted, ConnectionDenied)
- `NetworkManager`: máquina de estados completa con `m_pendingClients` / `m_establishedClients` (`std::map<EndPoint, RemoteClient>`), timeout de 5s en `CheckTimeouts()`, límite de `kMaxClients = 10`, `OnClientConnectedCallback`, helpers `SendHeaderOnly` / `SendChallenge` / `SendConnectionAccepted`

## Desviaciones del Design Handoff

| Cambio | Motivo |
|--------|--------|
| `OnClientConnectedCallback` añadido | La capa de juego necesita saber cuándo un cliente completa el handshake para registrarlo en el estado de partida. No estaba en el handoff pero es necesario para cualquier consumidor de NetworkManager |
| `ChallengeResponsePayload` = alias de `ChallengePayload` | El payload es idéntico (mismo salt, misma serialización). Un alias evita duplicar código |
| `GetEstablishedCount()` / `GetPendingCount()` en NetworkManager | Útil para el tick loop (logs de estado, validación de tests) |

## Archivos modificados

- Creado: `Shared/Network/HandshakePackets.h`
- Creado: `Core/RemoteClient.h`
- Modificado: `Shared/Network/PacketTypes.h` — 5 tipos nuevos
- Modificado: `Shared/NetworkAddress.h` — `operator==`, `operator<`
- Modificado: `Core/NetworkManager.h` — maps, callbacks, helpers declarados
- Modificado: `Core/NetworkManager.cpp` — implementación completa del handshake
- Modificado: `Shared/CMakeLists.txt` — `HandshakePackets.h`
- Modificado: `Core/CMakeLists.txt` — `RemoteClient.h`

## Fragmentos clave para revisar

```cpp
// Máquina de estados en Update()
switch (static_cast<Shared::PacketType>(header.type)) {
    case Shared::PacketType::ConnectionRequest:
        HandleConnectionRequest(sender); break;
    case Shared::PacketType::ChallengeResponse:
        HandleChallengeResponse(reader, sender); break;
    default:
        // Solo pasa al callback si el cliente está establecido
        if (m_establishedClients.contains(sender)) {
            m_establishedClients.at(sender).seqContext.RecordReceived(header.sequence);
            if (m_onDataReceived) m_onDataReceived(header, reader, sender);
        }
}
```

```cpp
// Salt uint64_t serializado en 2×32 bits (BitWriter solo acepta uint32_t)
void ChallengePayload::Write(BitWriter& writer) const {
    writer.WriteBits(static_cast<uint32_t>(salt & 0xFFFFFFFF), 32);
    writer.WriteBits(static_cast<uint32_t>((salt >> 32) & 0xFFFFFFFF), 32);
}
```

## Preguntas para Gemini

1. **kMaxClients = 10**: ¿Confirmamos este límite como definitivo para el MOBA, o lo dejamos configurable en runtime?
2. **Welcome + primer Snapshot**: ¿El paquete `ConnectionAccepted` debería incluir ya el primer Snapshot completo del estado de partida, o esperamos al primer tick de simulación? (Impacta en el diseño del payload de `ConnectionAcceptedPayload`)
3. **`OnClientConnectedCallback` no estaba en el handoff**: ¿Lo apruebas como adición necesaria o prefieres un mecanismo diferente para notificar al game layer?

## Estado actual del sistema

- El servidor acepta conexiones autenticadas: ignora paquetes de juego de IPs no handshakeadas
- Timeout de 5s en handshakes sin respuesta
- `SequenceContext` de cada cliente se actualiza automáticamente al recibir paquetes de juego
- La retransmisión (Reliability, P-3.3) y el Clock Sync (P-3.4) aún no implementados

## Siguiente propuesta sugerida

[[proposals/P-3.3 Jitter Buffer]] o [[proposals/P-3.4 Reliability Tercer Canal]]

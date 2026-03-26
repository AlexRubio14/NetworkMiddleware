---
type: implementation-report
proposal: P-3.6
date: 2026-03-22
status: pending-gemini-validation
---

# Implementation Report — P-3.6 Session Recovery (Heartbeats & Timeouts)

**Propuesta:** P-3.6 Session Recovery — Heartbeats, Session Timeout, Zombie State & Reconnection
**Fecha:** 2026-03-22
**De:** Claude → Gemini

---

## Qué se ha implementado

- **Heartbeat automático** en `NetworkManager` — si el servidor no envía nada a un cliente durante `kHeartbeatInterval` (1s), `ProcessSessionKeepAlive` envía un `PacketType::Heartbeat`. Los Heartbeats recibidos no disparan el callback de datos (son keepalive puro).
- **Session Timeout → Zombie** — si no llega ningún paquete de un cliente durante `kSessionTimeout` (10s), el cliente pasa a estado zombie (`isZombie = true`). El cliente sigue en `m_establishedClients` (por su endpoint antiguo) durante `kZombieDuration` (120s).
- **Zombie expiry** — tras `kZombieDuration` sin reconexión, el cliente se elimina y se dispara `OnClientDisconnectedCallback`.
- **Graceful Disconnect** (`PacketType::Disconnect = 0x0B`) — el cliente cierra limpiamente; el servidor elimina la sesión y dispara el callback de desconexión de inmediato.
- **ReconnectionRequest** (`PacketType::ReconnectionRequest = 0x0C`) — el cliente zombie presenta su `networkID` anterior y el `reconnectionToken` emitido al conectarse. Si el token es válido, la sesión se mueve al nuevo endpoint, se des-zombifica y se envía `ConnectionAccepted` (mismo token). Si el token es inválido o el cliente no está en estado zombie, se envía `ConnectionDenied`.
- **Reconnection Token** — `uint64_t` aleatorio generado con `mt19937_64` en `HandleChallengeResponse` y entregado en `ConnectionAcceptedPayload`. Almacenado en `RemoteClient`.
- **Constantes fácilmente configurables** — `kHeartbeatInterval`, `kSessionTimeout`, `kZombieDuration` son `static constexpr` en `NetworkManager`, cambiables en un solo lugar.
- **`ProcessSessionKeepAlive(time_point now)` público** — permite inyectar un tiempo sintético en tests sin infraestructura de mock de reloj.
- **10 tests nuevos** en `SessionRecoveryTests.cpp`. **Total: 104 tests, 100% passing** (Windows/MSVC).

---

## Archivos creados / modificados

| Archivo | Tipo | Descripción |
|---------|------|-------------|
| `Shared/Network/PacketTypes.h` | Modificado | `Disconnect=0x0B`, `ReconnectionRequest=0x0C` |
| `Shared/Network/HandshakePackets.h` | Modificado | `ConnectionAcceptedPayload`: +`reconnectionToken` (80 bits totales); nuevo `ReconnectionRequestPayload` (oldNetworkID + token, 80 bits) |
| `Core/RemoteClient.h` | Modificado | +`reconnectionToken`, `isZombie`, `lastIncomingTime`, `lastOutgoingTime`, `zombieTime` |
| `Core/NetworkManager.h` | Modificado | +`kHeartbeatInterval/kSessionTimeout/kZombieDuration`; declaraciones `ProcessSessionKeepAlive`, `HandleDisconnect`, `HandleReconnectionRequest`, `IsClientZombie` |
| `Core/NetworkManager.cpp` | Modificado | Implementación completa de P-3.6; routing de nuevos tipos de paquete; fix Heartbeat no dispara data callback; tracking de `lastOutgoingTime` en `Send()` |
| `tests/Core/SessionRecoveryTests.cpp` | Creado | 10 tests — heartbeat, disconnect, timeout, zombie, reconexión |
| `tests/CMakeLists.txt` | Modificado | Registro de `SessionRecoveryTests.cpp` |

---

## Desviaciones del Design Handoff

| Cambio | Motivo |
|--------|--------|
| `kZombieDuration = 120s` (no 60s del handoff) | Petición explícita del usuario: ventana de reconexión más amplia, variable fácil de cambiar. |
| `ProcessSessionKeepAlive(time_point now)` es **público** | Necesario para tests deterministas sin mock de reloj. Alternativa (inyección de reloj vía constructor) habría sido over-engineering para el scope actual. |
| Los clientes zombie permanecen en `m_establishedClients` (no en un mapa separado) | Simplifica la búsqueda por endpoint y el flujo de expiry. La bandera `isZombie` actúa como filtro en el routing de paquetes. |
| Mismo token en la reconexión (no se genera uno nuevo) | El handoff no especifica rotación de tokens. Mantenerlo igual simplifica el flujo; si se añade rotación futura, es un cambio de una línea. |
| `Disconnect` gestionado dentro del `default` branch (no case propio) | El cliente desconectándose aún tiene su endpoint original en `m_establishedClients`. El `find(sender)` del default branch localiza al cliente correctamente. `ReconnectionRequest` sí tiene case propio porque puede llegar desde un endpoint diferente. |

---

## Fragmentos clave para revisar

### Constantes configurables (NetworkManager.h)
```cpp
static constexpr std::chrono::seconds kHeartbeatInterval{1};   // Send heartbeat after 1s of silence
static constexpr std::chrono::seconds kSessionTimeout{10};     // Mark zombie after 10s no incoming
static constexpr std::chrono::seconds kZombieDuration{120};    // Remove zombie after 120s
```

### ProcessSessionKeepAlive (NetworkManager.cpp)
```cpp
void NetworkManager::ProcessSessionKeepAlive(std::chrono::steady_clock::time_point now) {
    std::vector<Shared::EndPoint> toExpire;

    for (auto& [endpoint, client] : m_establishedClients) {
        if (client.isZombie) {
            if (now - client.zombieTime > kZombieDuration)
                toExpire.push_back(endpoint);
            continue;
        }
        if (now - client.lastIncomingTime > kSessionTimeout) {
            client.isZombie   = true;
            client.zombieTime = now;
            continue;
        }
        if (now - client.lastOutgoingTime > kHeartbeatInterval)
            Send(endpoint, {}, Shared::PacketType::Heartbeat);
    }
    // ... expiry loop
}
```

### HandleReconnectionRequest — búsqueda por networkID (no endpoint)
```cpp
auto it = std::find_if(m_establishedClients.begin(), m_establishedClients.end(),
    [&](const auto& entry) {
        return entry.second.networkID == req.oldNetworkID && entry.second.isZombie;
    });
// Si token válido: move client, update endpoint, des-zombificar, SendConnectionAccepted
```

### Test clave — timeout determinista sin sleep
```cpp
TEST(SessionRecovery, SessionTimeout_MarksClientAsZombie) {
    // Inyectar un "now" artificial 11s en el futuro (> kSessionTimeout de 10s)
    nm.ProcessSessionKeepAlive(std::chrono::steady_clock::now() + std::chrono::seconds(11));
    EXPECT_TRUE(nm.IsClientZombie(ep));
}
```

---

## Cobertura de tests por módulo

| Suite | Tests | Escenarios cubiertos |
|-------|-------|----------------------|
| `SessionRecovery` (Heartbeat) | 2 | Heartbeat recibido no dispara data callback; servidor envía heartbeat tras 1s sin outgoing |
| `SessionRecovery` (Disconnect) | 2 | Disconnect limpio dispara callback + elimina cliente; Disconnect de desconocido ignorado |
| `SessionRecovery` (Timeout/Zombie) | 3 | Timeout → zombie; dentro del intervalo → no zombie; zombie expira → callback + eliminado |
| `SessionRecovery` (Reconnection) | 3 | Token válido → cliente en nuevo endpoint; token inválido → ConnectionDenied; cliente no-zombie → rechazado |

---

## Resultado local

```text
100% tests passed, 0 tests failed out of 104
Total Test time (real) = 0.01 sec
Platform: Windows / MSVC 19.44
```

---

## Estado actual del sistema

- 104 tests verdes localmente (Windows/MSVC)
- Branch: `Session-Recovery` → pendiente de PR a `develop`
- CI pendiente de ejecución (Ubuntu + Windows)

## Siguiente propuesta sugerida

P-4.x — Brain / Predictive AI Layer (compensación de pérdida de paquetes en posición)

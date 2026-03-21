---
type: dev-log-alex
proposal: P-3.2
date: 2026-03-21
status: personal
---

# DEV LOG — P-3.2 Connection Handshake

**Propuesta:** [[proposals/P-3.2 Client-Side Prediction]]
**Fecha:** 2026-03-21

---

## ¿Qué problema resolvíamos?

Hasta ahora, el servidor aceptaba paquetes UDP de cualquier IP sin verificar nada. Cualquiera podía mandar bytes con formato correcto y el servidor los procesaría como si fuera un jugador legítimo. Esto es un problema de seguridad básico.

El **handshake** es el "apretón de manos" inicial que todo cliente debe completar antes de que el servidor le haga caso. Si no lo completa, sus paquetes se descartan silenciosamente.

---

## ¿Qué hemos construido?

- Un **protocolo de 4 pasos** para establecer conexiones autenticadas
- Una **máquina de estados** en el NetworkManager que gestiona quién está conectado y quién no
- Un **sistema de timeout** que limpia intentos de conexión abandonados
- Los **payloads de handshake** serializados con nuestro BitWriter

---

## Cómo funciona — el flujo completo

### El problema de IP Spoofing

Antes de explicar el handshake, hay que entender qué problema resuelve. UDP no tiene conexión — cualquiera puede enviar un paquete con IP de origen falsa. Si el servidor simplemente aceptara el primer paquete que llegue de una IP, un atacante podría:

1. Ver el tráfico de red y saber el formato del `ConnectionRequest`
2. Enviar ese mismo formato con IP de origen de otro jugador
3. Inyectar datos falsos como si fuera ese jugador

El handshake resuelve esto con un **desafío matemático** que solo puede completar alguien que realmente recibe paquetes en esa IP.

### Paso 1 — Cliente manda ConnectionRequest (0x6)

Solo el header de P-3.1, sin payload. Dice: *"quiero conectarme"*.

El servidor comprueba:
- ¿Ya está conectado? → ignorar (idempotente)
- ¿El servidor está lleno (≥10 clientes)? → mandar ConnectionDenied
- Si no → generar un número aleatorio de 64 bits (el **salt**) y mandarlo

### Paso 2 — Servidor manda ConnectionChallenge (0x7)

El servidor genera un `uint64_t` aleatorio usando un generador criptográfico (`std::mt19937_64` con `std::random_device`).

Guarda este número internamente asociado a la IP del cliente, y lo manda en el paquete Challenge.

El razonamiento: si el cliente realmente tiene esa IP, recibirá este número. Si estaba usando una IP falsa (spoofing), el paquete llegará a la IP real, no al atacante.

### Paso 3 — Cliente devuelve ChallengeResponse (0x8)

El cliente devuelve exactamente el mismo número que recibió. Dice: *"pruebo que recibí tu paquete devolviendo el número"*.

### Paso 4 — Servidor verifica y acepta/rechaza

El servidor compara el salt que guardó con el que recibió:
- ¿Coinciden? → el cliente es legítimo → mandar ConnectionAccepted con su **NetworkID** asignado
- ¿No coinciden? → posible ataque → mandar ConnectionDenied y eliminar el intento

```
Cliente                           Servidor
  |                                   |
  |-- ConnectionRequest (0x6) ------> |
  |                                   | genera salt = 0xABCD1234...
  | <----- ConnectionChallenge (0x7)--|
  |                                   |
  |-- ChallengeResponse (0x8) ------> |  ← devuelve el mismo salt
  |                                   | salt coincide → NetworkID = 1
  | <----- ConnectionAccepted (0x9) --|
  |                                   |
  |== PAQUETES DE JUEGO ==============|  ← solo ahora se aceptan
```

---

## El código clave

### La máquina de estados en Update()

```cpp
switch (static_cast<Shared::PacketType>(header.type)) {
    case Shared::PacketType::ConnectionRequest:
        HandleConnectionRequest(sender);
        break;

    case Shared::PacketType::ChallengeResponse:
        HandleChallengeResponse(reader, sender);
        break;

    default:
        // Paquete de juego: SOLO pasa si el cliente está en m_establishedClients
        if (m_establishedClients.contains(sender)) {
            auto& client = m_establishedClients.at(sender);
            client.seqContext.RecordReceived(header.sequence);  // actualiza el ACK bitmask
            if (m_onDataReceived)
                m_onDataReceived(header, reader, sender);
        }
        // Si no está establecido → silencio total. Ningún atacante llega al Brain.
        break;
}
```

El `default` es el escudo de seguridad: cualquier paquete que no sea de handshake, si viene de alguien que no completó el proceso, desaparece sin traza.

### La serialización del salt de 64 bits

El salt es `uint64_t` (64 bits), pero nuestro `BitWriter::WriteBits` solo acepta `uint32_t` (32 bits). La solución: partirlo en dos mitades.

```cpp
void ChallengePayload::Write(BitWriter& writer) const {
    writer.WriteBits(static_cast<uint32_t>(salt & 0xFFFFFFFF), 32);          // mitad baja
    writer.WriteBits(static_cast<uint32_t>((salt >> 32) & 0xFFFFFFFF), 32);  // mitad alta
}

static ChallengePayload Read(BitReader& reader) {
    ChallengePayload p;
    const uint32_t lo = reader.ReadBits(32);  // primero la mitad baja
    const uint32_t hi = reader.ReadBits(32);  // luego la mitad alta
    p.salt = (static_cast<uint64_t>(hi) << 32) | lo;  // recomponer
    return p;
}
```

El orden importa: escribimos low primero, leemos low primero. Si lo invertimos, el salt reconstruido sería diferente aunque los bits sean los mismos.

### RemoteClient — guardar el estado de cada conexión

```cpp
struct RemoteClient {
    Shared::EndPoint                       endpoint;       // IP:Puerto del cliente
    uint16_t                               networkID = 0;  // ID único de sesión
    Shared::SequenceContext                seqContext;     // ACK bitmask de P-3.1
    uint64_t                               challengeSalt = 0; // el número que mandamos
    std::chrono::steady_clock::time_point  challengeSentAt;   // cuándo mandamos el Challenge

    bool IsTimedOut(std::chrono::seconds timeout) const {
        const auto elapsed = std::chrono::steady_clock::now() - challengeSentAt;
        return elapsed > timeout;
    }
};
```

El servidor tiene dos colecciones:
- `m_pendingClients` — han mandado ConnectionRequest, esperando ChallengeResponse
- `m_establishedClients` — handshake completo, pueden mandar datos de juego

### El timeout de 5 segundos

```cpp
void NetworkManager::CheckTimeouts() {
    std::erase_if(m_pendingClients, [this](const auto& entry) {
        if (entry.second.IsTimedOut(kHandshakeTimeout)) {
            // Log y eliminar
            return true;
        }
        return false;
    });
}
```

`std::erase_if` elimina del map todas las entradas para las que la función devuelve `true`. Se llama al inicio de cada `Update()`, antes de procesar el nuevo paquete. Un cliente que recibe el Challenge pero no responde en 5 segundos desaparece limpiamente.

---

## Conceptos nuevos que aparecen aquí

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **IP Spoofing** | Enviar paquetes UDP con una IP de origen falsa | Sin el handshake, un atacante podría hacerse pasar por otro jugador |
| **Salt criptográfico** | Número aleatorio de un solo uso para verificar identidad | Si el atacante no puede recibir el salt (porque tiene IP falsa), no puede responder |
| **Máquina de estados** | Un sistema donde el comportamiento depende del estado actual | Un cliente "pendiente" y uno "establecido" se tratan de forma completamente distinta |
| **std::mt19937_64** | Generador de números pseudoaleatorios de 64 bits de Mersenne Twister | Calidad criptográfica suficiente para el salt; seeded con `std::random_device` (entropía del SO) |
| **std::erase_if** | C++20: elimina elementos de un contenedor según una condición | Forma limpia de purgar timeouts sin bucles manuales |
| **NetworkID** | Identificador único de sesión (uint16_t, 1–65535) | Permite al game layer identificar quién mandó qué sin usar IP:Puerto como clave |

---

## Diagramas visuales

### Handshake completo — prevención de IP spoofing

```
  CLIENT (real IP: 192.168.1.5)        SERVER                    ATTACKER (fake IP: 192.168.1.5)
         │                                │                                │
         │──── ConnectionRequest ────────►│                                │
         │                                │  salt = 0xDEADBEEF12345678     │
         │                                │  m_pendingClients[1.5] = salt  │
         │◄─── ConnectionChallenge(salt) ─│                                │
         │     (UDP → arrives at 1.5)     │                                │
         │                                │         ╔══════════════════╗   │
         │                                │         ║ attacker NEVER   ║──►│
         │                                │         ║ receives this    ║   │
         │                                │         ╚══════════════════╝   │
         │──── ChallengeResponse(salt) ───►│                                │
         │     (salt matches ✓)           │  NetworkID = 1 assigned        │
         │◄─── ConnectionAccepted(ID=1) ──│  move to m_establishedClients  │
         │                                │                                │
         │════ GAME PACKETS ══════════════│  only established clients pass │
```

### Máquina de estados — transiciones de RemoteClient

```
                    ┌─────────────────────────────────────────────────────┐
                    │              m_pendingClients                       │
    ConnectionReq   │  { EndPoint → RemoteClient{                        │
  ──────────────►   │      networkID  (reserved),                        │
    (first time)    │      salt       (random 64-bit),                   │
                    │      timer      (challengeSentAt)                  │
                    │  } }                                                │
                    └──────────────┬──────────────────┬───────────────────┘
                                   │                  │
                    ChallengeResponse                 timeout > 5s
                    (salt matches ✓)                  │
                                   │                  ▼
                                   │             ERASED silently
                                   ▼
                    ┌─────────────────────────────────────────────────────┐
                    │             m_establishedClients                    │
                    │  { EndPoint → RemoteClient{                        │
                    │      networkID,  seqContext,                        │
                    │      m_reliableSents, m_rtt, ...                   │
                    │  } }                                                │
                    └──────────────────────────┬──────────────────────────┘
                                               │
                                        kMaxRetries exceeded
                                        (Link Loss, P-3.3)
                                               │
                                               ▼
                                        DisconnectClient()
                                        → OnClientDisconnected fired
```

### Seguridad — por qué el salt previene inyección

```
  WITHOUT handshake:
    Attacker knows packet format ──► sends fake Snapshot with hero data ──► SERVER ACCEPTS ✗

  WITH challenge-response:
    Step 1: Server sends random 64-bit salt to CLIENT's real IP
    Step 2: Only the real client receives it (UDP routing)
    Step 3: Client echoes salt back — server verifies
    Result: Attacker cannot forge salt they never received ──► REJECTED ✓

  Probability of guessing a 64-bit salt: 1 / 2^64 ≈ 5.4 × 10^-20
```

## Cómo encaja en el sistema completo

```
SFMLTransport (UDP)
    ↓ bytes crudos
NetworkManager::Update()
    ↓ CheckTimeouts() — limpia pendientes expirados
    ↓ PacketHeader::Read() — parsea header de P-3.1
    ↓
    ├── ConnectionRequest  → HandleConnectionRequest()
    │       → genera salt → m_pendingClients → SendChallenge()
    │
    ├── ChallengeResponse → HandleChallengeResponse()
    │       → verifica salt → m_establishedClients → SendConnectionAccepted()
    │       → dispara OnClientConnectedCallback (notifica al Brain / main.cpp)
    │
    └── Cualquier otro tipo
            → ¿está en m_establishedClients?
                SÍ → seqContext.RecordReceived() + m_onDataReceived (payload al game)
                NO → descartado silenciosamente
```

---

## Decisiones de diseño (y por qué)

| Decisión | Alternativa | Motivo |
|----------|-------------|--------|
| `std::map<EndPoint, RemoteClient>` con `operator<` | `unordered_map` con hash | Más simple: solo necesito `operator<`, no una función hash. El coste O(log n) es irrelevante con ≤10 clientes |
| `kMaxClients = 10` como constante | Configurable en runtime | Suficiente para desarrollo. Gemini aprueba moverlo a `NetworkConfig` en el futuro cuando haga falta |
| No incluir Snapshot en ConnectionAccepted | Incluirlo para ahorrar un RTT | El Snapshot inicial puede superar los 1500 bytes (MTU). Fragmentación IP es peor que un RTT extra |
| `ChallengeResponsePayload` = alias de `ChallengePayload` | Struct separada | El payload es idéntico. Alias = DRY sin perder claridad semántica |
| `OnClientConnectedCallback` (no estaba en el handoff) | Game layer consulta el estado periódicamente | El callback sigue el patrón existente. El NetworkManager no sabe nada de héroes ni gameplay — solo notifica |

---

## Qué podría salir mal (edge cases)

- **Cliente reconecta antes del timeout:** `insert_or_assign` sobrescribe el pending anterior con nuevo salt. Correcto.
- **Servidor lleno cuando llega ConnectionRequest:** Se manda ConnectionDenied inmediatamente. El cliente puede reintentarlo (en Fase 3.7 habrá reconexión automática).
- **ChallengeResponse con salt correcto pero de otra IP:** Imposible — el salt está indexado por EndPoint. Una IP diferente buscaría en `m_pendingClients` con otra clave y no la encontraría.
- **Paquete de juego llega antes de completar handshake:** Descartado silenciosamente en el `default` del switch.

---

## Qué aprender si quieres profundizar

- Fiedler, G. (2016). *Client Server Connection* — describe exactamente este patrón de handshake con challenge: https://gafferongames.com/post/client_server_connection/
- El concepto de "peaje de autenticación" (Authentication Gate) es estándar en cualquier servidor de juego online

---

## Estado del sistema tras esta implementación

**Funciona:**
- El servidor solo procesa datos de juego de clientes que han completado el handshake
- Los intentos de conexión expirados se limpian automáticamente cada tick
- El game layer recibe notificación (`OnClientConnectedCallback`) cuando entra un nuevo jugador

**Pendiente (próxima propuesta):**
- P-3.3 Reliability Layer: usar el ACK bitmask de P-3.1 para retransmitir paquetes importantes (habilidades, muertes)
- P-3.4 Clock Sync: el campo `timestamp` del header empieza a tener valor real
- `NetworkConfig` (futuro): mover `kMaxClients` y `kHandshakeTimeout` a struct configurable

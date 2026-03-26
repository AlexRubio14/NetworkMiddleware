---
type: dev-log-alex
proposal: P-4.1+4.2
date: 2026-03-22
status: personal
---

# DEV LOG — P-4.1+4.2 Infraestructura de Simulación y Carga

**Propuesta:** P-4.1+4.2 Dockerización + HeadlessBot (combinados)
**Fecha:** 2026-03-22

---

## ¿Qué problema resolvíamos?

Tras la Fase 3 tenemos un protocolo de red completo y correctísimo — validado con 106 tests que pasan en milisegundos porque usan `MockTransport` en memoria. Pero eso tiene un límite:

- ¿Cómo sabemos que el servidor aguanta **10 clientes a 60 Hz** simultáneos?
- ¿Cuánto ancho de banda consume realmente con 5 jugadores en teamfight?
- ¿El tick rate se mantiene estable bajo carga o hay jitter?

No podemos responder ninguna de esas preguntas sin un cliente real que genere tráfico UDP real. Y no podemos reproducir esas mediciones de forma consistente si el entorno de ejecución cambia (mi máquina, un servidor de cloud, el servidor del tribunal).

P-4.1+4.2 resuelve ambos problemas de una vez:

1. **HeadlessBot** — un cliente C++ real que ejecuta el protocolo completo y genera carga a 60 Hz
2. **Docker** — el entorno de ejecución del servidor y los bots es reproducible, portátil y controlable

---

## ¿Qué hemos construido?

| Componente | Dónde | Qué hace |
|-----------|-------|---------|
| **`InputPayload`** | `Shared/Network/InputPackets.h` | Wire format de 24 bits para comandos de input del cliente |
| **`BotClient`** | `Core/BotClient.h/.cpp` | Máquina de estados cliente — protocolo completo hasta Established |
| **`HeadlessBot`** | `HeadlessBot/main.cpp` | Ejecutable que conecta y envía inputs a 60 Hz |
| **`Dockerfile`** | raíz | Build multi-stage: compilación Release → imagen runtime mínima |
| **`docker-compose.yml`** | raíz | Orquestación servidor + N bots escalables |
| **`BotIntegrationTests`** | `tests/Core/` | 3 tests de integración bidireccionales |

---

## InputPayload — el lenguaje del bot

El servidor ya tenía `PacketType::Input = 0x2` desde P-3.1, pero nadie había definido qué iban los bits después del header. Era un tipo sin payload.

Para el stress test, el bot necesita generar paquetes que el servidor no rechace y que sean representativos del tráfico real de un MOBA. Un input de MOBA contiene:
- Dirección de movimiento (vector normalizado XY)
- Botones presionados (habilidades, ataque)

La solución mínima viable es **24 bits**:

```text
Wire format del InputPayload (24 bits totales):
  ┌─────────────┬─────────────┬──────────────────────────────┐
  │  dirX (8b)  │  dirY (8b)  │        buttons (8b)          │
  └─────────────┴─────────────┴──────────────────────────────┘
   [-1.0, 1.0]   [-1.0, 1.0]   bit0=Ability1 bit1=Ability2
   quantizado     quantizado    bit2=Ability3 bit3=Ultimate
   a 8 bits       a 8 bits      bit4=Attack
```

Para cuantizar el rango [-1.0, 1.0] a 8 bits reutilizamos `NetworkOptimizer::QuantizeFloat` que ya existe del sistema de posición. 8 bits dan 256 valores en el rango — más que suficiente para distinguir las 8 direcciones de movimiento de un MOBA.

Con 24 bits, el paquete completo (header + payload) es:
- Header: 104 bits
- InputPayload: 24 bits
- **Total: 128 bits = 16 bytes** — encaja perfectamente en cualquier MTU UDP

---

## BotClient — la máquina de estados

### Por qué BotClient vive en `Core/` y no en `HeadlessBot/`

La tentación era meter toda la lógica del bot en `HeadlessBot/main.cpp`. Pero los tests de integración están en `tests/Core/` y necesitan instanciar un `BotClient` con `MockTransport` — sin tocar SFML ni UDP real.

Si la lógica estuviera en el ejecutable, los tests no podrían usarla. Al meterla en `Core/` (biblioteca estática), tanto `HeadlessBot/main.cpp` como `tests/Core/BotIntegrationTests.cpp` pueden incluirla. Mismo patrón que `NetworkManager` — la lógica es independiente del transport.

### Los 5 estados

```text
                    Connect()
                       │
                       ▼
  ┌────────────┐  ConnectionRequest  ┌─────────────┐
  │Disconnected│─────────────────────►  Connecting  │
  └────────────┘                     └──────┬──────┘
        ▲                                   │ ConnectionChallenge
        │                                   ▼ (extrae salt)
    Disconnect()                    ┌──────────────┐
        │                           │  Challenging  │
        │                           └──────┬───────┘
        │                                  │ ConnectionAccepted
        │                                  ▼ (guarda networkID + token)
        │                          ┌──────────────┐
        └──────────────────────────│  Established  │
                                   └──────────────┘
                                          │
                           ConnectionDenied (cualquier estado)
                                          │
                                          ▼
                                    ┌─────────┐
                                    │  Error  │
                                    └─────────┘
```

### Sequence tracking — por qué el bot lleva su propio SequenceContext

El servidor, al recibir paquetes del bot, lee los campos `ack` y `ack_bits` del header para avanzar su cola de paquetes fiables y sus baselines de Delta Compression. Si el bot enviara siempre `ack=0, ack_bits=0`, el servidor nunca limpiaría sus colas — acumulación indefinida de `PendingPacket`.

`BotClient` tiene un `SequenceContext m_seqCtx` que:
- `localSequence`: número de secuencia de los paquetes salientes del bot (incrementa con cada send)
- `remoteAck / ackBits`: registro de los paquetes del servidor que el bot ha recibido (actualizado en `Update()` con `RecordReceived`)

Cada paquete saliente lleva `seq=localSequence, ack=remoteAck, ack_bits=ackBits` — el servidor sabe exactamente qué ha recibido el bot y puede limpiar su estado.

### Flujo de un handshake completo

```text
BotClient                                   NetworkManager
    │                                             │
    │─── Connect() ──────────────────────────────►│
    │    Send(ConnectionRequest, seq=0)           │
    │                                             │ HandleConnectionRequest()
    │                                             │ m_pendingClients.emplace(ep)
    │                                             │ Send(Challenge, salt=0xDEAD...)
    │◄── ConnectionChallenge(salt) ───────────────│
    │                                             │
    │─── HandleChallenge() ───────────────────────►│
    │    m_pendingSalt = 0xDEAD...               │
    │    m_state = Challenging                    │
    │    Send(ChallengeResponse, seq=1)           │
    │                                             │ HandleChallengeResponse()
    │                                             │ move → m_establishedClients
    │                                             │ reconnectionToken = random
    │                                             │ Send(ConnectionAccepted, networkID, token)
    │◄── ConnectionAccepted(networkID, token) ────│
    │                                             │
    │─── HandleConnectionAccepted() ─────────────►│
    │    m_networkID = id                         │
    │    m_reconnectionToken = token              │
    │    m_state = Established                    │
```

---

## HeadlessBot — el ejecutable de estrés

### ParseIpv4 — sin DNS, sin dependencias externas

El bot necesita convertir `SERVER_HOST=127.0.0.1` (string de entorno) a un `uint32_t` compatible con `EndPoint`. No podemos usar `sf::IpAddress` (viola el aislamiento SFML) ni `getaddrinfo` (no necesario para IPs numéricas).

`EndPoint` almacena los octetos en little-endian (igual que `sf::IpAddress::toInteger()` en x86):

```cpp
// "127.0.0.1" → 0x0100007F
// octet A en bits 0-7, B en 8-15, C en 16-23, D en 24-31
static uint32_t ParseIpv4(const std::string& ip) {
    uint32_t result = 0;
    int shift = 0;
    std::istringstream ss(ip);
    std::string token;
    while (std::getline(ss, token, '.') && shift < 32) {
        result |= (static_cast<uint32_t>(std::stoi(token)) << shift);
        shift += 8;
    }
    return result;
}
```

### Initialize(0) — puertos efímeros

El bot llama `transport->Initialize(0)`. El 0 es especial para `sf::UdpSocket::bind(0)` — el OS asigna cualquier puerto libre. Con `network_mode: host` y 10 bots en el mismo host, si todos se intentaran ligar al mismo puerto fijo habría conflicto. Con port=0, cada instancia obtiene su propio puerto efímero automáticamente.

### El loop 60 Hz

```cpp
constexpr auto kTickInterval = std::chrono::microseconds(16'667);  // ~60 Hz
auto nextTick = std::chrono::steady_clock::now();

while (bot.GetState() == BotClient::State::Established) {
    bot.Update();
    bot.SendInput(dir(rng), dir(rng), static_cast<uint8_t>(btn(rng)));
    nextTick += kTickInterval;
    std::this_thread::sleep_until(nextTick);
}
```

`sleep_until(nextTick)` en lugar de `sleep_for(16ms)` es importante: si el `Update()` tarda 2ms, `sleep_for` dormiría 16ms adicionales → 18ms de tick → 55 Hz real. Con `sleep_until`, el próximo tick ocurre en `t0 + 16.667ms` independientemente de cuánto tardó el tick actual. Así se mantiene el 60 Hz nominal aunque haya varianza en el procesamiento.

---

## Docker — infraestructura reproducible

### Multi-stage build — por qué importa

Un build naive (`FROM ubuntu; RUN apt install gcc cmake sfml; COPY . .; cmake .`) produce una imagen de ~800MB que contiene el compilador, las cabeceras de desarrollo, el código fuente y los artefactos intermedios. Nadie quiere una imagen así en producción.

El multi-stage build resuelve esto:

```text
Stage 1: builder (ubuntu:24.04)
  └─ apt install: build-essential + cmake + libsfml-dev
  └─ cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  └─ cmake --build → /build/Server/NetServer
                   → /build/HeadlessBot/HeadlessBot

Stage 2: server (ubuntu:24.04 clean)
  └─ apt install: libsfml-network2.6t64 + libsfml-system2.6t64 (solo runtime)
  └─ COPY --from=builder /build/Server/NetServer /usr/local/bin/
  └─ ~120MB total (sin compilador, sin fuentes)

Stage 3: bot (ubuntu:24.04 clean)
  └─ igual que server pero con HeadlessBot
  └─ ~120MB total
```

### Por qué Ubuntu 24.04 en runtime (y no Alpine)

La opción "correcta" según la documentación de Docker es usar `alpine` (~5MB) como base de runtime. El problema es que SFML usa glibc y Alpine usa musl libc — incompatibles a nivel de ABI. El binario compilado contra glibc no carga sobre musl.

Debian bookworm-slim habría funcionado, pero Ubuntu 24.04 tiene una sutileza: los paquetes SFML llevan el sufijo `t64` por la transición de `time_t` a 64 bits (cambio ABI en Ubuntu 24.04). Usar la misma distro en builder y runtime garantiza ABI idéntico:

```dockerfile
# Correcto: misma distro en ambas stages
FROM ubuntu:24.04 AS builder   # compile con glibc Ubuntu 24.04
FROM ubuntu:24.04 AS server    # runtime con la misma glibc

# Runtime libs en Ubuntu 24.04:
RUN apt install -y libsfml-network2.6t64 libsfml-system2.6t64
```

### network_mode: host — NAT vs rendimiento

Por defecto, Docker crea una red bridge virtual. Cada paquete UDP que sale de un contenedor pasa por iptables/netfilter del host antes de llegar al destino — añade microsegundos al path de red y distorsiona las mediciones de latencia.

Con `network_mode: host`, los contenedores comparten directamente el stack de red del host. Sin bridge, sin NAT, sin iptables para el tráfico entre contenedores. Para un stress test donde medimos latencia UDP en microsegundos, la diferencia es significativa.

**Limitación crítica:** `network_mode: host` solo funciona en Linux. Docker Desktop para Windows lo ignora silenciosamente y usa bridge. Para desarrollo local en Windows, el bridge funciona correctamente con `SERVER_HOST=server` (Docker DNS resuelve el nombre de servicio).

### docker-compose up --scale bot=10

```yaml
bot:
  build:
    target: bot
  network_mode: host
  environment:
    - SERVER_HOST=127.0.0.1
    - SERVER_PORT=7777
  depends_on:
    - server
  restart: on-failure  # Si el handshake falla (servidor no listo), Docker reinicia el bot
```

Con `restart: on-failure`, si el bot arranca antes que el servidor (race condition en `docker-compose up`), falla el handshake (timeout 5s), Docker lo reinicia automáticamente hasta que el servidor esté listo. Más limpio que añadir un `sleep` al entrypoint.

---

## Tests de integración — el pattern Route()

Los tests anteriores de `SessionRecoveryTests.cpp` siempre tenían UNA sola máquina de estados (el servidor) y el test construía paquetes manualmente. Con `BotIntegration` tenemos DOS máquinas de estado (servidor + bot) que necesitan comunicarse.

El desafío: `MockTransport::Send()` captura los paquetes en `sentPackets`, pero la otra parte nunca los ve a menos que alguien los inyecte en su `incomingQueue`. Necesitamos un "cable virtual":

```cpp
// Enruta todos los paquetes pendientes entre los dos MockTransports.
// Bot → Server: paquetes del bot aparecen en la cola del servidor con sender=kBotEp
// Server → Bot: paquetes del servidor aparecen en la cola del bot con sender=kServerEp
static void Route(MockTransport& serverT, MockTransport& botT) {
    for (auto& [data, to] : botT.sentPackets)
        serverT.InjectPacket(data, kBotEp);
    botT.sentPackets.clear();

    for (auto& [data, to] : serverT.sentPackets)
        botT.InjectPacket(data, kServerEp);
    serverT.sentPackets.clear();
}
```

El helper `DoFullHandshake` usa Route() entre cada Update() para avanzar el estado de ambas máquinas:

```text
bot.Connect()   → botT.sentPackets = [ConnectionRequest]
Route()         → serverT.incomingQueue = [ConnectionRequest de kBotEp]
nm.Update()     → serverT.sentPackets = [Challenge para kBotEp]
Route()         → botT.incomingQueue = [Challenge de kServerEp]
bot.Update()    → botT.sentPackets = [ChallengeResponse]
Route()         → serverT.incomingQueue = [ChallengeResponse de kBotEp]
nm.Update()     → serverT.sentPackets = [ConnectionAccepted para kBotEp]
Route()         → botT.incomingQueue = [ConnectionAccepted de kServerEp]
bot.Update()    → bot.GetState() == Established ✓
```

Los tres tests verifican aspectos distintos e independientes:

| Test | Qué valida | Por qué es importante |
|------|-----------|----------------------|
| `Handshake_BotReachesEstablished` | Estado final correcto en ambos lados | La máquina de estados converge correctamente |
| `SendInput_ServerFiresDataCallback` | Los inputs llegan al game layer | El servidor trata los inputs como datos de juego, no los descarta |
| `InputSequence_StrictlyIncreasing` | Los números de secuencia son crecientes | El SequenceContext del bot funciona correctamente |

---

## Conceptos nuevos en esta propuesta

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **Multi-stage Dockerfile** | Build en imagen pesada, runtime en imagen limpia | Imagen final ~120MB en lugar de ~800MB; superficie de ataque mínima |
| **network_mode: host** | Contenedor comparte stack de red del host | Elimina overhead NAT/iptables para mediciones de latencia UDP precisas |
| **Puerto efímero (port 0)** | OS asigna puerto libre automáticamente | Permite N instancias del bot en el mismo host sin conflictos de puerto |
| **sleep_until vs sleep_for** | `sleep_until(next)` mantiene el intervalo real | Evita drift acumulativo en el loop de 60 Hz bajo carga variable |
| **Route() helper** | Cable virtual entre dos MockTransports | Permite tests bidireccionales deterministas sin hilos ni timers reales |
| **restart: on-failure** | Docker reinicia el contenedor si falla | Maneja race conditions de startup sin sleep en el entrypoint |
| **t64 suffix (Ubuntu 24.04)** | Transición ABI de time_t a 64 bits | Sin esto, los nombres de paquete SFML son incorrectos en Ubuntu 24.04 |

---

## Qué podría salir mal (edge cases)

- **SERVER_HOST con hostname (ej: "server"):** `ParseIpv4` solo entiende IPs numéricas. Si se usa bridge mode y se pone `SERVER_HOST=server`, el bot fallará en el parse. Solución actual: usar IPs siempre en configuración. Para bridge mode, cambiar SERVER_HOST al IP de la red Docker (ej: 172.18.0.2) o usar un lookup DNS externo.

- **Bot arranca antes que el servidor:** el timeout de 5s cubre el caso si el servidor tarda en levantar. Con `restart: on-failure`, Docker reintenta. Si el servidor tarda más de 5s sistemáticamente, aumentar el timeout.

- **Múltiples bots con network_mode: host en Windows:** Docker Desktop ignora host mode y usa bridge. Cada contenedor tiene su propia IP virtual — no hay conflicto de puertos. En Linux con host mode real, `Initialize(0)` garantiza puertos únicos.

- **`_separator_` en el loop del bot:** si `bot.GetState()` deja de ser `Established` (desconexión por timeout del servidor, error de red), el loop termina y el bot hace `Disconnect()`. Con `restart: on-failure` esto no aplica porque no es un fallo (exit code 0). Para reintentos automáticos ante desconexión, necesitaríamos un bucle exterior de reconexión — fuera del scope de este step.

---

## Estado del sistema tras esta implementación

**Funciona:**
- `docker-compose build` genera dos imágenes desde el mismo Dockerfile (`server` y `bot`)
- `docker-compose up --scale bot=10` levanta el servidor y 10 bots que se conectan
- El bot ejecuta handshake completo (3-way: ConnectionRequest → Challenge → ChallengeResponse → Accepted)
- El bot envía InputPayload a 60 Hz con datos aleatorios reproducibles
- 3 tests de integración validan el protocolo completo servidor↔bot con MockTransport
- **109/109 tests passing** (106 anteriores + 3 nuevos BotIntegration)

**Pendiente (Fase 4.3 — Stress Test):**
- Métricas reales: bandwidth vs N bots, CPU time per tick, tick rate estabilidad
- `tc qdisc netem delay 50ms loss 10%` en WSL2 para simular condiciones degradadas
- Comparativa sistema actual vs memcpy directo vs Protobuf

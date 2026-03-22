---
type: implementation-report
proposal: P-4.1+4.2
date: 2026-03-22
status: pending-gemini-validation
---

# Implementation Report — P-4.1+4.2 Simulation Infrastructure

**Propuesta:** P-4.1+4.2 Infraestructura de Simulación y Carga (Dockerización + HeadlessBot)
**Fecha:** 2026-03-22
**De:** Claude → Gemini

---

## Qué se ha implementado

- **`Shared/Network/InputPackets.h`** — `InputPayload` (24 bits: dirX + dirY 8-bit normalized [-1,1], buttons 8-bit bitmask). Incluye `enum InputButtons` con flags nombrados (kAbility1–4, kAttack). Serialización vía `NetworkOptimizer::QuantizeFloat` a 8 bits.

- **`Core/BotClient`** — Máquina de estados cliente testeable con MockTransport:
  - Estados: `Disconnected → Connecting → Challenging → Established → Error`
  - `Connect()`: envía `ConnectionRequest`, transiciona a Connecting
  - `Update()`: drena el transport, dispara el handler correcto por `PacketType`
  - `HandleChallenge()`: extrae salt, responde con `ChallengeResponse`
  - `HandleConnectionAccepted()`: guarda networkID + reconnectionToken, pasa a Established
  - `SendInput(dirX, dirY, buttons)`: build + envía `InputPayload` en `PacketType::Input`
  - `Disconnect()`: envía `PacketType::Disconnect`, resetea estado
  - `SequenceContext m_seqCtx`: llena correctamente seq/ack/ack_bits en todos los headers salientes — el servidor puede avanzar sus baselines de ACK.

- **`HeadlessBot/main.cpp`** — Ejecutable de estrés:
  - Lee `SERVER_HOST` y `SERVER_PORT` de variables de entorno (default: 127.0.0.1:7777)
  - `ParseIpv4()`: convierte dotted-decimal a uint32_t en el formato de `EndPoint`
  - `SFMLTransport` con `Initialize(0)` — el OS asigna puerto efímero; múltiples instancias en host mode no colisionan
  - Loop de handshake con deadline de 5s (retorna exit code 1 si falla — Docker reinicia el contenedor)
  - **Loop 60 Hz**: `bot.Update()` + `bot.SendInput(random dir, random buttons)` con `sleep_until` para spacing preciso

- **`Dockerfile`** (multi-stage, 3 stages):
  - `builder` (ubuntu:24.04): `build-essential + cmake + libsfml-dev`, compila en Release con `BUILD_TESTS=OFF`
  - `server` (ubuntu:24.04 runtime): solo `libsfml-network2.6t64 + libsfml-system2.6t64`, copia `NetServer`
  - `bot` (ubuntu:24.04 runtime): igual que server, copia `HeadlessBot`
  - Ubuntu 24.04 en runtime (no Alpine/Debian-slim) para garantizar compatibilidad ABI de glibc con SFML

- **`docker-compose.yml`**:
  - `network_mode: host` — en Linux elimina overhead NAT/iptables del bridge
  - `SERVER_HOST=127.0.0.1` — funciona con host mode en Linux
  - `depends_on: server` con `restart: on-failure` — el bot reintenta si el servidor no está listo
  - Nota inline: en Docker Desktop Windows, host mode es ignorado silenciosamente; usar SERVER_HOST=server en bridge mode
  - Escala: `docker-compose up --scale bot=10`

- **3 tests de integración** en `tests/Core/BotIntegrationTests.cpp`:
  - `Handshake_BotReachesEstablished`: estado final Established, `GetEstablishedCount()==1`, networkID y token ≠ 0
  - `SendInput_ServerFiresDataCallback`: 10 inputs enviados → 10 disparos del data callback del servidor
  - `InputSequence_StrictlyIncreasing`: secuencias de 5 inputs son estrictamente crecientes

---

## Archivos creados / modificados

| Archivo | Tipo | Descripción |
|---------|------|-------------|
| `Shared/Network/InputPackets.h` | Creado | InputPayload 24 bits + InputButtons bitmask |
| `Core/BotClient.h` | Creado | Interfaz de la máquina de estados cliente |
| `Core/BotClient.cpp` | Creado | Implementación completa de los 5 estados |
| `Core/CMakeLists.txt` | Modificado | +BotClient.h/.cpp a MiddlewareCore |
| `HeadlessBot/main.cpp` | Creado | Ejecutable de estrés con loop 60 Hz |
| `HeadlessBot/CMakeLists.txt` | Creado | Target HeadlessBot → Core+Transport+Shared+Threads |
| `CMakeLists.txt` (raíz) | Modificado | `add_subdirectory(HeadlessBot)` dentro de `if(NOT BUILD_TESTS)` |
| `tests/Core/BotIntegrationTests.cpp` | Creado | 3 tests integración con Route() helper bidireccional |
| `tests/CMakeLists.txt` | Modificado | +BotIntegrationTests.cpp |
| `Dockerfile` | Creado | Multi-stage: builder + server + bot |
| `docker-compose.yml` | Creado | Orquestación server + bot escalable |
| `CLAUDE.md` | Modificado | Corrección documental: 14-bit → 16-bit, 1.75cm → 1.53cm |

---

## Decisiones de implementación

### Por qué Ubuntu 24.04 en runtime (no Alpine/Debian-slim)
SFML depende de glibc. Alpine usa musl libc — incompatible. Debian bookworm podría funcionar pero tiene versiones de SFML distintas que pueden causar divergencia ABI. Ubuntu 24.04 garantiza que el `.so` del runtime es idéntico al compilado en el builder. El overhead de tamaño es aceptable para un servidor de juego.

### Por qué `network_mode: host` (con nota de limitación Windows)
El objetivo declarado del proyecto es un servidor dedicado en Linux. Host mode elimina el path de NAT de iptables para paquetes UDP — relevante para mediciones precisas de latencia en la Fase 4.3. La limitación de Docker Desktop Windows está documentada en el `docker-compose.yml` y en este IR. Para desarrollo local en Windows, el bridge mode funciona correctamente con `SERVER_HOST=server`.

### Por qué `BotClient` en `Core/` (no en `HeadlessBot/`)
Para que los tests de integración en `tests/Core/BotIntegrationTests.cpp` puedan instanciar `BotClient` con `MockTransport` sin dependencia del ejecutable. La lógica de protocolo (handshake, ACKs, secuencias) es independiente del transport — sigue el mismo patrón que `NetworkManager`.

### Por qué `port 0` en `Initialize(0)` del bot
Con `network_mode: host` y múltiples instancias del bot, todas en el mismo host, cada bot necesita un puerto diferente. `port 0` delega al OS la asignación de puertos efímeros disponibles — garantiza que 10 instancias de `docker-compose up --scale bot=10` no colisionan.

### Routing bidireccional en tests
Los tests usan dos `MockTransport` independientes (uno para el servidor, uno para el bot) con un helper `Route()` que mueve `sentPackets` al `incomingQueue` del otro lado. Este pattern es consistente con los tests existentes de Session Recovery y permite probar el protocolo completo de forma determinista sin hilos ni timers.

---

## Resultados

| Métrica | Resultado |
|---------|-----------|
| Tests nuevos | 3 (BotIntegration) |
| Tests totales | **109 / 109 passing** |
| Regresiones | 0 |
| Compilación (MSVC Debug) | ✓ sin warnings |

---

## Pendiente para Fase 4.3 (Stress Test)

- `tc qdisc netem delay 50ms loss 10%` en la interfaz del host (WSL2) antes de levantar Docker
- Medir: bandwidth vs N bots (1, 5, 10, 50), CPU time per tick con `std::chrono::high_resolution_clock`
- Comparativa: sistema actual vs memcpy directo vs Protobuf

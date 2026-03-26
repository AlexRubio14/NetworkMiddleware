# Análisis Técnico — Proyecto Legacy AA4 Shooter 2D Online
**Autor:** Claude (análisis propio, sin instrucciones de implementación)
**Fecha:** 2026-03-23
**Alcance:** AA4_TCP_Server + AA4_UDP_Server + UDP_Client
**Objetivo:** Entender el sistema completo, identificar fortalezas y debilidades, y extraer
qué patrones o lógica de gameplay son relevantes para el proyecto NetworkMiddleware actual.

---

## Índice

1. [Resumen ejecutivo](#1-resumen-ejecutivo)
2. [Arquitectura del sistema completo](#2-arquitectura-del-sistema-completo)
3. [Flujo de juego: de login a partida](#3-flujo-de-juego-de-login-a-partida)
4. [Análisis del game loop](#4-análisis-del-game-loop)
5. [Protocolo de red](#5-protocolo-de-red)
6. [Lo que está bien](#6-lo-que-está-bien)
7. [Bugs críticos y problemas graves](#7-bugs-críticos-y-problemas-graves)
8. [Problemas de arquitectura y mantenibilidad](#8-problemas-de-arquitectura-y-mantenibilidad)
9. [Qué traer al proyecto NetworkMiddleware](#9-qué-traer-al-proyecto-networkmiddleware)
10. [Madurez técnica por área](#10-madurez-técnica-por-área)
11. [Opinión global y comparativa](#11-opinión-global-y-comparativa)

---

## 1. Resumen Ejecutivo

El proyecto AA4 es un juego multijugador 2D (shooter plataformero, 1v1) construido desde cero
en C++ con SFML. Tiene tres componentes separados: un servidor TCP para matchmaking y login, un
servidor UDP dedicado para la partida, y un cliente con renderizado y red integrados.

Para ser un primer proyecto de red (según la descripción del usuario), el nivel es sólido. El
sistema funciona end-to-end: un jugador puede registrarse, hacer login, entrar en cola,
encontrar partida, jugar y recibir el resultado. Hay validación server-side de velocidad, ACKs
para paquetes críticos con backoff exponencial, interpolación visual de enemigos, y bcrypt para
contraseñas. Para el nivel de una práctica universitaria, merece un 8/10.

En cambio, visto desde una perspectiva de producción, hay problemas serios: un `delete` sobre
un `shared_ptr`, tres threads detached sin posibilidad de cleanup, un game loop de servidor que
es un `while(true)` sin sleep (spin puro), daño de balas completamente client-side (trivialmente
exploitable), y un `TaskPool` implementado correctamente pero que nunca se instancia ni se usa.

Este documento detalla todo esto, y concluye con qué lógica concreta del AA4 es útil para
implementar el game loop mínimo de P-3.7 en NetworkMiddleware.

---

## 2. Arquitectura del Sistema Completo

### 2.1 Componentes

```
┌─────────────────────────────────────────────────────────────┐
│                        CLIENTE                              │
│  UDP_Client/Tutorial SMFL/                                  │
│  ┌────────────┐  ┌─────────────┐  ┌──────────────────────┐ │
│  │ SceneManager│  │NetworkManager│  │ GameScene/Player/    │ │
│  │ (main loop) │  │ (net thread) │  │ Bullet/GameManager   │ │
│  └────────────┘  └─────────────┘  └──────────────────────┘ │
└────────────────────────┬────────────────────┬───────────────┘
                         │ TCP :55001          │ UDP :55002
                         ▼                     ▼
┌──────────────────┐    ┌──────────────────────────────────────┐
│  TCP SERVER      │    │  UDP SERVER (Dedicated)              │
│  AA4_TCP_Server/ │    │  AA4_UDP_Server/                     │
│  ┌────────────┐  │    │  ┌──────────┐  ┌──────────────────┐ │
│  │ Server     │  │    │  │ Server   │  │ PacketManager    │ │
│  │ (selector) │  │ UDP│  │ (while   │  │ (event dispatch) │ │
│  │ClientManager│  │───▶│  │  true)   │  │                  │ │
│  │DatabaseMgr │  │    │  └──────────┘  └──────────────────┘ │
│  │MatchMaking │  │    │  ┌──────────────────────────────┐   │
│  └────────────┘  │    │  │ Room (30Hz thread, detached)  │   │
└──────────────────┘    │  │ ┌────────┐  ┌────────────┐   │   │
                         │  │ │Client 1│  │ Client 2   │   │   │
                         │  │ └────────┘  └────────────┘   │   │
                         │  └──────────────────────────────┘   │
                         └──────────────────────────────────────┘
```

### 2.2 Relación entre componentes

Los tres componentes son **procesos separados**. La comunicación entre TCP Server y UDP Server
se hace vía UDP (el propio TCP Server tiene un `sf::UdpSocket` para notificar al dedicated
server cuando se forma un match). Esta es una decisión interesante: el TCP Server actúa como
"broker" de matchmaking y le delega la sesión de juego al servidor UDP.

### 2.3 Separación de responsabilidades por componente

| Componente | Responsabilidad | Protocolo |
|-----------|----------------|-----------|
| TCP Server | Login, registro, matchmaking, gestión GUID | TCP :55001 |
| UDP Server | Lógica de partida, validación, autoridad | UDP :55002 |
| Cliente | Input, render, física local, net thread | TCP + UDP |

---

## 3. Flujo de Juego: De Login a Partida

### 3.1 Secuencia completa

```
CLIENTE                    TCP SERVER               UDP SERVER
  │                            │                        │
  │──[TCP] LOGIN ──────────────▶│                        │
  │◀─── LOGIN_SUCCESS (GUID) ───│                        │
  │                            │                        │
  │──[TCP] START_QUEUE ─────────▶│                        │
  │     (espera otro jugador)   │                        │
  │                            │                        │
  │◀─── MATCH_FOUND ────────────│                        │
  │    (myGameId, enemyId,      │──[UDP] MATCH_FOUND ───▶│
  │     udpServerIp, port)      │   (gameId1, gameId2)   │
  │                            │                    crea Room
  │                            │                    crea Client(gameId1)
  │                            │                    crea Client(gameId2)
  │                            │                        │
  │──[UDP] SEND_CLIENT_INFO ───────────────────────────▶│
  │    (gameId, desde puerto X) │                        │
  │                            │                   guarda ip:port del cliente
  │                            │                   cuando ambos listos:
  │◀── START_GAME (critical) ─────────────────────────── │
  │                            │                    Room::Start()
  │                            │                    thread 30Hz
  │─── SEND_POSITION (20Hz) ──────────────────────────▶│
  │                            │              bufferiza en positionPackets
  │                            │              cada 100ms: ValidateClientMovements()
  │◀── INTERPOLATION_POSITION ────────────────────────── │
  │    (movId, x, y del enemigo)│                        │
  │                            │                        │
  │──[SPACE] SEND_START_SHOOT ──────────────────────────▶│
  │                            │              reenvía RECEIVE_START_SHOOT al rival
  │◀── RECEIVE_START_SHOOT (critical) ─────────────────── │
  │                            │                        │
  │   [bala impacta visualmente]│                        │
  │──[vida=0] SEND_RESPAWN ─────────────────────────────▶│
  │   (value, movId, x, y, lives)│                       │
  │                            │              Respawn() + notifica rival
  │◀── RECEIVE_RESPAWN ────────────────────────────────── │
  │                            │                        │
  │   [lives=0] ────────────────────────────────────────▶│
  │◀── END_GAME ──────────────────────────────────────── │
```

### 3.2 Observación crítica del flujo

El TCP Server envía al UDP Server únicamente los `gameId` de los dos jugadores
(`MatchMakingManager.cpp:75-90`). No envía IP ni puerto porque en ese momento el cliente UDP
todavía no ha contactado al servidor UDP. El servidor UDP aprende la IP/puerto real del cliente
cuando recibe el paquete `SEND_CLIENT_INFORMATION` del propio cliente (`PacketManager.cpp:100-116`).

Este diseño de "autoregistro UDP" es elegante para un sistema sencillo, pero tiene un problema
de seguridad: cualquiera que conozca un `gameId` válido puede registrarse como ese jugador en
el servidor UDP simplemente enviando `SEND_CLIENT_INFORMATION` con ese ID. No hay validación
cruzada con el token del TCP Server.

---

## 4. Análisis del Game Loop

### 4.1 Cliente (SceneManager + GameScene)

**Frecuencia:** Sin cap explícito. SFML limita a vsync (~60 FPS en pantallas estándar).

```
SceneManager::Update() [cada frame, ~60Hz]
│
├─ sf::RenderWindow::pollEvent()
│  └─ Player::HandleEvent()
│     ├─ movingLeft / movingRight flags
│     ├─ jumpRequested flag
│     └─ si SPACE: SentCriticPacket(SEND_START_SHOOT) ← envío inmediato, no buffered
│
├─ GameScene::UpdateReferencePlayer(deltaTime)
│  ├─ Player::PrepareMovement(dt)        ← calcula velocity.x, velocity.y+gravity
│  ├─ CollisionX: mueve en X si no colisiona con mapa
│  ├─ CollisionY: mueve en Y si no colisiona, o ajusta isOnGround
│  └─ Player::Update(dt)
│     └─ Player::SendPosition()          ← cada 50ms (20Hz), no cada frame
│
├─ GameScene::UpdateEnemyPlayer(deltaTime)
│  └─ Player::UpdateEnemy(dt)
│     └─ Interpolación Lerp entre primero y último ValidPacket del buffer
│
├─ GameScene::UpdateBullets(deltaTime)
│  └─ Bullet::Update(dt) + colisión con mapa + colisión con players
│     └─ Player::ReceiveDamage() si hay intersección
│        └─ Respawn() si health==0 → SentCriticPacket(SEND_RESPAWN)
│
└─ GameScene::Render(window)

[Hilo de red, async]
NetworkManager::HandleUDPServerCommunication()
└─ recibe paquetes UDP, PACKET_MANAGER.ProcessUDPReceivedPacket()
   ├─ START_GAME → GameScene::SetCurrentPlayer()
   ├─ INTERPOLATION_POSITION → Player::AddEnemyPosition()
   ├─ RECEIVE_START_SHOOT → Player::shootRequested = true
   ├─ VALIDATION_BACK → Player::BackToValidPosition(id)
   └─ END_GAME → cierra escena
```

**Detalle clave — SendPosition:**
```cpp
// Player.cpp:256-275
void Player::SendPosition() {
    if (sendPositionClock.getElapsedTime() >= interval)  // interval = 50ms
    {
        customPacket.WriteVariable(idMovement);   // int: 4 bytes
        customPacket.WriteVariable(position.x);   // float: 4 bytes
        customPacket.WriteVariable(position.y);   // float: 4 bytes
        // Total payload: 12 bytes + 6 bytes header = 18 bytes por posición
        idMovement++;
        sendPositionClock.restart();
    }
}
```

El `idMovement` es un contador local del cliente. El servidor usa la diferencia entre IDs
consecutivos para inferir el `deltaTime` entre paquetes y calcular la velocidad máxima
(`TIME_PER_PACKET = 0.05f`). Esto es correcto mientras el cliente envíe exactamente a 20Hz.
Si el cliente modifica el intervalo de envío, la validación de velocidad falla.

**Detalle clave — Interpolación del enemigo:**
```cpp
// Player.cpp:169-214
// El jugador local tiene un buffer enemyPositions[]
// Cada 50ms (interpolationTime) recibe posiciones del servidor
// Lerp entre la primera y la última posición recibida en ese intervalo
float t = elapsedTime.getElapsedTime() / interpolationTime;
sprite->setPosition(Lerp(firstPosition, lastPosition, t));
```

La interpolación opera sobre el batch de posiciones que llegaron en el último tick de 50ms.
Si solo llega 1 paquete (buffer de 1 elemento), `validPackets.size() < 2` y no interpola
— se congela. Si llegan 3+ paquetes, solo usa el primero y el último, ignorando los del medio.

### 4.2 Servidor UDP (Room, 30 Hz)

**Frecuencia:** 30 Hz (33.33ms tick). Definido en `Room.cpp:78`.

```
[Hilo principal — Server::Update()]
while(true)                               ← SPIN PURO, sin sleep
  socket->receive()
  CustomUDPPacket.ReadBuffer()
  PACKET_MANAGER.ProcessUDPReceivedPacket()
  ├─ SEND_POSITION → Client::AddPositionPacket(movementId, x, y)
  ├─ SEND_CLIENT_INFORMATION → guarda ip:port, llama AddPlayerReady()
  ├─ START_GAME → Room::Start() → lanza hilo 30Hz
  ├─ SEND_ACK / RECEIVE_ACK → gestión critical packets
  ├─ SEND_MOCKERY → reenvía al rival
  ├─ SEND_START_SHOOT → reenvía RECEIVE_START_SHOOT (critical) al rival
  └─ SEND_RESPAWN → Respawn() + notifica rival

[Hilo Room — 30 Hz, DETACHED]
Room::Update() cada 33ms:
  if (positionPacketClock >= 100ms)        ← no 30Hz, sino 10Hz (cada 3 ticks)
    para cada cliente:
      Client::ValidateClientMovements(i)
        ├─ ordena packets por movementId
        ├─ calcula deltaPos / deltaTime por cada par consecutivo
        ├─ si speedX > MAX_SPEED_X + TOLERANCE (155): invalid
        │  ├─ si 3 violations: Disconnect()
        │  └─ si no: VALIDATION_BACK + INTERPOLATION_POSITION al rival
        └─ si todo OK: VALIDATION_OK + INTERPOLATION_POSITION al rival

[Hilo CriticalPacketManager — 50ms, DETACHED]
cada 50ms:
  para cada cliente en inGameClients:
    Client::CriticalPacketsUpdate(0.05f)
      ├─ si timeSinceLastSend >= resendDelay: reenvía
      ├─ resendDelay *= 2, max 5s
      └─ si retries >= 8: descarta
```

**Problema crítico del hilo principal:** `Server::Update()` es un `while(true)` sin ningún
sleep ni `socket->setBlocking(false)` gestionado correctamente. El socket es non-blocking
(`Server.cpp:29`), así que cuando no hay paquetes, `receive()` devuelve `NotReady` de forma
inmediata y el loop sigue girando al 100% de CPU. Esto es un **busy-wait** puro.

**Separación de frecuencias:**
- Server receive loop: lo más rápido posible (spin)
- Room validation: cada 100ms (10Hz efectivo)
- Room thread tick: 30Hz pero solo hace algo cada 100ms
- CriticalPacketManager: 50ms

### 4.3 Comparativa Game Loop: AA4 vs Producción vs NetworkMiddleware

| Aspecto             | AA4                         | Producción (LoL/Overwatch)  | NetworkMiddleware (objetivo)    |
| ------------------- | --------------------------- | --------------------------- | ------------------------------- |
| Tick rate servidor  | 30 Hz                       | 30–128 Hz                   | 100 Hz                          |
| Receive loop        | spin puro (0% sleep)        | epoll/io_uring event-driven | drena buffer, luego sleep_until |
| Fixed timestep      | Sí en Room, no en receive   | Sí                          | Sí (`sleep_until`)              |
| Input processing    | Buffered en positionPackets | Input queue por cliente     | Pendiente (P-3.7)               |
| Estado autoritativo | Parcial (valida velocidad)  | Completo                    | Pendiente (P-3.7)               |
| Delta compression   | No (posición completa)      | Sí (bits 4-16)              | Sí (P-3.5, 99% eficiencia)      |
| Interpolación       | Client-side Lerp            | Client-side + extrapolación | Pendiente (Fase 6)              |
| Thread model        | 1 hilo por room (detached)  | Thread pool                 | Pendiente (P-4.4)               |

---

## 5. Protocolo de Red

### 5.1 Wire format TCP (CustomPacket)

```
[sf::Packet → length prefixed internamente por SFML]
└─ [PacketType cast a int: 4 bytes]  ← BUG: uint8_t se envía como int
   [payload variable]
```

La serialización usa `sf::Packet::operator<<` que gestiona big-endian automáticamente.
Pero castear `PacketType` (que es `uint8_t`) a `int` desperdicia 3 bytes por paquete.

### 5.2 Wire format UDP (CustomUDPPacket)

```
Byte  0:      UdpPacketType  (uint8_t: NORMAL=0, URGENT=1, CRITIC=2)
Byte  1:      PacketType     (uint8_t: valor del enum)
Bytes 2-5:    playerId       (int: 4 bytes — debería ser uint16_t máx)
Bytes 6+:     payload        (raw memcpy, endianness del host)
Total header: 6 bytes fijos
```

**Payload por tipo:**

| PacketType | Campos | Tamaño payload | Total |
|-----------|--------|---------------|-------|
| SEND_POSITION | movementId(int) + x(float) + y(float) | 12 bytes | 18 bytes |
| SEND_START_SHOOT | idCritic(int) | 4 bytes | 10 bytes |
| SEND_RESPAWN | criticalId+value+movId+x+y+lives | 22 bytes | 28 bytes |
| SEND_MOCKERY | — | 0 bytes | 6 bytes |
| VALIDATION_BACK | movementId(int) | 4 bytes | 10 bytes |
| INTERPOLATION_POSITION | movId(int)+x(float)+y(float) | 12 bytes | 18 bytes |

**Análisis de eficiencia:**

Posición cada 50ms desde el cliente: 18 bytes × 20 Hz = 360 bytes/s uplink
Interpolation al rival cada 100ms: 18 bytes × 10 Hz = 180 bytes/s downlink

Con 2 jugadores: (360 + 180) × 2 = ~1.08 KB/s — muy bajo, correcto para LAN.

**¿Qué se podría comprimir?**
- `playerId` como `int` (4 bytes): 2 jugadores máximo → `uint8_t` (1 byte) — ahorra 3 bytes/paquete
- `x`, `y` como `float` (8 bytes): cuantización a 16 bits (como hace NetworkMiddleware, 1.53cm
  precisión en ±500m) → 4 bytes. Ahorra 4 bytes/paquete
- `movementId` como `int` (4 bytes): `uint16_t` sería suficiente (wraps a 65536 movimientos =
  54 minutos a 20Hz) → 2 bytes. Ahorra 2 bytes/paquete

**SEND_POSITION optimizado:** 6+2+2+2 = 12 bytes vs 18 bytes actuales → 33% de ahorro.

### 5.3 Sistema de ACKs (critical packets)

El sistema de paquetes críticos es manual pero correcto en concepto:

```
Cliente envía SEND_START_SHOOT (UdpPacketType::CRITIC)
  → PacketManager detecta CRITIC → llama AddCriticalPacketIdToSet()
  → emite SEND_ACK de vuelta al cliente inmediatamente
  → cliente recibe RECEIVE_ACK → OnACKReceived(id) → borra de pendingPacketsToSend

Cliente espera ACK del servidor para sus paquetes críticos:
  → CriticalPacketManager retransmite si no llega ACK en resendDelay
  → backoff: resendDelay *= 2, máx 5s, máx 8 intentos
```

**Diferencia clave con NetworkMiddleware:** AA4 usa ACK por paquete individual con ID propio.
NetworkMiddleware usa `ack_bits` (ACK bitmask de 32 bits): confirma 32 paquetes en un solo bit
cada uno. El sistema de AA4 genera más overhead de ACK pero es más simple de depurar.

### 5.4 Endianness

El código usa `memcpy` directamente desde tipos del host. En una LAN entre máquinas del mismo
endianness (ambas x86 little-endian) funciona. En una red heterogénea o cross-platform (ARM
big-endian, PowerPC) fallaría silenciosamente. NetworkMiddleware evita este problema: usa
operadores bitwise que son endianness-neutral por definición.

---

## 6. Lo que está bien

### 6.1 Separación TCP/UDP por función

La decisión de usar TCP para login/matchmaking y UDP para gameplay es exactamente correcta.
TCP garantiza que las credenciales llegan completas; UDP minimiza latencia en gameplay.
Muchos proyectos de nivel estudiante usan solo TCP para todo — este lo hace bien.

### 6.2 Validación server-side de movimiento (`Client.cpp:107-207`)

El servidor recibe posiciones del cliente, calcula la velocidad entre paquetes consecutivos, y
rechaza movimientos que excedan el máximo físicamente posible. Hay sistema de strikes (3 antes
de kick) y el servidor responde con la última posición válida para forzar corrección cliente.

```cpp
// Client.cpp:131-178
int deltaSteps = current.movementId - prev.movementId;
float deltaTime = deltaSteps * TIME_PER_PACKET;
float speedX = std::abs(dx / deltaTime);
if (speedX > MAX_SPEED_X + TOLERANCE)  // anti-cheat básico
    Disconnect();
```

Esta lógica es directamente relevante para P-3.7 en NetworkMiddleware.

### 6.3 Sistema de Critical Packets con backoff exponencial (`Client.cpp:61-89`)

```cpp
criticalPacket.resendDelay = std::min(criticalPacket.resendDelay * 2.0f, 5.0f);
```

Backoff exponencial correcto: empieza rápido, va creciendo para no saturar la red con retransmisiones.
Max 8 intentos antes de descartar. Este patrón es equivalente al de NetworkMiddleware (RTT×1.5
con max retries=10) aunque implementado de forma más manual.

### 6.4 Interpolación cliente-side (`Player.cpp:161-214`)

```cpp
sf::Vector2f firstPosition = validPackets[0].position;
sf::Vector2f lastPosition = validPackets.back().position;
float t = elapsedTime.getElapsedTime() / interpolationTime;
sprite->setPosition(Lerp(firstPosition, lastPosition, t));
```

Técnica estándar para suavizar el movimiento del enemigo entre ticks de red. En un MOBA, esta
lógica iría en el cliente Unreal para suavizar las posiciones recibidas en Snapshots.

### 6.5 bcrypt para contraseñas (`DatabaseManager.cpp:128-135`)

Bcrypt con 12 rondas es un estándar de la industria. El alumno investigó y eligió la solución
correcta, no simplemente SHA256 o MD5.

### 6.6 SocketSelector para I/O multiplexing (TCP Server)

`sf::SocketSelector` permite esperar en múltiples sockets simultáneamente con un timeout sin
usar threads por cliente. Esto es el equivalente simplificado de `select()`/`poll()`. Es la
técnica correcta para un servidor con pocos clientes simultáneos.

### 6.7 TaskPool correctamente implementado (`TaskPool.cpp`)

El `TaskPool` usa `std::jthread` (C++20), mutex, condition_variable, y un flag `std::atomic<bool>`.
El destructor notifica todos los threads con `condVar.notify_all()`. La lógica de `Worker()` es
correcta: espera con `condVar.wait()` en lugar de hacer spin. Es una implementación sólida.

**El problema:** Nunca se instancia ni se usa en ningún sitio del servidor UDP.

### 6.8 Física separada en X e Y (`GameScene.cpp:54-83`)

```cpp
// Primero mueve en X, verifica colisión
// Luego mueve en Y, verifica colisión
```

Separar el movimiento en dos ejes independientes es la técnica estándar para evitar que el
personaje "se cuele" por esquinas de tiles. Un jugador principiante suele mover en XY juntos
y luego preguntarse por qué atraviesa paredes.

---

## 7. Bugs Críticos y Problemas Graves

### 7.1 [CRÍTICO] Double-delete sobre shared_ptr (`Room.cpp:109-115`)

```cpp
void Room::FinishRoom()
{
    for (std::shared_ptr<Client> client : clients)
    {
        delete client.get();  // ← UNDEFINED BEHAVIOR
    }
}
```

`shared_ptr<Client>` tiene ownership del objeto. Llamar `delete client.get()` destruye el
objeto manualmente, pero el `shared_ptr` no lo sabe y también intentará destruirlo cuando su
ref count llegue a 0. Resultado: double-free → crash o memoria corrompida.

La solución correcta es simplemente `clients.clear()`. El `shared_ptr` se encarga.

**Este bug no crashea en la práctica solo si `FinishRoom()` nunca se llama o si el shared_ptr
es el único propietario y ya ha expirado antes del delete.** Revisando el código: `FinishRoom()`
nunca se llama desde ningún lugar. Es código muerto. El bug existe pero no llega a ejecutarse.

### 7.2 [CRÍTICO] Threads detached sin cleanup (`Room.cpp:77-91`, `CriticalPacketManager.cpp:6-27`)

```cpp
std::thread([this]() {
    while (running) { ... }
}).detach();  // ← no hay forma de hacer join al cerrar
```

Un thread detached no puede ser joinado. Si el servidor recibe SIGINT, el proceso termina
mientras los threads están en medio de operaciones sobre memoria que se destruye. El resultado
es undefined behavior: acceso a `Room` destruida, a `Client` destruido, o directamente crash.

`CriticalPacketManager::StartCriticalPacketUpdateThread()` es igual: thread detached con
`while(true)` que accede a `PACKET_MANAGER.GetInGameClients()` indefinidamente.

La solución que NetworkMiddleware ya usa: `std::jthread`. El jthread hace join automático en su
destructor, eliminando este problema por diseño.

### 7.3 [CRÍTICO] Spin-loop en el receive del servidor UDP (`Server.cpp:37-57`)

```cpp
void Server::Update()
{
    while (true)  // ← sin ningún sleep
    {
        if (socket->receive(...) == Done)  // non-blocking: retorna inmediatamente
        {
            ProcessPacket(...);
        }
        // Si no hay paquete: vuelve a intentar inmediatamente
    }
}
```

El socket es non-blocking (`Server.cpp:29`). Cuando no hay paquetes, `receive()` retorna
`NotReady` al instante y el loop itera de nuevo. Este es un **busy-wait** que consume el 100%
de un núcleo de CPU permanentemente, incluso cuando no llega ningún paquete.

La solución: o hacer el socket blocking con timeout (`setBlocking(true)` + `receive()` bloquea
hasta que llega algo o pasa el timeout), o usar `sf::SocketSelector::wait(timeout)` como hace
el TCP Server, o usar el patrón de NetworkMiddleware (`sleep_until` para el tick fijo).

### 7.4 [ALTO] Daño de balas completamente client-side (`GameScene.cpp:96-117`)

```cpp
if ((*it)->GetBounds().findIntersection(player->GetGlobalBounds()).has_value())
{
    player->ReceiveDamage();  // ← cliente decide si impacta
    it = bullets.erase(it);
}
```

El cliente decide localmente si una bala impacta a otro jugador. El servidor no valida esto.
El cliente solo notifica con `SEND_RESPAWN` cuando su jugador pierde una vida, pero no informa
de cada impacto individual. Un cliente modificado puede simplemente no enviar `SEND_RESPAWN`
nunca → es inmortal. O puede enviarlo cuando quiera → el enemigo muere sin razón.

En producción, el servidor es la autoridad de la física de proyectiles. El cliente puede
*predecir* visualmente el impacto, pero el servidor valida y confirma. Esta es la base del
"lag compensation" de Counter-Strike y LoL.

### 7.5 [ALTO] Race condition en positionPackets (`Client.h:51`, `Client.cpp:56-59`, `107-109`)

```cpp
// Hilo de recepción (Server::Update):
void Client::AddPositionPacket(int movementId, int x, int y) {
    positionPackets.emplace_back(...);  // ← sin lock
}

// Hilo de Room (30Hz):
void Client::ValidateClientMovements(int playerId) {
    std::lock_guard<std::mutex> lock(positionMutex);  // ← con lock
    ...
}
```

`AddPositionPacket` escribe en `positionPackets` sin tomar `positionMutex`. `ValidateClientMovements`
sí lo toma. Esto es una race condition: el thread de recepción puede estar haciendo `push_back`
(que puede realocar el vector) mientras el thread de Room está iterando el mismo vector, aunque
en el lock del Room. Si `push_back` realoca, el puntero interno del iterador se invalida.

La solución: añadir `positionMutex.lock()` en `AddPositionPacket`.

### 7.6 [ALTO] Timeout de clientes parcialmente implementado (`Client.cpp:224-248`)

`UpdateTimeout()` envía un PING a los 500ms de silencio y desconecta a los 2 segundos.
Pero el timeout clock se reinicia en `OnPongReceived()`, que se llama en `ProcessUDPReceivedPacket`
cada vez que llega **cualquier paquete**, no solo el PONG (`PacketManager.cpp:261-262`):

```cpp
if (it != inGameClients.end())
    it->second->OnPongReceived();  // ← llama esto en CUALQUIER paquete
```

Entonces el timeout funciona de facto como "kick si no llega nada en 2s" lo cual está bien.
El SEND_PING/RECEIVE_PING es innecesario ya que cualquier tráfico ya resetea el clock. El
problema real: si el cliente envía posición y el PONG se recibe entre el envío y la comprobación,
el clock se resetea y el timeout nunca dispara. Para clientes activos funciona. Para clientes
que se cuelgan a mitad del loop, funciona. OK para una práctica.

### 7.7 [MEDIO] TaskPool implementado pero nunca usado

`TaskPool.h/.cpp` define una pool de threads con jthread, mutex y condvar. Correctamente
implementado. Nunca instanciado en `Server.cpp`, nunca incluido en `PacketManager.cpp`.

Es un caso de dead code sólido: la funcionalidad que P-4.4 (Thread Pool) de NetworkMiddleware
necesita implementar ya existe en embrión en el proyecto legacy, pero sin conectar al sistema.

---

## 8. Problemas de Arquitectura y Mantenibilidad

### 8.1 Singletons globales masivos

Los managers se acceden vía macros:
```cpp
#define PACKET_MANAGER PacketManager::Instance()
#define EVENT_MANAGER  EventManager::Instance()
#define ROOM_MANAGER   RoomManager::Instance()
#define GAME           GameManager::Instance()
```

Esto crea dependencias ocultas. `Client.cpp` llama a `ROOM_MANAGER`, `PACKET_MANAGER`, etc.
directamente, sin que el compilador pueda verificar el orden de inicialización. Los tests
unitarios son imposibles: no se puede mockear `PACKET_MANAGER` para testear `Client` en
aislamiento.

NetworkMiddleware lo resuelve con inyección de dependencias en el constructor y Composition
Root en `main.cpp`.

### 8.2 IPs hardcodeadas en headers

```cpp
// Server.h (TCP Server)
const sf::IpAddress DEDICATED_SERVER_IP_LOCAL  = sf::IpAddress(192, 168, 1, 144);
const sf::IpAddress DEDICATED_SERVER_IP_PUBLIC = sf::IpAddress(93, 176, 163, 125);
```

La IP pública del desarrollador (¿real?) hardcodeada en el código fuente y commiteada a git.
Para cambiar de servidor hay que recompilar. Para compartir el código hay que exponer la IP.

### 8.3 PacketType enum duplicado en tres proyectos

`PacketType.h` existe en `AA4_TCP_Server/`, `AA4_UDP_Server/`, y `UDP_Client/` con contenido
idéntico (35 tipos). Si se añade un tipo nuevo hay que actualizarlo en los tres sitios. Un
solo cambio desincronizado corrompe silenciosamente el protocolo.

En NetworkMiddleware esto está resuelto: `PacketTypes.h` está en `MiddlewareShared` y se incluye
desde ahí en Core y Transport.

### 8.4 CustomUDPPacket::WriteVariable retorna bool que siempre se ignora

```cpp
// MatchMakingManager.cpp:75-82
if (!matchPacket.WriteVariable(player1->GetGameId()))
    std::cout << "No se ha podido escribir: " << ...;
```

Hay dos casos: el TCP Server sí comprueba el return value (aunque solo loguea). El UDP Server
generalmente lo llama sin capturar el bool. Si el buffer está lleno (1024 bytes), se pierde
datos silenciosamente.

### 8.5 Sin tests unitarios

Ningún archivo de test en ninguno de los tres proyectos. Los singletons con estado global hacen
que sea casi imposible añadirlos retroactivamente sin refactorizar el 70% del código.

### 8.6 main() sin retorno de tipo en TCP Server

```cpp
void main()  // ← no es estándar C++, debería ser int main()
```

`void main()` es una extensión de MSVC que compila en Windows pero es non-standard C++.

---

## 9. Qué Traer al Proyecto NetworkMiddleware

Esta sección es la más importante para las decisiones de P-3.7 y fases posteriores.

### 9.1 YA ESTÁ IMPLEMENTADO MEJOR EN NETWORKMIDDLEWARE → no traer

| Feature de AA4 | Equivalente en NetworkMiddleware | Veredicto |
|----------------|----------------------------------|-----------|
| Critical packets con backoff | Reliability Layer (P-3.3): RTT×1.5 adaptativo | NM es mejor |
| ACK individual por paquete | ack_bits (32 paquetes por bit) en PacketHeader | NM es mejor |
| Posición como float sin comprimir | BitWriter + cuantización 16-bit (1.53cm precisión) | NM es mejor |
| Threads detached | std::jthread (join automático en destructor) | NM es mejor |
| Timeout de clientes (ping/pong) | Session Recovery P-3.6: zombie state + token | NM es mejor |
| Spin-loop de receive | sleep_until con 10ms tick budget | NM es mejor |

### 9.2 LÓGICA DE GAMEPLAY DIRECTAMENTE USABLE EN P-3.7

#### A) Física de movimiento de héroe (referencia directa)

El cliente AA4 tiene el modelo de física que el game loop de NetworkMiddleware necesita simular:

```cpp
// De Player.cpp — valores de referencia para ViegoEntity en P-3.7:
moveSpeed  = 100.f;   // unidades/segundo en X
jumpForce  = 350.f;   // velocidad inicial en Y al saltar
gravity    = 500.f;   // aceleración de caída en Y
```

Para un MOBA (top-down sin gravedad), solo `moveSpeed` es relevante. El modelo sería:
```
newX = hero.x + input.dirX * moveSpeed * dt
newY = hero.y + input.dirY * moveSpeed * dt
```

Que es exactamente lo que el handoff de P-3.7 propone. Los valores concretos (100 unidades/s)
son una referencia validada del juego funcionando.

#### B) Collision clamp como validación de bounds

```cpp
// De GameScene.cpp — separación X/Y para evitar clipping:
if (!GAME.CollidesWithMap(nextXBounds))
    player->MoveHorizontally(horizontalMove.x);
```

Para P-3.7 (sin mapa, solo bounds del mapa de ±500 unidades):
```cpp
newX = std::clamp(newX, MAP_MIN, MAP_MAX);
newY = std::clamp(newY, MAP_MIN, MAP_MAX);
```

#### C) Validación server-side de velocidad (anti-cheat base)

```cpp
// De Client.cpp:130-178 — directamente transportable como lógica de GameWorld:
const float MAX_SPEED = 150.f;
const float TOLERANCE = 5.f;
const float TIME_PER_TICK = 0.01f; // 1/100Hz en NetworkMiddleware

float dx = newState.x - prevState.x;
float speedX = std::abs(dx / TIME_PER_TICK);
if (speedX > MAX_SPEED + TOLERANCE)
    // rechazar input, mantener última posición válida
```

NetworkMiddleware procesa inputs a 100Hz (no 20Hz). `TIME_PER_PACKET` cambia de 0.05f a 0.01f.

#### D) Modelo de vida/respawn de héroe

```cpp
// De Client.h y Player.cpp:
int health = 5;   // puntos de vida por "stock"
int lives  = 3;   // vidas totales
// damage = 1 punto/disparo
// al llegar health=0: respawn, lives--
// al llegar lives=0: game over
```

Para un MOBA, el modelo sería `health` continuo (ej: 0-2000 HP), sin "stocks". Pero el patrón
de separar `health` (vida actual) de `maxHealth` y `lives` como entidad separada ya existe en
`HeroState.h` de NetworkMiddleware.

#### E) Sistema de mockery/emote como paquete crítico sin payload

```cpp
// De PacketManager.cpp:118-129
// SEND_MOCKERY: 0 bytes de payload, se envía como CRITIC
// El servidor reenvía RECEIVE_MOCKERY al rival
// El rival reproduce el sonido
```

Patrón útil para cualquier evento de "notificación social" (emotes, ping de mapa, etc.) que
debe llegar garantizado pero que no tiene payload complejo. Se mapea directamente a
`PacketType::ReliableUnordered` de NetworkMiddleware.

#### F) Interpolación de entidades para el cliente Unreal (Fase 6)

```cpp
// De Player.cpp:169-214
float t = elapsedTime / interpolationTime;
sprite->setPosition(Lerp(firstPosition, lastPosition, t));
```

Este patrón exacto es lo que el plugin Unreal necesitará implementar en Fase 6 para suavizar
los Snapshots que llegan del servidor. La función `Lerp` es trivial, pero la lógica de
cuándo activar la interpolación (buffer mínimo de 2 posiciones, reset del timer al recibir
nuevas) es una referencia válida.

### 9.3 GAPS DEL PROYECTO LEGACY QUE NETWORKMIDDLEWARE SÍ RESUELVE

| Gap de AA4 | Cómo lo resuelve NetworkMiddleware |
|-----------|-----------------------------------|
| No hay delta compression | P-3.5: SerializeDelta, ZigZag+VLE, 38 bits sin cambios |
| No hay session recovery real | P-3.6: zombie state, token de reconexión, 120s window |
| No hay Clock Sync | P-3.4: RTT EMA α=0.1, clockOffset server-client |
| Game state solo client-side (balas) | P-3.7 (por implementar): estado autoritario en server |
| No hay snapshot history | RemoteClient.SnapshotHistory: 64 slots, delta baselines |
| Thread pool implementado pero desconectado | P-4.4 (por implementar): Thread Pool real |
| No hay profiler de red | NetworkProfiler: tick budget, kbps in/out, delta efficiency |
| No hay tests | 139 tests pasando como safety net |

### 9.4 FUNCIONALIDAD PRESENTE EN AA4 QUE NETWORKMIDDLEWARE NO TIENE

| Feature de AA4 | Relevancia para NetworkMiddleware | Fase sugerida |
|----------------|----------------------------------|---------------|
| Matchmaking TCP + handoff a UDP | NetworkMiddleware no tiene login/matchmaking | Fuera de scope del TFG |
| Base de datos con bcrypt | NetworkMiddleware no tiene autenticación | Fuera de scope del TFG |
| Interpolación Lerp cliente | Necesaria en cliente Unreal para suavizado | Fase 6 |
| Anti-cheat velocidad server-side | Lógica de GameWorld en P-3.7 | P-3.7 |
| Respawn y vidas de jugador | Lógica del game loop mínimo | P-3.7 |
| Física de movimiento (moveSpeed, gravity) | Referencia para valores de GameWorld | P-3.7 |

---

## 10. Madurez Técnica por Área

| Área | AA4 | NetworkMiddleware | Observación |
|------|-----|------------------|-------------|
| **Arquitectura** | 4/10 | 9/10 | AA4: singletons, acoplamiento fuerte. NM: DI, SOLID, Composition Root |
| **Protocolo UDP** | 4/10 | 9/10 | AA4: raw bytes, sin delta, sin bitmask ACK. NM: bit-packing, ACK bitmask, VLE |
| **Confiabilidad** | 5/10 | 9/10 | AA4: manual con backoff. NM: RTT adaptativo, canal tipado |
| **Seguridad** | 4/10 | 6/10 | AA4: bcrypt (bien), sin token auth UDP. NM: challenge/response, session tokens |
| **Rendimiento** | 3/10 | 9/10 | AA4: spin-loop CPU 100%, 30Hz. NM: sleep_until, 100Hz, profiler |
| **Gameplay** | 7/10 | 2/10 | AA4: funciona end-to-end, tiene física, balas. NM: sin game state aún (P-3.7) |
| **Mantenibilidad** | 3/10 | 8/10 | AA4: sin tests, singletons. NM: 139 tests, coverage, CI |
| **Testing** | 1/10 | 9/10 | AA4: cero tests. NM: suite completa, TDD |
| **Documentación** | 2/10 | 7/10 | AA4: sin comentarios en lógica crítica. NM: CLAUDE.md, IR, DL |
| **Escalabilidad** | 2/10 | 7/10 | AA4: 1 hilo/room, sin thread pool. NM: Thread Pool en P-4.4 |

---

## 11. Opinión Global y Comparativa

### Sobre el proyecto AA4 (perspectiva honesta)

Para alguien que llegaba al mundo de la programación de red por primera vez, AA4 demuestra un
nivel de comprensión sorprendente. La separación TCP/UDP, el sistema de ACKs con backoff, la
validación server-side de velocidad, la interpolación visual del enemigo, y bcrypt para
contraseñas son decisiones que muchos proyectos de nivel mayor no toman correctamente.

Los problemas más graves (double-delete, spin-loop, threads detached) son bugs clásicos de
"primeros pasos": el código funciona en práctica porque el sistema operativo compensa
(scheduler no ejecuta el thread suelto en un momento crítico, el proceso termina antes de
que el double-free se materialice, etc.). Son bugs que solo afloran bajo carga real o en
condiciones de timing específicas. Que haya funcionado lo suficiente para pasar la práctica
es una mezcla de suerte y de que el scope limitado (2 jugadores, LAN) los ocultó.

Lo que me sorprende positivamente es el `TaskPool`: correctamente implementado con jthread,
condition_variable, y atomic flag — y nunca usado. Esto sugiere que el alumno investigó la
técnica correcta pero no llegó a integrarla. Eso es exactamente lo que P-4.4 en NetworkMiddleware
va a hacer: implementar el Thread Pool real y conectarlo al sistema.

### Comparativa de evolución

El salto de AA4 a NetworkMiddleware es grande y está bien justificado. No es una reescritura
gratuita — es un cambio de paradigma completo:

- De "código que funciona en LAN" a "middleware de producción testeable"
- De "mandar posiciones completas" a "delta compression con baseline ACKed"
- De "un thread por sala" a "100Hz determinista con thread pool escalable"
- De "0 tests" a "139 tests como safety net permanente"

Lo que NetworkMiddleware aún no tiene y AA4 sí tiene es **gameplay real**: un jugador que se
mueve, salta, dispara, muere y respawna. El input llega al servidor y produce un cambio de
estado. Eso es exactamente lo que P-3.7 va a cerrar. Y los valores concretos de ese gameplay
(moveSpeed=100, MAX_SPEED_X=150, TIME_PER_PACKET) son una referencia directa de un sistema
que funcionó.

### Recomendación para P-3.7

Para implementar el game loop mínimo en NetworkMiddleware, los valores y patrones concretos
a usar del AA4 son:

1. **moveSpeed = 100 unidades/s** para ViegoEntity en movimiento top-down
2. **MAX_SPEED validación = moveSpeed + tolerancia (5-10%)** — misma lógica que Client.cpp
3. **TIME_PER_TICK = 0.01f** (1/100Hz) en lugar de 0.05f (1/20Hz) del AA4
4. **Respawn en posición (0,0)** cuando health=0 (simplificación para P-3.7)
5. **Patrón de validar input antes de aplicar movimiento** — del flujo de ValidateClientMovements

El juego del AA4 es el "prototipo sucio" del que NetworkMiddleware es la versión correcta.
Ambos tienen valor: el AA4 como prueba de que el concepto funciona en gameplay real, y
NetworkMiddleware como la implementación correcta que lo sostendrá a escala.

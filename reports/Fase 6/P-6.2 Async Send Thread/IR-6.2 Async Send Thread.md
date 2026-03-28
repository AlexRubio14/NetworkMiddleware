# IR-6.2 — Async Send Thread (AsyncSendDispatcher)

**Branch:** `P-6.2-Async-Send-Thread`
**Date:** 2026-03-28
**Tests:** 236 / 236 passing (231 previos + 5 nuevos `AsyncSendDispatcher`)

---

## What was implemented

### Motivación

P-6.1 redujo las syscalls de envío de 49 a 1 (`sendmmsg`). Sin embargo, esa syscall todavía ocurre **en el hilo del game loop**, dentro de `FlushTransport()` tras Phase B. Aunque `sendmmsg` es más rápida que 49 × `send()`, cualquier bloqueo temporal del kernel (buffer de envío lleno, contención del scheduler) puede hacer que el tick supere los 10ms.

El objetivo de P-6.2 es **mover la syscall `sendmmsg` completamente fuera del hilo principal**. El game loop llama `Signal()` (≈ 0µs) y retorna de inmediato; un jthread dedicado despierta y ejecuta `Flush()` de forma asíncrona.

---

### Decisiones de diseño

**¿Por qué `std::jthread` + `std::condition_variable` y no `std::async` o un pool?**

`std::jthread` provee un hilo persistente con `std::stop_token` integrado — el destructor solicita stop automáticamente y hace join, garantizando que el hilo termine limpiamente sin código boilerplate. `std::async` crearía un nuevo hilo por `Signal()` o devolvería un future que el game loop tendría que gestionar. Un pool es overhead innecesario para un único productor (game loop) y un único consumidor (send thread).

**¿Por qué `condition_variable` + timeout y no `std::atomic<bool>` con spinning?**

Spinning quema CPU en el núcleo asignado al send thread. La `condition_variable` duerme en `futex_wait` y solo consume CPU cuando hay trabajo. El timeout de `kFlushInterval` (33ms, matching 30Hz) es una red de seguridad: si un `Signal()` se pierde por una race mínima al inicio (imposible por el diseño actual, pero defensivo), el thread igualmente flushea.

**¿Por qué `kFlushInterval = 33ms` (30Hz)?**

Los snapshots se envían a 30Hz (cada 3 ticks a 100Hz). Si el dispatcher no recibe ninguna señal, la tasa de flush máxima sin señales es también ~30Hz — sin work-amplification.

**¿Por qué el lock se suelta antes de llamar `Flush()`?**

`Flush()` ejecuta `sendmmsg` — una syscall que puede tardar microsegundos o, en condiciones adversas, más. Mantener `m_cvMutex` durante ese tiempo bloquearía `Signal()` en el game loop. El diseño suelta el lock antes de `Flush()`, de modo que el game loop puede volver a señalizar sin esperar.

**¿Por qué un final drain en `ThreadLoop` tras el stop?**

El destructor llama `request_stop()` + `notify_one()`. El thread puede salir del `wait_for` con `m_signaled = false` (stop ganó la race). Los paquetes encolados entre el último `Flush()` y el `request_stop()` se perderían. La última línea `m_transport->Flush()` fuera del while drena esos paquetes antes de que el jthread destruya el socket.

**¿Por qué el dispatcher es nullable (`unique_ptr`, default = nullptr)?**

- En **Linux producción**: `AsyncSendDispatcher` activo → `FlushTransport()` llama `Signal()`.
- En **Windows / CI / tests**: sin dispatcher → `FlushTransport()` hace `m_transport->Flush()` síncronamente (comportamiento P-6.1 idéntico).

Esto garantiza zero regresión en la suite de Windows y permite que los tests de `AsyncSendDispatcher` usen `MockTransport` sin dependencias de plataforma.

---

## Modified files

### `Core/AsyncSendDispatcher.h` (nuevo)

```cpp
class AsyncSendDispatcher {
public:
    explicit AsyncSendDispatcher(std::shared_ptr<Shared::ITransport> transport);
    ~AsyncSendDispatcher();
    void Signal();          // Non-blocking. Notifica al send thread.
private:
    void ThreadLoop(std::stop_token st);
    std::shared_ptr<Shared::ITransport> m_transport;
    std::jthread                        m_thread;
    std::mutex                          m_cvMutex;
    std::condition_variable             m_cv;
    bool                                m_signaled{false};
    static constexpr auto kFlushInterval = std::chrono::milliseconds(33);
};
```

### `Core/AsyncSendDispatcher.cpp` (nuevo)

| Método | Descripción |
|--------|-------------|
| `AsyncSendDispatcher(transport)` | Mueve el transport; arranca el jthread con `ThreadLoop`. |
| `~AsyncSendDispatcher()` | `request_stop()` + `notify_one()`; jthread destructor hace join. |
| `Signal()` | `lock_guard` → `m_signaled = true` → `notify_one()`. Nunca bloquea. |
| `ThreadLoop(st)` | `wait_for(kFlushInterval, [m_signaled || stop])` → suelta lock → `Flush()`. Loop hasta stop. Final drain tras while. |

### `Core/NetworkManager.h`

```cpp
// Antes (P-6.1):
// void FlushTransport();  — delegaba directamente a m_transport->Flush()

// Ahora (P-6.2):
std::unique_ptr<AsyncSendDispatcher> m_dispatcher;  // nullable; null = sync flush

explicit NetworkManager(std::shared_ptr<Shared::ITransport> transport,
                        std::unique_ptr<AsyncSendDispatcher> dispatcher = nullptr);
```

### `Core/NetworkManager.cpp`

```cpp
// Constructor:
NetworkManager::NetworkManager(shared_ptr<ITransport> transport,
                               unique_ptr<AsyncSendDispatcher> dispatcher)
    : m_transport(std::move(transport))
    , m_dispatcher(std::move(dispatcher)) {}

// FlushTransport():
void NetworkManager::FlushTransport() {
    if (m_dispatcher)
        m_dispatcher->Signal();   // async path — zero I/O en el game loop
    else
        m_transport->Flush();     // sync path — SFML / Windows / tests
}
```

### `Server/main.cpp`

```cpp
// Construcción condicional por plataforma:
#ifdef __linux__
    auto dispatcher = std::make_unique<Core::AsyncSendDispatcher>(transport);
    NetworkManager manager(transport, std::move(dispatcher));
#else
    NetworkManager manager(transport);  // sin dispatcher — sync flush
#endif
```

`FlushTransport()` en el game loop no cambia: el mismo call site sirve ambas rutas.

### `Transport/RawUDPTransport.h` y `Transport/RawUDPTransport.cpp`

Ampliados en P-6.2 con el swap-buffer thread-safe:

```cpp
// Añadido en P-6.2 (estaban ausentes en P-6.1):
std::vector<OutMessage>  m_sendQueue;    // productor (main thread)
std::vector<OutMessage>  m_flushQueue;   // consumidor (send thread)
std::mutex               m_queueMutex;  // protege solo el swap O(1)
```

`Send()` adquiere `m_queueMutex` solo durante el `push_back`. `Flush()` adquiere `m_queueMutex` solo durante `std::swap` (intercambia punteros internos del vector — O(1)). La syscall `sendmmsg` corre sin lock sobre `m_flushQueue`, que el game loop nunca toca.

### `Core/CMakeLists.txt`

```cmake
add_library(MiddlewareCore STATIC
    AsyncSendDispatcher.h
    AsyncSendDispatcher.cpp
    # ... resto sin cambios
)
```

`Threads::Threads` ya era dependencia de `MiddlewareCore` — no se añadió nada nuevo.

### `tests/CMakeLists.txt`

```cmake
add_executable(MiddlewareTests
    # ...
    Core/AsyncSendDispatcherTests.cpp   # nuevo
)
```

Los tests de `AsyncSendDispatcher` son cross-platform (usan `MockTransport`, sin POSIX sockets).

### `tests/Core/MockTransport.h`

```cpp
// Añadido:
int flushCount = 0;
void Flush() override { ++flushCount; }
```

`flushCount` permite a los tests verificar cuántas veces el dispatcher llamó `Flush()` sin necesitar un transport real.

---

## New tests (5 — cross-platform)

Ubicación: `tests/Core/AsyncSendDispatcherTests.cpp`

| Test | Qué verifica |
|------|-------------|
| `SignalWakesFlushPromptly` | `Signal()` despierta el thread y produce ≥ 1 `Flush()` en < 10ms |
| `FlushCalledByTimeoutWithoutSignal` | Sin Signal, el thread flushea tras ≥ 33ms (timeout de seguridad) |
| `DestructorFlushesOnShutdown` | El destructor produce ≥ 1 `Flush()` extra (final drain) |
| `MultipleSignalsCoalesceIntoFewFlushes` | 10 Signal() rápidos coalesan en ≤ 5 Flush() (no 1-por-señal) |
| `NetworkManagerFlushTransport_UsesDispatcher` | `Signal()` → `flushCount` crece; confirma el path async |

---

## Invariantes de thread safety

### AsyncSendDispatcher (CV + jthread)

| Operación | Hilo | Protección |
|-----------|------|-----------|
| `Signal()` — escribe `m_signaled` | Game loop (main) | `lock_guard(m_cvMutex)` |
| `wait_for(...)` — lee `m_signaled` | Send thread | `unique_lock(m_cvMutex)` |
| `Flush()` / `sendmmsg` | Send thread | Lock ya suelto — sin contención con `Signal()` |

### RawUDPTransport (swap-buffer)

La race identificada por Gemini es real: si `sendmmsg` tarda más de 10ms, el game loop del siguiente tick puede llamar `Send()` mientras el send thread sigue en `Flush()`. El fix es un **swap-buffer** ya implementado en P-6.2:

```
m_sendQueue   — cola del productor (game loop). Send() hace push_back aquí.
m_flushQueue  — cola del consumidor (send thread). sendmmsg opera aquí.
m_queueMutex  — protege solo el swap O(1), no la syscall.
```

```
Send()  [main thread]:
  lock(m_queueMutex)
  m_sendQueue.push_back(msg)      ← O(1), lock brevísimo
  unlock

Flush() [send thread]:
  lock(m_queueMutex)
  if m_sendQueue.empty() return
  swap(m_sendQueue, m_flushQueue) ← O(1) swap de punteros
  unlock
  // sendmmsg opera sobre m_flushQueue — sin lock, sin contención
  sendmmsg(m_flushQueue)
  m_flushQueue.clear()            // retiene capacidad — evita realloc
```

**Resultado**: `Send()` y `sendmmsg` nunca tocan la misma cola simultáneamente. El mutex se mantiene solo durante el swap — microsegundos, no durante la syscall.

| Operación | Hilo | Cola tocada | Protección |
|-----------|------|------------|-----------|
| `Send()` | Game loop | `m_sendQueue` | `lock(m_queueMutex)` + push |
| `swap` | Send thread | ambas | `lock(m_queueMutex)` |
| `sendmmsg` + `clear` | Send thread | `m_flushQueue` | sin lock — main no toca esta cola |
| `GetOrBuildAddr()` | Game loop | `m_addrCache` | solo main thread — sin lock |

---

## Notas para Gemini

- **`recvmmsg` / `epoll`** siguen fuera de scope (igual que en P-6.1). El receive path (`Receive()` en el game loop) no es el bottleneck actual.
- La **coalescing** de Signal() es implícita: si el send thread está procesando `Flush()` mientras llegan N Signal(), solo verá 1 (el `m_signaled = true` se comparte). Esto es correcto — lo que importa es que el siguiente `Flush()` envíe todos los paquetes acumulados hasta ese momento.
- El **timeout de 33ms** es un safety net, no el path normal. En producción, cada tick de 30Hz genera un `Signal()` que despierta el thread en < 1ms. El timeout solo dispara si el game loop se detiene inesperadamente.
- Si en el futuro se implementa **`recvmmsg`** en un receive thread dedicado, `AsyncSendDispatcher` no necesita cambios — es independiente del path de recepción.

---
type: dev-log-alex
proposal: P-4.4
date: 2026-03-23
status: personal
---

# DEV LOG — P-4.4 Dynamic Work-Stealing Job System

**Propuesta:** P-4.4 Dynamic Work-Stealing Job System & Split-Phase Snapshots
**Fecha:** 2026-03-23

---

## ¿Qué problema resolvíamos?

Tras P-4.3 el middleware usa el **1.1% del tick budget** con 47 clientes. Ese margen es cómodo hoy, pero hay dos razones para no quedarse quieto:

1. **Fase 5 (Spatial Hashing + Kalman)** añadirá predicción de posición y relevancy filtering — ambas operaciones aumentan el coste por tick de forma cuadratica con el número de clientes.
2. **La serialización de snapshots es secuencial.** En el loop de P-3.7, el servidor itera sobre todos los clientes establecidos y llama `SendSnapshot()` uno a uno. Con 100 clientes, son 100 serializaciones en cadena antes de avanzar al siguiente tick.

La oportunidad es clara: la serialización de cada snapshot (`BitWriter + HeroSerializer + delta compression`) es independiente por cliente. No hay estado compartido entre dos clientes distintos en la fase de lectura. Se puede paralelizar con garantías de corrección si sabemos exactamente qué puede ocurrir en paralelo y qué no.

El problema es que la solución no es trivial. `SendSnapshot()` hacía tres cosas en una sola llamada:
1. **Serializar** (leer baseline + estado → construir payload) — paralelizable
2. **Registrar** (escribir el snapshot en el historial de 64 slots) — modifica estado del cliente
3. **Enviar** (llamar al socket UDP) — SFML no es thread-safe

Mezclar leer y escribir en paralelo da data races. La solución es separar las fases: P-4.4.

---

## ¿Qué hemos construido?

| Componente | Dónde | Qué hace |
|-----------|-------|---------|
| **EMA reactiva** | `Core/NetworkProfiler.h/.cpp` | `RecordTick()` mantiene EMA(α=0.1) del tiempo de tick; expuesto como `recentAvgTickMs` |
| **`SerializeSnapshotFor()`** | `Core/NetworkManager.h/.cpp` | Read-only sobre RemoteClient; llamable desde workers |
| **`CommitAndSendSnapshot()`** | `Core/NetworkManager.h/.cpp` | Main-thread only; escribe historial y envía el payload ya serializado |
| **`WorkStealingQueue`** | `Core/JobSystem.h/.cpp` | deque + mutex por hilo; LIFO propietario, FIFO ladrón |
| **`JobSystem`** | `Core/JobSystem.h/.cpp` | Pool dinámico: round-robin dispatch, escalado EMA, hysteresis 5s |
| **Split-Phase loop** | `Server/main.cpp` | Phase A: serialize en paralelo con `std::latch`; Phase B: commit+send secuencial |
| **24 tests nuevos** | `ProfilerTests.cpp` + `JobSystemTests.cpp` | 6 EMA + 5 WorkStealingQueue + 12 JobSystem + 1 SnapshotIntegrity |

---

## Split-Phase — la idea central

El diseño más importante de P-4.4 no es el thread pool, sino el **protocolo Split-Phase**. El thread pool sin este protocolo sería incorrecto; con él es seguro por construcción.

La idea: dividir cada tick en dos fases con una barrera explícita entre ellas.

```
Phase A — lectura paralela:
  ┌─────────────────────────────────────────────────────────┐
  │  Por cada cliente:                                       │
  │  buffer[i] = SerializeSnapshotFor(ep, state, tickID)     │
  │  ← workers del JobSystem, read-only sobre RemoteClient  │
  │  ← std::latch cuenta regresiva al terminar cada job     │
  └─────────────────────────────────────────────────────────┘
              latch.wait()  ← main thread bloqueado aquí
Phase B — escritura secuencial:
  ┌─────────────────────────────────────────────────────────┐
  │  Por cada cliente (main thread):                        │
  │  CommitAndSendSnapshot(ep, state, buffer[i])            │
  │  ← escribe m_history, actualiza seqContext, UDP send    │
  └─────────────────────────────────────────────────────────┘
```

**¿Por qué `std::latch` y no `std::barrier`?**

`std::latch` es single-use — una vez que llega a 0 no puede resetearse. `std::barrier` es reutilizable. Para este patrón, la naturaleza single-use de `latch` es exactamente lo que queremos: instanciar un nuevo `latch(N)` cada tick deja cero estado residual entre ticks. Si usáramos un `std::barrier` reutilizable, tendríamos que razonar sobre si el barrier del tick N-1 ya ha terminado completamente antes de iniciar el tick N. Con `latch`, no hay esa pregunta — cada tick tiene su propio objeto de sincronización.

**¿Por qué copiar `HeroState` por valor en `SnapshotTask`?**

La alternativa natural es capturar `const HeroState*` del `GameWorld`. Pero entre el momento en que se hace dispatch de los jobs (paso 4a) y el momento en que el worker los ejecuta, `gameWorld.Tick()` (paso 3 del tick anterior) podría haber modificado el estado. La copia por valor al colectar las tasks congela el estado del tick actual en el momento correcto. `HeroState` son 60 bytes — 100 copias son 6KB, completamente negligible.

---

## NetworkManager — refactoring de `SendSnapshot()`

`SendSnapshot()` existía desde P-3.7 y los tests de `GameWorldTests` y `NetworkManagerTests` lo usaban directamente. No podía eliminarse ni cambiar su API sin romper 156 tests.

La solución fue **añadir** las dos nuevas funciones y hacer que `SendSnapshot()` delegue internamente:

```
SendSnapshot(ep, state, tickID)
    → SerializeSnapshot(client, state, tickID)  [privado, sin efectos]
    → RecordSnapshot(usedSeq, state)            [escribe historial]
    → m_transport->Send(buffer, ep)             [envía]

CommitAndSendSnapshot(ep, state, buffer)
    → RecordSnapshot(usedSeq, state)            [escribe historial]
    → m_transport->Send(buffer, ep)             [envía]
```

El contrato de thread-safety de `SerializeSnapshotFor()` es estricto:

```
Durante Phase A (workers corriendo):
  • m_establishedClients: sin modificaciones (main bloqueado en latch::wait())
  • RemoteClient::m_history: solo lecturas (GetBaseline)
  • RemoteClient::m_lastClientAckedServerSeq: solo lecturas
  • RemoteClient::seqContext.localSequence: solo lecturas (sin AdvanceLocal)
  • HeroState: copia local en SnapshotTask, no compartida

Durante Phase B (solo main thread):
  • latch::wait() ha retornado → todos los jobs terminados
  • CommitAndSendSnapshot escribe m_history, lastOutgoingTime, seqContext
  • m_transport->Send(): single-threaded, SFML socket seguro
```

---

## WorkStealingQueue — por qué mutex y no lock-free

La decisión más técnicamente cuestionable de P-4.4 es usar `std::mutex` en lugar de una implementación lock-free (Chase-Lev deque, CAS circular array, etc.). Argumento:

Las implementaciones lock-free de work-stealing requieren operaciones CAS doble (DCAS) o manejo del ABA problem. El paper original de Chase-Lev (2005) asume un garbage collector para manejar la recolección segura del array subyacente. Sin GC en C++, la implementación lock-free correcta es considerablemente compleja y tiene potencial para memory ordering bugs sutiles.

Para un pool de ≤8 threads con ≤100 tasks por tick, un `std::mutex` por cola **reduce contención >90% respecto a una cola centralizada**. El coste de adquisición de mutex sin contención en x86 es ~20ns — irrelevante vs. el coste de serializar un snapshot (>1µs). Si hubiera contención, la media de wait time sería también ~20-50ns — aún irrelevante para un tick de 10ms.

La correctitud vale más que el rendimiento marginal aquí. Un mutex por cola es la elección correcta para el TFG.

```
Contención:
  Cola centralizada (1 mutex global): todos los threads compiten por el mismo lock
  Cola por thread  (N mutexes):       el propietario tiene su mutex solo para él
                                      los ladrones compiten entre sí solo cuando roban
                                      (caso inusual, carga desbalanceada)
```

---

## WorkerLoop — el ciclo de vida de un worker

El loop de cada worker sigue tres pasos en orden de prioridad:

**1. Pop propio queue (LIFO):** el trabajo más reciente dispatch es el más probable de estar en caché L1/L2 del thread. LIFO maximiza temporal locality.

**2. Robo circular (FIFO desde el fondo):** el ladrón roba del fondo de la deque de la víctima. Esto es FIFO desde la perspectiva del propietario — los jobs más antiguos son los primeros en robarse. ¿Por qué? Si el ladrón robara del mismo extremo que el propietario (frente), habría colisiones cabeza-cola cuando la cola tenga 1 elemento. Robar del fondo elimina esa carrera.

**3. Sleep 200µs:** si no hay trabajo, el worker duerme en un `condition_variable::wait_for`. No es busy-spin — 200µs es suficiente resolución para un loop de 10ms sin quemar CPU innecesariamente. El `notify_one()` en `Execute()` despertará al worker antes del timeout si llega trabajo.

---

## MaybeScale — escalado adaptativo con EMA y hysteresis

La decisión de cuándo añadir o quitar threads se basa en el `recentAvgTickMs` del `NetworkProfiler`. Hay dos diseños posibles:

**Ventana deslizante de N ticks:** ring-buffer de `float[N]` + suma acumulada. Preciso pero requiere memoria extra y lógica de wrap-around.

**EMA(α=0.1):** `EMA_new = 0.1 × current + 0.9 × EMA_old`. Un solo float. Half-life de ~7 ticks a 100 Hz (tarda 7 ticks en perder la mitad del peso de un evento). Converge al 99% del valor estacionario tras 100 ticks.

Para las necesidades del TFG (detectar tendencias de varios cientos de ticks, no spikes individuales), la EMA es la elección correcta.

Los umbrales de escalado:

```
> 7.0ms → AddThread   (tick usando >70% del budget → bajo presión)
< 3.0ms → RemoveThread (tick usando <30% del budget → holgura)
```

La **hysteresis de 5s** en el downscale evita el problema del thrashing: sin ella, un servidor con carga borderline (3.0-7.0ms) podría añadir un thread, ver que baja a 2.9ms, quitarlo, ver que sube a 3.1ms, añadirlo... infinitamente. La hysteresis garantiza que el pool no puede encogerse más de una vez cada 5 segundos.

---

## AddThread / RemoveThread — ordenamiento de operaciones crítico

El orden en que se actualiza `m_activeCount` respecto a otras operaciones es fundamental para la correctitud.

### `AddThread()` — incrementar ANTES de lanzar el thread

```cpp
m_activeCount.fetch_add(1, memory_order_release);   // PRIMERO
m_slots[idx]->thread.emplace([this, idx](stop_token st) {
    WorkerLoop(idx, st);
});                                                    // DESPUÉS
```

Si se lanzara el thread primero y se incrementara después, habría una ventana donde `Execute()` podría hacer dispatch a un índice dentro del range `[0, count-1]` que aún no tiene worker loop corriendo. El job quedaría huérfano en una cola que nadie está escuchando. Incrementar primero cierra esa ventana: cuando el worker arranca, ya puede ver el count correcto.

### `RemoveThread()` — decrementar DESPUÉS de drenar, ANTES de señalizar stop

```cpp
m_slots[retiring]->shouldRun.store(false);   // 1. No-dispatch desde Execute()
// Drain: mover tasks pendientes a slot 0   // 2. Tasks no se pierden
m_activeCount.fetch_sub(1);                  // 3. Count baja
m_slots[retiring]->thread->request_stop();   // 4. Señalizar al worker
```

La secuencia tiene una razón para cada paso:
- **Paso 1 antes de 2:** si decrementáramos el count antes de drenar, `Execute()` podría ver el slot como fuera de range pero el slot aún tiene tasks pendientes. Al marcar `shouldRun=false` primero, `Execute()` skippea el slot y lo protege de nuevos encoles.
- **Paso 2 antes de 3:** drenar las tasks pendientes a slot 0 garantiza que ningún job desaparece durante el downscale. Sin este drain, las tasks que estaban en la cola del slot retiring se perderían.
- **Paso 3 antes de 4:** el worker moribundo debe dejar de robar a sus peers antes de recibir el stop. Si recibiera el stop y continuara intentando robar durante la fase de apagado, estaría compitiendo por tasks que corresponden a los threads supervivientes.

---

## EMA en NetworkProfiler — preparación para P-4.4 desde P-4.3

Un detalle de continuidad: el `NetworkProfiler` ya usaba `std::atomic<uint64_t>` desde P-4.3, anticipando que P-4.4 añadiría threads. En P-4.4 solo fue necesario añadir el campo EMA y el accessor — no hubo que refactorizar los atomics.

```cpp
// En RecordTick() — 2 operaciones FP más por tick:
const float newEma = kEmaAlpha * static_cast<float>(microseconds)
                   + (1.0f - kEmaAlpha) * m_recentAvgTickUs.load(memory_order_relaxed);
m_recentAvgTickUs.store(newEma, memory_order_relaxed);
```

`relaxed` ordering es suficiente aquí porque tanto el escritor (`RecordTick()`, main thread) como el lector (`MaybeScale()`, también main thread) están en el mismo hilo. No hay race condition que ordenamiento más fuerte pudiera prevenir.

---

## CodeRabbit reviews — 12 issues en 2 rondas

La PR de P-4.4 pasó por dos rondas de revisión de CodeRabbit (4 + 8 issues). Las correcciones más interesantes:

**Ronda 1 — 4 issues:**
- `Execute()` no skippeaba slots con `shouldRun=false` — si un thread estaba siendo retirado, el round-robin podía dispatch a su slot justo después de que `RemoveThread()` hubiera vaciado su cola pero antes de que saliera del sistema. El fix añade el bucle que skippea slots retiring.
- La memoria del `std::latch` debía estar en `SnapshotTask` y no como variable local raw en el cuerpo del loop — riesgo de que la lambda capturara una referencia a un objeto destruido.

**Ronda 2 — 8 issues:**
- `RemoveThread()` faltaba el drain de tasks pendientes antes de decrementar el count. Sin esto, jobs podían perderse en el downscale.
- El destructor no hacía `request_stop()` explícito antes del join — los jthreads se destruyen con stop implícito, pero la documentación del C++ standard es ambigua sobre el ordering cuando hay `m_cv.notify_all()` de por medio. Añadir el request_stop explícito elimina la ambigüedad.
- Varios comentarios de documentación de contratos de thread-safety faltaban en el header.

El proceso de CodeRabbit fue útil — 12 issues en una implementación de ~260 líneas es una densidad de bugs potenciales no trivial para threading code.

---

## Conceptos nuevos en esta propuesta

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **Split-Phase snapshots** | Separar serialización (Phase A, paralela) de commit+send (Phase B, secuencial) | Permite paralelización sin locks en el hot path del tick |
| **std::latch (C++20)** | Contador descendente single-use para sincronización | Más simple y sin estado residual vs barrier; correcto por construcción para tick-by-tick sync |
| **Work-stealing** | Worker inactivo roba del fondo de la cola de un peer activo | Elimina el overhead de coordinación en carga balanceada; equilibra automáticamente en carga desbalanceada |
| **LIFO owner / FIFO thief** | Propietario pop del frente (caliente), ladrón roba del fondo (frío) | Evita colisión head-tail con 1 elemento; maximiza temporal locality del propietario |
| **EMA(α=0.1)** | Media exponencial móvil — 1 float + 2 operaciones FP por tick | Half-life ~7 ticks; no requiere ring-buffer; suficiente para detectar tendencias de carga |
| **Hysteresis en scaling** | Cooldown de 5s en downscale | Previene thrashing add/remove en cargas borderline |
| **AddThread antes de launch** | Incrementar count antes de lanzar el jthread | Cierra ventana donde Execute() dispatch a slot sin worker |
| **Drain en RemoveThread** | Migrar tasks del slot retiring a slot 0 antes de decrementar | Garantiza que ningún job se pierde en downscale |
| **`safeRun` wrapper** | try/catch alrededor de cada job en WorkerLoop | Un job que lanza excepción no puede terminar el proceso servidor |

---

## Qué podría salir mal (edge cases conocidos)

- **Tick con 0 clientes establecidos:** `latch(0)` no es válido en C++20 (UB o assert). El código lo evita con un guard: si `tasks.empty()`, se skippea la Phase A completamente. Pero si se añade lógica que llama `Execute()` con 0 tasks antes de este guard, se rompería.

- **`Execute()` durante destructor:** si el destructor llama `request_stop()` pero hay código de usuario que sigue llamando `Execute()` después (carrera de destrucción), el job podría caer al fallback inline de slot 0, que ya no tiene worker. El job se ejecutaría en el main thread pero correctamente. Para el TFG donde el lifetime del `JobSystem` está controlado en `main()`, no es un problema.

- **Hysteresis y reinicio del servidor:** `m_lastDownscaleTime` se inicializa como `time_point{}` (epoch). En la primera evaluación de downscale, `now - epoch` es >>5s, por lo que el primer downscale nunca está bloqueado por hysteresis. Este es el comportamiento correcto (queremos poder bajar threads desde el inicio si la carga es baja), pero es un edge case no obvio.

- **hardware_concurrency() retorna 0:** POSIX no garantiza que `std::thread::hardware_concurrency()` retorne un valor > 0. El código lo maneja: `max(kMinThreads, hardware_concurrency - 1)` con comprobación de underflow → `kMinThreads = 2` como fallback.

- **Spatial Hashing en Fase 5 + thread pool:** cuando Fase 5 añada relevancy filtering, la lógica que determina qué clientes ven qué entidades deberá ser read-only en Phase A, igual que la serialización. Si el spatial hash se modifica durante Phase A (ej: por un move de entidad), habría una data race. La solución será tomar un snapshot del spatial hash antes de Phase A (mismo patrón que HeroState).

---

## Estado del sistema tras esta implementación

**180 / 180 tests passing.** 0 regresiones.

El tick budget validado en P-4.3 (1.1% con 47 clientes) no ha cambiado en medición — los benchmarks de P-4.3 se hicieron sin job system activo (serialización puramente secuencial). Con el job system activo, la serialización de N clientes debería escalar como `O(N/k)` donde `k` es el número de workers, en lugar de `O(N)`. La mejora medible solo será visible con Fase 5 añadiendo coste por cliente.

**Lo que está pendiente para Fase 5:**
- Spatial Hashing: `GridCell` → `vector<EntityID>`, query de vecinos por radio
- Kalman Filter: predicción de posición, reduce snapshots a `Δpred` en lugar de `Δraw`
- Ambas operaciones son paralelizables en Phase A con el mismo patrón Split-Phase

Fase 4 cerrada.
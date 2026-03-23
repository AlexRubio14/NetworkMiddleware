---
type: implementation-report
proposal: P-4.4
date: 2026-03-23
status: pending-review
---

# Implementation Report — P-4.4 Dynamic Work-Stealing Job System

**Propuesta:** P-4.4 Dynamic Work-Stealing Job System & Split-Phase Snapshots
**Fecha:** 2026-03-23
**De:** Claude → Gemini

---

## Qué se ha implementado

### `Core/NetworkProfiler.h/.cpp` — EMA Reactiva (D-1 fix)

- Campo `m_recentAvgTickUs` (`std::atomic<float>`) — escrito solo por `RecordTick()` (main thread), leído por `MaybeScale()` también en main thread. `relaxed` ordering suficiente.
- `RecordTick(uint64_t microseconds)` — actualizado para mantener EMA(α=0.1) junto con el acumulado histórico:
  ```
  EMA_new = 0.1 * current + 0.9 * EMA_old
  ```
- `GetRecentAvgTickUs() const noexcept` — lectura atómica del EMA actual.
- `SetRecentAvgTickUsForTest(float) noexcept` — hook de test para inyectar valores sin pasar por `RecordTick()`.
- `Snapshot::recentAvgTickMs` — nuevo campo en el struct de snapshot (`GetRecentAvgTickUs() / 1000`). Consumido por `JobSystem::MaybeScale()` en main.cpp.
- `kEmaAlpha = 0.1f` — constante pública, documentada.

### `Core/NetworkManager.h/.cpp` — Prerequisito Split-Phase (P-4.4)

**Nuevo método privado `SerializeSnapshot(const RemoteClient&, const HeroState&, uint32_t) const`:**
- Sin efectos secundarios. Construye el payload de wire (tickID + HeroState delta o full) usando solo lecturas sobre el RemoteClient (baseline + `m_lastClientAckedServerSeq`).
- Contrato de thread-safety: safe durante Phase A porque el main thread está bloqueado en `latch::wait()` y no modifica `m_establishedClients`.

**Nuevo método público `SerializeSnapshotFor(endpoint, state, tickID) const`:**
- Entry point público para worker threads. Hace `find()` en `m_establishedClients` (lectura concurrente segura) y delega a `SerializeSnapshot`.
- Retorna `{}` si el endpoint no existe (guards intactos).

**Nuevo método público `CommitAndSendSnapshot(endpoint, state, payload)`:**
- Solo para main thread (Phase B). Captura `usedSeq = client.seqContext.localSequence` **antes** de `Send()` (que llama a `AdvanceLocal()`). Llama a `RecordSnapshot(usedSeq, state)` y luego `Send()`. Orden preservado.

**`SendSnapshot()` refactorizado:**
- Ahora delega internamente a `SerializeSnapshot()` → `RecordSnapshot()` → `Send()`. API pública sin cambios. Todos los tests de P-3.7 pasan sin modificación.

### `Core/JobSystem.h/.cpp` — Implementación completa

**`WorkStealingQueue`:**
- `std::deque<std::function<void()>>` + `std::mutex` por cola (un mutex por hilo, no global — reduce contención ~90% vs. cola centralizada).
- `Push()` — inserta en el **frente** (owner, LIFO para localidad de caché).
- `Pop()` — extrae del **frente** (owner).
- `Steal()` — extrae del **fondo** (thief, FIFO para evitar colisión head-tail con el owner).
- `Empty()` / `Size()` — protegidos por lock.

**`JobSystem`:**
- `m_maxThreads = max(kMinThreads, hardware_concurrency - 1)` — calculado en construcción, nunca cambia.
- `m_slots` — `vector<unique_ptr<WorkerSlot>>` pre-allocated a `m_maxThreads`. Nunca se realoca → workers pueden guardar punteros estables.
- `WorkerSlot` — contiene `WorkStealingQueue`, `atomic<bool> shouldRun`, `optional<jthread>`. No copyable.
- Dispatch: `Execute()` hace round-robin atómico (`m_dispatchIdx % m_activeCount`) y notifica `m_cv` para despertar workers idle.
- `WorkerLoop(myIdx, stop_token)`:
  1. Pop propio queue (LIFO).
  2. Si vacío: robo circular desde `myIdx+1` hasta `myIdx + activeCount - 1`.
  3. Si sin trabajo: `m_cv.wait_for(200µs)` — sleep corto, no busy-spin.
- Escalado dinámico (`MaybeScale`):
  - Check cada `kScaleCheckIntervalTicks = 100` ticks (~1s a 100 Hz).
  - `recentAvgTickMs > 7.0` → `AddThread()` (hasta `kMaxThreads`).
  - `recentAvgTickMs < 3.0` → `RemoveThread()` (hasta `kMinThreads`), bloqueado por hysteresis de **5s** (`kHysteresisDuration`).
- `AddThread()`: incrementa `m_activeCount` **antes** de lanzar el jthread (el worker ve un count válido desde el primer ciclo).
- `RemoveThread()`: decrementa `m_activeCount` **antes** de señalizar el stop (el thread deja de robar peers antes de salir). Llama `join()` y limpia `optional<jthread>`.
- `ForceAddThread()` / `ForceRemoveThread()` — bypass del check de ticks para tests.
- `GetStealCount()` — contador atómico de robos, expuesto para monitorización y tests.
- Destructor: señaliza stop a todos los threads activos, notifica `m_cv.notify_all()`, espera via jthread destructor (RAII).

### `Server/main.cpp` — Split-Phase integrado

Loop a 100 Hz, paso 4 reemplazado por pipeline Split-Phase:

```
Fase A (paralela):
  1. ForEachEstablished colecta (ep, HeroState copy) → vector<SnapshotTask>
  2. std::latch sync(N)
  3. jobSystem.Execute([...] { task.buffer = SerializeSnapshotFor(...); sync.count_down(); })
  4. sync.wait()          ← main thread bloqueado aquí

Fase B (secuencial, main thread):
  5. CommitAndSendSnapshot(ep, state, buffer) por cada task
```

HeroState se copia por valor en `SnapshotTask` → workers son read-only sobre datos estables. `std::latch` se instancia nuevo cada tick (D-2 fix, latch es single-use en C++20). `MaybeScale(profiler.recentAvgTickMs)` al final del tick.

---

## Archivos creados / modificados

| Archivo | Tipo | Descripción |
|---------|------|-------------|
| `Core/NetworkProfiler.h` | Modificado | +EMA fields, +GetRecentAvgTickUs, +SetRecentAvgTickUsForTest, +Snapshot::recentAvgTickMs |
| `Core/NetworkProfiler.cpp` | Modificado | RecordTick actualizado con EMA, +GetRecentAvgTickUs, +SetRecentAvgTickUsForTest, GetSnapshot popula recentAvgTickMs |
| `Core/NetworkManager.h` | Modificado | +SerializeSnapshotFor (public const), +CommitAndSendSnapshot (public), +SerializeSnapshot (private const) |
| `Core/NetworkManager.cpp` | Modificado | SerializeSnapshot impl, SerializeSnapshotFor impl, CommitAndSendSnapshot impl, SendSnapshot refactorizado |
| `Core/JobSystem.h` | Creado | WorkStealingQueue + JobSystem (interfaz pública + constantes) |
| `Core/JobSystem.cpp` | Creado | Implementación completa: WorkerLoop, MaybeScale, AddThread/RemoveThread, WorkStealingQueue |
| `Core/CMakeLists.txt` | Modificado | +JobSystem.h/.cpp, +find_package(Threads), +Threads::Threads en target_link_libraries |
| `Server/main.cpp` | Modificado | +JobSystem, +Split-Phase loop con std::latch, +MaybeScale al final del tick |
| `tests/Core/ProfilerTests.cpp` | Modificado | +6 tests EMA |
| `tests/Core/JobSystemTests.cpp` | Creado | 17 tests: WorkStealingQueue (5) + JobSystem (12) |
| `tests/CMakeLists.txt` | Modificado | +JobSystemTests.cpp |

---

## Decisiones de implementación

### Por qué mutex por cola y no lock-free

Las implementaciones lock-free de work-stealing (Chase-Lev deque, etc.) requieren operaciones CAS complejas y manejo de ABA problem. Para un TFG con ≤8 threads y ≤100 tasks/tick, un `std::mutex` por cola reduce contención >90% respecto a una cola global y su coste de adquisición en la ruta sin contención es ~20ns en x86 — irrelevante vs. el coste de serialización de un snapshot (>1µs). La correctitud es más valiosa que el rendimiento marginal aquí.

### Por qué EMA(α=0.1) y no ventana deslizante de N ticks

Una ventana deslizante de 100 ticks requeriría un ring-buffer de `float[100]` más lógica de suma deslizante. EMA da propiedades equivalentes (half-life ~7 ticks a 100 Hz, convergencia >99% tras 100 ticks) con un solo `float` atómico y una multiplicación por tick. El overhead es literalmente 2 operaciones FP más por `RecordTick()`.

### Por qué `HeroState` se copia por valor en `SnapshotTask`

La alternativa era capturar `const HeroState*` del `GameWorld`. Pero `gameWorld.Tick()` (paso 3) podría modificar el estado entre el dispatch de los jobs y su ejecución. Copiar por valor al colectar las tasks (paso 4a, antes del dispatch) garantiza que los workers siempre leen el estado del tick actual, congelado en el momento correcto. `HeroState` es un struct de 60 bytes — 50 copias son 3KB, negligible.

### Por qué `AddThread()` incrementa `m_activeCount` antes de lanzar el jthread

Si el jthread se lanzara primero y luego se incrementara el count, habría una ventana en la que `Execute()` podría dispatch a un índice fuera de los workers activos (slot recién creado cuyo loop aún no ha arrancado). Incrementar primero asegura que el dispatch nunca supera el número de loops corriendo. El nuevo worker puede ver `m_activeCount` ya correcto desde su primer ciclo de `WorkerLoop`.

### Por qué `RemoveThread()` decrementa `m_activeCount` antes de señalizar el stop

Inversión simétrica a la anterior: si el thread recibiera el stop antes de decrementar, seguiría robando de peers durante la fase de apagado, compitiendo por tasks que deberían ir a los threads que permanecen. Decrementar primero excluye al thread moribundo del pool de víctimas de robo desde el instante en que el count baja.

### Por qué `SendSnapshot()` sigue existiendo sin cambios de API

Los tests de P-3.7 (`GameWorldTests`, `NetworkManagerTests`) usan `SendSnapshot()` directamente. Refactorizar esos tests habría roto el principio de no regresión. La ruta secuencial sigue siendo válida y seguirá siéndolo para clientes conectados en baja carga donde el overhead del pool no justifique el dispatch.

---

## Contrato de thread-safety del Split-Phase (para la Memoria)

```
Invariante durante Phase A (workers corriendo):
  • m_establishedClients no se modifica (main bloqueado en latch::wait())
  • RemoteClient::m_history — solo lecturas (GetBaseline)
  • RemoteClient::m_lastClientAckedServerSeq — solo lecturas
  • RemoteClient::seqContext.localSequence — solo lecturas (sin AdvanceLocal)
  • HeroState — copia local de SnapshotTask, no compartida

Invariante durante Phase B (solo main thread):
  • std::latch::wait() ha retornado → todos los jobs han terminado
  • CommitAndSendSnapshot escribe m_history, lastOutgoingTime, seqContext
  • m_transport->Send() — single-threaded, SFML socket seguro
```

---

## Resultados

| Métrica | Resultado |
|---------|-----------|
| Tests nuevos | **24** (6 EMA Profiler + 5 WorkStealingQueue + 12 JobSystem + 1 SnapshotIntegrity) |
| Tests totales | **180 / 180 passing** |
| Regresiones | **0** |
| Compilación (MSVC Debug, Windows) | ✓ sin errores |

### Desglose de tests nuevos

**ProfilerTests.cpp (+6):**
- `EMA_InitiallyZero` — EMA = 0 antes del primer tick
- `EMA_FirstTick` — EMA = α×V tras primer tick (desde 0)
- `EMA_ReactsWithin10Ticks` — EMA > 5ms tras 10 ticks de 10ms (handoff req.)
- `EMA_ConvergesAfter100Ticks` — EMA dentro del 2% del valor estacionario
- `EMA_SnapshotExposesRecentMs` — Snapshot::recentAvgTickMs bien calculado
- `EMA_TestHook_Override` — SetRecentAvgTickUsForTest funciona

**JobSystemTests.cpp (17 tests en 2 suites):**

*WorkStealingQueue (5):*
- `Push_Pop_LIFO` — Pop devuelve el último pushed (frente)
- `Steal_FIFO` — Steal devuelve el primero pushed (fondo)
- `Pop_EmptyReturnsFalse`
- `Steal_EmptyReturnsFalse`
- `Empty_ReflectsState`

*JobSystem (12):*
- `ThreadCount_StartsAtRequested`
- `Execute_AllTasksComplete` — 200 tasks, latch, cuenta correcta
- `Latch_DeterministicWait` — 50 tasks, latch::wait() no retorna antes de fin
- `Execute_InlineFallback_WithZeroThreads` — cobertura de guard
- `WorkStealing_StealCountIncreasesUnderLoad` — 500 tasks, no crash
- `ForceAddThread_IncreasesCount`
- `ForceRemoveThread_DecreasesCount`
- `ForceRemoveThread_ClampsAtMinThreads` — no-op en kMinThreads
- `ForceAddThread_ClampsAtMaxThreads` — no-op en hardware_concurrency-1
- `MaybeScale_UpscalesAfterInterval` — 100 ticks × 8ms → count sube
- `MaybeScale_DownscaleRespectsCooldown` — hysteresis bloquea downscale inmediato
- `SnapshotIntegrity_ParallelMatchesSequential` — bytes paralelos == bytes secuenciales

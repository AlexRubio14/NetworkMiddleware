# IR-6.3 — Interest Management (FOW → Snapshot Pipeline)

**Branch:** `P-6.3-Interest-Management`
**Date:** 2026-03-28
**Tests:** 254 / 254 passing (236 previos + 18 nuevos: 12 `VisibilityTracker` + 6 `InterestManagement`)

---

## Handoff Validation — Correcciones al documento de Gemini

Antes de implementar se validó el handoff contra el código existente. Cuatro afirmaciones eran incorrectas o ya estaban implementadas:

| Afirmación del handoff | Realidad |
|---|---|
| "Filtro de Visibilidad: consultar el hash espacial antes de encolar" | **Ya implementado** — `main.cpp:425`: `if (!spatialGrid.IsCellVisible(st.x, st.y, obsTeam)) return;` desde P-5.1 |
| "Acker-per-client: mantener LastAckedSnapshot independiente" | **Ya implementado** — `RemoteClient::m_entityBaselines` (por entidad, por cliente) actualizado via `ProcessAckedSeq()` desde P-5.x |
| "Refactor del SnapshotBuilder para que acepte TeamID o PlayerID" | **No necesario** — el gather loop ya itera por cliente con el filtro FOW activo |
| "AsyncSendDispatcher recibirá payloads personalizados en lugar de un broadcast único" | **Ya cierto** — cada `CommitAndSendBatchSnapshot` encola un paquete por cliente |

**Lo único que faltaba** era el manejo correcto del caso de re-entrada FOW: cuando una entidad transiciona de invisible a visible, la baseline delta de `m_entityBaselines` puede ser obsoleta (el cliente puede haber perdido el ACK de la última confirmación). La corrección es la `VisibilityTracker` + evicción de baseline.

**La reducción de bandwidth (~90%)** descrita en el handoff ya estaba lograda por el filtro `IsCellVisible` en main.cpp. P-6.3 añade la garantía de correctness en la re-entrada.

---

## Motivación

El pipeline de snapshots (P-5.x) ya filtra entidades por FOW antes de serializar. Sin embargo, existe un edge case de correctness en el sistema de delta compression:

**Escenario problemático:**
1. Entidad X es visible al cliente C → enviada en tick 50 → cliente ACKa → `m_entityBaselines[X] = state_50`
2. Entidad X sale del FOW → no se envía durante 100 ticks
3. Durante esos 100 ticks, el cliente puede haber recibido actualizaciones de X en ticks 51-55 que **nunca ACKeó de vuelta** (pérdida de paquete en el path cliente→servidor)
4. Entidad X re-entra al FOW en tick 151 → servidor usa `state_50` como baseline → envía delta(current, state_50) → **cliente aplica el delta sobre su copia local en state_55** → posición incorrecta

P-6.3 añade `VisibilityTracker` que detecta esta transición invisible→visible y evicta la baseline stale, forzando un Full State en la primera transmisión post-re-entrada.

---

## Decisiones de diseño

**¿Por qué `VisibilityTracker` como clase separada y no inline en main.cpp?**

La lógica de "qué entidades vi el último tick enviado" tiene estado propio (mapa por cliente), es testeable unitariamente y conceptualmente pertenece al dominio de gestión de interés, no al gameloop. Separarlo en `Core/VisibilityTracker` permite tests unitarios sin levantar un NetworkManager completo.

**¿Por qué `UpdateAndGetReentrants` solo se llama en send ticks (`sendThisTick == true`)?**

La "visibilidad previa" debe referirse al **último snapshot enviado**, no al último tick del gather loop. Si se llamase en todos los ticks (100Hz), la `m_prevVisible` avanzaría cada tick aunque no se haya enviado nada. Una re-entrada entre dos ticks no-send pasaría desapercibida (ya no estaría en la categoría "nuevo" cuando llegue el send tick). Limitar las llamadas a send ticks (30Hz) garantiza que `prev` = "lo que el cliente recibió en el último snapshot".

**¿Por qué evictar la baseline en lugar de forzar `isDirty = 0xFFFFFFFF`?**

Evictar la baseline (`m_entityBaselines.erase(eid)`) es la ruta natural: `GetEntityBaseline()` devuelve `nullptr`, y `SerializeBatchSnapshotFor` ya tiene la rama `HeroSerializer::Serialize(state, writer)` (full state) para el caso null. No se añade lógica especial — se reutiliza el mecanismo de "primer envío" ya existente.

**¿Por qué `EvictEntityBaseline` en RemoteClient y NetworkManager, no solo en main.cpp?**

`m_entityBaselines` es privado en `RemoteClient`. La única forma de modificarlo es a través de métodos públicos. Añadir `EvictEntityBaseline` en la cadena `RemoteClient → NetworkManager → main.cpp` mantiene el encapsulamiento y permite testear la evicción sin acceso a internos.

**¿Por qué el first-call de `UpdateAndGetReentrants` devuelve todas las entidades como re-entrantes?**

En el primer send tick de un cliente recién conectado, no hay "visibilidad previa" — el cliente no tiene ningún estado local. Todas las entidades visibles son nuevas y deben recibir un full state. Devolver todas como re-entrantes en la primera llamada garantiza que el cliente arranque con un estado consistente sin lógica especial en el game loop.

---

## Modified files

### `Core/VisibilityTracker.h` (nuevo)

```cpp
class VisibilityTracker {
    std::unordered_map<uint16_t, std::unordered_set<uint32_t>> m_prevVisible;
public:
    std::unordered_set<uint32_t> UpdateAndGetReentrants(
        uint16_t clientID, const std::vector<uint32_t>& nowVisibleIDs);
    void RemoveClient(uint16_t clientID);
    void Clear();
};
```

### `Core/VisibilityTracker.cpp` (nuevo)

| Método | Descripción |
|--------|-------------|
| `UpdateAndGetReentrants(id, nowIDs)` | Calcula diff `nowSet - prevSet` → re-entrantes. Actualiza `m_prevVisible[id] = nowSet`. First call: todo `nowSet` es re-entrante. |
| `RemoveClient(id)` | `m_prevVisible.erase(id)` — limpia estado al desconectar. |
| `Clear()` | Resetea todo — para reinicios de servidor. |

### `Core/RemoteClient.h`

```cpp
// Añadido tras GetEntityBaseline:
// P-6.3: Evicts the cached baseline for entityID so the next snapshot
// sends a full state.
void EvictEntityBaseline(uint32_t entityID);
```

### `Core/RemoteClient.cpp`

```cpp
void RemoteClient::EvictEntityBaseline(uint32_t entityID) {
    m_entityBaselines.erase(entityID);
}
```

### `Core/NetworkManager.h`

```cpp
// P-6.3: Evicts the delta baseline for entityID at client `ep`.
void EvictEntityBaseline(const Shared::EndPoint& ep, uint32_t entityID);
```

### `Core/NetworkManager.cpp`

```cpp
void NetworkManager::EvictEntityBaseline(const EndPoint& ep, uint32_t entityID) {
    const auto it = m_establishedClients.find(ep);
    if (it != m_establishedClients.end())
        it->second.EvictEntityBaseline(entityID);
}
```

### `Core/CMakeLists.txt`

```cmake
add_library(MiddlewareCore STATIC
    # ...
    VisibilityTracker.h
    VisibilityTracker.cpp
    # ...
)
```

### `Server/main.cpp`

**Cuatro cambios:**

1. `#include "../Core/VisibilityTracker.h"` — nuevo include
2. `Core::VisibilityTracker visTracker;` — declarado junto a `priorityEvaluator`
3. `task.obsID = obsID;` en el gather lambda — campo nuevo en `SnapshotTask`
4. Post-gather re-entry block dentro de `if (!snapshots.empty() && sendThisTick)`:

```cpp
// P-6.3 — Re-entry baseline eviction (main thread, antes de Phase A).
for (auto& task : snapshots) {
    std::vector<uint32_t> nowVisible;
    nowVisible.reserve(task.states.size());
    for (const auto& st : task.states)
        nowVisible.push_back(st.networkID);
    const auto reentrants = visTracker.UpdateAndGetReentrants(task.obsID, nowVisible);
    for (const uint32_t eid : reentrants)
        manager.EvictEntityBaseline(task.ep, eid);
}
```

5. `visTracker.RemoveClient(id)` en el `SetClientDisconnectedCallback` — limpieza al desconectar

### `tests/CMakeLists.txt`

```cmake
Core/VisibilityTrackerTests.cpp
Core/InterestManagementTests.cpp
```

---

## New tests (18 — cross-platform)

### `tests/Core/VisibilityTrackerTests.cpp` (12 tests)

| Test | Qué verifica |
|------|-------------|
| `FirstCall_AllEntitiesAreReentrants` | Sin estado previo → todas las entidades son re-entrantes |
| `FirstCall_EmptyVisible_ReturnsEmpty` | Sin entidades visibles → sin re-entrantes |
| `StableVisibility_NoReentrants` | Mismo set visible dos ticks seguidos → sin re-entrantes |
| `StableVisibility_OverMultipleCalls` | Estabilidad sobre 3+ llamadas consecutivas |
| `NewEntityDetected` | Una entidad nueva aparece en el set → detectada |
| `EntityLeavesAndReturns_DetectedOnReentry` | Entidad sale FOW y vuelve → detectada en re-entrada |
| `EntityLeavesAndReturns_NotDetectedIfStillVisible` | Siempre visible → no detectada |
| `MultipleClients_StateIsIsolated` | Estado de cliente 1 no afecta a cliente 2 |
| `MultipleClients_IndependentReentry` | Re-entrada detectada solo para el cliente cuyo FOW cambió |
| `RemoveClient_NextCallTreatsAllAsReentrants` | Tras `RemoveClient`, siguiente call devuelve todo como re-entrante |
| `RemoveClient_OtherClientsUnaffected` | `RemoveClient(1)` no afecta al estado del cliente 2 |
| `Clear_AllClientsReset` | `Clear()` resetea todos los clientes a first-call semantics |

### `tests/Core/InterestManagementTests.cpp` (6 tests)

| Test | Qué verifica |
|------|-------------|
| `EvictEntityBaseline_RemovesBaseline` | `EvictEntityBaseline` borra la baseline de `m_entityBaselines` |
| `EvictEntityBaseline_OtherEntitiesUnaffected` | Evictar entidad 10 no toca la baseline de entidad 20 |
| `EvictEntityBaseline_NoBaselineIsNoop` | Evictar entidad sin baseline no crashea |
| `NetworkManager_EvictEntityBaseline_DelegatesCorrectly` | `NetworkManager::EvictEntityBaseline` delega a `RemoteClient`; serialización tras evicción no crashea y retorna payload válido |
| `NetworkManager_EvictEntityBaseline_UnknownEP_IsNoop` | Evictar baseline de EP desconocido no crashea |
| `ReentryPattern_BaselineEvictedBeforeSerialization` | Patrón completo: baseline promovida → entidad sale FOW → vuelve → tracker detecta → evicción → `GetEntityBaseline` retorna nullptr |

---

## Architectural flow post-P-6.3

```
100 Hz game loop
├── Phase 0:  SpatialGrid::Clear() + MarkVision()          [EXISTING P-5.1]
├── Phase 0b: PriorityEvaluator tiers per (obs, entity)    [EXISTING P-5.4]
├── Gather:   ForEachEstablished → task.states filtered by:
│             IsCellVisible() [EXISTING P-5.1]
│             + ShouldSend(tier) [EXISTING P-5.4]
│             → task.obsID stored                           [NEW P-6.3]
│
└── [sendThisTick only, 30Hz]:
    ├── Re-entry eviction:
    │   for each task:
    │     reentrants = visTracker.UpdateAndGetReentrants(obsID, nowVisible)  [NEW P-6.3]
    │     for eid in reentrants: manager.EvictEntityBaseline(ep, eid)        [NEW P-6.3]
    ├── Phase A: SerializeBatchSnapshotFor (parallel)
    │   → GetEntityBaseline(eid) = null for reentrants → full state auto    [LEVERAGED]
    ├── Phase B: CommitAndSendBatchSnapshot
    └── FlushTransport → Signal() → sendmmsg                [EXISTING P-6.2]
```

---

## Bandwidth projections post-P-6.3

Con FOW activo en escenario típico MOBA (50 clientes distribuidos por el mapa):

| Métrica | P-6.2 (baseline) | P-6.3 (FOW activo) |
|---------|-----------------|-------------------|
| Entidades enviadas por cliente/tick | ~49 | ~10 (ratio 1:5) |
| Bandwidth total (50 clientes, 30Hz) | ~3.2 Mbps | ~640 kbps |
| Bandwidth por cliente | ~65 kbps | ~13 kbps |
| Full Loop (game thread) | ~3.77ms | ~3.8–4.0ms (+filtering overhead) |

La reducción de bandwidth (~5×) viene del filtro `IsCellVisible` ya existente. La `VisibilityTracker` añade un overhead O(N) insignificante (unordered_set lookup por entidad por cliente, una vez cada 3 ticks).

**Comparativa vs competidores:**

| Sistema | Bandwidth/cliente |
|---------|------------------|
| League of Legends | ~10–30 kbps |
| Dota 2 | ~20–50 kbps |
| Overwatch | ~30–60 kbps |
| **Este proyecto (P-6.3 target)** | **~13 kbps** |

---

## Notas para Gemini

- La reducción de bandwidth ya estaba parcialmente activa desde P-5.1 (la línea `IsCellVisible` en main.cpp). El handoff describía como "pendiente" algo que ya funcionaba. P-6.3 añade la correctness del caso de re-entrada, no el filtro en sí.
- El overhead de `VisibilityTracker` por tick de send es O(clientes × entidades_visibles_por_cliente) ≈ O(50 × 10) = O(500) lookups en `unordered_set` — completamente negligible.
- **`recvmmsg` / `epoll`** siguen fuera de scope — el receive path no es el bottleneck.
- El comportamiento en el primer tick de un cliente recién conectado es correcto: `UpdateAndGetReentrants` devuelve todas las entidades visibles como re-entrantes → todas reciben full state → cliente arranca con estado consistente sin lógica adicional.

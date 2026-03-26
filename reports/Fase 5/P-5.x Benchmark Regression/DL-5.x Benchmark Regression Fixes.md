# DL-5.x — P-5.x Regression Benchmark & Fixes

**Date:** 2026-03-25
**Commit inicial (benchmark roto):** `cb62ad3`
**Commit final (fixes):** rama `develop` post-fixes
**Tests:** 223 → 230 (7 nuevos)

---

## El punto de partida

Al completar P-5.4 el sistema tenía cinco fases de features nuevas: FOW con spatial hashing, predicción Kalman, lag compensation con rewind buffer, y Network LOD con PriorityEvaluator. 223 tests pasando. El siguiente paso natural era correr el script de regresión `run_p5_regression.sh` — el mismo banco de pruebas que validó P-4.3 — para ver qué habían costado todas esas features en bandwidth y tick budget.

Los resultados del primer benchmark fueron alarmantes:

| Escenario | Full Loop | Delta Efficiency | Out kbps |
|-----------|-----------|-----------------|----------|
| A: Clean Lab (10 bots, 0ms, 0% loss) | 0.89ms | **15%** | 1760.6kbps |
| B: Real World (10 bots, 50ms, 5% loss) | 0.76ms | **0%** | 1480.5kbps |
| C: Stress/Zerg (50 bots, 100ms, 2% loss) | **39.50ms** | **0%** | 12239.1kbps |

Tres problemas distintos: un escenario que violaba el tick budget de 10ms (C), eficiencia de delta compresión rota en dos escenarios (B y C), y eficiencia muy por debajo de lo esperado incluso en el mejor caso (A: 15% en lugar de 60-80%).

La reacción inicial fue centrarse en el escenario C como el único crítico. Error de análisis: A y B también estaban rotos. La diferencia era de grado, no de tipo.

---

## Bug 1 — Baseline de delta incorrecto (escenarios A, B, C)

### Diagnóstico

La eficiencia del 15% en A y 0% en B/C no era coherente con ningún escenario de uso real:

- En A (red perfecta, 10 clientes, todos conectados) se esperaba ~60-80%.
- En B (5% loss, 50ms) bajar al 0% es posible si los ACKs no llegan, pero 5% de loss debería preservar la mayoría.
- En C la combinación con 39.5ms hacía imposible distinguir si la eficiencia era 0% por bugs o por la carga.

La clave fue leer el código de `SerializeSnapshot` después del benchmark:

```cpp
// Antes del fix — SerializeSnapshot (NetworkManager.cpp):
const Shared::Data::HeroState* baseline = nullptr;
if (client.m_lastClientAckedServerSeqValid)
    baseline = client.GetBaseline(client.m_lastClientAckedServerSeq);
```

`GetBaseline(seq)` busca en `m_history[seq % 64]`. `m_lastClientAckedServerSeq` es el último seq que el cliente confirmó mediante ACK (campo `header.ack` de los Input packets).

Este mecanismo era correcto en P-3.5, cuando había **una sola entidad por cliente por tick**. Un tick = un paquete = un seq = un `HeroState`. El ACK del cliente apuntaba exactamente al estado que necesitabas como baseline.

Con P-5.1+ el pipeline cambió a multi-entidad: por cada tick, el servidor enviaba **N paquetes por cliente**, uno por entidad visible. Cada llamada a `CommitAndSendSnapshot` consumía un seq diferente:

```
Tick T, cliente C, 10 entidades visibles:
  Entidad E0 → seq=100, RecordSnapshot(100, stateE0)
  Entidad E1 → seq=101, RecordSnapshot(101, stateE1)
  ...
  Entidad E9 → seq=109, RecordSnapshot(109, stateE9)
```

Cuando el cliente respondía con ACK=109, el servidor hacía `GetBaseline(109)` y obtenía `stateE9`. Luego usaba ese mismo `stateE9` como baseline para **todas** las entidades del siguiente tick. Delta de E0 se computaba contra el estado anterior de E9 — entidades completamente distintas. Resultado: todos los campos siempre distintos, todos los dirty bits siempre a 1, full sync en cada paquete.

Solo la última entidad del tick (E9) obtenía un baseline correcto. Con 10 entidades, ~1/10 paquetes se comprimían correctamente → eficiencia teórica ≈ 10-15%. Eso es exactamente el 15% que muestra el escenario A. Con pérdida de paquetes (B, C), incluso el ACK de E9 llega con retraso o se pierde → 0%.

**El bug llevaba en producción desde P-5.1. Pasó desapercibido porque los tests de P-3.5 validaban la lógica single-entity y ningún test de integración cubría el path multi-entidad end-to-end.**

### Fix

La solución correcta requería cambiar la granularidad del baseline de "por secuencia global" a "por entidad". El diseño elegido:

```
RemoteClient antes:
  SnapshotEntry { uint16_t seq, bool valid, HeroState state }
  array<SnapshotEntry, 64> m_history   // 1 estado por seq

RemoteClient después:
  BatchEntry { uint16_t seq, bool valid, vector<pair<entityID, HeroState>> entities }
  array<BatchEntry, 64>                m_history        // N estados por seq (un batch)
  unordered_map<uint32_t, HeroState>   m_entityBaselines // entityID → último estado confirmado
```

Tres métodos nuevos reemplazan los dos anteriores:

| Método viejo | Método nuevo | Semántica |
|---|---|---|
| `RecordSnapshot(seq, state)` | `RecordBatch(seq, entities)` | Guarda todos los estados del tick bajo un seq |
| `GetBaseline(seq)` | `GetEntityBaseline(entityID)` | Busca el último baseline confirmado por entidad |
| — | `ProcessAckedSeq(seq)` | Cuando llega un ACK, promueve el batch a `m_entityBaselines` |

En `ProcessAcks` se añadió el procesado del campo `ack_bits` (ventana de 32 seqs anteriores), no solo `header.ack`. Esto es crítico: con N entidades por tick, el ACK del cliente puede confirmar varios seqs anteriores de una vez. Sin procesar `ack_bits`, en escenarios de pérdida solo se actualizaba el baseline de la última entidad.

```cpp
// En ProcessAcks — NetworkManager.cpp:
client.ProcessAckedSeq(header.ack);
uint32_t bits = header.ack_bits;
for (int i = 0; i < 32 && bits; ++i, bits >>= 1)
    if (bits & 1u)
        client.ProcessAckedSeq(static_cast<uint16_t>(header.ack - 1 - i));
```

`SerializeSnapshot` cambió a:

```cpp
const HeroState* baseline = client.GetEntityBaseline(state.networkID);
```

Simple, sin la indirección `lastAckedSeq → history[seq] → state`. Cada entidad busca directamente su propio historial confirmado.

---

## Bug 2 — O(N×M) UDP sends por tick (escenario C: 39.5ms)

### Diagnóstico

Con el Fix 1 aplicado pero sin Fix 2, el escenario C seguía roto. La causa era diferente.

Con 46 clientes conectados y 46 entidades visibles (Zerg: todos en combate, todos en FOW), la fase de gather producía 46×46 = **2116 `SnapshotTask`** por tick. Phase B ejecutaba 2116 llamadas secuenciales a `CommitAndSendSnapshot`, cada una con un `SFML::UdpSocket::send()`.

En WSL2, cada `send()` involucra una transición host↔VM (la syscall cruza el boundary del hipervisor). El coste medido es ~15-18µs por send en loopback WSL2 — significativamente mayor que en Linux nativo (~1-3µs). Con 2116 sends:

```
2116 × 17µs ≈ 35.9ms  solo en Phase B
```

Eso explica el 39.5ms casi completamente. Phase A (serialización paralela con el JobSystem) era rápida; Phase B era el cuello de botella.

El job system de P-4.4 no ayudaba aquí: Phase A puede paralizar la serialización, pero Phase B es inherentemente secuencial (escribe en el historial del cliente y llama al transport, que no es thread-safe).

### Fix

Si el problema es el número de `send()`, la solución es reducirlo. Con multi-entidad, enviar un paquete por entidad es un artefacto del diseño single-entity original — no hay razón protocolar para no agrupar todas las entidades de un cliente en un único paquete.

El fix cambia la unidad del pipeline:

```
Antes: SnapshotTask { EndPoint ep, HeroState state, vector<uint8_t> buffer }
         → 1 paquete por (cliente, entidad)
         → N×M sends por tick

Después: SnapshotTask { EndPoint ep, vector<HeroState> states, vector<uint8_t> buffer }
         → 1 paquete por cliente
         → N sends por tick
```

**Nuevo wire format del paquete Snapshot:**

```
Antes:  [header:104b] [tickID:32b] [HeroState bits — delta o full] [CRC32:32b]
Después: [header:104b] [tickID:32b] [count:8b] [entity_0 bits] ... [entity_N-1 bits] [CRC32:32b]
```

`count` cabe en 8 bits (máximo 255 entidades por juego — asunción segura para el scope del TFG). `BotClient` solo lee `tickID:32` del payload de Snapshot y para (`break`), por lo que el `count` y las entidades restantes simplemente quedan sin leer — ningún cambio necesario en BotClient.

Los métodos nuevos en `NetworkManager`:

- `SerializeBatchSnapshotFor(ep, states[], tickID)` — Phase A (read-only, thread-safe)
- `CommitAndSendBatchSnapshot(ep, states[], buffer)` — Phase B (main thread, una sola llamada `Send()`)

El `latch` de Phase A pasa de `latch(N×M)` a `latch(N)`: un job por cliente, no por entidad. Esto además reduce el overhead de dispatch del JobSystem.

---

## Por qué ambos bugs coexistían sin detectarse

**Los tests de P-3.5 (`SnapshotHistory`) validaban la API single-entity:**

```cpp
client.RecordSnapshot(10u, state);
const HeroState* result = client.GetBaseline(10u); // correcto para 1 entidad
```

Este test pasa perfectamente con el sistema multi-entidad roto — porque testea exactamente el path que sigue siendo correcto cuando solo hay una entidad.

**Los tests de integración del Job System (`SnapshotIntegrity`)** validan que `SerializeSnapshotFor` es determinístico en paralelo, pero llaman al método con una entidad a la vez — no ejercitan el path donde el ACK del cliente apunta a la entidad equivocada.

**No había ningún test end-to-end** que:
1. Conectara un cliente
2. Le enviara snapshots de múltiples entidades
3. Simulara que el cliente hace ACK de la última
4. Verificara que la siguiente serialización de la primera entidad usa el baseline correcto

Esa es la laguna que los 7 tests nuevos cierran.

---

## Tests nuevos

Los tests de `SnapshotHistory` existentes fueron reemplazados por tests que reflejan la nueva semántica:

| Test | Lo que valida |
|------|---------------|
| `GetEntityBaseline_NullBeforeAck` | `RecordBatch` solo no expone el baseline — hace falta `ProcessAckedSeq` |
| `GetEntityBaseline_CorrectAfterAck` | Tras ACK el baseline correcto está disponible |
| `UnknownEntity_ReturnsNullptr` | Sin batch registrado → nullptr → full sync |
| `EntitiesHaveIndependentBaselines` | El bug central: E0 y E1 en el mismo batch → baselines independientes, no mezclados |
| `EvictedSeq_DoesNotUpdateBaseline` | Slot circular eviccionado → `ProcessAckedSeq(stale)` es no-op |
| `BatchSnapshot_ContainsTickIDAndCount` | Wire format: los primeros 40 bits = `tickID:32 + count:8` |
| `BatchSnapshot_DeltaBaselinePerEntity` | Sin ACK ambos envíos son full-sync (mismo tamaño) |

El test `EntitiesHaveIndependentBaselines` es el test de regresión directo del Bug 1. Si el bug vuelve a aparecer, ese test lo detecta inmediatamente.

---

## Resultados tras los fixes

| | Antes | Después | Mejora |
|---|---|---|---|
| **A — Full Loop** | 0.89ms | **0.24ms** | 3.7× |
| **B — Full Loop** | 0.76ms | **0.21ms** | 3.6× |
| **C — Full Loop** | 39.50ms | **1.62ms** | **24.4×** |
| **A — Delta Efficiency** | 15% | **69%** | +54pp |
| **B — Delta Efficiency** | 0% | **64%** | +64pp |
| **C — Delta Efficiency** | 0% | **74%** | +74pp |
| **A — Out kbps** | 1760.6 | **746.9** | 2.4× menos |
| **B — Out kbps** | 1480.5 | **362.2** | 4.1× menos |
| **Tests** | 223 | **230** | +7 |

**Escenario C bandwidth (12239 → 13936 kbps): el único número que sube, y es correcto.** Con el código roto, el servidor corría a ~25Hz efectivos (39.5ms/tick → ~25fps). El profiler medía bytes/segundo sobre un servidor a cuarta parte de velocidad. Con el fix, el servidor corre a 100Hz reales y entrega datos comprimidos de verdad. El aumento del 14% en kbps representa 4× más ticks por segundo con 74% de compresión activa — la red trabaja más y produce datos más útiles.

---

## Lección

La premisa del diseño de delta compression en P-3.5 era "una entidad por cliente por tick". P-5.1 rompió esa premisa al introducir el pipeline multi-entidad, pero el contrato de `RemoteClient::m_history` (1 `HeroState` por seq) no se actualizó. El sistema compilaba, los tests pasaban, y el protocolo era silenciosamente incorrecto.

**La invariante violada:**

> `GetBaseline(lastAckedSeq)` solo devuelve un baseline válido si hay una correspondencia 1-a-1 entre seq y entidad. Con N entidades por tick esa correspondencia no existe.

El fix no fue complicado una vez diagnosticado. La dificultad fue que el sistema fallaba de forma "razonable" — 15% de eficiencia en lugar de 0% hace que el sistema parezca funcionar con compresión moderada, cuando en realidad estaba haciendo full sync para 9/10 entidades y compresión aleatoria para la décima.

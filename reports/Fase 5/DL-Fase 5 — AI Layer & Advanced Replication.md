---
type: dev-log-alex
phase: Fase 5
date: 2026-03-25
status: personal
---

# DEV LOG — Fase 5: AI Layer, Advanced Replication & Regression Fixes

**Steps cubiertos:** P-5.1 + P-5.2 + P-5.3 + P-5.4 + P-5.x (regression fixes)
**Fecha:** 2026-03-25

---

## El problema de partida

Al terminar la Fase 4 el middleware tenía infraestructura sólida: 190 tests, benchmarks validados a 87 clientes, CRC32, Job System con Split-Phase. Tick budget 6% bajo carga real.

Pero el sistema de replicación era uniforme: todos los clientes recibían todos los héroes, a 100Hz, siempre. En un MOBA eso es incorrecto por tres razones distintas:

1. **Información perfecta.** El equipo contrario nunca debería ver tu posición en el mapa a menos que estés en su línea de visión. Replicar a todos sin FOW viola la mecánica de guerra de niebla del género.

2. **Bandwidth desperdiciado.** Un héroe al otro extremo del mapa que no tiene ninguna influencia en tu combate consume el mismo ancho de banda que el que te está atacando.

3. **Lag compensation inexistente.** El servidor aplicaba los inputs cuando llegaban. Con RTT = 100ms, el cliente dispara donde vio al enemigo, el servidor valida donde está ahora — 10 ticks después. Las habilidades fallaban por diseño.

Las cinco features de Fase 5 atacan estos tres problemas. Pero la historia de la fase no es solo añadir features: el benchmark de regresión al final descubrió dos bugs silenciosos que llevaban en producción desde P-5.1 y que corrompían la compresión delta del 100% de los clientes.

---

## Mapa de la Fase

```text
┌─────────────────────────────────────────────────────────────────────────┐
│  P-5.x  Regression Fixes                                                │
│  Bug 1: per-entity delta baselines · Bug 2: batch packets               │
│  39.5ms → 1.62ms · 0% → 74% efficiency · 7 tests nuevos                │
├─────────────────────────────────────────────────────────────────────────┤
│  P-5.4  Network LOD                                                     │
│  Brain::PriorityEvaluator · Tier 0/1/2 · interest formula              │
│  ComputeInCombat O(N²) · Phase 0b en main loop                         │
├─────────────────────────────────────────────────────────────────────────┤
│  P-5.3  Server-Side Lag Compensation                                    │
│  InputPayload 24→40 bits · GameWorld rewind buffer (32 slots)          │
│  HitValidator::CheckHit · clientTickID echo desde Snapshot             │
├─────────────────────────────────────────────────────────────────────────┤
│  P-5.2  Silent Kalman Prediction                                        │
│  Brain::KalmanPredictor · 4-state CV model · predict+update cycle      │
│  Sin cambio en wire format · "silent" = sin efectos en red             │
├─────────────────────────────────────────────────────────────────────────┤
│  P-5.1  Spatial Hashing & Fog of War                                   │
│  SpatialGrid 20×20 · bitset<400>[2] team visibility · MarkVision       │
│  Multi-entity snapshot pipeline · round-robin teamID                   │
└─────────────────────────────────────────────────────────────────────────┘
            ↑
     Infrastructure (Fase 4: 190 tests, Job System, CRC32)
```

---

## P-5.1 — Spatial Hashing & Fog of War

### La decisión de diseño central: spatial hash vs. k-d tree

El problema es: dado un punto (x, y) con radio R, ¿qué celdas de visión cubre? Las dos opciones clásicas son:

- **k-d tree:** O(log N + k) para range queries. Óptimo para N grande y distribución irregular. Complejo de implementar y actualizar.
- **Spatial hash (grid uniforme):** O(1) insert + O(R²/cell_size²) range query. Simple. Óptimo cuando el mapa tiene distribución uniforme — exactamente el caso de un MOBA.

Con MAP = 1000×1000 unidades y un grid de 20×20 celdas (50u/celda), `VISION_CELL_RADIUS = 4` celdas = radio de visión de 200u. La query de visibilidad es un bucle de (2×4+1)² = 81 celdas máximo — O(1) constante pequeña.

La visibilidad se almacena en `std::bitset<400>[2]`: 400 celdas (20×20), 2 equipos. `bitset` usa 50 bytes por equipo — cabe en una línea de caché. `IsCellVisible(x, y, team)` es un `test()` en O(1).

### El bug del primer test de equipo: datos sin inicializar

El primer test de FOW fallaba de forma no determinista. La causa: `SpatialGrid::Clear()` solo hacía `reset()` en los bitsets, pero el constructor por defecto de `SpatialGrid` no garantizaba que los bitsets estuvieran inicializados a cero antes de la primera llamada a `Clear()`. En algunos entornos de test (stack frames reutilizados) los bitsets contenían basura.

Fix: inicialización explícita en el constructor: `m_visibility[0].reset(); m_visibility[1].reset();`.

Lección: `std::bitset` default-construye a 0 según el estándar, pero dependencias de inicialización implícita son frágiles. La inicialización explícita documenta la intención.

### El pipeline multi-entidad: la semilla del bug de Fase 5

P-5.1 fue el primer paso donde el servidor pasó de enviar 1 snapshot por cliente a N snapshots por cliente por tick. El gather loop:

```cpp
gameWorld.ForEachHero([&](uint32_t eid, const HeroState& st) {
    if (!spatialGrid.IsCellVisible(st.x, st.y, obsTeam)) return;
    snapshots.push_back({ep, st, {}});  // 1 SnapshotTask por (cliente, entidad)
});
```

Esto funcionaba correctamente para la transmisión. Pero `CommitAndSendSnapshot` seguía registrando cada entidad en `m_history` bajo su propio seq — sin cambiar la lógica de selección de baseline, que asumía que el último seq confirmado por el cliente correspondía a la entidad correcta.

El sistema de delta compression empezó a estar silenciosamente roto en P-5.1. No había ningún test que lo detectara.

---

## P-5.2 — Silent Kalman Prediction

### "Silent" como decisión de diseño

La predicción de movimiento puede implementarse en dos lugares: en el servidor (predice el input que falta) o en el cliente (interpola la posición que falta). P-5.2 elige el servidor porque el servidor es autoritativo.

"Silent" significa que la predicción no cambia ningún mensaje en la red. `BotClient` no sabe que el servidor está prediciendo sus movimientos — el protocolo es idéntico. Cuando el input real llega, el servidor corrige la posición con un "update" del filtro Kalman. El cliente observa el resultado (nueva posición en el siguiente snapshot) sin recibir nunca una notificación explícita de "hubo predicción".

Esto contrasta con la predicción client-side (P-3.7 intent) donde el cliente predice localmente y reconcilia cuando llega el estado autoritativo. La versión servidor-side de P-5.2 es más simple: no hay reconciliación porque el servidor siempre tiene la posición correcta.

### El modelo CV y por qué no usar CA

El modelo elegido es **Constant Velocity (CV)**: estado `[x, y, vx, vy]`, transición `x' = x + vx·dt`.

La alternativa es **Constant Acceleration (CA)** con estado `[x, y, vx, vy, ax, ay]`. CA modela mejor cambios de dirección... pero los MOBAs no tienen aceleración suave. Un héroe cambia de dirección instantáneamente cuando el jugador hace click en un punto nuevo. CA añadiría complejidad sin beneficio para esta dinámica discreta.

La constante `Q_vel = 5.0` (ruido de proceso para velocidad) fue el único parámetro que requirió tuning. Demasiado bajo → el filtro reacciona lento a cambios de dirección. Demasiado alto → el filtro ignora su propia estimación y amplifica el ruido. 5.0 fue validado contra los test cases de tracking lineal y cambio de dirección brusco.

### La separación Brain/Core

`KalmanPredictor` vive en `Brain/` (capa de IA), pero `main.cpp` en `Server/` necesita acceder a él. El tipo de retorno (`PredictedInput`) no puede incluir headers de `Brain` desde `Core` (violaría el DAG de dependencias del CMake).

La solución: `PredictedInput` es un struct simple en `Brain/` — no extiende `InputPayload` de `Shared/`. `main.cpp` construye un `InputPayload` a partir del `PredictedInput` manualmente. Sin dependencia cruzada.

---

## P-5.3 — Server-Side Lag Compensation

### El bug del clientTickID: un reloj equivocado

El primer handoff de Gemini para P-5.3 proponía estampar un contador local del cliente en `clientTickID` — `m_localTick++` en `BotClient`. CodeRabbit lo marcó correctamente: ese contador empieza en 0 y crece independientemente del reloj del servidor.

En un servidor que lleva 10,000 ticks running, `clientTickID` llegaría como 150 (el cliente lleva 1.5s corriendo) y el delta `uint16_t(tickID) - clientTickID` sería `10000 - 150 = 9850`, que supera `kMaxRewindTicks = 20`. El servidor siempre caería al máximo rewind, haciendo la lag compensation efectivamente inútil.

El fix: el cliente no necesita mantener ningún reloj. El servidor ya le manda `tickID:32` al inicio de cada snapshot. `BotClient` almacena ese valor en `m_lastServerTick` y lo escribe en `clientTickID` al enviar el siguiente input. El delta resultante es exactamente el RTT en ticks — siempre dentro del rango útil mientras el RTT sea < 200ms (20 ticks × 10ms/tick).

### El rewind buffer: alias de slot

`GameWorld::m_rewindHistory` usa un array circular de 32 slots por entidad, indexado por `tickID % 32`. Al guardar el estado, se escribe `entry.tickID = tickID` además del estado. Al leer, se verifica que `entry.tickID == tickID` antes de devolver el puntero.

Sin esa verificación, después de 32 ticks el slot `k % 32` contiene el estado del tick `k+32`, no el del tick `k`. Si el servidor intenta rewind al tick `k` (ya eviccionado), leerá el estado futuro equivocado. La verificación hace que `GetStateAtTick` devuelva `nullptr` en ese caso, y el servidor usa la posición actual como fallback — correcto y seguro.

---

## P-5.4 — Network LOD

### El bug O(N³): PriorityEvaluator llamado mal

El handoff original describía `Evaluate(obsID, ...)` como una llamada que internamente computaba `inCombat[]` para todos los targets. Si eso se llamaba N veces (una por observador), el coste total era:

```
N observers × O(N²) inCombat check por observer = O(N³)
```

Con 50 observadores y 50 entidades: 50 × 2500 = 125,000 operaciones por tick a 100Hz = 12.5M operaciones/segundo. Medible como overhead en el benchmark.

El fix: pre-computar `inCombat[]` **una sola vez** antes del bucle de observadores, con `ComputeInCombat(allTargets)`. Luego cada llamada a `Evaluate(obsID, ...)` recibe `inCombatFlags` como parámetro y solo hace el O(N) de interés por entidad:

```
1 × O(N²) ComputeInCombat + N observers × O(N) Evaluate = O(N²)
```

50 observadores: 2500 + 50×50 = 5000 operaciones. La mitad del O(N³) para N=50, y la diferencia crece exponencialmente con N.

Este bug no tenía test que lo detectara directamente — era un problema de complejidad asintótica, no de correctitud. La forma de detectarlo hubiera sido un benchmark de Phase 0b aislado midiendo el tiempo con N=10, 20, 50 y verificando que no escalaba como N³.

### La variable observerTeam que no hacía nada

El handoff original incluía `observerTeam` como parámetro de `Evaluate`. CodeRabbit observó que nunca se usaba: la detección de combate compara `entities[i].teamID` con `entities[j].teamID` — relaciones entre entidades, no entre observador y entidades. El observador no es parte del cálculo de `inCombat`.

Parámetros no usados en interfaces públicas son una deuda: confunden al lector y crean la ilusión de que hay un comportamiento por equipo del observador cuando no lo hay. Se eliminó en la segunda iteración.

---

## P-5.x — El benchmark que descubrió todo

### Por qué el primer benchmark fue engañoso en una dirección

El escenario A mostraba **15% de eficiencia**. No es intuitivamente obvio que ese número sea el resultado de un bug. 15% significa "el middleware envía un 85% menos que full sync" — suena bien, aunque esté muy por debajo del objetivo. Si el benchmark hubiera mostrado 0% en todos los escenarios, el bug habría sido obvio. Mostrar 15% en el mejor caso creó la ilusión de que la compresión funcionaba con moderación.

El escenario C con 39.5ms era el único número claramente roto. La primera lectura fue "B y A están bien, C tiene un problema de escala". Esa lectura era incorrecta — B y A también estaban rotos, pero la rotura era más sutil.

### Bug 1: la invariante rota silenciosamente desde P-5.1

El sistema de delta compression en P-3.5 se diseñó con una premisa implícita: **hay exactamente una entidad por cliente por tick**. La historia de snapshots (`m_history`) tenía 1 slot por seq, y `lastAckedSeq` apuntaba exactamente al estado que importaba.

P-5.1 rompió esa premisa sin tocar `RemoteClient`. Con N entidades por cliente, N seqs diferentes se consumían por tick. El cliente confirmaba el último (N-1) con su ACK, y el servidor usaba ese baseline para todas. Solo la última entidad comprimía correctamente. Las otras N-1 hacían full sync.

La raíz profunda: **la premisa implícita no estaba documentada ni testeada**. Los tests de P-3.5 validaban single-entity porque el sistema era single-entity en P-3.5. Al añadir multi-entity en P-5.1, los tests no se actualizaron para cubrir el nuevo invariante.

### Bug 2: 2116 syscalls por tick en WSL2

Con 46 clientes × 46 entidades visibles = 2116 paquetes por tick. En Linux nativo cada `send()` cuesta ~1-3µs. En WSL2, la syscall cruza el boundary del hipervisor → ~15-18µs. 2116 × 17µs = 35.9ms solo en Phase B.

El Job System de P-4.4 no podía ayudar: Phase B es secuencial por diseño (escribe en el historial del cliente y llama al transport). Paralelizar Phase B introduciría race conditions sobre `RemoteClient::m_history`.

La solución no era "hacer Phase B más rápida" sino "hacer que Phase B hiciera menos trabajo": batch de todas las entidades de un cliente en un solo paquete.

### El fix que también arregló el bandwidth

Un efecto colateral del batching: el overhead de cabecera se amortiza. Con 10 entidades y un header de 13 bytes + CRC de 4 bytes:

```
Antes (10 paquetes): 10 × (13 + 4) = 170 bytes de overhead por cliente por tick
Después (1 paquete): 1 × (13 + 4)  = 17 bytes de overhead por cliente por tick
```

Reducción del 90% en overhead de protocolo. Combinado con el Fix 1 (delta compression funcionando), la reducción total de bandwidth en escenario A fue del 58% (1760 → 747 kbps).

---

## El escenario C en perspectiva

El escenario Stress/Zerg merece análisis separado porque su número de bandwidth subió (12239 → 13936 kbps, +14%) mientras todo lo demás mejoró.

Con el código roto, el servidor corría a ~25Hz efectivos en C (39.5ms/tick). El profiler reportaba bytes/segundo sobre un servidor lento. El número 12239 kbps era el resultado de 25 ticks por segundo con full-sync.

Con el fix, el servidor corre a 100Hz. Cuatro veces más ticks por segundo, con 74% de compresión real. El cálculo:

```
25Hz × 2116 paquetes × ~35 bytes/paquete × 8 bits = 14,812 kbps  (aprox. los 12239 kbps)
100Hz × 46 paquetes × ~30 bytes/paquete × 8 bits = 11,040 kbps
+ overhead control (heartbeats, ACKs, etc.) → ~13936 kbps
```

El sistema trabaja más (100Hz real vs 25Hz roto) y entrega datos correctos con compresión. El ligero aumento de bandwidth es el coste de hacer el trabajo correcto.

---

## Estado del sistema al final de la Fase 5

- **230/230 tests passing** (Windows/MSVC)
- Fog of War con Spatial Hash — visibilidad O(1) por tick
- Kalman Prediction — movimiento suave sin cambio de protocolo
- Lag Compensation — rewind buffer 32 slots / 320ms, validación de hits
- Network LOD — Tier 0/1/2 por (observador, entidad), Phase 0b O(N²)
- Batch snapshots — 1 paquete por cliente por tick, wire format `[tickID:32][count:8][entities...]`
- Per-entity delta baselines — `m_entityBaselines` confirmados por ACK vía `ProcessAckedSeq`

**Benchmarks validados (WSL2, Release, 2026-03-25):**

| Escenario                          | Full Loop | Budget    | Delta Eff. | Out kbps    | Conectados |
| ---------------------------------- | --------- | --------- | ---------- | ----------- | ---------- |
| A: Clean Lab (10 bots, 0ms/0%)     | 0.24ms    | **2.4%**  | **69%**    | 746.9kbps   | 10/10      |
| B: Real World (10 bots, 50ms/5%)   | 0.21ms    | **2.1%**  | **64%**    | 362.2kbps   | 7/10       |
| C: Stress/Zerg (50 bots, 100ms/2%) | 1.62ms    | **16.2%** | **74%**    | 13936.2kbps | 46/50      |

El tick budget más alto es 16.2% en el escenario más exigente (50 bots, red degradada, Zerg). Margen amplio para Fase 6.

---

## Análisis de los resultados del benchmark

### El problema de comparabilidad con P-4.3

La primera lectura del benchmark genera una reacción instintiva: "el middleware ha empeorado — antes tenía 99% de eficiencia, ahora tiene 74%". Esa lectura es incorrecta porque compara sistemas en condiciones de carga radicalmente distintas.

**P-4.3** fue un test de infraestructura pura. Los bots se conectaban pero no recibían estado de juego real: no había `gameWorld` activo, las posiciones no cambiaban, las dirty masks eran siempre 0 entre ticks. El delta compression veía secuencias idénticas de HeroState → eficiencia vacua del 99% (*"nada cambia, nada hay que enviar"*). La salida de 1 kbps no era bandwidth eficiente — era bandwidth casi inexistente.

**P-5.x** corre con un mundo vivo: 50 héroes moviéndose a 100Hz, visibilidad por equipo calculada cada tick, lag compensation grabando 32 estados históricos por entidad, LOD priorizando en tiempo real. Es el primer benchmark que mide el middleware haciendo su trabajo real.

La analogía correcta para la Memoria del TFG: el camión de P-4.3 viajaba vacío y reportaba 99% de eficiencia de combustible. El camión de P-5.x lleva 100 toneladas de carga y sigue operando al 16% de su presupuesto de CPU. El segundo número es el que importa.

### Lo que validan los tres escenarios

**Escenario A (Clean Lab, 10 bots, red perfecta):** 69% de eficiencia con héroes moviéndose activamente. En condiciones reales de MOBA — donde posición y HP cambian cada tick — encontrar el 69% de datos redundantes confirma que el diseño de dirty bits y ZigZag+VLE funciona correctamente.

**Escenario B (Real World, 10 bots, 50ms/5% loss):** La pérdida de paquetes reduce la eficiencia a 64% — el servidor no puede delta-comprimir cuando el cliente no puede confirmar los ACKs. Es el comportamiento esperado: sin baseline confirmado, full sync. La caída de solo 5 puntos respecto al escenario limpio indica que el sistema de ACK window (`ack_bits` 32-seq) recupera la mayoría de las confirmaciones incluso con 5% de pérdida.

**Escenario C (Stress/Zerg, 50 bots, 100ms/2% loss):** 74% de eficiencia supera al escenario A porque la escena Zerg concentra a todos los héroes — más entidades visibles por cliente significa más entidades que rara vez cambian de estado (las que están fuera del foco de combate) y que el delta comprime bien. El LOD (Tier 1/2) suprime ticks enteros para entidades lejanas, reduciendo el volumen bruto.

### El número que no escala mal: 302 kbps por cliente

```
13,936 kbps / 46 clientes = 302 kbps/cliente
```

Una conexión doméstica de fibra tiene 50–100 Mbps de upstream. 302 kbps representa el 0.6% de esa capacidad. Para comparar: Counter-Strike 2 consume ~128 kbps en modo competitivo; Valorant ~300 kbps; League of Legends ~60–100 kbps (tick rate más bajo). El middleware está en rango competitivo en el peor escenario de carga, a 100Hz real — cinco veces el tick rate de la mayoría de los juegos comerciales.

### El 84% de margen que no se ve en los números

El Full Loop de 1.62ms en Scenario C descompuesto:

```
Phase 0b (PriorityEvaluator O(N²)):  ~0.3ms estimado
Phase A  (serialización paralela):   ~0.6ms estimado
Phase B  (46 sends seriales):        ~0.5ms estimado (46 × ~11µs en WSL2 post-fix)
Overhead (RecordTick, Kalman, etc.): ~0.2ms estimado
─────────────────────────────────────────────────────
Total:                                 1.62ms
Budget:                               10.00ms
Margen:                                8.38ms (83.8%)
```

Esos 8.38ms son el espacio donde vive Fase 6: físicas, habilidades complejas, más entidades, interpolación. El diseño modular permite añadir capas sin presión sobre el tick budget.

---

## Conceptos nuevos en esta fase

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **Spatial Hash** | Grid uniforme con hash O(1) por celda | Range queries O(constante pequeña) para distribuciones uniformes |
| **bitset<N>** | Array de bits compacto, operaciones por bloque | 400 celdas de visibilidad = 50 bytes; `test()` en O(1) |
| **Fog of War** | Visibilidad por equipo basada en posiciones | Información correcta en MOBA; culling de bandwidth |
| **Kalman Filter** | Estimador óptimo bayesiano para sistemas lineales | Predicción de movimiento con incertidumbre cuantificada |
| **Modelo CV** | Constant Velocity: estado [x,y,vx,vy] | Apropiado para MOBA con cambios de dirección discretos |
| **Lag Compensation** | Rewind del servidor al tick de fuego del cliente | Hits justos bajo cualquier latencia ≤ kMaxRewindTicks |
| **Rewind Buffer** | Array circular de estados históricos | O(1) escritura y lectura; autoeviction tras kRewindSlots ticks |
| **Network LOD** | Frecuencia de replicación proporcional a relevancia | Bandwidth reducido sin degradar gameplay |
| **Interest Formula** | `(base + combat_bonus) / dist` | Captura importancia táctica con una sola expresión |
| **Per-entity baseline** | `m_entityBaselines[entityID]` → último estado ACKed | Corrige la asunción 1-a-1 de P-3.5 que P-5.1 rompió |
| **Batch snapshot** | N entidades en un solo paquete UDP | Reduce sends de O(N×M) a O(N); crítico en WSL2 |
| **ProcessAckedSeq + ack_bits** | Ventana de 32 seqs anteriores procesada en cada ACK | Garantiza baselines actualizados aunque lleguen varios ACKs juntos |
| **Invariante implícita** | Premisa de diseño no documentada ni testeada | La causa raíz del Bug 1; detectable solo con tests end-to-end multi-entidad |

---

## Lo que haría diferente

**Documentar invariantes de protocolo como contratos en el código.** La premisa "1 entidad por seq" de P-3.5 debería haber sido un comentario explícito en `RemoteClient::m_history`. Cuando P-5.1 añadió multi-entidad, ese comentario habría sido un flag inmediato.

**Test end-to-end de delta efficiency tras cada cambio en el pipeline de snapshots.** Un test que enviara snapshots multi-entidad, simulara ACKs, y verificara que el segundo envío es más pequeño que el primero habría detectado el Bug 1 en P-5.1, no seis semanas después en el benchmark de regresión.

**Benchmark de regresión como parte del CI, no como paso manual.** El benchmark reveló bugs reales. Si hubiera corrido automáticamente tras el PR de P-5.1, los bugs habrían sido detectados antes de acumularse con P-5.2, P-5.3 y P-5.4.

**Analizar el baseline antes de declarar "más lento".** La comparativa inicial entre P-4.3 y P-5.x mezclaba dos sistemas incomparables: P-4.3 con mundo vacío (99% efficiency vacua) vs P-5.x con mundo real (heroes moviéndose). El primer análisis se dejó llevar por los números sin cuestionar si la baseline era justa. La eficiencia real de P-4.3 bajo carga de movimiento era 0% (Gauntlet P-4.5 lo confirmó). Con esa corrección, P-5.x con 64-74% efficiency es el sistema funcionando mejor de lo que nunca lo hizo.

---

## Siguiente paso (Fase 6)

Fase 5 cierra los problemas fundamentales de replicación: qué se envía (FOW), cuándo se envía (LOD), cómo se envía eficientemente (batching + delta correcto), y cómo se valida (lag comp).

Fase 6 tiene dos candidatos naturales: interpolación y extrapolación en el cliente (el Visual Debugger Unreal que consume los snapshots), o protocolo de daño y estado de juego (health mutation, muerte, respawn). La decisión de prioridad depende de qué es más relevante para la Memoria del TFG — mostrar el sistema completo funcionando visualmente, o demostrar que el protocolo sustenta mecánicas de gameplay reales.

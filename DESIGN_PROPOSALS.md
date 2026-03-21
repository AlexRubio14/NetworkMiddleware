# Propuestas de Diseño Pendientes
## Middleware de Red MOBA-IA

**Documento:** Propuestas identificadas durante la investigación — NO son decisiones tomadas.
Cada propuesta debe ser **discutida y decidida** antes de implementarla.

---

## Cómo usar este documento

Antes de iniciar cualquier fase de implementación, revisar las propuestas marcadas con esa fase.
Si hay propuestas pendientes de decisión para esa fase → **diseñar antes de codificar**.

---

## FASE 3 — Propuestas pendientes de decisión

### [P-3.1] ACK Bitmask en el Header

**Problema identificado:** El diseño actual del header (Sequence, Ack, Type) usa ACK por paquete individual. Valorant y la mayoría de sistemas modernos usan un ACK bitmask.

**Propuesta:** Añadir un campo `AckBits` (uint32) al header que confirme los 32 paquetes anteriores de una vez.

```
[ Sequence (16b) | Ack (16b) | AckBits (32b) | Type (8b) | Timestamp (32b) ]
```

**Ventaja:** El baseline avanza mucho más rápido — en un solo paquete confirmas 32 estados anteriores.
**Coste:** 4 bytes extra en el header de cada paquete.
**Referente:** Valorant, documentado en Fiedler (2016).

**Preguntas a decidir:**
- ¿Añadimos AckBits o mantenemos ACK simple para empezar?
- ¿Añadimos Timestamp al header ahora (necesario para interpolación en Fase 6) o lo dejamos para Fase 6?

---

### [P-3.2] Client-Side Prediction — Soporte en el servidor

**Problema identificado:** El diseño actual asume que el servidor recibe estados del cliente. Sin client-side prediction, 50ms de ping = 50ms de input delay en el click-to-move del MOBA.

**Propuesta:** El servidor acepta `InputCommands` con timestamp en lugar de estados. El cliente aplica su propio input inmediatamente (sin esperar al servidor) y reconcilia cuando llega el estado autoritario.

**Qué necesita el servidor:**
- Estructura `InputCommand { sequence_id, timestamp, input_type, input_data }`
- Cola de inputs por `RemoteClient` procesada en orden
- Respuesta con `sequence_id` del input que se confirma, para que el cliente pueda reconciliar

**Qué necesita el cliente Unreal (Fase 6):**
- `RingBuffer<PredictedState>` indexado por sequence_id
- Lógica de reconciliación suave (< N frames) cuando hay divergencia

**Preguntas a decidir:**
- ¿Implementamos client-side prediction o asumimos que el servidor envía el estado y el cliente espera?
- Si sí, ¿qué inputs predice el cliente? (propuesta: solo movimiento propio; daño recibido y estados = siempre del servidor)
- ¿Cuál es el threshold de divergencia aceptable antes de aplicar corrección?

---

### [P-3.3] Jitter Buffer — Parámetros concretos

**Problema identificado:** El diseño de Clock Synchronization (3.4) menciona RTT pero no especifica el jitter buffer.

**Propuesta:** El cliente bufferiza 2-3 ticks antes de renderizar. El mundo se ve ligeramente en el pasado (~20-30ms) pero el movimiento es perfectamente fluido.

**Tradeoff a decidir:**
- Buffer pequeño → posible stutter si los paquetes llegan irregulares
- Buffer grande → latencia percibida mayor

**Preguntas a decidir:**
- ¿Cuántos ticks de buffer? (propuesta: 2 ticks = 20ms a 100Hz como punto de partida, ajustar en Fase 4.3)
- ¿Extrapolación si el buffer se vacía, o congelamos el último estado?

---

### [P-3.4] Canales de Reliability — Tercer canal

**Problema identificado:** El diseño actual especifica Reliable Ordered y Unreliable. Falta un tercer caso.

**Propuesta:** Añadir `Reliable Unordered` para eventos que deben llegar pero no importa el orden.

| Canal | Contenido | Comportamiento |
|-------|-----------|---------------|
| Unreliable | Posición, movimiento | Si se pierde, la IA Predictiva compensa |
| Reliable Ordered | Compra items, habilidades, cambio de nivel | Retransmisión hasta ACK, en orden estricto |
| Reliable Unordered | Muertes de entidades, mensajes de chat | Retransmisión hasta ACK, cualquier orden |

**Preguntas a decidir:**
- ¿Implementamos el tercer canal desde el principio o empezamos con dos y añadimos el tercero después?

---

### [P-3.5] Server-side Input Validation (Anti-Cheat base)

**Problema identificado:** El roadmap actual no incluye validación de inputs en el servidor más allá del Handshake.

**Propuesta:** Validaciones por tick:
- `delta_posición ≤ max_speed × tick_duration` → detección de speed hacks
- Cast de habilidad requiere verificación de línea de visión server-side
- Sequence numbers previenen replay de paquetes válidos anteriores

**Preguntas a decidir:**
- ¿En qué fase implementamos esto? ¿Junto con Fase 3 o separado en Fase 5?
- ¿Qué hacemos cuando detectamos una violación? ¿Kick inmediato o logging + corrección silenciosa?

---

## FASE 4 — Propuestas pendientes de decisión

### [P-4.1] Adaptive Tick-Rate bajo Congestión

**Problema identificado:** El roadmap asume 100Hz constante. Si el tick budget (10ms) se supera consistentemente, el servidor acumula deuda y el tick-rate efectivo baja de forma caótica.

**Propuesta:** Sistema controlado de reducción de tick-rate:
- Si `cpu_time_last_tick > 0.8 × 10ms` durante N ticks consecutivos → reducir a 80Hz temporalmente
- Si persiste → reducir a 60Hz, priorizar solo héroes (Tier 0)
- Recovery automático cuando la carga baja

**Preguntas a decidir:**
- ¿Implementamos esto o asumimos que el hardware del servidor siempre aguanta 100Hz?
- ¿En qué fase entra? (propuesta: Fase 4, junto con el profiler que medirá si es necesario)

---

### [P-4.2] IA de Thread Pool — Modelo de decisión concreto

**Problema identificado:** El roadmap dice "el Brain escala jthreads según la carga" pero no especifica el algoritmo.

**Propuesta de algoritmo (threshold-based controller):**

Métricas de entrada:
- `packets_per_tick`: paquetes procesados en el último tick
- `entities_in_combat_zone`: entidades a distancia < X entre sí
- `cpu_time_last_tick_ms`: tiempo real del último tick
- `thread_count_current`: jthreads activos

Lógica de control:
```
si cpu_time > 80% del budget Y entities_in_combat > TEAMFIGHT_THRESHOLD:
    escalar +2 jthreads
sino si cpu_time < 30% del budget Y threads > MIN_THREADS:
    reducir -1 jthread
```

Constantes a calibrar en Fase 4.3: `TEAMFIGHT_THRESHOLD`, `MIN_THREADS`, `MAX_THREADS`.

**Preguntas a decidir:**
- ¿Threshold-based (simple, justificable académicamente) o algo más sofisticado?
- ¿Cuál es la definición de "teamfight" en términos de métricas medibles?

---

## FASE 5 — Propuestas pendientes de decisión

### [P-5.1] Spatial Hashing — Por equipo vs por jugador

**Problema identificado:** El roadmap dice "rejilla espacial para filtrar qué entidades ve cada jugador" pero no especifica si la visibilidad se calcula por jugador o por equipo.

**Propuesta:** Por equipo (como LoL), no por jugador individual.
- Computacionalmente mucho más barato: 2 cálculos por tick (un equipo vs otro) en lugar de 10 (uno por jugador)
- En MOBA la visión es compartida por equipo — si un aliado ve al enemigo, tú también lo ves

**Preguntas a decidir:**
- ¿Visibilidad por equipo o por jugador?
- ¿Tamaño de las celdas del grid? (depende del tamaño del mapa y el rango de visión estándar del MOBA)

---

### [P-5.2] IA Predictiva — Modelo concreto

**Problema identificado:** El roadmap dice "modelo de regresión" sin especificar cuál.

**Opciones analizadas:**

| Modelo | Precisión | Coste CPU | Justificación académica |
|--------|-----------|-----------|------------------------|
| Regresión lineal | Baja (no funciona para click-to-move con cambios de dirección) | Mínimo | Insuficiente para MOBA |
| **Filtro de Kalman** | Alta para movimiento con ruido gaussiano | Bajo-medio | Kalman (1960) — referencia canónica |
| LSTM / Red Recurrente | Muy alta | Alto (inaceptable para tiempo real) | Excesivo para TFG |

**Propuesta:** Filtro de Kalman — académicamente citable, implementable en C++, coste computacional bajo.

**Preguntas a decidir:**
- ¿Kalman filter o empezamos con algo más simple (velocidad × tiempo) y mejoramos?
- ¿Cuándo se activa la predicción? ¿Solo ante packet loss o siempre como suavizado?

---

### [P-5.3] Entity Tier Replication (crítica para escala)

**Problema identificado:** El roadmap actual replica todas las entidades por igual en cada tick. Con 100 minions a 100Hz esto es inviable en términos de ancho de banda.

**Propuesta (inspirada en LoL):**

| Tier | Entidades | Frecuencia | Justificación |
|------|-----------|------------|---------------|
| 0 | Héroes, habilidades activas | Full tick-rate + delta compression completa | Máxima precisión necesaria |
| 1 | Waves de minions | Corrección periódica cada 5 ticks (50ms) | Cliente simula localmente, servidor corrige |
| 2 | Torres, objetivos | Event-driven (solo en cambio de estado) | No necesitan actualización periódica |

**Variante avanzada:** Promoción dinámica de Tier — si un minion está siendo atacado por un héroe, sube temporalmente a Tier 0 hasta que el combate termina.

**Preguntas a decidir:**
- ¿Implementamos tiering o empezamos replicando todo igual y optimizamos después?
- ¿La simulación local de Tier 1 en el cliente (Unreal) entra dentro del alcance del TFG?
- ¿Cada cuántos ticks la corrección de Tier 1? (propuesta: 5 ticks = 50ms)

---

### [P-5.4] Server-side Input Validation avanzada

**Problema identificado:** Si se implementa P-3.5 (validación básica en Fase 3), en Fase 5 se puede extender.

**Propuesta:** Detección de patrones de input humanamente imposibles:
- Reacciones < 100ms a eventos visuales (posible script de auto-dodge)
- Varianza de input timing estadísticamente perfecta (posible bot)

**Preguntas a decidir:**
- ¿Esta complejidad entra dentro del alcance del TFG o es línea futura?

---

## FASE 6 — Propuestas pendientes de decisión

### [P-6.1] Entity Interpolation en el cliente Unreal

**Problema identificado:** Sin interpolación, el movimiento de entidades en Unreal aparece a tirones (un salto por tick de red, no continuo).

**Propuesta:** El cliente bufferiza 2-3 ticks y renderiza entre los dos últimos estados confirmados. Usa los timestamps del header (si se decide añadirlos en P-3.1) para calcular la posición exacta entre ticks.

**Tradeoff:** +20-30ms de "smoothing lag" percibido a cambio de movimiento perfectamente fluido.

**Preguntas a decidir:**
- ¿Implementamos interpolación en el cliente Unreal o lo dejamos como línea futura?
- Depende de si se añade Timestamp al header (P-3.1) — las dos propuestas están vinculadas.

---

### [P-6.2] Reconciliation Visual — Debug overlay

**Propuesta:** Overlay en Unreal que muestre visualmente la diferencia entre predicción local (P-3.2) y el estado autoritario del servidor.

**Preguntas a decidir:**
- Solo si se implementa Client-Side Prediction (P-3.2). Sin esa propuesta, este overlay no aplica.

---

## Propuestas Transversales (sin fase asignada aún)

### [P-T.1] Justificación del modelo Snapshot vs Lockstep

**Problema:** La decisión de usar Snapshot-based en lugar de Lockstep/Semi-determinístico no está documentada en ningún sitio. La comisión puede preguntarlo.

**Para incluir en la Memoria (Capítulo de Disseny de l'Arquitectura):**

| Modelo | Por qué no |
|--------|-----------|
| Lockstep | Requiere toda la lógica de juego determinística → incompatible con engine-agnostic. Un packet loss = todos esperan. |
| Semi-determinístico (LoL) | Óptimo para MOBA pero requiere código determinístico. Fuera del alcance del TFG. |
| **Snapshot (elegido)** | El servidor es la única fuente de verdad. Compatible con engine-agnostic y con la arquitectura propuesta. |

**Acción pendiente:** Añadir esta justificación al capítulo de arquitectura de la Memoria.

---

### [P-T.2] Bibliografía académica pendiente de añadir

Las siguientes referencias están identificadas como importantes pero aún no aparecen en la Memoria:

| Referencia | Por qué añadirla |
|-----------|-----------------|
| **Bernier, Y. W. (2001).** Latency Compensating Methods... GDC 2001. | LA referencia académica para lag compensation. Más citable que blogs. |
| **Fiedler, G. (2015).** Snapshot Interpolation. Gaffer on Games. | El sistema exactamente implementado |
| **Fiedler, G. (2015).** Client Side Prediction. Gaffer on Games. | Si se decide P-3.2 |
| **Fiedler, G. (2016).** State Synchronization. Gaffer on Games. | Complementa Reliability over UDP |
| **Kalman, R. E. (1960).** A New Approach to Linear Filtering... | Si se decide P-5.2 con Kalman |
| **Valve Corporation.** Source Multiplayer Networking. Valve Developer Wiki. | Documenta el sistema de Baselines de Dota 2 |
| **ISO/IEC 60559:2020 (IEEE 754)** | Justifica compatibilidad binaria de floats Linux↔Windows |

**Acción pendiente:** Añadir las referencias relevantes a la Bibliografía de la Memoria según las decisiones que se tomen.

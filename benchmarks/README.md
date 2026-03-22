# NetworkMiddleware — Benchmark Reference

## Cómo ejecutar

```bash
# En WSL2, desde la raíz del repo:
bash scripts/run_stress_test.sh               # build completo + 3 escenarios
bash scripts/run_stress_test.sh --skip-build  # reutiliza el último build
```

Los resultados se guardan automáticamente en `benchmarks/results/` con
el formato `YYYY-MM-DD_HHhMM_<git-hash>.md`.

---

## Definición de métricas

### Avg Tick (ms)
Tiempo que tarda `NetworkManager::Update()` en completar un tick del servidor.
Incluye: drenar el socket buffer UDP (while-drain), procesar todos los paquetes
recibidos, reenviar paquetes fiables pendientes y ejecutar la lógica de sesión
(heartbeats, zombies).

**Presupuesto:** 10ms a 100 Hz. Un valor de 0.11ms significa que el servidor
consume el 1.1% del presupuesto disponible.

---

### Budget %
`avg_tick_ms / 10ms × 100`

El indicador más importante para el TFG. Cuantifica cuánto del presupuesto de
tick está usando el middleware, dejando el resto disponible para lógica de juego
(AI, física, serialización de snapshots, spatial hashing).

**Referencia:** con 47 clientes bajo red degradada (100ms/2% loss) el middleware
usa el **1.1%** del presupuesto.

---

### Out (kbps)
Ancho de banda de salida promedio (servidor → clientes) desde el inicio del
profiler. Calculado como `totalBytesSent × 8 / elapsedSeconds / 1000`.

Incluye: paquetes de handshake, heartbeats, reenvíos de paquetes fiables, y
cualquier snapshot/dato de juego enviado desde el servidor.

En los benchmarks actuales el valor es bajo (1-5 kbps) porque el servidor no
tiene lógica de juego que genere snapshots de posición. En producción (con
serialización de HeroState a 60 Hz) el valor esperado sería ~10-50 kbps.

---

### In (kbps)
Ancho de banda de entrada promedio (clientes → servidor). Calculado igual que
Out pero con `totalBytesReceived`.

En los benchmarks, cada bot envía un `InputPayload` de 24 bits + header de 104
bits = 16 bytes a 60 Hz. Con 47 bots: `47 × 16 × 60 × 8 / 1000 ≈ 36 kbps`.
El valor observado (~28 kbps) es menor porque algunos ticks se pierden bajo
condiciones de red degradada.

---

### Retrans.
Número total de reenvíos de paquetes fiables (canal `Reliable` y
`ReliableUnordered`) desde el inicio del profiler.

Los inputs de los bots usan el canal `Unreliable` (fire and forget), así que
este contador solo sube si el servidor envía paquetes fiables a los clientes
(ej: ConnectionAccepted, ítems comprados, confirmaciones de nivel). En los
benchmarks actuales siempre es 0 porque no hay lógica de juego que use el
canal fiable.

---

### Delta Efficiency %
`1 - (avg_bytes_sent_per_tick / kFullSyncBytesPerClient × connected_clients)`

Compara el ancho de banda real enviado por tick con lo que costaría enviar un
full sync completo a todos los clientes. Un 99% significa que el servidor envía
el 1% de lo que un sistema sin delta compression enviaría.

**Nota:** en los benchmarks actuales el servidor solo envía heartbeats (sin
snapshots de estado del héroe), por eso la eficiencia es siempre 99% — no hay
casi nada que enviar. En producción, con snapshots de HeroState activos, la
eficiencia esperada es 60-80% dependiendo del movimiento de los jugadores.

---

### Connected / Launched
`Clientes_establecidos / Bots_lanzados`

Clientes que completaron el handshake de 3 mensajes vs los que se intentaron
conectar. La diferencia son bots que fallaron el handshake por packet loss.

Con 5% de pérdida y handshake de 3 mensajes: P(éxito) = `0.95^3 ≈ 85.7%`.
Con 2% de pérdida: P(éxito) = `0.98^3 ≈ 94.1%`. Los valores observados son
consistentes con estas probabilidades.

---

### Lost
`Launched - Connected`

Bots que no completaron el handshake. Comportamiento esperado y correcto: el
servidor no tiene información de clientes que nunca completaron el protocolo de
autenticación. Sin reconexión automática en el bot (fuera del scope del benchmark).

---

## Escenarios

| Escenario | Bots | Delay | Loss | Objetivo |
|-----------|------|-------|------|---------|
| **A: Clean Lab** | 10 | 0ms | 0% | Baseline — mide el coste base sin degradación |
| **B: Real World** | 10 | 50ms | 5% | Simula red europea a servidor NA (≈50ms RTT) |
| **C: Stress/Zerg** | 50 | 100ms | 2% | Carga máxima — ¿escala el middleware? |

tc netem se aplica en la interfaz loopback (`lo`) y afecta a todo el tráfico
entre servidor y bots (ambas direcciones).

---

## Cómo interpretar una regresión

Cuando se implementa una nueva feature (ej: spatial hashing, Kalman), hay que
volver a ejecutar el benchmark y comparar:

1. **Avg Tick / Budget%** — ¿ha subido el tick time? ¿cuánto presupuesto consume
   la nueva feature?
2. **Out (kbps)** — si la feature envía más datos (snapshots más frecuentes,
   mensajes nuevos), el Out subirá. ¿Es proporcional al valor añadido?
3. **Delta Efficiency** — si la feature activa serialización de HeroState, la
   eficiencia debería bajar de 99% a 60-80%. Si sube (más compresión), mejor.
4. **Retrans.** — si aparecen retransmisiones donde antes había 0, puede indicar
   que la nueva feature está sobrecargando el canal fiable.

Los archivos en `benchmarks/results/` permiten comparar cualquier run anterior
con el estado actual del código.

---

## Comparativa con competidores (para la Memoria)

| Sistema | Tick budget (50 clientes) | Notas |
|---------|--------------------------|-------|
| **NetworkMiddleware** | **1.1%** (0.11ms / 10ms) | P-4.3, WSL2, 47 clientes bajo 100ms/2% |
| Photon Bolt | No publicado | SaaS — sin acceso a métricas internas |
| Mirror (Unity) | ~3-8% (estimado) | Tick a 60Hz, overhead de Unity |
| UE Replication | ~5-15% (estimado) | Overhead del subsistema de replicación |

*Los valores de competidores son estimaciones basadas en benchmarks públicos de
la comunidad — ver bibliografía de la Memoria para referencias exactas.*

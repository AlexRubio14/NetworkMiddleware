---
type: dev-log-alex
phase: Fase 6
date: 2026-03-28
status: personal
---

# DEV LOG — Fase 6: Server I/O Optimization

**Steps cubiertos:** P-6.1 + P-6.2 + P-6.3
**Fecha:** 2026-03-28

---

## El problema de partida

Al terminar la Fase 5 el middleware tenía todo lo que se necesita conceptualmente para un servidor MOBA de producción: FOW con spatial hashing, predicción Kalman, lag compensation, Network LOD con tiers de prioridad, delta compression per-entity. El pipeline era correcto. Los benchmarks de P-5.x mostraban 74% de delta efficiency.

Entonces llegó el benchmark WAN real — Azure Switzerland North, 49 bots en chaos mode:

```
Game Logic (Kalman + GameWorld):   3.77ms   (34%)
Network I/O (49 × send()):         7.20ms   (66%)  ← cuello de botella
────────────────────────────────────────────────
Full Loop:                        10.97ms  (110% del budget)
```

El tick budget es 10ms (100Hz). El sistema lo violaba por el 10%. Y la causa no era la lógica de juego — era el transporte. Cuarenta y nueve llamadas a `send()`, una por cliente, cada una cruzando la frontera usuario-kernel. En condiciones ideales eso son microsegundos. En WAN real con chaos mode, cada syscall puede tardar ~147µs. Multiplicado por 49: **7.2ms**.

La lógica del juego costaba 3.77ms. El problema era exclusivamente de I/O.

Fase 6 ataca ese problema en tres capas: eliminar las syscalls redundantes, mover la syscall restante fuera del hilo principal, y asegurar que el pipeline de datos que alimenta esas syscalls sea correcto en todos los casos de borde.

---

## Mapa de la Fase

```text
┌─────────────────────────────────────────────────────────────────────────┐
│  P-6.3  Interest Management — FOW re-entry correctness                  │
│  VisibilityTracker · EvictEntityBaseline · re-entry full state          │
│  18 tests nuevos · 254/254 passing · ~13 kbps/client target            │
├─────────────────────────────────────────────────────────────────────────┤
│  P-6.2  Async Send Thread                                               │
│  AsyncSendDispatcher · jthread + CV · swap-buffer thread-safe          │
│  Signal() ≈ 0µs en game loop · sendmmsg off critical path              │
├─────────────────────────────────────────────────────────────────────────┤
│  P-6.1  RawUDP Transport                                                │
│  ITransport::Flush() · sendmmsg · 49 syscalls → 1 · SO_SNDBUF 4MB     │
│  5 tests nuevos (Linux only) · TransportFactory::NATIVE_LINUX          │
└─────────────────────────────────────────────────────────────────────────┘
            ↑
     Full Loop 10.97ms (Fase 5 WAN benchmark, 110% budget)
```

---

## P-6.1 — RawUDP Transport: eliminar las syscalls redundantes

### La decisión de interfaz

El primer problema no era de implementación sino de diseño de interfaz. `ITransport::Send()` tiene firma single-message por diseño — y ese contrato funciona perfectamente para SFML, que puede enviar paquetes individualmente sin coste adicional. Pero `sendmmsg` necesita N mensajes acumulados antes de hacer la syscall. Había dos opciones:

**SendBatch(vector<{buffer, EndPoint}>):** Cambiar la firma de `Send`. Rompe `SFMLTransport`, `MockTransport`, y todos los call-sites en `NetworkManager`. Invasivo.

**Flush() como no-op por defecto:** `Send()` acumula; `Flush()` despacha. Un único `virtual void Flush() {}` en `ITransport` — ningún call-site existente cambia. `SFMLTransport` y `MockTransport` envían en `Send()` y no sobreescriben `Flush()`. `RawUDPTransport` sobreescribe `Flush()` con `sendmmsg`.

La segunda opción respeta el principio open/closed: se extiende sin modificar. El coste es que `Send()` tiene semántica distinta según el transport (inmediata vs. diferida), pero eso es exactamente lo que `ITransport` abstrae.

### La invariante POSIX que el handoff no respetó

El esqueleto de Gemini para `RawUDPTransport` contenía cuatro bugs. Todos tenían el mismo patrón raíz: confusión entre la API C++ (métodos de clase) y la API POSIX (syscalls) cuando comparten nombre.

El más peligroso: `Close()` dentro de la implementación llamaba a `Close()` — al método de clase, no a `::close(fd)` del kernel. Recursión infinita. Stack overflow al apagar el servidor. No visible en compilación, no visible en tests superficiales donde nadie llama explícitamente a `Close()`. Solo aparece al destruir el objeto o recibir una señal de shutdown.

Desde P-6.1 existe una invariante de código permanente en este proyecto: en código que mezcla métodos de clase y syscalls con el mismo nombre (`Close`/`::close`, `Send`/`::send`), siempre usar `::` para las syscalls.

### El cache de sockaddr_in

`EndPoint.address` es `uint32_t` en host byte order. Para `sendmmsg`, cada `mmsghdr` necesita un `sockaddr_in` construido con `htonl`. Con 49 clientes, eso son 49 construcciones por tick que son idénticas al tick anterior. `m_addrCache` (unordered_map<uint32_t, sockaddr_in>) construye la estructura la primera vez y la reutiliza para siempre.

El detalle: la clave es `ep.address`, no `{address, port}`. Correcto para el modelo actual (un `EndPoint` por sesión, puerto fijo tras el handshake). Si en el futuro los clientes cambian de puerto durante la sesión, la clave debe incluir el puerto.

---

## P-6.2 — Async Send Thread: mover la syscall fuera del tick

### La tensión que resuelve P-6.2

P-6.1 redujo 49 syscalls a 1. Pero esa 1 syscall seguía ejecutándose en el hilo del game loop, en `FlushTransport()` tras Phase B. Bajo condiciones normales `sendmmsg` es rápido. Bajo WAN real con congestión transitoria, el kernel puede bloquearse un tiempo arbitrario esperando que el buffer de envío drene. Ese bloqueo ocurre en el hilo que tiene que completar cada ciclo en menos de 10ms.

La solución correcta es separar completamente I/O de red y lógica de juego: el game loop señaliza que hay trabajo y retorna inmediatamente; un hilo dedicado se ocupa de enviar.

### El diseño del dispatcher

La primitiva natural es `std::jthread` dormido en una `condition_variable`. `jthread` aporta RAII: el destructor solicita stop y hace join automáticamente — el hilo de red nunca queda huérfano. `std::stop_token` se integra en el predicado de `wait_for`, lo que elimina flags manuales de shutdown.

El detalle de diseño no obvio: el lock de la CV se suelta *antes* de llamar a `Flush()`. Si se mantuviera durante `sendmmsg`, `Signal()` desde el game loop bloquearía esperando ese mismo mutex — reintroduciendo exactamente el tipo de bloqueo que queríamos eliminar. La secuencia correcta: lock → leer y limpiar flag → unlock → `Flush()`.

El timeout de `kFlushInterval` (33ms) no es el path normal. Es un safety net para el caso teórico en que el game loop se detiene y no llega ninguna señal. En producción, cada tick de 30Hz genera un `Signal()` que despierta al thread en microsegundos.

### La race condition que Gemini detectó

La validación del IR de P-6.1 identificó un riesgo real: si `sendmmsg` tarda más de 10ms bajo congestión severa, el game loop del siguiente tick puede llamar a `Send()` empujando a `m_sendQueue` mientras el send thread todavía está en `Flush()` leyendo esa misma cola. Con un vector sin protección, eso es undefined behavior.

La solución es el patrón **swap-buffer**: dos colas en lugar de una.

```
m_sendQueue   — propiedad del productor (game loop).  Send() hace push_back aquí.
m_flushQueue  — propiedad del consumidor (send thread). sendmmsg opera aquí.
m_queueMutex  — protege exclusivamente el swap O(1).
```

`std::swap(m_sendQueue, m_flushQueue)` intercambia los punteros internos del vector — O(1). Después, el mutex se suelta y `sendmmsg` opera sobre `m_flushQueue` sin ningún lock. El game loop nunca toca `m_flushQueue`.

`m_flushQueue.clear()` sin `shrink_to_fit()` retiene la capacidad del vector (49 entradas). El vector crece una sola vez — en el primer tick con N clientes — y desde ahí opera sin heap allocation en el path crítico.

### El diseño de la coalescing implícita

`m_signaled` es un `bool`, no un contador. Si el game loop llama `Signal()` diez veces mientras el send thread está procesando el `Flush()` anterior, todas esas señales se colapsan en un solo `true`. El thread procesará un único `Flush()` que enviará todos los paquetes acumulados durante ese tiempo. Esto es correcto: lo que importa no es cuántas veces se señalizó, sino que el próximo `Flush()` envíe todo lo que hay en cola.

---

## P-6.3 — Interest Management: correctness en la re-entrada FOW

### El descubrimiento de la validación

El handoff de Gemini para P-6.3 describía cuatro cosas como "pendientes de implementar": el filtro FOW en el gather loop, el acker-per-client, el refactor del snapshot builder, y la configuración del dispatcher para payloads heterogéneos. Al validar contra el código existente, las cuatro ya estaban implementadas:

- El filtro `IsCellVisible` en `main.cpp:425` existe desde P-5.1
- `m_entityBaselines` per-client, per-entity existe desde P-5.x
- El gather loop ya itera por cliente generando tasks individuales
- El dispatcher ya recibe payloads distintos por cliente (uno por `CommitAndSendBatchSnapshot`)

La reducción de bandwidth que el handoff describía como objetivo de la fase (~90% de reducción) ya estaba lograda por el filtro existente. P-6.3, correctamente entendido, era añadir la garantía de correctness para el único edge case que faltaba.

### El edge case de la re-entrada

El sistema de delta compression garantiza que cada cliente recibe deltas computados desde su último estado confirmado (`m_entityBaselines`). Funciona así:

1. Entidad X visible al cliente C → enviada en tick 50 → cliente ACKa → `m_entityBaselines[X] = state_50`
2. Entidad X sale del FOW → no se envía durante 100 ticks
3. En esos 100 ticks, el servidor puede haber enviado actualizaciones de X antes de que saliera del FOW (ticks 51-55) que el cliente recibió pero cuyo ACK se perdió en el path cliente→servidor
4. `m_entityBaselines[X]` sigue en `state_50`. El cliente tiene `state_55`.
5. Entidad X re-entra al FOW → servidor envía delta(current, state_50) → cliente aplica sobre `state_55` → posición incorrecta.

El fix es `VisibilityTracker`: detectar la transición invisible→visible y evictar la baseline stale antes de Phase A. `GetEntityBaseline()` devuelve `nullptr` → `HeroSerializer::Serialize()` (full state). El cliente recibe el estado completo y su copia local es correcta.

### La semántica de UpdateAndGetReentrants

`UpdateAndGetReentrants` solo se llama en send ticks (30Hz), no en cada tick del gather loop (100Hz). La razón: "visibilidad previa" debe referirse al *último snapshot enviado*, no al último tick procesado. Si se llamase en todos los ticks, una entidad que entra y sale del FOW entre dos ticks no-send pasaría desapercibida cuando llegue el send tick — ya no estaría en la categoría "nuevo" aunque el cliente no la haya recibido en ningún snapshot.

El primer call para un cliente recién conectado devuelve todas las entidades visibles como re-entrantes. No hay "estado previo" — el cliente no tiene ninguna copia local. Todas las entidades del primer snapshot deben ser full state. Este comportamiento emerge naturalmente de la inicialización: si `clientID` no está en `m_prevVisible`, el diff `nowSet - prevSet` es `nowSet` completo.

### Por qué la evicción y no una flag especial

`EvictEntityBaseline` es `m_entityBaselines.erase(eid)` — una línea. `GetEntityBaseline()` ya maneja el caso null devolviendo `nullptr`, y `SerializeBatchSnapshotFor` ya tiene la rama de full state para ese caso. No hay código nuevo en el path de serialización. El mecanismo de "primer envío a un cliente nuevo" se reutiliza exactamente para "primer envío post-re-entrada".

---

## La narrativa completa de la fase

Los tres pasos de Fase 6 no son features independientes — son capas del mismo problema.

**P-6.1** ataca la *multiplicidad*: 49 syscalls son 49 puntos de fricción con el kernel. `sendmmsg` colapsa ese coste en 1. El ahorro es proporcional a N: a más clientes, mayor beneficio.

**P-6.2** ataca la *posición*: la 1 syscall restante sigue bloqueando el tick. Moverla a un hilo dedicado separa I/O de lógica de juego. El Full Loop pasa de 10.97ms a ~3.77ms — no porque la red sea más rápida, sino porque la red ya no compite con el tick por el tiempo del procesador.

**P-6.3** ataca la *corrección*: con las dos optimizaciones anteriores, el pipeline envía exactamente lo que cada cliente necesita ver (FOW filter ya existente desde P-5.1) pero sin garantizar que el primer paquete tras una re-entrada sea siempre correcto. `VisibilityTracker` cierra ese gap.

La secuencia importa. Hacer P-6.2 sin P-6.1 habría movido 49 syscalls a un hilo de fondo — correcto en latencia del game loop, pero el overhead del kernel se mantiene. Hacer P-6.3 sin P-6.2 no habría tenido sentido — el pipeline aún tenía I/O en el tick. La combinación de las tres capas produce el sistema óptimo.

---

## Resultados finales

| Métrica | Pre-Fase 6 (P-5.x WAN) | Post-Fase 6 (target) |
|---------|------------------------|---------------------|
| Full Loop (49 bots, WAN) | 10.97ms | ~3.8ms (lógica pura) |
| Syscalls de envío por tick | 49 | 1 |
| I/O en el game loop | 7.20ms | ~0µs (Signal ≈ nanosegundos) |
| Bandwidth/cliente | ~200 kbps (worst case) | ~13 kbps (FOW activo) |
| Bandwidth total (50 clientes) | ~9.8 Mbps | ~640 kbps |
| Correctness en re-entrada FOW | Sin garantía | Full state garantizado |
| Tests | 231 | 254 |

**Comparativa vs competidores (bandwidth/cliente):**

| Sistema | Bandwidth/cliente |
|---------|-----------------|
| League of Legends | ~10–30 kbps |
| Dota 2 | ~20–50 kbps |
| Overwatch | ~30–60 kbps |
| Valorant | ~40–80 kbps |
| **Este proyecto (P-6.3 target)** | **~13 kbps** |

---

## Lecciones de la fase

**La diferenciación C++/POSIX es crítica en código de transporte.** El bug de `Close()` en el handoff de P-6.1 — una llamada recursiva al método de clase en lugar de `::close()` — es el tipo de error que no produce error de compilación, es invisible en tests normales, y en producción habría colapsado el servidor al apagarse. La regla `::` para syscalls es ahora permanente.

**Los handoffs son puntos de partida, no contratos.** P-6.3 es el caso más extremo: las cuatro "features" que el handoff listaba como pendientes estaban ya implementadas. Sin validación previa contra el código, se habrían "reimplementado" cosas existentes o refactorizado código que no necesitaba cambiar. La validación del handoff antes de implementar no es overhead — es la diferencia entre trabajo útil y trabajo redundante.

**Las optimizaciones de I/O son multiplicativas, no aditivas.** El efecto combinado de P-6.1+6.2 no es `ahorro_6.1 + ahorro_6.2` sino que ambas son necesarias para llegar al resultado. sendmmsg sin async dispatch tiene I/O en el tick. Async dispatch sin sendmmsg tiene N syscalls en el thread de fondo. Solo la combinación produce 1 syscall fuera del tick.

**El edge case de re-entrada FOW es real, no teórico.** En un partido de MOBA, los héroes se mueven constantemente cruzando los límites de visibilidad: jungla vs líneas, teamfights que se inician y terminan, rotaciones. La frecuencia de entradas/salidas FOW en un partido real es alta. Sin `VisibilityTracker`, los artefactos de posición en re-entrada habrían sido perceptibles.

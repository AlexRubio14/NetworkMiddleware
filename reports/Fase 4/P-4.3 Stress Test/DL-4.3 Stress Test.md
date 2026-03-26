---
type: dev-log-alex
proposal: P-4.3
date: 2026-03-22
status: personal
---

# DEV LOG — P-4.3 Stress Test & Benchmarking

**Propuesta:** P-4.3 Stress Test & Benchmarking
**Fecha:** 2026-03-22

---

## ¿Qué problema resolvíamos?

Tras P-4.1+4.2 teníamos infraestructura de carga: HeadlessBot que ejecuta el protocolo completo a 60 Hz, Docker para escalar N bots. Pero solo sabíamos "los bots se conectan" — sin ninguna medición real:

- ¿Cuánto tiempo tarda cada tick del servidor con 10, 50, 100 bots?
- ¿Cuánto ancho de banda consume? ¿Escala linealmente o sublinealmente?
- ¿Qué pasa bajo condiciones de red degradada (pérdida, latencia)?
- ¿El tick rate de 100 Hz se mantiene o hay saturación?

Sin estas métricas, el TFG solo tiene afirmaciones teóricas. Con ellas, hay datos que permiten comparar con Photon, Mirror y UE Replication en la Memoria.

---

## ¿Qué hemos construido?

| Componente | Dónde | Qué hace |
|-----------|-------|---------|
| **NetworkProfiler** | `Core/NetworkProfiler.h/.cpp` | Telemetría thread-safe con `std::atomic<>`, reporte cada 5s |
| **while-drain** | `Core/NetworkManager.cpp` | Drain completo del buffer UDP en un tick (if → while) |
| **kMaxClients 100** | `Core/NetworkManager.h` | Permite Escenario C con 48-100 bots |
| **Servidor 100Hz** | `Server/main.cpp` | Game loop de producción reemplaza el demo visual de Fase 3 |
| **Chaos bot** | `HeadlessBot/main.cpp` | Dirección cambia cada 0.5s — estrena el snapshot buffer |
| **Benchmark script** | `scripts/run_benchmarks.sh` | Automatiza tc netem + escenarios + colección de logs |
| **11 tests nuevos** | `ProfilerTests.cpp` + `NetworkManagerTests.cpp` | Cobertura del profiler y del while-drain |

---

## NetworkProfiler — telemetría thread-safe desde el principio

El profiler usa `std::atomic<uint64_t>` para todos sus contadores. La pregunta obvia es: ¿por qué no usar `int` simple y proteger con mutex en `MaybeReport`?

Respuesta: P-4.4 (thread pool) va a introducir múltiples threads para el dispatch de callbacks. Si los contadores fueran enteros normales, habría data races desde el primer día de P-4.4. El coste de `std::atomic` con `memory_order_relaxed` es **cero en x86** — se compila a las mismas instrucciones MOV/XADD que el int no-atomic. No es optimización prematura, es corrección anticipada.

```cpp
void RecordBytesSent(size_t bytes) noexcept {
    m_bytesSent.fetch_add(bytes, std::memory_order_relaxed);
}
```

`MaybeReport` calcula la eficiencia de delta comparando bytes enviados reales vs el teórico de full sync:

```text
DeltaEfficiency = max(0, 1 - (avgBytesSentPerTick / kFullSyncBytesPerClient))
```

Donde `kFullSyncBytesPerClient = 19` (149 bits / 8 redondeado hacia arriba). Si el servidor envía en promedio 0.19 bytes/tick/cliente, la eficiencia es 99%.

---

## while-drain — el fix más importante del step

Este es el cambio más crítico de P-4.3, aunque parezca trivial:

```cpp
// ANTES (incorrecto bajo carga):
if (m_transport->Receive(buffer, sender)) {
    // procesa 1 paquete por tick
}

// DESPUÉS (correcto):
while (m_transport->Receive(buffer, sender)) {
    // procesa TODOS los paquetes pendientes en 1 tick
}
```

Con N bots a 60 Hz y el servidor a 100 Hz, en cada tick del servidor llegan ~0.6 paquetes por bot. Con 10 bots: 6 paquetes/tick. Con 50 bots: 30 paquetes/tick.

Con el `if` original: el servidor procesa exactamente 1 paquete por tick. Los demás quedan en el buffer del OS. En el siguiente tick, procesa 1 más. El buffer crece indefinidamente → latencia creciente → eventualmente pérdida de paquetes por desbordamiento del socket buffer del kernel.

Con el `while`: el servidor drena **todo** lo que el OS tiene antes de avanzar el estado. Los paquetes del tick N están todos procesados antes de empezar el tick N+1. Este es el patrón correcto para servidores autoritativos de juegos (Quake, DOOM netcode lo hacían igual).

El test `WhileDrain_ProcessesAllPendingPacketsInOneUpdate` lo valida: inyectar 3 paquetes en MockTransport → 1 `Update()` → los 3 procesados.

---

## BUG CRÍTICO: ParseIpv4 endianness (el bug más costoso en tiempo)

Este bug consumió toda la sesión de benchmarking antes de descubrirse. Síntoma: el servidor arrancaba correctamente, los bots arrancaban, pero el servidor siempre mostraba "Clients: 0". Los bots nunca completaban el handshake.

### La causa

`sf::IpAddress(uint32_t)` espera el IP en **big-endian (network byte order)** — el MSB en los bits más altos.

La implementación original de `ParseIpv4` en `HeadlessBot/main.cpp` construía el uint32_t en **little-endian**:

```cpp
// ANTES (incorrecto):
int shift = 0;
// "127.0.0.1" → octet[0]=127, octet[1]=0, octet[2]=0, octet[3]=1
result |= (127 << 0);   // → 0x0000007F
result |= (0   << 8);   // → 0x0000007F
result |= (0   << 16);  // → 0x0000007F
result |= (1   << 24);  // → 0x0100007F

sf::IpAddress(0x0100007F) → interpreta como 1.0.0.127  ← INCORRECTO
```

El bot enviaba cada paquete UDP a `1.0.0.127` — una IP inexistente. El OS lo descartaba silenciosamente (o lo enrutaba a un destino inalcanzable). El servidor en `127.0.0.1` nunca recibía nada.

```cpp
// DESPUÉS (correcto — big-endian / network byte order):
int shift = 24;
// "127.0.0.1" → octet[0]=127 va en los bits 24-31 (MSB)
result |= (127 << 24);  // → 0x7F000000
result |= (0   << 16);  // → 0x7F000000
result |= (0   << 8);   // → 0x7F000000
result |= (1   << 0);   // → 0x7F000001

sf::IpAddress(0x7F000001) → interpreta como 127.0.0.1  ← CORRECTO
```

### Cómo se descubrió

El proceso de diagnóstico fue:

1. **`ss -ulnp`** — confirmó que tanto el servidor (`:7777`) como el bot tenían sus sockets UDP abiertos y escuchando. La infraestructura de red funcionaba.

2. **`echo "test" | nc -u 127.0.0.1 7777`** — el servidor recibió el paquete y lo procesó (aunque era basura no válida). Confirmó que el servidor SÍ recibía UDP en el puerto correcto.

3. Por eliminación: si el servidor recibe de `nc` pero no de los bots, el problema está en a dónde envían los bots. Tracing de `ParseIpv4("127.0.0.1")` → descubrimiento del little-endian vs big-endian.

### Por qué es fácil cometer este bug

`sf::IpAddress` tiene dos constructores relevantes:
- `sf::IpAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)` — intuitivo, toma los 4 octetos por separado
- `sf::IpAddress(uint32_t address)` — espera big-endian sin documentarlo prominentemente

El segundo constructor sigue la convención de Berkley sockets (POSIX), donde `sin_addr.s_addr` también es big-endian. Es una convención correcta pero contraintuitiva en un sistema x86 little-endian.

**Lección:** cuando un subsistema de red recibe un `uint32_t` como dirección IP, asumir siempre que espera network byte order. Documentar explícitamente si el almacenamiento interno es diferente.

---

## Benchmarks en WSL2 nativo (no Docker)

### Por qué no Docker

Docker Desktop para Windows con WSL2 backend no soporta `network_mode: host` de forma efectiva — el modo host en Docker Desktop usa el stack de red virtualizado, no el del host real. Esto introduce overhead no medible que contaminaría los resultados.

La alternativa: compilar los binarios nativamente en WSL2 Ubuntu 24.04 y ejecutarlos directamente. Sin contenedor, sin overhead de virtualización de red. Las mediciones reflejan el comportamiento real del middleware.

```bash
# En WSL2:
mkdir /tmp/nm-bench && cd /tmp/nm-bench
cmake /mnt/c/Users/alexr/RiderProjects/NetworkMiddleware \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build . --config Release
```

### tc netem — inyección de condiciones de red

`tc` (Traffic Control) es la herramienta del kernel Linux para manipular el stack de red. El módulo `netem` (Network Emulation) permite inyectar delay, pérdida, jitter y corrupción en una interfaz de red.

```bash
# Simular 50ms de latencia + 5% de pérdida en loopback:
sudo tc qdisc add dev lo root netem delay 50ms loss 5%

# Limpiar (idempotente):
sudo tc qdisc del dev lo root 2>/dev/null || true
```

Aplicar netem en `lo` (loopback) afecta a TODO el tráfico por loopback — tanto servidor→bot como bot→servidor. Con 50ms de delay, un paquete enviado del bot al servidor tiene un RTT aparente de 100ms (ida 50ms + vuelta 50ms). Esto es realista para un jugador europeo conectándose a un servidor en Norteamérica.

La limitación de este setup es que `sudo tc` requiere contraseña del usuario y no puede automatizarse desde un script sin permisos previos. El usuario ejecutó los comandos tc manualmente y luego lanzó los benchmarks.

### Limitación del Bash tool con procesos background

Durante la implementación se intentó automatizar los benchmarks completos desde el Bash tool con comandos en background (`&`). El sandbox del Bash tool aplica `SIGTERM` a los procesos al finalizar la ejecución del comando, incluyendo los procesos en background.

Síntoma: el servidor arrancaba, pero al llegar al `sleep 35` el sandbox lo mataba antes de completar el benchmark (exit code 15 = SIGTERM).

Solución: el usuario ejecutó los benchmarks directamente en su terminal WSL2, redirigiendo la salida a archivos de log (`/tmp/bench_a.log`, etc.) y leyendo los resultados después.

---

## Ejecución de los 3 escenarios

### Escenario A — Clean Lab (baseline)

```bash
# 10 bots, sin degradación de red
for i in $(seq 1 10); do
    /tmp/nm-bench/HeadlessBot/HeadlessBot &
done
/tmp/nm-bench/Server/NetServer > /tmp/bench_a.log 2>&1
```

Resultado: 0.05ms tick time, 1.0 kbps, 99% delta efficiency.

El bajo throughput (1.0 kbps) tiene una explicación: el servidor solo envía heartbeats — no hay lógica de juego que genere snapshots de posición. Los bots envían inputs pero el servidor no tiene entidades de juego que serializar y reenviar. En producción (con game loop real), el throughput sería significativamente mayor.

### Escenario B — Real World (50ms / 5% loss)

```bash
sudo tc qdisc add dev lo root netem delay 50ms loss 5%
# lanzar servidor + 9 bots (1 perdido en handshake por packet loss)
```

Resultado: 0.06ms tick time idéntico a A. El degradado de red NO afecta el tiempo de procesamiento del servidor — correcto, porque el game loop no espera ACKs UDP.

1 bot perdió el handshake: con 5% de pérdida y el handshake de 3 mensajes, la probabilidad de éxito es `(0.95)^3 ≈ 85.7%` — estadísticamente esperado que 1-2 bots de 10 fallen.

### Escenario C — Stress/Zerg (48 bots / 100ms / 2% loss)

```bash
sudo tc qdisc add dev lo root netem delay 100ms loss 2%
for i in $(seq 1 50); do
    /tmp/nm-bench/HeadlessBot/HeadlessBot &
done
```

Resultado: 0.14ms tick time (×2.8 respecto a A, con ×4.8 clientes). **Crecimiento sublineal** — el `while`-drain absorbe todos los paquetes eficientemente. 2 bots perdidos en handshake (esperado).

El dato más importante para la Memoria: **1.4% del tick budget de 10ms** con 48 clientes bajo red degradada. Margen enorme para añadir lógica de juego.

---

## Chaos bot mode

El bot original enviaba inputs con dirección aleatoria en cada tick (ruido blanco). El problema: el ruido blanco en cuantización de 8 bits genera muchos "cambios" mínimos que activan dirty bits, pero no representa el movimiento real de un jugador (vectores continuos).

El "chaos mode" cambia la dirección cada **0.5s** (30 ticks a 60 Hz con la misma dirección antes de cambiar). Esto:
1. Genera deltas grandes al cambiar (estrena el snapshot buffer con diferencias reales)
2. Simula movimiento coherente dentro del intervalo (el jugador va en línea recta)
3. Es más representativo del tráfico real de un MOBA

```cpp
constexpr auto kChaosInterval = std::chrono::milliseconds(500);
if (now >= nextDirectionChange) {
    chaosDirX = dir(rng);  // nuevo vector aleatorio
    chaosDirY = dir(rng);
    nextDirectionChange = now + kChaosInterval;
}
// Usar chaosDirX/Y para este tick (persistentes)
```

---

## Servidor 100 Hz — reemplazo del demo visual

`Server/main.cpp` pasó de ser el demo visual de Fase 3 (que incluía DemoTransport y visualización de estados del protocolo) a ser un servidor de producción limpio:

```cpp
constexpr auto kTickInterval = std::chrono::microseconds(10'000); // 100 Hz

while (g_running.load(std::memory_order_relaxed)) {
    manager.Update();
    nextTick += kTickInterval;
    std::this_thread::sleep_until(nextTick);
}
```

El demo visual de Fase 3 se conserva en el historial de git y en los IRs correspondientes — sin pérdida de valor documentado.

`sleep_until(nextTick)` es la implementación correcta (igual que en BotClient). `sleep_for(10ms)` introduciría drift acumulativo si `Update()` tarda 0.14ms — el tick sería de 10.14ms → ~98.6 Hz real. Con `sleep_until`, cada tick ocurre exactamente en `t0 + N * 10ms`.

---

## Resultado final de los benchmarks

| Escenario | Clientes | tc netem | Avg Tick | Out (kbps) | Retrans. | Delta Efficiency |
|-----------|----------|----------|----------|------------|----------|-----------------|
| A: Clean Lab | 10 | 0ms / 0% | 0.05ms | 1.0 kbps | 0 | 99% |
| B: Real World | 9* | 50ms / 5% | 0.06ms | 0.5 kbps | 0 | 100% |
| C: Stress/Zerg | 48* | 100ms / 2% | 0.14ms | 4.2 kbps | 0 | 99% |

*\* Bots perdidos en handshake por packet loss (esperado).*

**Conclusión:** el middleware usa el 1.4% del tick budget con 48 clientes bajo red degradada. P-4.4 (thread pool) no es bloqueante para el TFG.

---

## Conceptos nuevos en esta propuesta

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **std::atomic + memory_order_relaxed** | Contadores sin locks, sin overhead en x86 | Prepara el profiler para el thread pool de P-4.4 al coste de cero |
| **while-drain vs if** | Drena todo el socket buffer UDP por tick | Crítico para corrección bajo carga; sin él el buffer crece indefinidamente |
| **tc netem** | Emulación de red degradada en kernel | Reproducible, determinista, sin hardware adicional |
| **Network byte order (big-endian)** | Convención POSIX para uint32_t de IP | `sf::IpAddress(uint32_t)` espera big-endian — contraintuitivo en x86 |
| **sleep_until vs sleep_for** | Mantiene la frecuencia nominal bajo carga variable | Evita drift acumulativo en el game loop de 100 Hz |
| **Crecimiento sublineal** | ×4.8 clientes → ×2.8 tick time | El while-drain y la arquitectura callback absorben carga eficientemente |

---

## Qué podría salir mal (edge cases conocidos)

- **kMaxClients=100 sin rate limiting:** si 200 bots intentan conectarse simultáneamente, se rechazan los 101+ con `ConnectionDenied`. Sin blacklist ni backoff en el lado del cliente — potencial thundering herd de intentos de reconexión. Mitigado en el TFG porque controlamos los bots.

- **while-drain sin tick budget guard:** si el servidor recibe un burst de 10.000 paquetes (ataque o bug), procesará todos en un tick — el tick tardará mucho más de 10ms. Para producción, un límite configurable de paquetes por tick sería prudente. Para el TFG con carga controlada, no es un problema.

- **tc netem en `lo` afecta todo el loopback:** durante los benchmarks, TODO el tráfico loopback (incluidos conexiones SSH, localhost DBs, etc.) tendrá el delay/loss inyectado. Recordar limpiar con `sudo tc qdisc del dev lo root` al terminar.

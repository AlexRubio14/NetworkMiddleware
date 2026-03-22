---
type: dev-log-alex
phase: Fase 4
date: 2026-03-22
status: personal
---

# DEV LOG — Fase 4: Simulation Infrastructure & Stress Test

**Steps cubiertos:** P-4.1 + P-4.2 + P-4.3
**Fecha:** 2026-03-22

---

## El problema de partida

Al terminar la Fase 3 teníamos un protocolo de red completo y riguroso — 106 tests que pasan en milisegundos. Pero había un problema fundamental con esa validez:

Los tests de Fase 3 usan `MockTransport`. Los paquetes nunca viajan por un socket UDP real. El OS, los buffers del kernel, la latencia de red, la pérdida de paquetes — nada de eso existe en el entorno de test.

Esto es correcto para validar la lógica del protocolo, pero deja sin responder las preguntas que más importan para el TFG:

- **¿Cuánto bandwidth consume el middleware con N jugadores reales?**
- **¿El tick rate de 100 Hz aguanta bajo carga?**
- **¿Cómo se comporta con 50ms de latencia y 5% de pérdida?**
- **¿Escala linealmente o sublinealmente con el número de clientes?**

Sin responder estas preguntas, la sección de "Evaluación de Rendimiento" de la Memoria sería solo teoría. La Fase 4 convierte las afirmaciones teóricas en datos medidos.

---

## Mapa de la Fase

```text
┌─────────────────────────────────────────────────────────────────────────┐
│  P-4.3 Stress Test & Benchmarking                                       │
│  NetworkProfiler · while-drain · tc netem · 3 escenarios               │
├─────────────────────────────────────────────────────────────────────────┤
│  P-4.2 HeadlessBot                                                      │
│  BotClient · InputPayload · Chaos mode · ParseIpv4                     │
├─────────────────────────────────────────────────────────────────────────┤
│  P-4.1 Docker & Build Infrastructure                                    │
│  Multi-stage Dockerfile · docker-compose · network_mode:host           │
└─────────────────────────────────────────────────────────────────────────┘
            ↑
     Protocolo de red completo (Fase 3: 106 tests)
```

---

## P-4.1+4.2 — Infraestructura de simulación

### El headless bot: por qué en `Core/` y no en `HeadlessBot/`

La lógica del `BotClient` vive en `Core/` (biblioteca estática), no en el ejecutable `HeadlessBot/`. Razón: los tests de integración en `tests/Core/BotIntegrationTests.cpp` instancian `BotClient` con `MockTransport` — sin tocar SFML ni UDP real.

Si la lógica estuviera en el ejecutable, los tests no podrían importarla. Este es el mismo patrón que `NetworkManager`: la lógica es independiente del transport. El ejecutable es solo el punto de entrada (`main.cpp`) que ata la lógica al transport real.

### El patrón Route() para tests bidireccionales

Los tests de P-3.x siempre tenían UNA sola máquina de estados y el test construía paquetes manualmente. Con `BotIntegration` hay DOS máquinas de estados (servidor + bot) comunicándose.

El helper `Route()` actúa como "cable virtual" entre dos `MockTransport`:

```cpp
static void Route(MockTransport& serverT, MockTransport& botT) {
    for (auto& [data, to] : botT.sentPackets)
        serverT.InjectPacket(data, kBotEp);
    botT.sentPackets.clear();

    for (auto& [data, to] : serverT.sentPackets)
        botT.InjectPacket(data, kServerEp);
    serverT.sentPackets.clear();
}
```

El test llama `Route()` entre cada par de `Update()` — ambas máquinas de estados convergen de forma determinista sin threads, sin timers reales, sin carreras.

### Docker: multi-stage build y network_mode:host

Un Dockerfile naive produce ~800MB con compilador + fuentes + artefactos. El multi-stage build genera dos imágenes de ~120MB (solo runtime):

```
Stage 1 (builder): apt install build-essential cmake libsfml-dev → cmake --build
Stage 2 (server):  apt install libsfml-network2.6t64 + COPY binario de Stage 1
Stage 3 (bot):     igual que server pero con HeadlessBot
```

La sutileza de Ubuntu 24.04: los paquetes SFML llevan el sufijo `t64` por la transición de `time_t` a 64 bits. Usar la misma distro en builder y runtime garantiza compatibilidad ABI.

`network_mode: host` elimina el overhead de NAT/bridge de Docker para mediciones de latencia UDP. Limitación: Docker Desktop Windows lo ignora (solo funciona en Linux nativo/WSL2).

---

## P-4.3 — El bug más costoso: ParseIpv4 endianness

Este bug consumió toda la sesión de benchmarking inicial. Síntoma: servidor arrancado, bots arrancados, pero "Clients: 0" permanente.

`sf::IpAddress(uint32_t)` espera **big-endian (network byte order)**. La implementación original de `ParseIpv4` construía el uint32_t en **little-endian**:

```
"127.0.0.1" → ParseIpv4() → 0x0100007F  ← little-endian (incorrecto)
sf::IpAddress(0x0100007F) → envía a 1.0.0.127  ← IP inexistente
```

```
"127.0.0.1" → ParseIpv4() → 0x7F000001  ← big-endian (correcto)
sf::IpAddress(0x7F000001) → envía a 127.0.0.1  ← servidor real
```

El diagnóstico fue metodológico:
1. `ss -ulnp` — confirmó sockets abiertos en ambos lados
2. `nc -u 127.0.0.1 7777` — confirmó que el servidor recibe UDP de fuentes externas
3. Por eliminación: el problema es dónde envían los bots → tracing de ParseIpv4

Lección: en cualquier API de networking que recibe `uint32_t` como IP, asumir siempre network byte order (big-endian) a menos que se documente explícitamente lo contrario.

---

## P-4.3 — while-drain: el fix más importante

```cpp
// if → while: procesa TODOS los paquetes pendientes por tick
while (m_transport->Receive(buffer, sender)) {
    m_profiler.RecordBytesReceived(buffer.size());
    // ... procesar paquete ...
}
```

Con N bots a 60 Hz y servidor a 100 Hz: ~0.6 paquetes/tick/bot. 48 bots = 29 paquetes/tick. Con el `if` original, el servidor procesaba 1 paquete/tick — los 28 restantes se acumulaban en el socket buffer del kernel, añadiendo latencia creciente.

El `while`-drain drena todo antes de avanzar el estado del mundo — el patrón estándar de servidores autoritativos (Quake, DOOM). Si el tick budget se supera, el profiler lo reportará.

---

## P-4.3 — Benchmarks en WSL2 nativo

Docker Desktop Windows no soporta `network_mode: host` efectivamente → se compiló en WSL2 Ubuntu 24.04 y se ejecutó nativo. Sin contenedor, sin overhead de virtualización, mediciones limpias.

`tc netem` requiere `sudo` → el usuario ejecutó los comandos tc manualmente en su terminal WSL2. Los servidores se lanzaron redirigiendo stdout a archivos (`> /tmp/bench_X.log 2>&1`) para lectura posterior.

| Escenario | Clientes | tc netem | Avg Tick | Out (kbps) | Retrans. | Delta Efficiency |
|-----------|----------|----------|----------|------------|----------|-----------------|
| A: Clean Lab | 10 | 0ms / 0% | 0.05ms | 1.0 kbps | 0 | 99% |
| B: Real World | 9* | 50ms / 5% | 0.06ms | 0.5 kbps | 0 | 100% |
| C: Stress/Zerg | 48* | 100ms / 2% | 0.14ms | 4.2 kbps | 0 | 99% |

*\* Bots perdidos en handshake por packet loss (esperado).*

**Conclusión de rendimiento:** crecimiento sublineal (×4.8 clientes → ×2.8 tick time). El while-drain y la arquitectura callback absorben la carga eficientemente. Tick budget usado: 1.4% con 48 clientes.

---

## Estado del sistema al final de la Fase 4

- **119/119 tests passing** (Windows/MSVC)
- Binarios nativos WSL2: Server (100 Hz) + HeadlessBot (60 Hz, chaos mode)
- Métricas medidas bajo 3 escenarios con condiciones de red reales

---

## Conceptos nuevos en esta fase

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **Multi-stage Dockerfile** | Build en imagen pesada, runtime en imagen limpia | Imagen final ~120MB en lugar de ~800MB |
| **network_mode: host** | Contenedor comparte stack de red del host | Mediciones de latencia UDP sin overhead NAT |
| **Route() pattern** | Cable virtual entre dos MockTransports | Tests bidireccionales deterministas sin threads |
| **tc netem** | Emulación de red degradada en kernel Linux | Reproducible, sin hardware adicional |
| **Network byte order** | Big-endian para IPs en APIs de networking | `sf::IpAddress(uint32_t)` lo exige — fácil de olvidar |
| **while-drain** | Drena todo el socket buffer UDP por tick | Sin él el buffer crece indefinidamente bajo carga |
| **std::atomic + relaxed** | Contadores sin locks, coste ~0 en x86 | Prepara el profiler para el thread pool sin overhead |
| **sleep_until vs sleep_for** | Mantiene la frecuencia nominal bajo carga variable | Evita drift en loops de 100 Hz / 60 Hz |

---

## Impacto para la Memoria del TFG

La Fase 4 genera la sección "Evaluación de Rendimiento" de la Memoria con datos reales:

- **Tick budget:** 0.14ms / 10ms = **1.4% con 48 clientes bajo red degradada**
- **Escalabilidad:** sublineal — el middleware no es el cuello de botella en cargas MOBA estándar
- **Delta efficiency:** 99% — confirma que el sistema de delta compression de P-3.5 funciona en producción
- **Comparativa con competidores:** Photon Bolt y Mirror no publican tick time por cliente — estos datos son la diferenciación del TFG

---

## Siguiente paso (Fase 5)

Con los benchmarks confirmando amplio margen en el tick budget, la prioridad es Fase 5 (Spatial Hashing + Kalman Prediction) — las features diferenciadoras de mayor valor académico frente a Photon/Mirror/UE Replication.

P-4.4 (Thread Pool) tiene valor arquitectónico pero no es necesario para el TFG con la carga de un MOBA estándar (< 50 jugadores).

---
type: dev-log-alex
proposal: P-3.4
date: 2026-03-21
status: personal
---

# DEV LOG — P-3.4 Clock Synchronization & RTT Estimation

**Propuesta:** P-3.4 Clock Sync & RTT
**Fecha:** 2026-03-21

---

## ¿Qué problema resolvíamos?

En P-3.3 el servidor reintentaba paquetes fiables cada **100ms fijos**, sin importar si el cliente estaba en LAN (10ms de latencia) o en Australia (200ms). En LAN, 100ms es excesivamente conservador — el cliente ya habrá recibido el paquete mucho antes. En conexiones lentas, 100ms puede ser demasiado agresivo y generar ráfagas de retransmisiones.

Además, sin saber el RTT, el servidor no puede calcular cuánto "tiempo atrás" está el cliente respecto al servidor — información crítica para la IA Predictiva de la Fase 5.

---

## ¿Qué hemos construido?

- **RTT por cliente** via Exponential Moving Average (EMA, α=0.1) usando el ACK bitmask existente
- **Clock offset** por cliente: diferencia entre el reloj del servidor y el del cliente, para sincronización
- **Intervalo de reenvío adaptativo** basado en RTT real (RTT×1.5, mínimo 30ms)
- **Filtro de paquetes Unreliable caducados** (`m_lastProcessedSeq`) para evitar "teletransporte"
- **Brain API** `GetClientNetworkStats()` que expone RTT y clock offset al módulo de IA

---

## Cómo funciona — explicado paso a paso

### Paso 1 — Medir el RTT

Cada vez que el servidor envía un paquete, registra el instante exacto de envío en `sentTimes[seq]`. Cuando llega un paquete del cliente con `header.ack = N`, significa que el cliente confirmó recibir el paquete N. Calculamos cuánto tiempo pasó desde que lo enviamos hasta ahora: ese es el RTT bruto.

Para evitar que un pico de latencia (jitter puntual) arruine la estimación, usamos una EMA: la nueva muestra solo cuenta un 10% sobre la estimación acumulada.

### Paso 2 — Calcular el clock offset

Mientras medimos el RTT, sabemos que el paquete que el cliente acaba de confirmar fue enviado `RTT/2` ms antes de ahora. Si el cliente había escrito su timestamp en el header, podemos calcular cuánto se desvían los relojes.

### Paso 3 — Reenvío adaptativo

En lugar del `kResendInterval = 100ms` fijo de P-3.3, cada cliente tiene su propio intervalo calculado como `max(30ms, RTT×1.5)`. El factor ×1.5 da margen de seguridad. El suelo de 30ms evita saturar la red en LAN.

### Paso 4 — Filtro de paquetes Unreliable caducados

UDP puede reordenar paquetes. Si llega el Snapshot seq=5 antes que el seq=3, y luego llega el seq=3, sin filtro el game layer recibiría la posición antigua después de la nueva — "teletransporte hacia atrás". Con `m_lastProcessedSeq` solo dejamos pasar paquetes más nuevos que el último procesado.

---

## Diagramas visuales

### RTT sampling — de Send() a ProcessAcks()

```
  SERVIDOR                           CLIENTE
       │                                  │
       │  Send(seq=42)                    │
       │  sentTimes[42] = T0              │
       │──────────── pkt seq=42 ─────────►│
       │                                  │  ...procesa...
       │◄─────────── pkt ack=42 ──────────│
       │  T1 = now                        │
       │                                  │
  ProcessAcks(header.ack=42):
       │
       │  rawRTT = T1 - sentTimes[42]          e.g. rawRTT = 64ms
       │
       │  rttEMA = 0.1 × 64ms
       │         + 0.9 × 100ms (previous EMA)
       │         = 96.4ms
       │
       │  serverNow = CurrentTimeMs()
       │  clockOffset = serverNow - (header.timestamp + rttEMA/2)
       │             = serverNow - (clientSendTime + 48.2ms)
       │
       │  sentTimes.erase(42)  ← clean up
```

### EMA convergencia — suavizado frente a jitter

```
  Sample history (α=0.1, initial EMA=100ms):

  Sample  rawRTT   rttEMA
  ──────  ───────  ───────
    1      64ms    96.4ms   ← 10% of 64 + 90% of 100
    2      58ms    92.4ms
    3      61ms    88.9ms
    4      200ms   100.0ms  ← spike! EMA absorbs it (only 10% weight)
    5      60ms     96.0ms  ← quickly returns to real value
   ...
   20      60ms     ~62ms   ← converged to true RTT

  KEY: α=0.1 means outliers (packet bursts, retransmits) barely affect the estimate.
       Higher α (e.g. 0.5) would react faster but oscillate more.
```

### Intervalo de reenvío adaptativo

```
  dynamicInterval = max(30ms, rttEMA × 1.5)

  ┌──────────────────┬────────────┬──────────────────────────────────────┐
  │  Escenario       │  rttEMA    │  Resend interval                     │
  ├──────────────────┼────────────┼──────────────────────────────────────┤
  │  Initial EMA     │  100ms     │  150ms  (conservative baseline)      │
  │  LAN             │   20ms     │   30ms  (floor kicks in)             │
  │  WAN Europe      │   60ms     │   90ms                               │
  │  WAN Transoceanic│  150ms     │  225ms                               │
  │  Bad connection  │  300ms     │  450ms  (auto-adapts, no storms)     │
  └──────────────────┴────────────┴──────────────────────────────────────┘

  Timeline example (LAN, rttEMA=20ms):

  t=0ms   ──── Send seq=7 ──────────────────────────────────────►
  t=10ms  ◄─── ACK received ─── erase from m_reliableSents ✓
               (never reaches resend interval)

  Timeline example (WAN Europe, rttEMA=60ms):

  t=0ms   ──── Send seq=7 ────────────────────────────────────►
  t=30ms  ◄─── ACK received ─── erase ✓  (before 90ms interval)

  Timeline example (packet lost):

  t=0ms   ──── Send seq=7 ─────────────────────────────────────►  LOST
  t=90ms  ──── Resend seq=7 (retryCount=1) ────────────────────►
  t=140ms ◄─── ACK received ──── erase ✓
```

### Filtro de paquetes Unreliable caducados (m_lastProcessedSeq)

```
  Paquetes enviados en orden: seq=1,2,3,4,5
  Llegan reordenados por la red: 1, 3, 2, 5, 4

  Sin filtro — el game layer recibe todos:
  ┌───┬───┬───┬───┬───┐
  │ 1 │ 3 │ 2 │ 5 │ 4 │  ← posición "salta atrás" al recibir 2 después de 3
  └───┴───┴───┴───┴───┘       visual glitch: "teletransporte"

  Con m_lastProcessedSeq:
  ┌───┬───┬───┬───┐
  │ 1 │ 3 │ 5 │   │  ← seq=2 descartado (diff=-1≤0), seq=4 descartado (diff=-1≤0)
  └───┴───┴───┴───┘       solo posiciones crecientes en tiempo → sin glitches

  Lógica:
  Recv seq=1: lastSeq=0, diff=+1 > 0 → DELIVER  lastSeq=1
  Recv seq=3: lastSeq=1, diff=+2 > 0 → DELIVER  lastSeq=3
  Recv seq=2: lastSeq=3, diff=-1 ≤ 0 → DROP
  Recv seq=5: lastSeq=3, diff=+2 > 0 → DELIVER  lastSeq=5
  Recv seq=4: lastSeq=5, diff=-1 ≤ 0 → DROP

  Usa int16_t para wrap-around correcto a 65535 (mismo patrón que P-3.1).
  Solo aplica a: Snapshot, Input, Heartbeat. NO a Reliable/ReliableUnordered.
```

### Brain API — GetClientNetworkStats()

```
  Brain (Fase 4+)                      NetworkManager
       │                                     │
       │── GetClientNetworkStats(ep) ────────►│
       │                                     │  find ep in m_establishedClients
       │                                     │         │
       │                                     │  found? │
       │                                     │    NO ──┴──► return nullopt
       │                                     │
       │                                     │    YES ─► return ClientNetworkStats{
       │                                     │             rtt         = m_rtt.rttEMA,
       │                                     │             clockOffset = m_rtt.clockOffset,
       │                                     │             sampleCount = m_rtt.sampleCount
       │                                     │           }
       │◄── std::optional<ClientNetworkStats>─│
       │                                     │
       │  Use cases:                         │
       │  - Fase 4.4: IA Gestión Recursos    │
       │    "client RTT > 150ms → reduce     │
       │     snapshot rate for this client"  │
       │  - Fase 5.2: IA Predictiva          │
       │    "predict hero pos at serverNow   │
       │     using clockOffset + rtt/2"      │
```

### sentTimes cleanup — higiene de memoria

```
  At 60Hz × 10 clients = 600 sentTimes insertions/second

  Normal case (ACK arrives quickly):
  ┌──────────────────────────────────────────────────────────────────┐
  │  sentTimes[N] inserted → ACK arrives → erase(N)                 │
  │  Map stays small (< ~100 entries per client at any time)         │
  └──────────────────────────────────────────────────────────────────┘

  Worst case (persistent ACK loss):
  ┌──────────────────────────────────────────────────────────────────┐
  │  ACKs stop arriving → entries accumulate                         │
  │  kMaxRetries=10 → after ~10 resends → DisconnectClient()         │
  │  But before that: 2-second cutoff purges stale entries           │
  │                                                                  │
  │  2s × 60Hz = 120 entries max per client before purge kicks in   │
  │  At 10 clients: 1200 entries max total → negligible memory       │
  └──────────────────────────────────────────────────────────────────┘

  Purge runs after every successfully processed packet (not on DROP/duplicate).
```

---

## El código clave

### ProcessAcks() — RTT sampling y clock offset

```cpp
void NetworkManager::ProcessAcks(RemoteClient& client, const Shared::PacketHeader& header) {
    const auto sentIt = client.m_rtt.sentTimes.find(header.ack);
    if (sentIt != client.m_rtt.sentTimes.end()) {
        const float rawRTT = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - sentIt->second).count();

        constexpr float kAlpha = 0.1f;
        client.m_rtt.rttEMA = kAlpha * rawRTT + (1.0f - kAlpha) * client.m_rtt.rttEMA;
        ++client.m_rtt.sampleCount;

        // clockOffset válido cuando el cliente Unreal escriba su timestamp
        const float serverNow = static_cast<float>(Shared::PacketHeader::CurrentTimeMs());
        client.m_rtt.clockOffset =
            serverNow - (static_cast<float>(header.timestamp) + client.m_rtt.rttEMA / 2.0f);

        client.m_rtt.sentTimes.erase(sentIt);
    }
    // ... erase_if reliableSents ...
}
```

### ResendPendingPackets() — intervalo adaptativo

```cpp
const auto dynamicInterval = std::chrono::milliseconds(
    std::max(30LL, static_cast<long long>(client.m_rtt.rttEMA * 1.5f))
);
if (now - pending.lastSentTime < dynamicInterval) continue;
```

### Filtro Unreliable en Update()

```cpp
if (isUnreliableChannel && client.m_lastProcessedSeqInitialized) {
    const int16_t diff = static_cast<int16_t>(
        header.sequence - client.m_lastProcessedSeq);
    if (diff <= 0) { break; }  // stale — drop
}
if (isUnreliableChannel) {
    client.m_lastProcessedSeq            = header.sequence;
    client.m_lastProcessedSeqInitialized = true;
}
```

---

## Conceptos nuevos que aparecen aquí

| Concepto | Qué es | Por qué lo usamos |
|----------|--------|-------------------|
| **RTT (Round-Trip Time)** | Tiempo que tarda un paquete en ir al cliente y volver confirmado | Medir la latencia real para reenvíos adaptados y IA predictiva |
| **EMA (Exponential Moving Average)** | Media ponderada exponencial: las muestras recientes pesan más | Suaviza jitter sin reaccionar exageradamente a picos puntuales |
| **α (alpha)** | Peso de la nueva muestra en el EMA (0.1 = 10%) | 0.1 = estable pero reactivo. Valve y Riot usan valores similares |
| **Clock offset** | Diferencia entre el reloj del servidor y el del cliente | Permite al servidor predecir qué frame "está viendo" el cliente ahora |
| **Jitter** | Variación en el tiempo de llegada de paquetes | El EMA lo absorbe; en fases futuras habrá jitter buffer explícito |
| **Karn's Algorithm** | Excluir retransmisiones del cálculo de RTT | Diferido a P-3.5: el EMA ya absorbe suficientemente el ruido en MVP |
| **Stale packet** | Snapshot de posición que llegó tarde, describe un estado ya superado | Si lo entregamos, el héroe "retrocede" visualmente |

---

## Cómo encaja en el sistema completo

```
  Send(seq=N)                                   Recv(header.ack=N)
       │                                               │
       ├── sentTimes[N] = now ──────────────────────── ├── rawRTT = now - sentTimes[N]
       │                                               │
       │                                               ├── rttEMA updated (EMA α=0.1)
       │                                               │
       │                                               ├── clockOffset = serverNow
       │                                               │               - (clientTs + rttEMA/2)
       │                                               │
       ▼                                               ▼
  m_rtt.sentTimes                             m_rtt.rttEMA / clockOffset
       │                                               │
       ▼                                               ▼
  ResendPendingPackets()                    GetClientNetworkStats(ep)
  dynamicInterval = max(30, rttEMA×1.5)    → Brain (Phase 4+)
```

---

## Decisiones de diseño

| Decisión | Alternativa descartada | Motivo |
|----------|------------------------|--------|
| α=0.1 para EMA | α=0.5 (más reactivo) | 0.1 es el estándar de Valve/Riot. Absorbe jitter sin oscilar. |
| Floor de 30ms | Sin floor | LAN puede tener RTT <20ms. Sin floor generaría retransmit storm. |
| RTT×1.5 (no RTT×2) | RTT×2 como TCP | ×1.5 es más agresivo: en MOBA la latencia importa. TCP usa ×2 para conservadurismo. |
| Karn's algorithm diferido | Implementar ahora | Complejidad innecesaria en MVP. EMA absorbe el ruido. Revisar en Fase 3.5. |
| sentTimes para TODOS los paquetes | Solo trackear 1 por RTT window | Granularidad máxima. 600/s es negligible para un servidor Linux. |
| `int16_t` en stale filter | Comparación directa uint16_t | Mismo patrón de P-3.1 para wrap-around correcto en 65535. |

---

## Qué podría salir mal (edge cases)

- **clockOffset antes de que el cliente escriba timestamp:** `header.timestamp=0` → clockOffset incorrecto. Usar `sampleCount > 0 && clientImplementsTimestamp` para gatear su uso.
- **sentTimes crece si ACKs se pierden:** La purga de 2s protege. Además, kMaxRetries=10 desconectará al cliente antes de acumulación severa.
- **RTT spike en retransmisión (Karn's problem):** Un ACK de un reenvío puede parecer RTT muy bajo (el cliente lo tenía en buffer). EMA con α=0.1 lo amortigua. Solución completa en Fase 3.5.
- **Wrap-around de m_lastProcessedSeq:** Cubierto por el cast a int16_t. seq=1 después de seq=65534 → diff=+3 → DELIVER (correcto).

---

## Estado del sistema tras esta implementación

**Funciona:**
- RTT por cliente actualizado en cada ACK recibido (EMA, α=0.1)
- Intervalo de reenvío adaptativo: LAN usa ~30ms, WAN usa RTT×1.5
- Clock offset calculado (válido cuando el cliente Unreal escriba su timestamp)
- Paquetes Unreliable caducados descartados (sin teletransporte)
- `GetClientNetworkStats(ep)` disponible para el módulo Brain

**Pendiente (próxima propuesta):**
- P-3.5 Delta Compression: baseline snapshots + zig-zag encoding para reducir payload
- P-3.6 Session Recovery: heartbeats, tokens de reconexión, timeouts formales
- Karn's Algorithm: excluir retransmisiones del RTT sampling (si Stress Test lo requiere)
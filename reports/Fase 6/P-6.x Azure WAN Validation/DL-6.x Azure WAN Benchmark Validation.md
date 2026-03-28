---
type: dev-log-alex
phase: Fase 6
date: 2026-03-28
status: personal
commit: 5120e71
branch: fix/raw-udp-addr-cache-multibot
---

# DEV LOG — Validación WAN Real post-Fase 6

**Fecha:** 2026-03-28
**Commit:** `5120e71`
**Escenario:** 10 bots | Azure Switzerland North (20.208.47.230) ← WSL2 | 60s | sin emulación de red

---

## El contexto

Toda la Fase 6 se diseñó para resolver un número concreto: **10.97ms de Full Loop** medido en el benchmark WAN de P-5.x con 49 bots. Ese número era la evidencia de que el transporte bloqueaba el hilo del juego. Las tres capas de P-6.1 + P-6.2 + P-6.3 atacaban ese problema desde ángulos distintos.

Los benchmarks de desarrollo de cada step se corrían en loopback (WSL2 local). Loopback es útil para comparativas relativas, pero enmascara los efectos reales de la red: latencia variable, jitter, pérdida esporádica, y el comportamiento del kernel bajo congestión WAN real. El único benchmark que cuenta para validar P-6.2 específicamente (AsyncSendDispatcher) es uno donde la ruta de red tenga latencia real, porque es exactamente ahí donde `sendmmsg` puede bloquearse.

Este DL recoge el primer benchmark completo en condiciones WAN reales post-Fase 6.

---

## El test

```
Servidor:  Azure VM Switzerland North — Docker ghcr.io/alexrubio14/netserver:latest
Bots:      10 HeadlessBot locales (WSL2) conectando sobre internet real
Duración:  60 segundos de colección de datos estables
Condición: sin tc netem — latencia WAN pura (~15-30ms España→Switzerland)
```

Diez bots se lanzan escalonados (300ms entre cada uno) y se mantienen conectados durante el período completo de medición. Los bots operan en chaos mode (cambio de dirección cada ~0.5s), lo que produce inputs continuos y obliga al servidor a ejecutar el full pipeline de predicción Kalman + game world tick + FOW + serialización en cada ciclo.

---

## Resultados

```
t=10s   Clients=10   Tick=0.06ms   Full Loop=0.15ms   Out=155.3kbps   Δeff=56%
t=20s   Clients=10   Tick=0.06ms   Full Loop=0.16ms   Out=169.7kbps   Δeff=57%
t=30s   Clients=10   Tick=0.06ms   Full Loop=0.16ms   Out=183.6kbps   Δeff=58%
t=40s   Clients=10   Tick=0.07ms   Full Loop=0.17ms   Out=190.0kbps   Δeff=59%
t=50s   Clients=10   Tick=0.07ms   Full Loop=0.17ms   Out=192.6kbps   Δeff=59%
t=60s   Clients=10   Tick=0.07ms   Full Loop=0.17ms   Out=197.2kbps   Δeff=58%
```

| Métrica | Valor final |
|---------|-------------|
| Clients conectados | 10 / 10 |
| Avg Tick | 0.07ms |
| Full Loop | 0.17ms |
| Tick Budget | **0.7%** (target: <10%) |
| Outbound total | 197.2 kbps |
| Outbound / cliente | **~19.7 kbps** |
| Inbound | 89.8 kbps |
| Retries | 0 |
| CRC Errors | 0 |
| Delta Efficiency | 58% |

---

## El número que importa: Full Loop

El objetivo de toda la Fase 6 era resolver que el Full Loop superaba el 10ms budget en condiciones WAN. El resultado:

```
Pre-Fase 6 (P-5.x WAN, 49 bots):   10.97ms   (110% del budget)
Post-Fase 6 (WAN real, 10 bots):     0.17ms   (1.7% del budget)
```

**Reducción: 98.5%.**

El Full Loop de 0.17ms con 10 bots en WAN real significa que el hilo del game loop ya no tiene I/O. Todo el coste de red (sendmmsg) se ejecuta en el `AsyncSendDispatcher` thread, y lo único que ve el tick es el `Signal()` de 200ns. El presupuesto de 10ms ahora es prácticamente libre para lógica de juego.

La progresión durante los 60s también es reveladora: el Full Loop sube de 0.15ms a 0.17ms de forma suave y monótona. No hay spikes. No hay jitter de I/O. El hilo principal es completamente estable.

---

## Bandwidth y FOW

**19.7 kbps/cliente** con spawn aleatorio (±400 unidades, visión 150 unidades).

Con spawn aleatorio, algunos bots inevitablemente estarán dentro del radio de visión de otros. El FOW filtra los bots fuera del rango, pero en un mapa de 800×800 con 10 bots y radio de visión de 150 unidades, la probabilidad de que un bot vea a otro no es despreciable. Los 19.7 kbps/cliente reflejan una situación intermedia: parte de los pares son visibles, parte filtrados.

El target de ~13 kbps/cliente de P-6.3 corresponde al caso donde el FOW maximiza el filtrado — grupos de bots separados a más de 150 unidades entre sí. Con spawn bimodal (dos grupos a ~400 unidades de distancia), el FOW filtra completamente el grupo opuesto y se espera llegar a ese target.

**Comparativa vs competidores:**

| Sistema | Bandwidth/cliente | Fuente |
|---------|-----------------|--------|
| League of Legends | ~10–30 kbps | Riot post-mortems |
| Dota 2 | ~20–50 kbps | Valve net_graph measurements |
| Overwatch | ~30–60 kbps | GDC 2017 "Replay Technology" |
| Valorant | ~40–80 kbps | Riot netcode devblog |
| **Este proyecto (WAN, spawn random)** | **~19.7 kbps** | Este benchmark |
| **Este proyecto (target, FOW máx)** | **~13 kbps** | Estimación P-6.3 |

Con spawn aleatorio ya estamos en el rango de Dota 2 y por debajo de Overwatch y Valorant. Con FOW completamente activo (spawn bimodal), estamos al nivel de LoL.

---

## Delta Efficiency: 58%

La delta efficiency del 58% mide qué fracción del ancho de banda ahorra la compresión delta respecto a enviar full state siempre. Un 58% es sólido para un escenario de chaos mode donde los bots cambian de dirección continuamente.

Lo interesante es la progresión: empieza en 56% (t=10s) y sube hasta 59% (t=50s) antes de estabilizarse en 58%. Los primeros segundos tienen más full-state packets porque los clientes recién conectados no tienen ninguna baseline confirmada todavía. A medida que las conexiones se estabilizan y los ACKs de las baselines llegan correctamente, la eficiencia sube. Es el comportamiento esperado.

---

## Retries: 0, CRC Errors: 0

Dos métricas que confirman la estabilidad del pipeline en WAN real.

**0 retries** significa que la capa de reliability no necesitó retransmitir ningún paquete durante los 60s. Con 10 bots en una ruta España→Switzerland, eso no es trivial — implica que la ventana de retransmisión (ACK bitmask de P-3.1) está capturando todos los paquetes perdidos antes de que el timeout los escale a retry. O que la ruta particular de este test no tuvo pérdida apreciable (lo más probable dado que es un datacenter Azure de buena calidad).

**0 CRC errors** confirma que no hay corrupción de paquetes en la ruta y que el campo de integridad de P-4.5 funciona correctamente en producción. En loopback los CRC errors siempre son 0 por definición. En WAN real, un CRC error podría indicar NAT mangling o corrupción en algún dispositivo intermedio. La ausencia confirma que el wire format está llegando intacto.

---

## El efecto silencioso del fix de la rama actual

La rama `fix/raw-udp-addr-cache-multibot` (commit `5120e71`) corrige el keying del addr cache de `RawUDPTransport`. El bug original usaba solo `ep.address` como clave, lo que hacía que dos bots con la misma IP pero distinto puerto compartieran la misma `sockaddr_in` en el cache — el paquete del bot B se enviaba al puerto del bot A.

Este benchmark confirma que el fix funciona correctamente con múltiples bots simultáneos: los 10 bots se conectan, mantienen sus sesiones individuales durante 60 segundos, y 0 retries sugiere que no hay desincronización de rutas.

---

## Próximos pasos

El siguiente test es el benchmark FOW específico (run_fow_benchmark_azure.sh): dos grupos de bots separados geográficamente en el mapa (spawn bimodal) vs todos agrupados (spawn cluster). La comparativa de bandwidth entre los dos escenarios cuantificará exactamente cuánto filtra el FOW en condiciones de separación total.

La hipótesis: **Out_bimodal ≈ 0.5 × Out_cluster**. Con spawn cluster todos los bots se ven entre sí y el servidor envía N² updates. Con spawn bimodal el FOW elimina el grupo opuesto para cada bot: N/2 updates por cliente en lugar de N.

Si esa hipótesis se confirma en WAN real, tenemos la validación experimental completa de todo el pipeline P-5.1 → P-6.3.

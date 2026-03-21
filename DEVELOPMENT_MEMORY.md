# Memoria de Investigación y Desarrollo
## Middleware de Red Autoritativo para MOBAs con Optimización mediante IA

**Autor:** Alejandro Rubio Plana
**Proyecto:** TFG — 150 horas estimadas
**Última actualización:** 2026-03-21

---

## Índice

1. [Visión del Proyecto](#1-visión-del-proyecto)
2. [Estado del Arte y Referentes de la Industria](#2-estado-del-arte-y-referentes-de-la-industria)
3. [Análisis de Tecnologías de Serialización](#3-análisis-de-tecnologías-de-serialización)
4. [Arquitectura del Sistema](#4-arquitectura-del-sistema)
5. [Motor de Optimización — Implementado](#5-motor-de-optimización--implementado)
6. [Diario de Desarrollo](#6-diario-de-desarrollo)
7. [Roadmap Maestro](#7-roadmap-maestro)
8. [Métricas de Éxito](#8-métricas-de-éxito)
9. [Líneas Futuras de Investigación](#9-líneas-futuras-de-investigación)
10. [Bibliografía](#10-bibliografía)

---

## 1. Visión del Proyecto

Este proyecto consiste en el diseño y desarrollo de un **middleware de red de alto rendimiento** orientado a juegos competitivos de tipo MOBA. El objetivo es crear una infraestructura capaz de gestionar la comunicación en tiempo real de forma eficiente, agnóstica al motor de juego y potenciada por Inteligencia Artificial para la optimización de recursos y la mitigación de los efectos negativos de la latencia.

El enfoque no es solo una solución técnica de conectividad, sino un ejercicio de **ingeniería de software avanzada** donde la arquitectura y la optimización a bajo nivel son los pilares fundamentales.

### Por qué este proyecto existe

El mercado ofrece soluciones como Photon Bolt, Mirror o Unity Netcode. A pesar de su eficiencia, estas herramientas actúan como **"cajas negras"**, obligando al desarrollador a acoplar la lógica del juego a las limitaciones del middleware. En el caso de Unreal Engine, su sistema de replicación nativo es potente pero está íntimamente ligado al motor, dificultando la creación de servidores ligeros en Linux que no requieran dependencias de renderizado.

La validación no se hará mediante un juego completo, sino a través de un **Visual Debugger en Unreal Engine** que actuará como bridge de diagnóstico para monitorizar variables de red (vida, mana, estados) y visualizar los logs del servidor en tiempo real.

---

## 2. Estado del Arte y Referentes de la Industria

### 2.1 Middlewares comerciales — El problema

Los middlewares actuales (Photon Bolt, Mirror, Unity Netcode) gestionan la conectividad pero imponen su modelo de datos al desarrollador. El sistema de replicación nativo de Unreal Engine es potente pero está íntimamente ligado al motor: no se puede ejecutar un servidor Unreal en Linux sin sus dependencias de renderizado. Ninguno de estos sistemas permite optimización a nivel de bit.

### 2.2 Referentes de la industria

**Valve (Dota 2) — Delta Compression y Baselines**
Principal referente. El servidor mantiene un "baseline" por cliente (el último estado completo confirmado por ACK). Envía únicamente el Δ respecto a ese baseline. Si un paquete se pierde, el siguiente delta sigue siendo válido porque está anclado al baseline confirmado, no al paquete anterior.

**Riot Games (Valorant) — Tick-rates elevados y estabilidad**
Investigada la importancia de la estabilidad del frame-time en el servidor para evitar el jitter. La implementación de tick-rates elevados (128Hz) requiere una gestión precisa del presupuesto de tiempo por tick.

**Blizzard (Overwatch) — Modularidad basada en componentes**
Inspiración en su gestión de entidades basada en componentes, que permite desacoplar la lógica de IA del transporte de red.

### 2.3 Fundamentos académicos

**Fiedler (2016) — Reliability over UDP:**
El protocolo UDP es la base para juegos de tiempo real por su baja latencia. La implementación de una Reliability Layer personalizada permite diferenciar entre datos volátiles (movimiento) y mensajes críticos (estados de vida, habilidades MOBA) — una distinción que muchos sistemas genéricos no permiten optimizar al detalle.

**Hennessy y Patterson (2017):**
La eficiencia en el procesamiento de datos en sistemas de tiempo real depende de una gestión de memoria y hilos de ejecución óptima. C++20 ofrece herramientas como `std::span` y `jthread` que permiten un procesamiento asíncrono más seguro y rápido.

---

## 3. Análisis de Tecnologías de Serialización

Un punto crítico de la investigación ha sido decidir cómo empaquetar los datos para una transmisión ultra-eficiente.

### 3.1 Tecnologías Descartadas

| Tecnología | Funcionamiento | Pros | Contras | Decisión |
|-----------|---------------|------|---------|----------|
| **memcpy directo** | Copia binaria directa de variables | Velocidad máxima. Zero overhead | Riesgo de padding de memoria. Imposible optimización bit-level. | Descartado para capa de transporte crítica |
| **Protocol Buffers (Google)** | Serialización basada en esquemas .proto | Muy robusto, gestión de versiones automática | Requiere un paso de "parseig" que consume CPU al servidor. Metadata overhead excesivo. | Descartado |
| **FlatBuffers (Google)** | Acceso a datos binarios sin desempaquetar (Zero-copy) | No hay parsing; lees directamente del buffer | Mucho más complejo de implementar. Pierde el control bit a bit. | Descartado para capa crítica; posible en capas secundarias como línea futura |

### 3.2 Decisión: Bit-packing manual con Delta Compression

Implementación propia de Bit-packing porque para alcanzar la excelencia en la optimización hay que escribir los datos bit a bit, evitando los tipos de datos fijos del sistema operativo que desperdician ancho de banda.

**Ejemplo:** Si la vida de un héroe tiene un máximo de 2000, no utilizamos un `int` de 32 bits, sino solo **11 bits** (suficiente para 2047).

- **Paquete estándar:** Enviar vida, mana y posición (3 floats) consume mínimo **160 bits**
- **Paquete optimizado:** Con Delta Compression + Bit-packing, si una variable no cambia, solo enviamos 1 bit (0). Con los valores que sí cambian al mínimo de bits necesarios, ahorro superior al **70%**

**Sobre FlatBuffers:** A pesar de ser una solución potente, se descarta para la capa de transporte crítica en favor de una solución hecha a medida que ofrece control superior sobre el flujo de bits. Queda como línea futura para capas de datos menos críticas.

---

## 4. Arquitectura del Sistema

### 4.1 Principios de Diseño (SOLID + GRASP)

El sistema se basa en un desacoplamiento total mediante el patrón de Adaptadores e Interfaces.

- **Single Responsibility:** Cada clase hace solo una cosa.
- **Open/Closed:** Podemos añadir nuevos transportes sin tocar el Core.
- **Liskov Substitution:** `SFMLTransport` puede ser sustituido por cualquier `ITransport`.
- **Interface Segregation:** Las interfaces son minimalistas.
- **Dependency Inversion:** El Core depende de abstracciones, no de detalles.

### 4.2 Módulos CMake

```
MiddlewareShared     (sin dependencias externas)
    ↑
MiddlewareCore       ← MiddlewareShared
MiddlewareTransport  ← MiddlewareShared + sfml-network
Brain                (autónomo)
    ↑
NetServer (exe)      ← todos los módulos + Threads
```

**MiddlewareShared:**
El nexo de unión que garantiza que el servidor Linux y el cliente Unreal "hablen el mismo idioma de bits". Contiene: `BitWriter`/`BitReader`, `HeroState`, `BaseHero`, `ViegoEntity`, `NetworkOptimizer`, `HeroSerializer`, `Logger`.

**MiddlewareTransport:**
Completamente aislado. Solo contiene la implementación de SFML bajo la interfaz `ITransport`. Su única responsabilidad es mover vectores de bytes brutos (`std::vector<uint8_t>`) sin entender su contenido.

**MiddlewareCore:**
El cerebro organizativo. Recibe datos del Transport y los delega al `PacketManager` mediante inyección de dependencias. El `NetworkManager` usa `std::function` callbacks — no tiene referencia directa al Brain.

**Brain:**
Una pieza clave al mismo nivel que el Core. Aquí reside la IA Predictiva. El Core puede "consultar" al Brain cuando se pierde un paquete. Interfaces: `IDataProcessor`, `IBehaviorEngine`.

### 4.3 Patrones de Diseño Aplicados

**Adapter:** `SFMLTransport` es el único componente que conoce la existencia de SFML, actuando como traductor entre la librería externa y nuestras estructuras propias.

**Factory:** `TransportFactory::Create(TransportType)`. El main no realiza instanciación directa. Aplicación estricta del principio Open/Closed — añadir Asio o sockets nativos de Linux no requiere cambios en Core o main.

**Dependency Injection:** Eliminados completamente los Singletons en favor de un Composition Root en `main.cpp`. Cada clase recibe sus herramientas directamente en el constructor.

**Callback / Inversión de Control:** El `NetworkManager` no tiene referencia directa al `BrainManager`. Usa un callback basado en `std::function`. Flujo de datos: `Transport → NetworkManager → Main (Lambda) → Brain`. Esta arquitectura permite inyectar paquetes en una cola de tareas de una Thread Pool sin modificar el código de red.

### 4.4 Namespace y Estándares

- **Namespace único:** `NetworkMiddleware` en todo el proyecto
- **C++20 puro:** Eliminado `sf::sleep` en favor de `<thread>` y `<chrono>`
- **No macros:** Refactorizado a métodos type-safe con lógica en `.cpp`

### 4.5 Auditoría del Proyecto Legacy — Por qué reescribimos

Antes de este middleware existía un proyecto base de comunicación UDP básica. Una auditoría profunda identificó tres limitaciones críticas:

**Acoplamiento estricto con SFML:** `Server.cpp` y `PacketManager.cpp` tenían dependencia directa con `sf::UdpSocket` y `sf::IpAddress`. El Core necesitaba conocer los detalles de SFML — imposible portarlo a Asio o sockets nativos de Linux sin rehacer todo el proyecto.
*Solución: la interfaz `ITransport`.*

**Ineficiencia en serialización:** `CustomUDPPacket::WriteVariable` usaba `memcpy`. Transmitir la vida con un `int` de 32 bits cuando el máximo es 2000 es un desperdicio constante. Sin optimización a nivel de bit.
*Solución: `BitWriter`/`BitReader` + `NetworkOptimizer`.*

**God Object en Client.cpp:** Gestionaba simultáneamente validación de movimiento, timeouts, lógica de paquetes críticos y estado del jugador. Imposible introducir la IA Predictiva sin crear un entorno de desarrollo inestable.
*Solución: fragmentación en módulos con responsabilidades claras.*

---

## 5. Motor de Optimización — Implementado

### 5.1 Baselines y Delta Compression (inspirado en Valve/Dota 2)

**El problema de la delta naïve:** Si enviamos Δ(Estado#2 - Estado#1) y el cliente pierde el paquete #2, cuando reciba Δ(Estado#3 - Estado#2) no podrá aplicarlo — le falta la base. Esto genera teletransportes o errores de estado.

**La solución (Baseline):** Un Baseline es el último estado completo que el servidor **sabe con certeza** que el cliente ha recibido, confirmado mediante ACK.

**Mecanismo de resiliencia:**
1. Cliente confirma: "He recibido el Estado #10"
2. Servidor usa #10 como ancla. Si está en #15, calcula: `Δ = Estado₁₅ - Estado₁₀`
3. Si ese paquete se pierde, el servidor sigue enviando deltas desde la base #10 (16-10, 17-10...) hasta que el cliente confirme una nueva base
4. Garantía: el cliente siempre aplica datos sobre una base sólida

**Zig-Zag Encoding:** Para deltas negativos, mapea signed → unsigned antes de aplicar VLE. Ejemplo: -1 → 1, 1 → 2, -2 → 3, 2 → 4.

**Circular Buffer / Snapshot History:** 128-256 slots almacenando los últimos ~2 segundos de estado. Requerido tanto para Baselines como para el futuro sistema de Rewind (Fase 5.3).

### 5.2 Bit-packing Custom y Dirty Bits

**Rango de bits dinámico:** Si la vida de un héroe no supera los 2000, se mapea a solo **11 bits**. Reducción del **65%** respecto a un `int` de 32 bits.

**Dirty Bits:** Cada campo (vida, mana, posición) tiene un bit de control. Si el campo no ha cambiado, el servidor escribe un 0 y el cliente se salta la lectura de los bits del campo. La `dirtyMask` de 32 bits se inicializa a `0xFFFFFFFF` en el primer tick — el cliente recibe el estado completo en el Tick 0.

**`SetNetworkVar<T>`:** Método template en `BaseHero` que automatiza la comparación de valores y la activación de bits en la `dirtyMask` solo cuando hay un cambio real. Los campos de `m_state` nunca se acceden directamente.

### 5.3 BitWriter y BitReader — Implementación por Bloques

**Decisión crítica:** Abandono del bucle bit-a-bit en favor de operaciones bitwise de alto rendimiento.

**`BitWriter::WriteBits`:** Escribe bloques de bits directamente al byte actual usando máscaras. Reducción de iteraciones de 32 a un máximo de 5 (normalmente 1 o 2). Mejora de rendimiento del **~80% en CPU**.

**`BitReader::ReadBits`:** Implementación simétrica que extrae fragmentos de bits y los reconstruye en el valor original.

**`GetCompressedData()`:** Detecta el techo de bits escritos y recorta el buffer para enviar exclusivamente los bytes que contienen información real. Resultado en pruebas: **1500 bytes → 2 bytes reales**.

**Validación de Endianness:** Los operadores bitwise (`<<`, `>>`, `&`, `|`) son Endian-Neutral por definición. `SwapEndian` eliminado. Validado en round-trip Windows↔WSL el 18-03-2026.

### 5.4 Entidades MOBA

**`HeroState` (POD struct):**
`networkID` (32b) | `heroTypeID` (16b) | `x/y` (float, quantizados a 14b) | `health`/`maxHealth`/`mana`/`maxMana` (float, VLE) | `level` (uint32) | `experience` (float, VLE) | `stateFlags` (uint8: 0x01=Dead, 0x02=Stunned, 0x04=Rooted) | `dirtyMask` (uint32)

**`BaseHero`:** Clase abstracta. Implementa `SetNetworkVar<T>`. Puras virtuales: `Serialize()`, `Unserialize()`, `GetHeroName()`.

**`ViegoEntity`:** Primera implementación (Type ID 66). Hereda de `BaseHero`, delega serialización a `HeroSerializer`.

### 5.5 Cuantización y VLE

**Cuantización de posición (14 bits):**
- float 32 bits → entero 14 bits. Rango: ±500m
- Precisión: **1.75cm**. Reducción: **56%** respecto a float estándar

**Variable Length Encoding (Base-128 Varints):**
- Vida y maná se envían en 8 bits cuando los valores son bajos
- Crecen dinámicamente solo cuando es necesario

### 5.6 Network Logger — El Microscopio

**Arquitectura asíncrona productor-consumidor:** El log se delega a un hilo secundario (`std::jthread`) para no bloquear el hilo de red. El `NetworkManager` y el Logger comparten la propiedad de los datos mediante `std::shared_ptr` — el paquete sobrevive en memoria el tiempo exacto necesario, eliminando copias costosas de buffers.

**Inspección de bits:** Convierte los buffers en formato Hexadecimal y Binario para validar el Bit-packing y detectar errores de alineación entre servidor y cliente.

**Canales de filtrado:** Core, Transport, Brain — para separar el ruido del tráfico crítico.

---

## 6. Diario de Desarrollo

### 6.1 Fase 1: Infraestructura y Desacoplamiento (Completada)

**Cambio de paradigma: de Monolito a Capas**

El código inicial presentaba "Acoplamiento Fuerte": el servidor dependía directamente de SFML. Cualquier cambio en la tecnología de red obligaba a rehacer prácticamente todo el proyecto.

**Decisión:** Implementar un sistema de capas basado en SOLID y GRASP.

**Implementaciones:**

*Adaptador de Transport (Pattern: Adapter):*
`SFMLTransport` es ahora el único componente que conoce SFML. El Core es agnóstico a dependencias externas.

*Factoría de Transport (Pattern: Factory):*
El main ya no instancia directamente `SFMLTransport`. Si en el futuro añadimos Asio o sockets nativos de Linux, ni el main ni el Core sufrirán cambios.

*Inyección de Dependencias:*
Eliminados completamente los Singletons en favor de un Composition Root en `main.cpp`. Cada clase recibe sus herramientas en el constructor — el estado global oculto desaparece.

*Inversión de Control (Callback/Observer):*
El `NetworkManager` usa `std::function` en lugar de referencia directa al Brain. Flujo limpio: `Transport → NetworkManager → Main (Lambda) → Brain`. Preparación para futura Thread Pool: los paquetes pueden inyectarse en una cola de tareas sin tocar el código de red.

*Decisiones de estandarización:*
- Namespace único `NetworkMiddleware`
- C++20 puro: eliminado `sf::sleep` en favor de `<thread>` y `<chrono>`
- Modularidad CMake: cuatro bibliotecas con responsabilidades claras

### 6.2 Fase 2: Bit-packing y Serialización Avanzada (Completada — 18-03-2026)

**Sistema de diagnóstico asíncrono:**
Arquitectura productor-consumidor. El hilo principal de red deposita los logs en una cola protegida por `std::mutex`. Un `std::jthread` dedicado los procesa y escribe. `std::shared_ptr` para gestión de vida de los paquetes — sin copias costosas de buffers.

**Motor de serialización:**
`BitWriter` y `BitReader` para trascender la barrera del byte. Prueba de concepto: valor de vida (2000) empaquetado en 11 bits — reducción del **65%**.

**Abandono del bucle bit-a-bit:**
Bottleneck detectado en las primeras pruebas. Reemplazado por operaciones bitwise por bloques. Reducción de iteraciones de 32 a máximo 5 (normalmente 1 o 2). Mejora del **~80% en CPU**.

**`GetCompressedData()`:**
Buffer pre-asignado de 1500 bytes (MTU) generaba overhead inaceptable. Refactorizado a crecimiento dinámico + compresión de salida: **1500 bytes → 2 bytes reales** en pruebas.

**Validación de Endianness:**
Determinado que los operadores bitwise son Endian-Neutral. `SwapEndian` eliminado. Validado en round-trip Windows↔WSL.

### 6.3 Tabla de Decisiones de Diseño (Fases 1 y 2)

| Decisión | Razonamiento |
|----------|-------------|
| Bit-Packing vs Bit-Loop | El bucle bit a bit era un bottleneck a escala. Operaciones bitwise por bloques. |
| No SwapEndian | Los operadores `<<`, `>>` son independientes de la arquitectura. Ciclos de CPU ahorrados. |
| `dirtyMask = 0xFFFFFFFF` en Tick 0 | Garantiza full state en el primer paquete. El cliente siempre parte de un estado completo. |
| `SetNetworkVar<T>` | Automatiza dirty bits. Nunca se accede directamente a `m_state`. |
| `std::shared_ptr` en Logger | Elimina copias de buffers de red. Lifetime gestionado automáticamente. |
| Composition Root en main | Estado global oculto eliminado. La arquitectura de un módulo se lee en su header. |
| Buffer dinámico + GetCompressedData | MTU fija generaba overhead. Solo se envían los bytes con información real. |

### 6.4 Resultados del Test de Sincronización (Round-Trip — 18-03-2026)

Test completo de serialización/deserialización con ViegoEntity:

| Métrica | Resultado |
|---------|-----------|
| Peso del paquete Full Sync | **145 bits (~18 bytes)** |
| Error de cuantización de posición | **0.0175m (1.75cm)** |
| Reducción vs serialización estándar | **60-70%** |
| Buffer comprimido | **1500 bytes → 2 bytes reales** |
| Estado | Éxito total — integridad y alta precisión |

---

## 7. Roadmap Maestro

### Fase 1 & 2 — COMPLETADAS (18-03-2026)

Infraestructura, desacoplamiento, bit-packing, serialización de entidades, validación de endianness.

---

### Fase 3 — SPRINT ACTIVO (~12h) — Netcode Core y Protocolo de Sesión

*Objetivo: pasar de "bits volando" a un protocolo de comunicación real.*

- **3.1 Packet Topology:** Diseño del Header (Sequence, Ack, Type)
- **3.2 Connection Handshake:** Hello → Challenge → Welcome. Registro de RemoteClients, prevención de packet injection.
- **3.3 Reliability Layer (UDP-R):** Canales Reliable Ordered (items/habilidades) y Unreliable (movimiento)
- **3.4 Clock Synchronization:** Cálculo de RTT, alineación del Tick cliente↔servidor
- **3.5 Delta Compression & Zig-Zag:** Buffer circular de Snapshots (Baselines) y codificación de diferencias negativas
- **3.6 [DISEÑO PENDIENTE] Session Recovery:** Heartbeats, Timeouts, Tokens de reconexión

*Clase RemoteClient: gestión de networkID y estado individual por jugador.*

---

### Fase 4 — Brain I: Infraestructura, Stress Test y Recursos

- **4.1 Dockerización:** Contenedor Linux para el servidor middleware
- **4.2 Headless Simulation Suite:** Mock Clients en C++ para simular cientos de jugadores
- **4.3 Stress Testing:** CPU Time per Tick y ancho de banda bajo carga de Teamfight
- **4.4 IA de Thread Pool Dinámica:** El Brain escala jthreads automáticamente según la carga
- **4.5 Terminal Profiler (The Microscope):** Dashboard CLI asíncrono para monitorizar el servidor

---

### Fase 5 — Brain II: IA Predictiva y Gestión Espacial

- **5.1 Spatial Hashing (Interest Management):** Rejilla espacial para filtrar qué entidades ve cada jugador (también base del Anti-Cheat)
- **5.2 IA Predictiva de Trayectorias:** Modelo de regresión dentro del Brain para predecir movimiento y mitigar jitter
- **5.3 Authoritative Lag Compensation (Rewind):** Sistema de rebobinado en el servidor para validar skillshots basado en el ping del atacante
- **5.4 AI-Driven Replication:** La IA prioriza el envío de entidades según su relevancia en el combate (Network LOD)

---

### Fase 6 — Visual Bridge y Validación Final

- **6.1 Unreal Plugin Wrapper:** ActorComponent de Unreal que encapsula el middleware
- **6.2 Visual Debugger:** Comparativa visual hitboxes autoritarias (servidor) vs posición visual (cliente)
- **6.3 Benchmarking y Objetivos SMART:** Informe final de rendimiento bajo carga, estabilidad del tick-rate y ahorro de bits

---

## 8. Métricas de Éxito

| Métrica | Objetivo |
|---------|----------|
| Latencia y Jitter | Fluctuación < 5ms en condiciones normales |
| Optimización de datos | Reducción del 60-80% vs serialización estándar |
| Estabilidad del tick-rate | < 10ms por ciclo (100Hz) de forma constante |
| Precisión IA Predictiva | Error < 5% en estimación de trayectorias bajo 10% de packet loss |

---

## 9. Líneas Futuras de Investigación

**1. Serialización avanzada**
FlatBuffers como alternativa para capas de datos menos críticas (metadatos de partida, configuración). No para el transporte de estado por tick.

**2. Escalabilidad y Arquitectura Cloud**
Sharding Dinámico de Instancias: matchmaking que levante y destruya instancias del servidor en contenedores (Docker/Kubernetes) según la demanda. Arquitectura Multirregión para sincronizar el estado del Brain entre nodos geográficos.

**3. Seguridad y Anti-Cheat basado en IA**
Detección de comportamientos anómalos entrenando la IA para identificar scripts (auto-dodge, aimbots) analizando si los patrones de paquetes coinciden con comportamiento humano. Cifrado selectivo: solo en paquetes críticos para no penalizar la CPU general.

**4. Evolución de la Inteligencia Artificial**
Deep Learning at the Edge: migrar parte de la lógica del Brain al cliente Unreal mediante ONNX Runtime. Network LOD dinámico decidido por IA según importancia en la partida.

**5. Interoperabilidad y Protocolos Modernos**
Soporte para QUIC/HTTP3, evaluando si puede sustituir la Reliability Layer. Multi-Engine Bridge para Unity y Godot.

**6. Chaos Engineering para Redes**
Módulo que inyecte pérdida de paquetes, jitter y latencia de forma agresiva para estresar el sistema y generar informes automáticos sobre la resiliencia de la IA predictiva.

---

## 10. Bibliografía

**Fiedler, G. (2016).** Reliability over UDP. *Gaffer on Games.*
https://gafferongames.com/post/reliability_layer/

**Hennessy, J. L., & Patterson, D. A. (2017).** *Computer Architecture: A Quantitative Approach* (6th ed.). Morgan Kaufmann.

**International Organization for Standardization. (2020).** *Programming Languages C++* (ISO/IEC 14882:2020).
https://www.iso.org/standard/79358.html

**Epic Games. (2026).** Unreal Engine Documentation.
https://docs.unrealengine.com/

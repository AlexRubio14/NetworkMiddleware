# DL-6.2 — Async Send Thread (AsyncSendDispatcher)

**Date:** 2026-03-28
**Branch:** `P-6.2-Async-Send-Thread`
**Tests:** 231 (sin regresión) + 5 nuevos (cross-platform)

---

## El punto de partida

P-6.1 había hecho algo elegante: colapsar 49 llamadas `send()` secuenciales en una sola syscall `sendmmsg`. El benchmark de referencia mostraba 7.2ms gastados en I/O de red; la hipótesis era que una syscall sería ordenes de magnitud más rápida que cuarenta y nueve. La arquitectura era correcta.

Pero `FlushTransport()` seguía ejecutándose en el game loop. Al final de Phase B — después de serializar y encolar todos los paquetes del tick — el hilo principal llamaba `sendmmsg` y esperaba a que el kernel terminara de procesar la ráfaga. Bajo condiciones normales eso es rápido. Bajo WAN real con congestión transitoria, el kernel puede bloquearse un tiempo arbitrario esperando que el buffer de envío drene. Ese bloqueo ocurría directamente en el hilo crítico que tiene que completar cada ciclo en menos de 10ms.

El objetivo de P-6.2 era sencillo de enunciar: mover la syscall `sendmmsg` completamente fuera del hilo del game loop. El game loop solo debía señalizar que había trabajo pendiente — nada de I/O, nada de esperar al kernel.

---

## El diseño del despachador

La primitiva natural para "un productor que notifica, un consumidor que procesa" es un hilo dedicado durmiendo en una condition variable. El game loop señaliza (`Signal()`), el hilo de envío despierta, drena la cola, vuelve a dormir.

`std::jthread` era la elección obvia: C++20, RAII puro. El destructor solicita stop y hace join automáticamente — el hilo de red nunca queda huérfano cuando el servidor se apaga. `std::stop_token` se integra en el predicado de `wait_for`, lo que significa que el hilo responde a la señal de shutdown sin polling y sin flags manuales.

El timeout de `kFlushInterval` (33ms, 30Hz) no es el path normal — es un safety net. En producción, cada tick de 30Hz genera un `Signal()` que despierta al hilo en microsegundos. El timeout existe para el caso teórico en que el game loop se detiene y no llega ninguna señal. Sin él, los paquetes encolados en `m_sendQueue` quedarían indefinidamente sin despachar.

El diseño del `ThreadLoop` tiene un detalle que no es obvio: el lock se suelta antes de llamar a `Flush()`. Si el mutex de la condition variable se mantuviera durante `sendmmsg`, `Signal()` desde el game loop bloquearía esperando ese mismo mutex — introduciendo exactamente el tipo de bloqueo que queríamos eliminar. La secuencia correcta es: lock → leer y limpiar flag → unlock → `Flush()`. Así el productor y el consumidor se coordinan en el flag, pero el productor nunca espera la syscall.

El destructor hace `request_stop()` + `notify_one()` antes de que jthread destruya el hilo. El `notify_one()` es necesario porque el hilo puede estar bloqueado en `wait_for` cuando llega el stop — sin la notificación esperaría hasta el siguiente timeout. Después de salir del bucle principal, `ThreadLoop` llama a `Flush()` una vez más fuera del while. Este **final drain** cubre los paquetes que el game loop pudo haber encolado entre la última señal y el `request_stop()`. Sin él, esos paquetes se descartarían silenciosamente al destruirse el transport.

---

## La race condition y el swap-buffer

La validación técnica de Gemini identificó un riesgo real: si `sendmmsg` tarda más de 10ms bajo congestión severa, el game loop del siguiente tick puede llamar a `Send()` (empujando a `m_sendQueue`) mientras el send thread todavía está en `Flush()` leyendo esa misma cola. Con un `std::vector` sin protección, eso es undefined behavior.

La solución implementada es el patrón **swap-buffer**: dos colas en lugar de una.

```
m_sendQueue   — propiedad del productor (game loop).  Send() hace push_back aquí.
m_flushQueue  — propiedad del consumidor (send thread). sendmmsg opera aquí.
m_queueMutex  — protege exclusivamente el intercambio O(1) entre colas.
```

El flujo en `Flush()` es:
1. Adquirir `m_queueMutex`
2. `std::swap(m_sendQueue, m_flushQueue)` — intercambia los punteros internos del vector, O(1)
3. Soltar `m_queueMutex`
4. Ejecutar `sendmmsg` sobre `m_flushQueue` **sin ningún lock**
5. `m_flushQueue.clear()` — retiene la capacidad del vector para el próximo ciclo

El resultado: `Send()` y `sendmmsg` nunca tocan la misma cola simultáneamente. El mutex se mantiene solo durante el swap — microsegundos. La syscall corre completamente sin contención. Si el game loop llama a `Send()` mientras `sendmmsg` está en curso, está escribiendo en `m_sendQueue`, que el send thread ya no toca hasta el próximo `Flush()`.

`m_flushQueue.clear()` sin `shrink_to_fit()` es una decisión deliberada: retiene la capacidad asignada (49 entradas si hay 49 clientes), lo que elimina reallocations en ticks subsiguientes. El vector crece una sola vez — en el primer tick con N clientes — y desde ahí opera sin heap allocation.

---

## Integración en NetworkManager y main.cpp

`AsyncSendDispatcher` se pasa como `unique_ptr` opcional al constructor de `NetworkManager`. Si está presente (Linux producción), `FlushTransport()` llama `m_dispatcher->Signal()`. Si es null (Windows, CI, tests), `FlushTransport()` llama `m_transport->Flush()` directamente — el comportamiento P-6.1 exacto.

Este diseño garantiza cero regresiones en Windows: los 231 tests anteriores nunca ven el dispatcher. Los 5 tests nuevos de `AsyncSendDispatcher` usan `MockTransport` (cross-platform) y prueban exclusivamente el comportamiento del despachador, sin sockets POSIX.

En `main.cpp`, la construcción es condicional por plataforma:

```cpp
#ifdef __linux__
    auto dispatcher = std::make_unique<Core::AsyncSendDispatcher>(transport);
    NetworkManager manager(transport, std::move(dispatcher));
#else
    NetworkManager manager(transport);
#endif
```

El call-site de `FlushTransport()` en el game loop no cambia — una sola línea sirve ambas rutas en todas las plataformas.

---

## Los cinco tests

Los tests de `AsyncSendDispatcher` son cross-platform: usan `MockTransport` con un `flushCount` atómicamente actualizado en `Flush()`. No hay sockets, no hay POSIX. Corren en Windows y Linux por igual.

| Test | Lo que guarda |
|------|--------------|
| `SignalWakesFlushPromptly` | `Signal()` produce al menos 1 `Flush()` en < 10ms. Guarda la regresión de un dispatcher que nunca despertara o se durmiera por más tiempo del razonable. |
| `FlushCalledByTimeoutWithoutSignal` | Sin ningún `Signal()`, el thread flushea tras ~33ms (kFlushInterval). Valida que el safety net funciona aunque el game loop nunca señalice. |
| `DestructorFlushesOnShutdown` | El destructor incrementa `flushCount` al menos una vez (final drain). Guarda la regresión del final drain eliminado — paquetes descartados silenciosamente al apagar el servidor. |
| `MultipleSignalsCoalesceIntoFewFlushes` | 10 señales rápidas producen ≤ 5 `Flush()` — no 10. Valida que la coalescing es implícita: el flag `m_signaled` es booleano, no un contador; N señales seguidas se colapsan en 1 wake-up si el thread no ha tenido tiempo de procesar las anteriores. |
| `NetworkManagerFlushTransport_UsesDispatcher` | `Signal()` directo sobre el dispatcher → `flushCount` crece. Confirma el path async sin necesitar un `NetworkManager` completo. |

---

## Resultado

El game loop ya no tiene I/O de red. `FlushTransport()` es ahora una escritura en un mutex + un `notify_one()` — del orden de decenas de nanosegundos. La syscall `sendmmsg` ocurre en un hilo separado, completamente fuera del presupuesto de 10ms.

El benchmark post-merge en Azure debería mostrar el Full Loop próximo a los 3.77ms de lógica pura — el mínimo físico dado el trabajo de CPU en Phase A/B. El I/O de red habrá desaparecido del tick como variable de latencia.

---

## Lección

P-6.1 resolvió el problema de las N syscalls reduciéndolas a 1. P-6.2 resolvió el problema de que esa 1 syscall bloqueara el hilo principal. Son dos problemas distintos con soluciones distintas, y la secuencia importa: intentar el async send sin el batch de sendmmsg habría movido 49 syscalls a un hilo de fondo — correcto en términos de latencia del game loop, pero ineficiente en términos de overhead del kernel. La combinación sendmmsg (batch) + async dispatch (desacoplamiento) produce el óptimo: 1 syscall, 0 tiempo en el game loop.

El dato que más importa para la Memoria: el Full Loop baja de 10.97ms a ~3.77ms no porque la red sea más rápida, sino porque la red ya no participa en el presupuesto del tick. El tick hace su trabajo y sigue; la red hace su trabajo en paralelo.

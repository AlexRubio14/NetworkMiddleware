---
type: dev-log-alex
proposal: P-3.1 (iteración 2)
date: 2026-03-21
status: personal
---

# DEV LOG — P-3.1 Iteración 2: El byte de tipo se parte en dos

**Fecha:** 2026-03-21

---

## ¿Qué ha cambiado y por qué?

En el primer diseño, el tipo de paquete ocupaba 8 bits enteros. Gemini ha decidido partirlo en dos mitades de 4 bits:

- **4 bits para el tipo** (¿qué es este paquete? posición, input, reliable...)
- **4 bits para flags** (¿cómo hay que tratarlo? ¿es un reenvío? ¿es un fragmento?)

### ¿Por qué 4 bits son suficientes para el tipo?

4 bits pueden representar valores del 0 al 15. Tenemos 5 tipos de paquete ahora mismo, y aunque añadamos más en el futuro, es muy improbable que pasemos de 15. Los juegos de red reales (Valorant, Dota 2) también usan pocos tipos de paquete.

### ¿Por qué 4 bits de flags y no más?

Los flags son bits de control que modifican cómo se procesa el paquete. De momento solo necesitamos dos:
- `IsRetransmit`: este paquete es un reenvío de uno que se perdió (Fase 3.3)
- `IsFragment`: este paquete es un trozo de uno más grande (Fase 3.3)

4 bits = 4 flags independientes. Con 2 usados y 2 reservados, tenemos margen.

---

## El truco del header de 100 bits

El header anterior era de 104 bits (múltiplo perfecto de 8 → 13 bytes exactos).
El nuevo es de 100 bits (12.5 bytes → necesita redondeo).

¿Cómo se mandan 12.5 bytes por UDP? No se puede. Se mandan 13 bytes, con los últimos 4 bits a 0 como relleno.

¿Quién pone ese relleno? `GetCompressedData()`, que ya existía de la Fase 2. Su trabajo es exactamente ese: rellenar con ceros los bits que sobran para completar el último byte. Por eso Gemini dijo "no nos importa que sean 100 bits en vez de 104 — GetCompressedData ya lo maneja".

```
Wire format en memoria: [100 bits de datos][0000 relleno] → 13 bytes enviados por UDP
```

---

## El bug silencioso que corregimos

En el `NetworkManager` teníamos esta línea para calcular cuántos bytes mínimos debe tener un paquete válido:

```cpp
// ANTES — incorrecto con 100 bits:
constexpr size_t kHeaderBytes = Shared::PacketHeader::kBitCount / 8;
// 100 / 8 = 12  ← ¡INCORRECTO! Nos falta 1 byte
```

Con 104 bits daba 13 ✓. Pero con 100 bits, la división entera da 12 ✗. Un paquete de 12 bytes habría pasado la validación pero el BitReader habría intentado leer el bit 100 desde solo 96 bits disponibles → comportamiento indefinido (crash o datos corruptos).

La corrección:

```cpp
// DESPUÉS — correcto siempre:
static constexpr uint32_t kByteCount = (kBitCount + 7) / 8;
// (100 + 7) / 8 = 107 / 8 = 13  ✓
```

El truco `(n + 7) / 8` es la forma estándar de hacer "división entera redondeando hacia arriba" en programación. Sumar 7 antes de dividir entre 8 garantiza que cualquier resto (1–7 bits sobrantes) siempre sume 1 byte más.

---

## Wire format definitivo de P-3.1

```
Posición en el wire:
  bits   0–15  : sequence  (16 bits)
  bits  16–31  : ack       (16 bits)
  bits  32–63  : ack_bits  (32 bits)
  bits  64–67  : type      ( 4 bits) ← NUEVO: antes eran 8 bits
  bits  68–71  : flags     ( 4 bits) ← NUEVO: antes no existía
  bits  72–103 : timestamp (32 bits)
  bits 104–107 : 0000      ( 4 bits de relleno, escritos por GetCompressedData)

Total: 108 bits en memoria → 13 bytes enviados por UDP
```

---

## Conceptos nuevos en esta iteración

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **Flags de paquete** | Bits de control que no describen el contenido sino cómo procesarlo | Permiten que un mismo tipo de paquete tenga comportamientos distintos sin crear tipos nuevos |
| **División entera con redondeo hacia arriba** | `(n + 7) / 8` | Imprescindible cuando trabajas con bits y necesitas convertir a bytes sin perder el último byte parcial |
| **Comportamiento indefinido (UB)** | En C++, leer fuera de los límites de un buffer no da error garantizado — puede crashear, dar basura, o funcionar por casualidad | El bug de los 12 bytes habría sido difícil de detectar porque podría haber "funcionado" en algunos sistemas |

---

## Estado final de P-3.1

La propuesta está **cerrada**. El wire format es definitivo y no cambiará hasta que Gemini decida lo contrario explícitamente.

**Lo que está listo para la Fase 3.2:**
- `SequenceContext` definida y esperando a que `RemoteClient` la instancie
- `PacketFlags::IsRetransmit` y `IsFragment` definidos, esperando a la Reliability Layer (3.3)
- `PacketHeader::kByteCount` disponible para cualquier módulo que necesite saber el tamaño del header

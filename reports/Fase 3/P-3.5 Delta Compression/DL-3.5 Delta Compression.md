---
type: dev-log-alex
proposal: P-3.5
date: 2026-03-22
status: personal
---

# DEV LOG — P-3.5 Delta Compression & Zig-Zag Encoding

**Propuesta:** P-3.5 Delta Compression & Zig-Zag Encoding
**Fecha:** 2026-03-22

---

## ¿Qué problema resolvíamos?

Tras P-3.4 teníamos RTT calibrado y resend adaptativo. Pero el servidor seguía enviando el estado **completo** del héroe en cada tick — los ~149 bits de full sync, aunque el jugador llevara 20 ticks sin moverse.

En un MOBA real, la mayoría de ticks la situación es "estoy en base esperando resurrección" o "me muevo en línea recta". El estado cambia muy poco tick a tick. Enviar 149 bits cuando la diferencia real es "me moví 2cm" es un desperdicio masivo.

P-3.5 implementa el protocolo de delta: en lugar de enviar el estado, enviamos la **diferencia respecto al último estado conocido por el cliente**. Si no hay diferencia, el paquete mínimo es de 38 bits (32 de networkID + 6 flags de dirty bits = 0). Si hay movimiento pequeño, VLE + ZigZag comprime la diferencia a 1-2 bytes.

---

## ¿Qué hemos construido?

| Componente | Dónde | Qué hace |
|-----------|-------|---------|
| **ZigZagEncode / ZigZagDecode** | `NetworkOptimizer` | Mapea signed int a unsigned: deltas pequeños (+/−) producen valores VLE de 1 byte |
| **SnapshotHistory** | `RemoteClient` | Buffer circular de 64 slots para baselines de delta |
| **SerializeDelta / DeserializeDelta** | `HeroSerializer` | Protocolo completo con inline dirty bits + ZigZag+VLE por campo |
| **22 tests** | `DeltaCompressionTests.cpp` | Cobertura de ZigZag, round-trips, eficiencia, evicción circular |

---

## El problema central: deltas negativos → varints grandes

La compresión VLE (Variable-Length Encoding / base-128) funciona genial con valores pequeños no negativos. Un delta de posición de +5 quantized units se codifica en 1 byte. Pero un delta de −5 en int32 tiene el bit de signo activado → `0xFFFFFFFB` → VLE necesita 5 bytes.

**ZigZag** resuelve esto con una biyección matemática que intercala negativos con positivos:

```
0  → 0     (0 << 1) ^ (0 >> 31)  = 0
-1 → 1     (-1 << 1) ^ (-1 >> 31) = 1
1  → 2
-2 → 3
2  → 4
n  → 2n       (positivos)
n  → -2n-1    (negativos)
```

Resultado: el delta −3 se codifica como 5 (VLE: 1 byte). El delta +3 se codifica como 6 (VLE: 1 byte). Sin ZigZag, −3 sería 0xFFFFFFFD → VLE: 5 bytes.

```cpp
uint32_t NetworkOptimizer::ZigZagEncode(int32_t n) {
    return (static_cast<uint32_t>(n) << 1) ^ static_cast<uint32_t>(n >> 31);
}

int32_t NetworkOptimizer::ZigZagDecode(uint32_t n) {
    return static_cast<int32_t>((n >> 1) ^ (~(n & 1) + 1));
}
```

El `n >> 31` es un arithmetic shift right que propaga el bit de signo — produce `0xFFFFFFFF` para negativos y `0x00000000` para positivos. Funciona correctamente sin branches.

---

## SnapshotHistory — buffer circular de baselines

El servidor necesita recordar qué estado envió en el tick con secuencia N, para calcular el delta cuando vuelva a enviar al mismo cliente. El cliente envía su `ack` en cada paquete (del sistema de P-3.1), lo que le dice al servidor "yo conozco el estado del tick con seq=N".

Un buffer de 64 slots con indexación `seq % 64`:
- Bajo: N secuencias recientes accesibles instantáneamente con un índice directo
- Sin allocaciones dinámicas — tamaño fijo en el `RemoteClient`
- Evicción automática: cuando seq=64 llega, sobreescribe seq=0

```cpp
const Shared::Data::HeroState* GetBaseline(uint16_t seq) const {
    const auto& entry = m_history[seq % kHistorySize];
    if (!entry.valid || entry.seq != seq) return nullptr;
    return &entry.state;
}
```

La guarda `entry.seq != seq` detecta evicción: si seq=64 ha sobreescrito el slot de seq=0, pedir seq=0 devuelve `nullptr` (full sync needed). Si el cliente está muy por detrás (más de 64 ticks de lag), simplemente se le envía un full sync. Sin corrupción, sin estado inconsistente.

---

## SerializeDelta — el protocolo

El handoff original proponía incluir `baseline_seq` dentro del payload de `SerializeDelta`. Decidimos **no hacerlo**: `SerializeDelta` recibe la baseline ya resuelta (puntero al estado histórico), no necesita saber el número de secuencia. La resolución es responsabilidad del llamador (NetworkManager), que conoce el `remoteAck` del cliente.

Esto hace la función cohesiva y **testeable sin estado externo** — el test puede construir dos `HeroState` directamente y comparar bits, sin simular una sesión de red completa.

Formato del paquete delta:

```
[ networkID: 32 bits ]
[ dirty_pos: 1 bit  ] → si 1: [zigzag(dQx): VLE] [zigzag(dQy): VLE]
[ dirty_hp:  1 bit  ] → si 1: [zigzag(dHealth): VLE]
[ dirty_mhp: 1 bit  ] → si 1: [zigzag(dMaxHealth): VLE]
[ dirty_mp:  1 bit  ] → si 1: [zigzag(dMana): VLE]
[ dirty_mmp: 1 bit  ] → si 1: [zigzag(dMaxMana): VLE]
[ dirty_flg: 1 bit  ] → si 1: [stateFlags: 8 bits raw]
```

Los `stateFlags` no usan ZigZag — es un bitmask, no un entero numérico. El XOR de dos bitmasks no tiene semántica de "delta pequeño". Se envía el valor completo (8 bits) solo cuando cambia.

El delta se calcula en **espacio cuantizado**, no en float. Razón: la cuantización introduce un error de redondeo de hasta 1.53cm. Si calculáramos el delta en float y lo re-cuantizáramos, el error se podría acumular. Trabajar en quantized units garantiza que `encode(decode(delta)) == delta` exactamente.

```cpp
const int32_t dQx = static_cast<int32_t>(qXc) - static_cast<int32_t>(qXb);
```

---

## Tests clave y lo que validan

| Test | Bits esperados | Por qué importa |
|------|----------------|-----------------|
| `NoChanges_MinBitCount` | 38 bits | El mínimo absoluto: networkID + 6 flags a 0 |
| `AllFieldsChanged_FewerBitsThanFullSync` | < 149 bits | El delta siempre es más compacto que full sync |
| `PositionDelta_SmallMovement` | ~54 bits | VLE + ZigZag codifica deltas pequeños en 1 byte |
| `StaleSeq_ReturnsNullptr` | — | La evicción del buffer circular funciona correctamente |
| `IdentityMismatch_Rejected` | — | No se mezclan estados de héroes distintos |

El test de detección de movimiento de 2cm (= 1 quantized unit en 16 bits con MAP±500) confirma que la precisión de 1.53cm de P-2 no "desaparece" en el pipeline de delta — seguimos detectando cambios mínimos.

---

## Decisión: POS_BITS 14 → 16 bits

Durante la revisión de CodeRabbit se identificó que 14 bits dan una precisión de 6.1cm — suficiente para un MOBA pero con margen desaprovechado. 16 bits dan 1.53cm con un coste de solo 2 bits adicionales por campo (4 bits en total por posición en un full sync).

La mejora de precisión justificó el pequeño aumento del full sync de 145 → 149 bits. El delta no se ve afectado más que en el VLE de los deltas de posición (que siguen siendo comprimibles a 1-2 bytes para movimientos normales).

---

## Resultado

- 94 tests, 100% passing (Windows/MSVC) en el primer build
- Sin bugs durante la implementación — la lógica de ZigZag y el protocolo de delta compilaron y funcionaron correctamente desde el primer intento
- Ahorro de bits demostrado con tests cuantificados (no solo "debería funcionar")

---

## Conceptos nuevos en esta propuesta

| Concepto | Qué es | Por qué importa |
|----------|--------|-----------------|
| **ZigZag encoding** | Biyección signed → unsigned que intercala positivos y negativos | Hace que los deltas negativos pequeños sean VLE-compresibles |
| **Buffer circular de baselines** | 64 slots indexados por `seq % 64` | O(1) lookup, sin allocaciones, evicción automática con detección |
| **Delta en espacio cuantizado** | Calcular diferencias en quantized units, no en float | Evita acumulación de error de redondeo en operaciones de ida y vuelta |
| **Inline dirty bits** | 1 bit por campo antes del valor delta | El decodificador salta exactamente los campos que no cambiaron |

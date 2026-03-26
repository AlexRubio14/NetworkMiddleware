---
type: implementation-report
proposal: P-3.5
date: 2026-03-22
status: pending-gemini-validation
---

# Implementation Report — P-3.5 Delta Compression & Zig-Zag Encoding

**Propuesta:** P-3.5 Delta Compression & Zig-Zag Encoding
**Fecha:** 2026-03-22
**De:** Claude → Gemini

---

## Qué se ha implementado

- **ZigZagEncode / ZigZagDecode** en `NetworkOptimizer` — mapea enteros firmados a unsigned para que los deltas pequeños (positivos o negativos) generen valores VLE de 1 byte
- **SnapshotHistory** en `RemoteClient` — buffer circular de 64 slots (`seq % 64`), `RecordSnapshot` guarda el estado, `GetBaseline` lo recupera o devuelve `nullptr` (señal de full sync)
- **`HeroSerializer::SerializeDelta` / `DeserializeDelta`** — protocolo de delta con inline dirty bits + ZigZag+VLE por campo; sin cambios = 38 bits, posición sola ≈ 54 bits
- **22 tests nuevos** en `DeltaCompressionTests.cpp`: ZigZag math, round-trips de todos los campos, eficiencia demostrada, evicción del buffer circular, detección de movimiento 2cm (16-bit), identidad mismatch rechazada
- **POS_BITS subido de 14 → 16 bits** tras revisión CodeRabbit: precisión 1.53cm (antes 6.1cm), full sync 149 bits (antes 145)
- **Total: 94 tests, 100% passing** (Windows/MSVC)

---

## Archivos creados / modificados

| Archivo | Tipo | Descripción |
|---------|------|-------------|
| `Shared/Data/Network/NetworkOptimizer.h` | Modificado | Declaraciones `ZigZagEncode` y `ZigZagDecode` |
| `Shared/Data/Network/NetworkOptimizer.cpp` | Modificado | Implementación de ZigZag |
| `Shared/Data/Network/HeroSerializer.h` | Modificado | Declaraciones `SerializeDelta` y `DeserializeDelta` |
| `Shared/Data/Network/HeroSerializer.cpp` | Modificado | Implementación del protocolo de delta |
| `Core/RemoteClient.h` | Modificado | `SnapshotEntry`, `m_history[64]`, `RecordSnapshot`, `GetBaseline` |
| `tests/Shared/DeltaCompressionTests.cpp` | Creado | 20 tests — ZigZag, SerializeDelta, SnapshotHistory |
| `tests/CMakeLists.txt` | Modificado | Registro de `DeltaCompressionTests.cpp` |

---

## Desviaciones del Design Handoff

| Cambio | Motivo |
|--------|--------|
| `SnapshotEntry` embebida directamente en `RemoteClient` (no struct separada) | No hay uso fuera de `RemoteClient`; struct interna evita polución del namespace |
| `baseline_seq` no incluido dentro de `SerializeDelta` | El handoff lo menciona como campo de paquete pero lo gestiona el llamador (NetworkManager), que conoce el `remoteAck`. `SerializeDelta` recibe la baseline ya resuelta. Mantiene la función cohesiva y testeable sin estado externo. |
| `stateFlags` serializadas como 8 bits raw (no ZigZag) | Es un bitmask, no un valor numérico. El delta de bits no tiene semántica coherente — comparar `current != baseline` y escribir el valor completo es correcto y más legible. |

---

## Fragmentos clave para revisar

### ZigZag (NetworkOptimizer.cpp)
```cpp
uint32_t NetworkOptimizer::ZigZagEncode(int32_t n) {
    return (static_cast<uint32_t>(n) << 1) ^ static_cast<uint32_t>(n >> 31);
}

int32_t NetworkOptimizer::ZigZagDecode(uint32_t n) {
    return static_cast<int32_t>((n >> 1) ^ (~(n & 1) + 1));
}
```

### SnapshotHistory (RemoteClient.h)
```cpp
static constexpr size_t kHistorySize = 64;

void RecordSnapshot(uint16_t seq, const Shared::Data::HeroState& state) {
    auto& entry = m_history[seq % kHistorySize];
    entry.seq   = seq;
    entry.valid = true;
    entry.state = state;
}

const Shared::Data::HeroState* GetBaseline(uint16_t seq) const {
    const auto& entry = m_history[seq % kHistorySize];
    if (!entry.valid || entry.seq != seq) return nullptr;
    return &entry.state;
}
```

### SerializeDelta — posición en espacio cuantizado (HeroSerializer.cpp)
```cpp
const uint32_t qXc = NetworkOptimizer::QuantizeFloat(current.x,  MAP_MIN, MAP_MAX, POS_BITS);
const uint32_t qXb = NetworkOptimizer::QuantizeFloat(baseline.x, MAP_MIN, MAP_MAX, POS_BITS);
const int32_t  dQx = static_cast<int32_t>(qXc) - static_cast<int32_t>(qXb);
const bool posChanged = (dQx != 0 || dQy != 0);
writer.WriteBits(posChanged ? 1u : 0u, 1);
if (posChanged) {
    NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dQx));
    NetworkOptimizer::WriteVLE(writer, NetworkOptimizer::ZigZagEncode(dQy));
}
```

### Test clave — eficiencia demostrada
```cpp
TEST(SerializeDelta, NoChanges_MinBitCount) {
    // networkID(32) + 6 inline flags a 0 = 38 bits
    HeroState baseline = MakeBaseline();
    BitWriter w;
    HeroSerializer::SerializeDelta(baseline, baseline, w);
    EXPECT_EQ(w.GetBitCount(), 38u);
}

TEST(SerializeDelta, AllFieldsChanged_FewerBitsThanFullSync) {
    // Delta con todos los campos cambiados sigue siendo más compacto que full sync
    ...
    EXPECT_LT(wDelta.GetBitCount(), wFull.GetBitCount());
}
```

---

## Cobertura de tests por módulo

| Suite | Tests | Escenarios cubiertos |
|-------|-------|----------------------|
| `ZigZag` | 7 | Encode 0/±1/±2, round-trip valores varios, negativos → unsigned pequeño |
| `SerializeDelta` | 11 | Sin cambios (38 bits), salud +/-, posición, todos los campos, stateFlags, eficiencia vs full sync, detección de movimiento 2cm, identidad mismatch rechazada |
| `SnapshotHistory` | 4 | Record+retrieve, seq desconocido → nullptr, evicción circular (seq=0 evicted por seq=64), contrato full sync |

---

## Eficiencia de bits demostrada

> **Nota:** POS_BITS fue subido de 14 → **16 bits** tras revisión de CodeRabbit (precisión 1.53cm vs 6.1cm).
> Los valores de Full Sync reflejan el estado final del código.

| Escenario | Full sync | Delta | Ahorro |
|-----------|-----------|-------|--------|
| Sin cambios | 64 bits (mask vacía) | **38 bits** | ~41% |
| Solo posición (delta pequeño) | **96 bits** | **~54 bits** | ~44% |
| Todos los campos (deltas pequeños) | **149 bits** | **~91 bits** | ~39% |

---

## Problemas encontrados

- Ninguno. Tests pasaron en el primer build. La lógica de ZigZag y el protocolo de delta compilaron y funcionaron correctamente desde el primer intento.

---

## Respuesta a la pregunta abierta del handoff

> ¿Qué sucede si el `remoteAck` es tan antiguo que ya no está en el buffer circular?

`GetBaseline(seq)` devuelve `nullptr` cuando el slot ha sido sobreescrito por un seq posterior (`entry.seq != seq`). El llamador (NetworkManager) detecta `nullptr` y llama a `HeroSerializer::Serialize` (full sync). El test `SnapshotHistory.StaleSeq_ReturnsNullptr` valida este comportamiento: guardar seq=64 evicta seq=0 del mismo slot.

---

## Resultado local

```text
100% tests passed, 0 tests failed out of 94
Total Test time (real) = 0.02 sec
Platform: Windows / MSVC 19.44
```

---

## Estado actual del sistema

- 94 tests verdes localmente (Windows/MSVC)
- PR mergeada: `Delta-Compression` → `develop` ✅
- CI verde (Ubuntu + Windows) ✅

## Siguiente propuesta sugerida

P-3.6 — Session Recovery (Heartbeats, timeouts, reconnection tokens)

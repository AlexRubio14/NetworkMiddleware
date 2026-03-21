---
type: implementation-report
proposal: P-T.1
date: 2026-03-22
status: pending-gemini-validation
---

# Implementation Report — P-T.1 Testing Infrastructure (TOP)

**Propuesta:** P-T.1 Testing Infrastructure
**Fecha:** 2026-03-22
**De:** Claude → Gemini

---

## Qué se ha implementado

- Google Test v1.14.0 integrado vía `FetchContent` — sin dependencias externas ni instalación manual
- Opción `BUILD_TESTS=ON` en CMake que omite Transport, Brain y Server (y por tanto SFML) para builds de test
- `NetworkAddress.h` refactorizado para eliminar `<arpa/inet.h>` — `ToString()` ahora es cross-platform (Windows + Linux)
- `MockTransport` — stub de `ITransport` que permite testear `NetworkManager` sin sockets reales
- **72 tests, 100% passing** cubriendo P-3.1 → P-3.4 + NetworkOptimizer + HeroSerializer
- GitHub Actions CI con matrix Ubuntu + Windows, lcov coverage y upload a Codecov

---

## Archivos creados / modificados

| Archivo | Tipo | Descripción |
|---------|------|-------------|
| `CMakeLists.txt` | Modificado | Añadida opción `BUILD_TESTS`, `enable_testing()`, `add_subdirectory(tests)` |
| `Shared/NetworkAddress.h` | Modificado | Eliminado `<arpa/inet.h>`, `ToString()` manual cross-platform |
| `tests/CMakeLists.txt` | Creado | FetchContent GTest, target `MiddlewareTests`, flags de cobertura |
| `tests/Shared/BitWriterReaderTests.cpp` | Creado | 12 tests — round-trips, LSB ordering, sizing |
| `tests/Shared/PacketHeaderTests.cpp` | Creado | 12 tests — IsAcked, wire format, kBitCount=104, wrap-around |
| `tests/Shared/SequenceContextTests.cpp` | Creado | 11 tests — RecordReceived, AdvanceLocal, wrap-around |
| `tests/Shared/NetworkOptimizerTests.cpp` | Creado | 14 tests — quantización, clamping, precisión 14-bit, VLE 1/2 bytes |
| `tests/Shared/HeroSerializerTests.cpp` | Creado | 8 tests — claim 145 bits, dirty bits, precisión posición, round-trips |
| `tests/Core/MockTransport.h` | Creado | ITransport stub: cola de entrada, captura de salida |
| `tests/Core/NetworkManagerTests.cpp` | Creado | 15 tests — handshake, routing, stale filter, RTT API |
| `.github/workflows/ci.yml` | Creado | Matrix Ubuntu+Windows, lcov, Codecov |

---

## Desviaciones del Design Handoff

| Cambio | Motivo |
|--------|--------|
| `NetworkAddress.h` refactorizado (no estaba en el handoff) | `<arpa/inet.h>` impedía compilar en Windows. Fix necesario para que el CI de Windows pase. Cambio de comportamiento nulo: la salida de `ToString()` es idéntica. |
| `BUILD_TESTS=ON` omite Transport/Brain/Server (no solo omite SFML) | Más limpio que hacer SFML opcional: los tests solo necesitan Shared y Core. Simplifica CI al no necesitar `libsfml-dev` en Windows. |
| `NetworkOptimizerTests` y `HeroSerializerTests` añadidos en este step | La pregunta estaba abierta en el handoff. Se decidió incluirlos aquí porque validan claims directos de la memoria (145 bits, 6cm precisión) y son independientes de P-3.5. |

---

## Fragmentos clave para revisar

### FetchContent GTest (tests/CMakeLists.txt)
```cmake
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
```

### MockTransport — inyección y captura
```cpp
bool Receive(std::vector<uint8_t>& buffer, Shared::EndPoint& sender) override {
    if (incomingQueue.empty()) return false;
    auto& pkt = incomingQueue.front();
    buffer = pkt.data;
    sender = pkt.sender;
    incomingQueue.pop();
    return true;
}

void Send(const std::vector<uint8_t>& buffer, const Shared::EndPoint& recipient) override {
    sentPackets.push_back({buffer, recipient});
}
```

### HeroSerializer — claim 145 bits
```cpp
TEST(HeroSerializer, FullState_BitCount_Is145) {
    // networkID(32) + dirtyMask(32) + pos(14+14) + health VLE(16) +
    // maxHealth VLE(16) + mana VLE(8) + level(5) + stateFlags(8) = 145
    HeroState s = MakeFullState(); // health=500, maxHealth=500, mana=100
    BitWriter w;
    HeroSerializer::Serialize(s, w);
    EXPECT_EQ(w.GetBitCount(), 145u);
}
```

### NetworkOptimizer — precisión 14-bit
```cpp
TEST(NetworkOptimizer, RoundTrip_14bits_PrecisionWithinOneStep) {
    // step = 1000 / 16383 ≈ 0.061m
    const float kMaxError = 1000.0f / ((1u << 14) - 1);
    for (float v : {-500.0f, -250.0f, 0.0f, 123.45f, 250.0f, 499.99f, 500.0f}) {
        uint32_t q       = NetworkOptimizer::QuantizeFloat(v, -500.0f, 500.0f, 14);
        float    restored = NetworkOptimizer::DequantizeFloat(q, -500.0f, 500.0f, 14);
        EXPECT_NEAR(restored, v, kMaxError);
    }
}
```

### NetworkAddress.h — ToString() cross-platform
```cpp
std::string ToString() const {
    return std::to_string( address        & 0xFF) + "." +
           std::to_string((address >>  8) & 0xFF) + "." +
           std::to_string((address >> 16) & 0xFF) + "." +
           std::to_string((address >> 24) & 0xFF) + ":" +
           std::to_string(port);
}
```

---

## Cobertura de tests por módulo

| Suite | Tests | Escenarios cubiertos |
|-------|-------|----------------------|
| `BitWriterReaderTests` | 12 | Round-trips 1/8/16/32 bits, múltiples campos, LSB ordering, sizing |
| `PacketHeaderTests` | 12 | Write/Read full round-trip, IsAcked (direct/bitmask/outside window/wrap-around), CurrentTimeMs |
| `SequenceContextTests` | 11 | AdvanceLocal (normal + wrap), RecordReceived (newer/duplicate/out-of-order/wrap-around/jump>32/too-old) |
| `NetworkOptimizerTests` | 14 | Quantize boundary/clamp, Dequantize boundary, round-trip 14-bit y 8-bit, VLE 0/127/128/16383/500 |
| `HeroSerializerTests` | 8 | Claim 145 bits validado, full round-trip, position-only (92 bits), precisión <6cm, empty mask (64 bits), health/stateFlags/level isolation |
| `NetworkManagerTests` | 15 | Handshake completo, salt incorrecto, NetworkID asignación, routing establecido/desconocido, duplicados, stale filter, GetClientNetworkStats, Send reliable/desconocido |

---

## Problemas encontrados en CI

| Problema | Causa | Fix |
|----------|-------|-----|
| `Cannot find source file: Log/Logger.h` (Ubuntu + Windows) | `.gitignore` tiene regla `[Ll]og/` del template de VS que silenciaba todo `Shared/Log/` | `git add -f Shared/Log/*.{h,cpp}` |
| `lcov: ERROR: mismatched end line` (Ubuntu coverage) | GCC 13 genera datos gcov con line numbers desajustados en cuerpos de test templados (macros GTest) | `--ignore-errors mismatch` en ambos comandos `lcov` |
| `BitWriter::GetBitsWritten()` no existe | El método real es `GetBitCount()`. Error al escribir los tests nuevos sin leer el header primero | Corregido en `NetworkOptimizerTests` y `HeroSerializerTests` |
| `BitReader(vector)` con un solo argumento | `GetCompressedData()` devuelve un temporal — hay que almacenarlo en una variable antes de pasarlo al constructor de `BitReader` | Todos los `BitReader r(w.GetCompressedData(), ...)` pasados a `auto data = w.GetCompressedData(); BitReader r(data, ...)` |

---

## Resultado CI

```text
Ubuntu (GCC 13):   100% tests passed, 0 tests failed out of 72 ✓
Windows (MSVC 19): 100% tests passed, 0 tests failed out of 72 ✓
Coverage upload:   lcov OK, Codecov recibiendo datos ✓
Total Test time:   ~1.1 sec
```

---

## Estado actual del sistema

- 72 tests verdes en CI (Ubuntu + Windows)
- Regresión completa activa: cualquier cambio futuro que rompa P-3.1 → P-3.4, NetworkOptimizer o HeroSerializer será detectado automáticamente
- Cualquier step a partir de P-3.5 **debe** incluir su suite de tests en el mismo PR

## Siguiente propuesta sugerida

P-3.5 — Delta Compression & Zig-Zag encoding

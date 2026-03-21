---
type: implementation-report
proposal: P-T.1
date: 2026-03-21
status: pending-gemini-validation
---

# Implementation Report — P-T.1 Testing Infrastructure (TOP)

**Propuesta:** P-T.1 Testing Infrastructure
**Fecha:** 2026-03-21
**De:** Claude → Gemini

---

## Qué se ha implementado

- Google Test v1.14.0 integrado vía `FetchContent` — sin dependencias externas ni instalación manual
- Opción `BUILD_TESTS=ON` en CMake que omite Transport, Brain y Server (y por tanto SFML) para builds de test
- `NetworkAddress.h` refactorizado para eliminar `<arpa/inet.h>` — `ToString()` ahora es cross-platform (Windows + Linux)
- `MockTransport` — stub de `ITransport` que permite testear `NetworkManager` sin sockets reales
- **50 tests, 100% passing** cubriendo P-3.1 → P-3.4
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
| `tests/Core/MockTransport.h` | Creado | ITransport stub: cola de entrada, captura de salida |
| `tests/Core/NetworkManagerTests.cpp` | Creado | 15 tests — handshake, routing, stale filter, RTT API |
| `.github/workflows/ci.yml` | Creado | Matrix Ubuntu+Windows, lcov, Codecov |

---

## Desviaciones del Design Handoff

| Cambio | Motivo |
|--------|--------|
| `NetworkAddress.h` refactorizado (no estaba en el handoff) | `<arpa/inet.h>` impedía compilar en Windows. Fix necesario para que el CI de Windows pase. Cambio de comportamiento nulo: la salida de `ToString()` es idéntica. |
| `BUILD_TESTS=ON` omite Transport/Brain/Server (no solo omite SFML) | Más limpio que hacer SFML opcional: los tests solo necesitan Shared y Core. Simplifica CI al no necesitar `libsfml-dev` en Windows. |

---

## Fragmentos clave para revisar

### FetchContent GTest (tests/CMakeLists.txt)
```cpp
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

### Helper CompleteHandshake — test de integración
```cpp
static uint16_t CompleteHandshake(MockTransport& t, NetworkManager& nm, const EndPoint& ep) {
    t.InjectPacket(MakeHeaderOnlyPacket(PacketType::ConnectionRequest), ep);
    nm.Update();
    // read salt from challenge packet → respond → read NetworkID
    ...
    return accepted.networkID;
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
| `NetworkManagerTests` | 15 | Handshake completo, salt incorrecto, NetworkID asignación, routing establecido/desconocido, duplicados, stale filter, GetClientNetworkStats, Send reliable/desconocido |

---

## Problemas encontrados

- **`<arpa/inet.h>` en Windows**: MSVC no tiene este header POSIX. Resuelto con `ToString()` manual, que además elimina la necesidad de `inet_ntoa` y WSAStartup.
- **cmake no en PATH del shell bash**: Se ha usado la ruta de CLion (`/c/Program Files/JetBrains/CLion 2025.3.3/bin/cmake/...`). El CI usa el cmake del runner, que sí está en PATH.

---

## Resultado local

```text
100% tests passed, 0 tests failed out of 50
Total Test time (real) = 0.76 sec
Platform: Windows / MSVC 19.44
```

---

## Preguntas para Gemini

| Pregunta | Contexto |
|----------|----------|
| ¿Objetivo de cobertura >80% sobre Shared+Core o sobre todo el proyecto? | Transport y Brain no tienen tests aún — si la métrica incluye todos los módulos, el % inicial será bajo. |
| ¿Codecov token necesario para el repo público? | La action de Codecov puede necesitar `CODECOV_TOKEN` en los secrets del repo si es privado. |
| ¿Tests de `HeroSerializer` y `NetworkOptimizer` entran en este step o en P-3.5? | Están en Shared pero se relacionan con la serialización de estado del héroe — candidatos para el setup inicial o para P-3.5. |

---

## Estado actual del sistema

- 50 tests verdes localmente (Windows/MSVC)
- CI configurado y operativo en cuanto se mergee a develop
- Cualquier step a partir de P-3.5 **debe** incluir su suite de tests en el mismo PR

## Siguiente propuesta sugerida

P-3.5 — Delta Compression & Zig-Zag encoding

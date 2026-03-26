# Report Templates

Reference for writing IR and DL files consistently.
Claude: read this file instead of old reports when writing a new IR or DL.

---

## IR Template — Implementation Report

```markdown
# IR-X.Y — [Feature Name]

**Branch:** `branch-name`
**Date:** YYYY-MM-DD
**Tests:** NNN / NNN passing (N new)

---

## What was implemented

[2-4 paragraph description of the feature. Cover: what problem it solves,
the design chosen, and why. Mention any non-obvious trade-offs.]

---

## Modified files

### `Module/File.h` / `Module/File.cpp`

**Added:**

| Symbol | Description |
|--------|-------------|
| `ClassName` | What it does |
| `methodName(params)` | What it does |

**Modified:**
- `existingMethod()` — what changed and why

### `Module/OtherFile.cpp`

[Repeat per file. Only list files with meaningful changes.]

---

## New tests (N)

| Test | What it guards |
|------|---------------|
| `TestName` | One-line description of the invariant |

---

## Benchmark results (if applicable)

| Scenario | Metric A | Metric B | Metric C |
|----------|----------|----------|----------|
| Description | value | value | value |

[Add a note explaining any surprising numbers.]
```

---

## DL Template — Dev Log

```markdown
# DL-X.Y — [Feature Name]

**Date:** YYYY-MM-DD
**Branch:** `branch-name`
**Tests:** NNN → NNN (N nuevos)

---

## El punto de partida

[1-2 párrafos. Contexto: qué había implementado antes, qué problema
motivó esta fase, qué se esperaba encontrar.]

---

## [Decisión o problema principal]

### Diagnóstico / Contexto

[Describe el problema o la pregunta de diseño con detalle. Si hay código
relevante, incluye un snippet corto. Explica qué hacía el sistema antes
y por qué era insuficiente o incorrecto.]

```cpp
// Código relevante — antes / después / fragmento clave
```

### Solución / Decisión

[Explica la solución elegida. Si había alternativas, menciona brevemente
por qué se descartaron. Incluye el diseño final con pseudocódigo o
estructura de datos si ayuda.]

---

## [Segunda decisión o problema, si aplica]

[Misma estructura: contexto → diagnóstico → solución.]

---

## Tests nuevos

| Test | Lo que valida |
|------|---------------|
| `TestName` | Invariante que cubre |

[Si algún test es el "test de regresión directo" de un bug, señálalo explícitamente.]

---

## Resultados

| | Antes | Después | Mejora |
|---|---|---|---|
| **Métrica A** | valor | valor | Nx |
| **Tests** | NNN | NNN | +N |

---

## Lección

[1 párrafo. La conclusión que debe quedar. Qué cambiaría en retrospectiva,
qué invariante se aprendió, o qué patrón hay que vigilar en fases futuras.]
```

---

## Phase-level DL Template

```markdown
# DL-Fase X — [Phase Name]

**Período:** YYYY-MM-DD – YYYY-MM-DD
**Steps:** P-X.1 · P-X.2 · P-X.3 · ...
**Tests al cierre:** NNN / NNN

---

## Visión de la fase

[2-3 párrafos. Qué problema resolvía esta fase en conjunto,
qué arquitectura se estableció, y cómo se relaciona con las fases anteriores
y posteriores del roadmap.]

---

## Hilo conductor

[Narrative que conecta los steps. Explica cómo cada uno construyó sobre el anterior.
Menciona los puntos de inflexión (decisiones que cambiaron el rumbo).]

---

## Step por step

### P-X.1 — [Name]
[3-5 líneas. La decisión de diseño más importante y su justificación.]

### P-X.2 — [Name]
[3-5 líneas.]

[...]

---

## Métricas de cierre

| Métrica | Valor |
|---------|-------|
| Tests | NNN / NNN |
| Tick budget | X.X% |
| Delta Efficiency | XX% |
| [otra métrica relevante] | valor |

---

## Lecciones de fase

- **[Patrón 1]:** descripción
- **[Patrón 2]:** descripción
```

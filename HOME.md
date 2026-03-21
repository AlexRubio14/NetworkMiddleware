# NetworkMiddleware — Dashboard

## Estado del proyecto

**Fase activa:** Fase 3 — Netcode Core y Protocolo de Sesión
**Fases completadas:** 1 & 2 (18-03-2026)
**Siguiente sesión con Gemini:** Diseñar propuestas de Fase 3

---

## Documentos principales

| Documento | Descripción |
|-----------|------------|
| [[DEVELOPMENT_MEMORY]] | Decisiones reales tomadas — lo que se comparte con Gemini |
| [[DESIGN_PROPOSALS]] | Índice de propuestas pendientes |
| [[HANDOFF_TEMPLATES]] | Templates de traspaso Gemini ↔ Claude |
| [[CLAUDE]] | Guía para Claude Code |

---

## Propuestas pendientes — Fase 3 (sprint activo)

```dataview
TABLE title, status, depends_on
FROM "proposals"
WHERE phase = 3
SORT id ASC
```

---

## Propuestas pendientes — Fase 4

```dataview
TABLE title, status
FROM "proposals"
WHERE phase = 4
SORT id ASC
```

---

## Propuestas pendientes — Fase 5

```dataview
TABLE title, status
FROM "proposals"
WHERE phase = 5
SORT id ASC
```

---

## Propuestas pendientes — Fase 6

```dataview
TABLE title, status
FROM "proposals"
WHERE phase = 6
SORT id ASC
```

---

## Documentación pendiente (académico)

```dataview
TABLE title, status
FROM "proposals"
WHERE phase = 0
SORT id ASC
```

---

## Memoria interna de Claude

```dataview
TABLE type, description
FROM "claude-memory"
SORT file.name ASC
```

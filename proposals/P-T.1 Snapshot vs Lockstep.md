---
id: P-T.1
title: Justificación Snapshot vs Lockstep (para la Memoria)
phase: 0
status: pending
blocks: []
depends_on: []
---

# P-T.1 — Justificación del modelo de red

**Tipo:** Documentación académica (no implementación)
**Dónde va:** Capítulo de Disseny de l'Arquitectura de la Memoria

## Problema

La decisión de usar Snapshot-based en lugar de Lockstep o Semi-determinístico no está justificada explícitamente en ningún documento. La comisión puede preguntarlo.

## Justificación a documentar

| Modelo | Por qué no |
|--------|-----------|
| Lockstep | Requiere lógica de juego determinística → incompatible con engine-agnostic. Un packet loss = todos esperan. |
| Semi-determinístico (LoL) | Óptimo para MOBA pero requiere código determinístico. Fuera del alcance del TFG. |
| **Snapshot (elegido)** | Servidor = única fuente de verdad. Compatible con engine-agnostic y la arquitectura propuesta. |

## Acción pendiente

- [ ] Añadir esta justificación al capítulo de arquitectura de la Memoria

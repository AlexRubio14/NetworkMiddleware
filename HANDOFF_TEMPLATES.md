# Handoff Templates
## Flujo de colaboración Gemini ↔ Claude

---

## TEMPLATE 1: Design Handoff (Gemini → Claude)

Gemini usa esta plantilla al final de una sesión de diseño.
Claude la recibe, filtra, y decide si implementar o pedir cambios.

```markdown
## DESIGN HANDOFF
**Propuesta:** [ID de DESIGN_PROPOSALS.md — ej: P-3.2]
**Fase:** [Fase del roadmap]
**Fecha:** [fecha]

### Decisiones tomadas
<!-- Lista de decisiones concretas. No razonamiento, solo qué se decide. -->
- Decisión 1: ...
- Decisión 2: ...

### Diseño técnico
<!-- Estructuras de datos, interfaces, flujo de llamadas — lo suficiente para implementar -->

### Archivos a crear o modificar
<!-- Lista de archivos afectados -->
- Crear: ...
- Modificar: ...

### Qué NO entra en esta iteración
<!-- Límites explícitos del scope -->

### Preguntas abiertas (si las hay)
<!-- Cosas que no se han podido decidir aún -->

### Qué debe revisar Gemini después
<!-- Lo que Claude debe reportar en el Implementation Report para que Gemini lo valide -->
```

---

## TEMPLATE 2: Implementation Report (Claude → Gemini)

Claude usa esta plantilla al terminar de implementar.
El usuario la pega a Gemini para revisión.

```markdown
## IMPLEMENTATION REPORT
**Propuesta implementada:** [ID — ej: P-3.2]
**Fecha:** [fecha]

### Qué se ha implementado
<!-- Lista concreta de lo que se ha hecho -->
- ...

### Desviaciones del Design Handoff
<!-- Si Claude tuvo que cambiar algo del diseño al implementar, explicar por qué -->
- Cambio: ... | Motivo: ...

### Archivos modificados
<!-- Lista exacta de archivos creados o modificados -->
- Creado: `ruta/archivo.h` — descripción breve
- Modificado: `ruta/archivo.cpp` — qué cambió

### Fragmentos clave para revisar
<!-- El código más importante que Gemini debe leer y validar -->

### Problemas encontrados durante implementación
<!-- Si algo del diseño fue difícil o imposible de implementar exactamente como se diseñó -->

### Preguntas para Gemini
<!-- Dudas que surgieron durante la implementación que necesitan decisión de diseño -->

### Estado actual del sistema
<!-- Qué funciona ahora, qué queda pendiente para la siguiente iteración -->

### Siguiente propuesta sugerida
<!-- Qué debería diseñarse en la próxima sesión con Gemini -->
```

---

## Mensaje de inicio para Gemini (copiar al principio de cada sesión)

```
Contexto del proyecto: [adjunta DEVELOPMENT_MEMORY.md]
Propuestas pendientes: [adjunta DESIGN_PROPOSALS.md si es relevante]

Somos Gemini y Claude trabajando en colaboración en este proyecto.
Tú diseñas, Claude implementa, tú revisas lo implementado.

Sesión de hoy: diseñar [nombre de la propuesta].
Al terminar, dame un Design Handoff document con el template de HANDOFF_TEMPLATES.md.
```

---

## Mensaje de inicio para Claude (copiar al principio de cada sesión de implementación)

```
Aquí el Design Handoff de la sesión con Gemini:
[pega el Design Handoff]

Filtra el diseño, dime si hay algo que no encaje con el código actual,
y si todo está bien, implementa. Al terminar, dame el Implementation Report.
```

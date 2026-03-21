---
type: dev-log-alex
proposal: <% tp.system.prompt("ID de propuesta (ej: P-3.1)") %>
date: <% tp.date.now("YYYY-MM-DD") %>
status: personal
---

# DEV LOG — <% tp.system.prompt("Título") %>

**Propuesta:** [[proposals/<% tp.system.prompt("ID de propuesta") %>]]
**Fecha:** <% tp.date.now("YYYY-MM-DD") %>

---

## ¿Qué problema resolvíamos?

<!-- Explicación del problema en lenguaje llano. Por qué era necesario implementar esto. -->

---

## ¿Qué hemos construido?

<!-- Lista de lo que existe ahora que antes no existía. Sin tecnicismos todavía. -->

-
-
-

---

## Cómo funciona — explicado paso a paso

<!-- Explicación narrativa del flujo completo, como si se lo explicaras a alguien
     que sabe programar pero no sabe nada de networking. -->

### Paso 1 — ...

### Paso 2 — ...

### Paso 3 — ...

---

## El código clave

<!-- Los fragmentos más importantes con comentarios línea a línea explicando QUÉ hace
     cada parte y POR QUÉ se hace así y no de otra manera. -->

```cpp
// Explicación de qué hace este bloque

```

---

## Conceptos nuevos que aparecen aquí

<!-- Glosario de términos técnicos que aparecen en esta implementación.
     Definición simple + por qué importa en este proyecto. -->

| Concepto | Qué es | Por qué lo usamos |
|----------|--------|-------------------|
| | | |

---

## Cómo encaja en el sistema completo

<!-- Diagrama de texto o descripción de cómo este módulo se conecta con el resto.
     Qué llama a qué, qué datos entran y salen. -->

```
[Módulo A] → [Lo que acabamos de implementar] → [Módulo B]
```

---

## Decisiones de diseño que tomamos (y por qué)

<!-- Las elecciones no obvias: por qué esta estructura y no otra, por qué este
     número, por qué este algoritmo. -->

| Decisión | Alternativa descartada | Motivo |
|----------|------------------------|--------|
| | | |

---

## Qué podría salir mal (edge cases)

<!-- Situaciones límite que el código maneja, o que hay que tener en cuenta. -->

-
-

---

## Qué aprender si quieres profundizar

<!-- Referencias opcionales para entender mejor los conceptos. -->

-
-

---

## Estado del sistema tras esta implementación

<!-- Qué funciona ahora, qué queda pendiente, qué ha cambiado respecto al estado anterior. -->

**Funciona:**
-

**Pendiente (próxima propuesta):**
-

# Crate Decks

Crate Decks es un programa de DJ de escritorio que usa un servidor Crate como biblioteca y cerebro: biblioteca remota, análisis de Crate, recomendaciones y Smart Mix en los decks.

Es un repositorio independiente basado en el código de [Mixxx](https://mixxx.org).

- **Qué y por qué:** [diseño](docs/plans/2026-11-02-crate-decks-design.md).
- **Cómo y en qué orden:** [plan de implementación](docs/plans/2026-11-02-crate-decks-implementation-plan.md).
- **Reglas de trabajo:** [AGENTS.md](AGENTS.md).

## Relación con Mixxx

- **Origen:** Mixxx `main` en `bc49961` (2.7-alpha). La rama `upstream` apunta a esa base y solo sirve para traer cambios de Mixxx cuando se decide. Cómo hacerlo: [docs/mixxx-merges.md](docs/mixxx-merges.md).
- **Una sola dirección:** no se envía nada a Mixxx (ni PRs, ni issues, ni comentarios). Los fallos de Mixxx que encontremos se arreglan aquí.
- **Ficheros de Mixxx que cambiamos:** [CRATE_PATCHES.md](CRATE_PATCHES.md).
- **Licencia:** GPL-2.0-or-later, como Mixxx ([LICENSE](LICENSE)). Crate Decks se distribuye con su código fuente, y el código de Crate que se compila en Decks también es GPL. El servidor Crate no queda afectado: habla con Decks por HTTP.
- **Créditos:** Mixxx es obra de sus contribuidores. Se conservan los avisos de copyright y los créditos de Mixxx en *About*. El nombre y los logos de Mixxx no se usan en el producto, salvo en los créditos.

## Fallos y sugerencias

Se reportan en este repositorio, nunca en el de Mixxx.

## Compilar

Dependencias, compilación y tests: [AGENTS.md §7](AGENTS.md). Estilo de código y detalles de compilación de Mixxx: [CONTRIBUTING.md](CONTRIBUTING.md).

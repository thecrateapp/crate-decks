# Crate Decks

[![Build](https://github.com/thecrateapp/crate-decks/actions/workflows/develop.yml/badge.svg?branch=main)](https://github.com/thecrateapp/crate-decks/actions/workflows/develop.yml?query=branch%3Amain)

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
- **Iconos:** Solar Icons Set de 480 Design, bajo CC BY 4.0, extraídos del paquete `@solar-icons/react` (MIT). Licencias y atribución en `tools/crate-design/svg/solar/`.

## Fallos y sugerencias

Se reportan en este repositorio, nunca en el de Mixxx.

## Compilar

Dependencias, compilación y tests: [AGENTS.md §7](AGENTS.md). Estilo de código y detalles de compilación de Mixxx: [CONTRIBUTING.md](CONTRIBUTING.md).

El ejecutable es `crate-decks`. En Linux guarda los ajustes en `~/.cratedecks/` (o en la carpeta que se le pase con `--settingsPath`), así que no comparte `mixxx.cfg` ni `mixxxdb.sqlite` con un Mixxx instalado.

## Versión

- **Tags:** los de Crate Decks son `decks-vX.Y.Z`, y los crea Diego (AGENTS.md §2). El build solo mira esos (`git describe --match 'decks-v*'`); los tags de Mixxx que trae la historia no cuentan.
- **Versión del build:** desde un tag, la del tag (`0.1.0`, o `0.1.0-5-g<commit>` cinco commits después). Sin ningún tag, la versión de producto (`crate::kApplicationVersion`) más el commit: `0.1.0-dev+g<commit>`. Con cambios sin commit se añade `-modified`.
- **Dónde se ve:** en el nombre de la AppImage y de los paquetes, en `--version` y en *About*. La versión de Mixxx de la base solo sale en *About* ("based on Mixxx 2.7.0-alpha"), y `mixxx.cfg` sigue guardando la de Mixxx como versión de los ajustes.

## CI

Cada push y cada PR ejecutan `.github/workflows/develop.yml`, que llama a:

- **`crate-decks-build.yml`** (solo Linux):
  - **Ubuntu 24.04:** build y todos los tests de `mixxx-test` con `ctest`, sin excluir ninguno.
  - **AppImage x86_64:** se construye en Ubuntu 22.04, como la de Mixxx, para que funcione en más distribuciones. Es un build de desarrollo: se descarga como artefacto del workflow (14 días) y no se publica como release.
- **`pre-commit.yml`:** en los PRs, los hooks de `.pre-commit-config.yaml` sobre los ficheros cambiados; clang-format, solo sobre las líneas cambiadas. codespell no revisa los documentos en español y markdownlint no revisa `docs/plans/`, porque los reescribiría.
- **`checks.yml`:** clazy, clang-tidy y cobertura.

Lo mismo en local:

```shell
ctest --test-dir build --timeout 45             # tests como en el CI, un proceso por test
pre-commit run --from-ref main --to-ref HEAD   # hooks sobre lo cambiado desde main
```

`pre-commit` aplica los arreglos de los hooks, también el `fix: true` de `.markdownlint-cli2.yaml` (markdownlint-cli2 0.23 no tiene `--no-fix`): revisa el diff antes de hacer commit.

La AppImage del CI necesita el entorno de Mixxx de `tools/appimage_release_buildenv.sh`, con su Qt estática. Con las librerías del sistema, `cpack -G AppImage` sirve para revisar el empaquetado (`.desktop`, icono y ejecutable), pero esa AppImage no arranca: no lleva los plugins de plataforma de Qt.

## Tests

En la máquina de desarrollo (CachyOS, Qt 6.12), `build/mixxx-test` falla en tests que en el CI pasan:

| Test | Causa |
| --- | --- |
| `ControllerScriptEngineLegacyTimerTest.beginTimer_singleShot{BindFunction,ArrowFunction,BindFunctionInClass,ArrowFunctionInClass}` | Qt 6.12; el CI usa la Qt 6.4 de Ubuntu 24.04 |
| `TrackMetadataExportTest.keepWithespaceKey` | Solo cuando todos los tests van en un proceso; con `ctest` pasa |
| `QmlSkins/QmlStartupSmokeTest.Starts/NewUi` | Violación de segmento solo con todos los tests en un proceso; tumba el resto de la batería. Para pasarla entera: `build/mixxx-test --gtest_filter='-QmlSkins/QmlStartupSmokeTest.Starts/NewUi'` |

Cualquier otro fallo es nuevo.

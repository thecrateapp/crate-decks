# AGENTS.md — Crate Decks

Crate Decks es un fork de [Mixxx](https://github.com/mixxxdj/mixxx) que usa un servidor Crate como biblioteca y cerebro: biblioteca remota, análisis de Crate, recomendaciones y Smart Mix en los decks.

- **Qué y por qué:** [diseño de Crate Decks](docs/plans/2026-11-02-crate-decks-design.md).
- **Cómo y en qué orden:** [plan de implementación de Crate Decks](docs/plans/2026-11-02-crate-decks-implementation-plan.md).
- **Smart Mix 3** (solo servidor, en el monorepo de Crate): [diseño](docs/plans/2026-11-02-smart-mix-3-design.md) y [plan](docs/plans/2026-11-02-smart-mix-3-implementation-plan.md).
- **Compilar, estilo de código y flujo de Git de Mixxx:** [CONTRIBUTING.md](CONTRIBUTING.md). Se aplica tal cual salvo en lo que este fichero diga otra cosa.

## 1. Tres repositorios, tres políticas

| Repositorio | Qué se hace ahí | Política para agentes |
| --- | --- | --- |
| `thecrateapp/crate-decks` (este) | Cliente de escritorio | Este fichero |
| `mixxxdj/mixxx` (upstream) | Nada directamente. Solo hooks genéricos que una persona decide proponer | La de upstream, copiada literalmente en §9 |
| `thecrateapp/crate` (monorepo) | Cambios de servidor S1–S7 y todo Smart Mix 3 | Su propio `AGENTS.md` |

Ningún cambio de servidor se hace desde este repositorio.

## 2. Qué puede hacer un agente en este repositorio

**Sin preguntar:**

- Leer código, compilar, ejecutar los tests y arrancar la app (siempre con `--settingsPath` desechable, §7).
- Editar código y documentación dentro del alcance de la tarea.
- Crear ramas `feat/decks-<tema>` o `fix/decks-<tema>` y hacer commits locales en ellas.

**Solo si Diego lo pide de forma explícita:**

- `git push`, abrir o actualizar PRs, comentar en PRs o issues y crear issues.
- Instalar paquetes del sistema (`tools/*_buildenv.sh setup` o cualquier `sudo`).
- Crear tags o releases.

**Nunca:**

- Hacer merge a `main` o a `upstream`, ni force-push a ramas compartidas.
- Dar por superado un gate humano (§4).
- Abrir, comentar o actualizar nada en `mixxxdj/mixxx`: ni PRs, ni issues, ni reviews.
- Usar el servidor de producción de Crate o sus credenciales.
- Mergear la rama `spike/crate-decks`: su código es desechable.

**Prueba humana antes del merge:** los cambios que afectan al audio, al motor, a AutoDJ, a Smart Mix o a las controladoras los prueba Diego con un setup de DJ real antes del merge. El PR dice qué hay que probar y cómo.

## 3. Texto generado por IA

- **En este fork:** commits, comentarios y documentación van sin atribución ni aviso de IA. Diego revisa el texto y lo asume como propio.
- **Hacia upstream:** todo texto que vaya a `mixxxdj/mixxx` cumple la política de §9, incluido el aviso al principio y al final del texto generado por un agente.

## 4. Planes, decisiones y gates

- Las decisiones se citan por su ID: D1–D15 y S1–S7 (Crate Decks), SM3-D1–D12 (Smart Mix 3).
- Nada marcado como *pendiente* de un gate se implementa sin la aprobación explícita de Diego.
- **Gates:** G0–G6, G3b, G4b, GU1 y GU2 (§13 del diseño de Decks); SG0–SG4 (§13 del diseño de Smart Mix 3). Al llegar a uno, el agente para, presenta lo que pide el gate y espera.
- Las fases se ejecutan en el orden del plan, y cada tarea empieza por el test en rojo que indica.
- **Si el código no coincide con el plan** (rutas, números de línea o APIs que no existen o se comportan de otra forma): para, informa y propone el cambio al plan. No te desvíes en silencio. Los números de línea de los planes son de Mixxx `bc49961`; revalídalos antes de usarlos.
- Los planes viven en `docs/plans/` y se cambian por PR, como cualquier otro fichero. Los informes (spike y evaluaciones) también van a `docs/plans/`.

## 5. Arquitectura

### 5.1. Mixxx

- **ControlObject/ControlProxy:** comunicación entre componentes con `[Group], key_name`.
- **Hilo del motor:** audio en tiempo real, sin reservas de memoria ni locks. Puede emitir señales de Qt, pero no recibirlas.
- **`parented_ptr`/`make_parented`:** propiedad en el árbol de objetos de Qt. El objeto debe tener padre antes de que se destruya el `parented_ptr`.

### 5.2. Reglas de Crate Decks

- **Código propio aislado** en `src/crate/`, `src/library/crateserver/`, `src/test/crate/`, `res/skins/CrateDecks/`, `res/crate-design/` y `tools/crate-design/`, detrás de la opción de CMake `CRATE`.
- **Parches a ficheros de Mixxx:** la lista cerrada está en §5.2 del diseño. Cualquier otro necesita justificación en el PR. Desde F1, todo PR que toque un fichero de Mixxx actualiza `CRATE_PATCHES.md`. Se prefieren señales, slots y hooks a reescribir lógica de upstream.
- **Nombres:** solo se cambian las cadenas visibles (título, About, asistente de primer arranque y preferencias). Los identificadores, grupos de control, claves de configuración y nombres de fichero con "Mixxx" no se tocan.
- **Base de datos:** las tablas propias llevan el prefijo `crateserver_` y se crean de forma idempotente al arrancar. No se modifica `res/schema.xml` ni ninguna tabla de Mixxx.
- **Hilos:**
  - La red va por `crate::Client`, asíncrona con Qt. El hilo principal nunca bloquea más de 16 ms, y el parseo pesado va a `QtConcurrent`.
  - El `SmartMixDirector` corre en el hilo principal con `Qt::PreciseTimer`, nunca en el hilo del motor, salvo que G0 decida otra cosa.
- **Secretos:** solo en QtKeychain. Nunca en `mixxx.cfg`, logs, URLs, tests ni commits. Las descargas usan la cabecera `Authorization`.
- **Interfaz:**
  - La lógica de Crate vive en modelos sin `QWidget` (`QAbstractItemModel` u objetos con `Q_PROPERTY`), y las vistas son una capa fina encima.
  - Desde V1, ningún color, tamaño, radio ni fuente se escribe a mano en QSS, XML de skin ni C++: todo sale de `res/crate-design/tokens.json` y su generador.
  - Iconos: solo Solar. Nada de `QGraphicsEffect`.

## 6. Estilo, tests y commits

- **Estilo:** el de Mixxx ([CONTRIBUTING.md](CONTRIBUTING.md)): C++20, Qt6, clang-format con pre-commit y `kLogger` por fichero.
- **Idioma:** código, comentarios y mensajes de commit en inglés, como upstream. Planes y documentación de Crate en español.
- **Tests:**
  - GoogleTest en `src/test/crate/`, añadidos a mano a la lista de fuentes de `mixxx-test` en `CMakeLists.txt` (no hay glob).
  - Red simulada con `src/test/mock_networkaccessmanager.h` o con un `QNetworkAccessManager` inyectado. Nunca se depende de un servidor real en un test unitario.
  - Lo que no se pueda testear lleva `TEST_GAP:` y el motivo.
- **Commits:** conventional commits (`feat(decks): …`, `fix(crateserver): …`), primera línea de menos de 72 caracteres en modo imperativo y cuerpo con el porqué. Cada commit compila. Los refactors y los cambios de formato van en commits separados.

## 7. Compilar y ejecutar

```shell
# Dependencias: instalan paquetes con sudo, así que se pide confirmación antes
tools/archlinux_buildenv.sh setup   # Arch y CachyOS
tools/debian_buildenv.sh setup      # Debian y Ubuntu

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j"$(nproc)"
build/mixxx-test --gtest_filter='<Suite>.*'
build/mixxx --settingsPath "$(mktemp -d)"   # nunca contra ~/.mixxx
```

Hasta F1 el ejecutable se llama `mixxx`; a partir de F1, `crate-decks`.

## 8. Servidor Crate

- Nunca se usa producción: solo el stack dev del monorepo (`make dev`) o el servidor de pruebas que indique Diego.
- En F0, solo tokens `crv_` del stack dev, por variables de entorno (`CRATE_URL`, `CRATE_TOKEN`) y nunca commiteados.
- Si una tarea del cliente necesita un cambio de servidor que no está en S1–S7, para y avisa: primero se añade al plan.

## 9. Upstream

- La rama `upstream` sigue a `mixxxdj/mixxx` sin cambios, y `main` es `upstream` más los parches de Crate (desde F1).
- Los conflictos de la sincronía en ficheros parcheados los revisa Diego.
- Este fichero también es un parche. En cada sincronía, si upstream cambia su `AGENTS.md`, se actualiza la copia de abajo y se revisa si afecta a §2 y §3.

### 9.1. Política de IA de upstream

Copia literal del `AGENTS.md` de `mixxxdj/mixxx` en `bc49961`, con los títulos bajados de nivel. Se aplica a todo lo que se envíe a `mixxxdj/mixxx`. No se edita salvo al sincronizar.

> #### AI Agent Policy
>
> > **Important:** The Mixxx project only accepts contributions driven by human developers.
> > Automated AI agents acting autonomously on a user's behalf are not welcome and may result
> > in pull requests being closed without comment.
> > But using AI tools that **assist** the human contributor with code generation, refactoring,
> > documentation and reviewing the work before submission is welcome.
>
> ##### No Autonomous Pull Requests
>
> AI agents **must not** open, update, or re-open pull requests autonomously on behalf of a user.
> All pull requests must be explicitly reviewed, approved, and submitted by the human developer
> themselves. An agent may prepare and stage changes locally, but `git commit`, `git push`, and PR
> creation must be deliberate human actions. Before this, a test of the code change by a human,
> using a real DJ setup, is required.
>
> ##### No Automated Responses to PR Review Comments
>
> AI agents **must not** post replies to review comments, questions, or change requests left on
> a pull request or its commits. All communication in the PR thread must come directly from the human contributor.
>
> ##### AI Code-Reviews
>
> AI agents **may** and **should** perform code reviews on explicitly request in scope of the PR.
>
> ##### No Autonomous Creation of Issues, Bug Reports or Feature Requests
>
> AI agents **must not** open, update, or re-open Issues, Bug Reports or Feature Requests autonomously
> or on behalf of a user.
> All Issues, Bug Reports, and Feature Requests must be explicitly reviewed, approved, and submitted by
> the human user themselves.
>
> ##### AI-Generated Text Must Carry a Disclaimer
>
> Any text (PR description, commit message body, code comments, documentation) that was written
> autonomously by an AI Agent **must** be framed with an disclaimer that the text is autonomously generated
> by the AI Agent, at both the start and the end of that text block.

## 10. Estructura

```text
src/                      C++ de Mixxx (engine/, controllers/, library/, mixer/, effects/, qml/, preferences/, util/, test/)
src/crate/                Código de Crate: cliente, sesión, caché, análisis, Smart Mix, escritura de vuelta y UI
src/crate/spike/          Solo en F0 y en la rama spike/crate-decks; desechable
src/library/crateserver/  Feature, modelos y colección de Crate
src/test/crate/           Tests de Crate
res/                      Recursos de Mixxx (controllers/, skins/, qml/)
res/skins/CrateDecks/     Skin propia (desde V2)
res/crate-design/         tokens.json (desde V1)
tools/                    Scripts de Mixxx; tools/crate-design/ es el generador de tokens (desde V1)
docs/plans/               Diseños, planes e informes de Crate Decks
cmake/                    Módulos de CMake
```

Los directorios de Crate se crean en su fase; hasta entonces no existen.

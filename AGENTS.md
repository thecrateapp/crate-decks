# AGENTS.md — Crate Decks

Crate Decks es un producto independiente basado en el código de [Mixxx](https://github.com/mixxxdj/mixxx) (GPL-2.0-or-later) que usa un servidor Crate como biblioteca y cerebro: biblioteca remota, análisis de Crate, recomendaciones y Smart Mix en los decks.

- **Qué y por qué:** [diseño de Crate Decks](docs/plans/2026-11-02-crate-decks-design.md).
- **Cómo y en qué orden:** [plan de implementación de Crate Decks](docs/plans/2026-11-02-crate-decks-implementation-plan.md).
- **Smart Mix 3** (solo servidor, en el monorepo de Crate): [diseño](docs/plans/2026-11-02-smart-mix-3-design.md) y [plan](docs/plans/2026-11-02-smart-mix-3-implementation-plan.md).
- **Compilar, estilo de código y flujo de Git de Mixxx:** [CONTRIBUTING.md](CONTRIBUTING.md). Se aplica tal cual salvo en lo que este fichero diga otra cosa.

## 1. Repositorios

| Repositorio | Qué se hace ahí | Política para agentes |
| --- | --- | --- |
| `thecrateapp/crate-decks` (este) | Cliente de escritorio | Este fichero |
| `mixxxdj/mixxx` (origen del código) | Nada. Solo se leen sus cambios para traerlos (§9) | No se publica nada allí |
| `thecrateapp/crate` (monorepo) | Cambios de servidor S1–S7 y todo Smart Mix 3 | Su propio `AGENTS.md` |

Ningún cambio de servidor se hace desde este repositorio.

## 2. Qué puede hacer un agente en este repositorio

**Sin preguntar:**

- Leer código, compilar, ejecutar los tests y arrancar la app (siempre con `--settingsPath` desechable, §7).
- Editar código y documentación dentro del alcance de la tarea.
- Crear ramas `feat/decks-<tema>` o `fix/decks-<tema>` y hacer commits locales en ellas.

**Solo si Diego lo pide de forma explícita:**

- `git push`, abrir, actualizar y mergear PRs a `main` (con merge commit y el CI en verde), comentar en PRs o issues y crear issues.
- Instalar paquetes del sistema (`tools/*_buildenv.sh setup` o cualquier `sudo`).
- Crear tags o releases.

**Nunca:**

- Hacer merge a `upstream` ni force-push a ramas compartidas.
- Dar por superado un gate humano (§4).
- Abrir, comentar o actualizar nada en `mixxxdj/mixxx`: ni PRs, ni issues, ni reviews. No se contribuye a Mixxx (§9).
- Usar el servidor de producción de Crate o sus credenciales.
- Mergear la rama `spike/crate-decks`: su código es desechable.

**Evidencia antes del merge:** no hay prueba con un setup de DJ real. Los cambios que afectan al audio, al motor, a AutoDJ, a Smart Mix o a las controladoras llevan evidencia offline (clips A/B, métricas de escucha, capturas o pruebas headless de la app real), y el PR dice qué puede comprobar Diego en su escritorio y cómo.

## 3. Texto generado por IA

- Commits, comentarios y documentación van sin atribución ni aviso de IA. Diego revisa el texto y lo asume como propio.
- No se envía texto a `mixxxdj/mixxx` (§9).

## 4. Planes, decisiones y gates

- Las decisiones se citan por su ID: D1–D15 y S1–S7 (Crate Decks), SM3-D1–D13 (Smart Mix 3).
- Nada marcado como *pendiente* de un gate se implementa sin la aprobación explícita de Diego.
- **Gates:** G0–G6, G3b, G4b, GU1 y GU2 (§13 del diseño de Decks); SG0–SG4 y SG1b (§13 del diseño de Smart Mix 3). Al llegar a uno, el agente para, presenta lo que pide el gate y espera.
- Las fases se ejecutan en el orden del plan, y cada tarea empieza por el test en rojo que indica.
- **Si el código no coincide con el plan** (rutas, números de línea o APIs que no existen o se comportan de otra forma): para, informa y propone el cambio al plan. No te desvíes en silencio. Los números de línea de los planes son de Mixxx `bc49961`; revalídalos antes de usarlos.
- Los planes viven en `docs/plans/` y se cambian por PR, como cualquier otro fichero. Los informes (spike y evaluaciones) también van a `docs/plans/`.

## 5. Arquitectura

### 5.1. Mixxx

- **ControlObject/ControlProxy:** comunicación entre componentes con `[Group], key_name`.
- **Hilo del motor:** audio en tiempo real, sin reservas de memoria ni locks. Puede emitir señales de Qt, pero no recibirlas.
- **`parented_ptr`/`make_parented`:** propiedad en el árbol de objetos de Qt. El objeto debe tener padre antes de que se destruya el `parented_ptr`.

### 5.2. Reglas de Crate Decks

- **Código propio aislado** en `src/crate/`, `src/library/crateserver/`, `src/test/crate/`, `res/skins/CrateDecks/`, `res/crate-design/` y `tools/crate-design/`, detrás de la opción de CMake `CRATE`. La excepción es la identidad de la aplicación (`src/crate/appidentity.*` y sus tests): se compila siempre, también con `CRATE=OFF`, para que ningún build escriba en `~/.mixxx`.
- **Parches a ficheros de Mixxx:** la lista cerrada está en §5.2 del diseño. Cualquier otro necesita justificación en el PR. Desde F1, todo PR que toque un fichero de Mixxx actualiza `CRATE_PATCHES.md`. Se prefieren señales, slots y hooks a reescribir lógica de upstream.
- **Nombres:** solo se cambian las cadenas visibles (título, About, asistente de primer arranque y preferencias). Los identificadores, grupos de control, claves de configuración y nombres de fichero con "Mixxx" no se tocan, salvo los de la identidad de la aplicación de D3 (ejecutable, ID de aplicación, carpeta de ajustes y prefijo del keychain).
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
- **Idioma:** código, comentarios y mensajes de commit en inglés, como el código de Mixxx. Planes y documentación de Crate en español.
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
build/crate-decks --settingsPath "$(mktemp -d)"   # nunca contra ~/.cratedecks ni ~/.mixxx
```

Desde F1.PR2 el ejecutable se llama `crate-decks` y, en Linux, su carpeta de ajustes por defecto es `~/.cratedecks/`. Los tests siguen en `mixxx-test`.

## 8. Servidor Crate

- Nunca se usa producción: solo el stack dev del monorepo (`make dev`) o el servidor de pruebas que indique Diego.
- En F0, solo tokens `crv_` del stack dev, por variables de entorno (`CRATE_URL`, `CRATE_TOKEN`) y nunca commiteados.
- Si una tarea del cliente necesita un cambio de servidor que no está en S1–S7, para y avisa: primero se añade al plan.

## 9. Código de Mixxx y licencia

- **Independiente:** el repositorio sale de la red de forks de GitHub ("Leave fork network", decisión de Diego del 2026-11-03). No se publica nada en `mixxxdj/mixxx`: ni PRs, ni issues, ni reviews, ni texto. Los fallos de Mixxx que encontremos se arreglan aquí.
- **Origen:** Mixxx `main` en `bc49961` (2.7-alpha), la base elegida en G0.
- **Traer cambios de Mixxx** (en una sola dirección, opción A):
  - El remoto `upstream` y la rama `upstream` siguen a `mixxxdj/mixxx` sin cambios. `main` es `upstream` más el código de Crate.
  - Se trae lo que interese (un merge de un tag estable, o cherry-picks de arreglos de motor, audio, controladoras o seguridad) cuando Diego lo decide. No hay sincronía obligatoria.
  - Los conflictos en ficheros parcheados los revisa Diego.
- **Disciplina de aislamiento:** por eso sigue vigente la de §5.2. Cuanto menos se toque de Mixxx, más baratos son esos merges.
- **Licencia:** GPL-2.0-or-later.
  - Se conservan `LICENSE`, los avisos de copyright y los créditos de Mixxx en *About*.
  - Crate Decks se distribuye bajo GPL con su código fuente, y el código de Crate que se compila en Decks también.
  - El servidor Crate no queda afectado, porque habla con Decks por HTTP.
  - No se usan el nombre ni los logos de Mixxx en el producto, salvo en los créditos.
- **CI:** los workflows de `.github/workflows/` que vienen de Mixxx se recortan antes del primer push al repositorio independiente (F1.PR1).

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

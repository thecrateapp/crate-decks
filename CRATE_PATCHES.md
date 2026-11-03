# Parches a ficheros de Mixxx

Cada fichero de Mixxx que Crate Decks cambia o borra. Todo PR que toque un fichero de Mixxx actualiza esta tabla (AGENTS.md §5.2). La lista cerrada de parches previstos está en §5.2 del [diseño](docs/plans/2026-11-02-crate-decks-design.md).

Al traer cambios de Mixxx ([docs/mixxx-merges.md](docs/mixxx-merges.md)), los conflictos salen de estos ficheros. Cuanto más corta sea la tabla, más baratos son esos merges.

| Fichero | Cambio y motivo | Origen | ¿Hook genérico? | Estado |
| --- | --- | --- | --- | --- |
| `AGENTS.md` | Sustituido por las reglas de Crate Decks | Planes tras el spike | No | Activo |
| `.github/workflows/develop.yml` | Construye también `main` y las ramas con `/`; busca PRs abiertos en este repositorio en vez de en `mixxxdj/mixxx` | F1.PR1 | No | Activo |
| `.github/workflows/{appimage,benchmark,changelog,download_cleanup,flatpak,labeler,pr-command,release,stale,sync_branches}.yml` | Borrados: publican en servidores de Mixxx, usan sus bots o sus secretos, o solo los llamaba `release.yml` | F1.PR1 | No | Activo |
| `.github/{dependabot.yml,FUNDING.yml,labeler.yml,ISSUE_TEMPLATE/}` | Borrados: apuntan a ramas, financiación y canales de Mixxx | F1.PR1 | No | Activo |
| `CMakeLists.txt` | Identidad de Crate Decks (D3, F0 Q12): compila `src/crate/appidentity.cpp`, `res/crate.qrc` y los tests de `src/test/crate/` (con el directorio de cabeceras `ui_*.h` de `mixxx-lib`); `MIXXX_SETTINGS_PATH` = `.cratedecks/`, porque en Linux la carpeta de ajustes sale de ahí y no del nombre; ejecutable `crate-decks` en Linux; instala el `.desktop`, el metainfo y el icono de Crate Decks en vez de los de Mixxx; `CPACK_PACKAGE_NAME`, nombre del paquete Debian, fabricante y URL de Crate Decks; opción `CRATE` (ON, define `__CRATE__`) para la integración con el servidor, que no afecta a la identidad | F1.PR2 | No | Activo |
| `src/main.cpp` | Organización `Crate`, dominio `cratemusic.app`, nombre de escritorio de Wayland `app.cratemusic.decks` en vez de `org.mixxx.Mixxx` y versión de producto (`--version`), con `crate::applyApplicationIdentity()` (D3) | F1.PR2 | No | Activo |
| `src/util/versionstore.cpp` | `applicationName()` = "Crate Decks", desde `crate::kApplicationName`: título, diálogos y cliente de JACK (D3) | F1.PR2 | No | Activo |
| `src/preferences/broadcastprofile.cpp` | Prefijo de los servicios del keychain `"Crate - "` en vez de `"Mixxx - "`, desde `crate::kBroadcastKeychainPrefix`, para no compartir servicio con un Mixxx instalado (excepción a AGENTS.md §5.2 aprobada en G0) | F1.PR2 | No | Activo |
| `src/defs_urls.h` | `MIXXX_ICON_PATH` apunta al icono de Crate Decks (`res/crate.qrc`): icono de ventanas y diálogos sin el logo de Mixxx (D2) | F1.PR2 | No | Activo |
| `src/dialog/dlgabout.cpp` | *About* de Crate Decks (D2): versión de producto, icono de Crate Decks sin el logo de Mixxx, "Crate Decks is based on Mixxx" con enlace al proyecto, la GPL y el enlace al código fuente de esta versión; la web oficial es la de Crate y el botón de donar dice que va a Mixxx. Se conservan los créditos y el aviso de copyright de Mixxx | F1.PR2 | No | Activo |
| `packaging/appimage/appimage.cmake`, `packaging/appimage/AppRun`, `packaging/CPackConfig.cmake` | La AppImage usa el `.desktop`, el icono y el ejecutable de Crate Decks, y se llama `crate-decks-<versión>-<arquitectura>.AppImage` | F1.PR2 | No | Activo |

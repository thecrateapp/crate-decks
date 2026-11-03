# Parches a ficheros de Mixxx

Cada fichero de Mixxx que Crate Decks cambia o borra. Todo PR que toque un fichero de Mixxx actualiza esta tabla (AGENTS.md §5.2). La lista cerrada de parches previstos está en §5.2 del [diseño](docs/plans/2026-11-02-crate-decks-design.md).

Al traer cambios de Mixxx ([docs/mixxx-merges.md](docs/mixxx-merges.md)), los conflictos salen de estos ficheros. Cuanto más corta sea la tabla, más baratos son esos merges.

| Fichero | Cambio y motivo | Origen | ¿Hook genérico? | Estado |
| --- | --- | --- | --- | --- |
| `AGENTS.md` | Sustituido por las reglas de Crate Decks | Planes tras el spike | No | Activo |
| `.github/workflows/develop.yml` | Construye también `main` y las ramas con `/`; busca PRs abiertos en este repositorio en vez de en `mixxxdj/mixxx` | F1.PR1 | No | Activo |
| `.github/workflows/{appimage,benchmark,changelog,download_cleanup,flatpak,labeler,pr-command,release,stale,sync_branches}.yml` | Borrados: publican en servidores de Mixxx, usan sus bots o sus secretos, o solo los llamaba `release.yml` | F1.PR1 | No | Activo |
| `.github/{dependabot.yml,FUNDING.yml,labeler.yml,ISSUE_TEMPLATE/}` | Borrados: apuntan a ramas, financiación y canales de Mixxx | F1.PR1 | No | Activo |
| `CMakeLists.txt` | Compila `src/crate/appidentity.cpp`, la identidad de la aplicación | F1.PR2 | No | Activo |
| `src/main.cpp` | La organización, el dominio y el nombre de escritorio de Wayland salen de `crate::applyApplicationIdentity()` | F1.PR2 | No | Activo |
| `src/util/versionstore.cpp` | `applicationName()` sale de `crate::kApplicationName` | F1.PR2 | No | Activo |
| `src/preferences/broadcastprofile.cpp` | El prefijo de los servicios del keychain sale de `crate::kBroadcastKeychainPrefix` | F1.PR2 | No | Activo |

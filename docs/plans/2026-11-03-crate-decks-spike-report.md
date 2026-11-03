# Crate Decks — Informe del spike (F0)

**Estado:** código del spike terminado; faltan las pruebas de F0.4 que necesitan a Diego. **Inicio:** 2026-11-03. **Rama:** `spike/crate-decks`.
Preguntas y criterios: §F0.1 del [plan de implementación](2026-11-02-crate-decks-implementation-plan.md).

## Resumen para G0

**Decisión de Diego (2026-11-03):** Crate Decks sigue adelante pase lo que pase con las medidas; el spike queda como referencia y punto de partida. Las decisiones concretas de G0 siguen siendo suyas.

| Pregunta | Estado | En una línea |
| --- | --- | --- |
| Q1 | Contestada | Compila y pasa los tests; 4–5 fallos previos ajenos a Crate |
| Q2 | Pendiente (F0.4) | Base `main` o 2.6: hace falta usar las dos |
| Q3 | Medida a medias | Sin reanálisis; el deck muestra el grid de Crate tal cual. Pero el grid de Crate no coincide con el de Mixxx (de 30 a 350 ms) |
| Q4 | Medida | Mixxx respeta la tonalidad inyectada; coincide con el detector de Mixxx (Queen Mary) en 3 de 10 |
| Q5 | Medida a medias | Sin análisis de silencio. El final de Crate cae 1–6 s antes del de Mixxx. Falta mirarlo en la forma de onda |
| Q6 | Pendiente (F0.4) | Tonos de prueba y escucha |
| Q7 | Pendiente (F0.4) | El director y los CSV están listos y revisados; falta ejecutar 10 transiciones con audio |
| Q8 | Contestada | Hook en AutoDJ: 3 ficheros de Mixxx, unas 60 líneas |
| Q9 | Contestada (leyendo código) | `library.cpp`, 4 líneas; `ExternalTrackCollection` hoy no se puede registrar |
| Q10 | Medida en localhost | FLAC de 31 MB en 44 ms (p50). Falta la LAN y el tiempo hasta pintar la forma de onda |
| Q11 | Contestada | Funciona con GNOME; sin keyring se bloquea o falla. Hace falta un modo asíncrono con límite de tiempo |
| Q12 | Contestada | En Linux, el nombre no basta: hay que cambiar `MIXXX_SETTINGS_PATH`, el ID de Wayland y el prefijo del keychain |
| Q13–Q16 | Pendientes (F0.4) | Skin, rendimiento, legibilidad y QML |
| Q17 | Contestada sin motor | Viable para FLAC, AAC, ALAC y Opus; MP3 no por FFmpeg 9. La prueba en deck se deja para F4b |

**Riesgos nuevos que cambian el diseño:**

1. **Calidad del análisis del servidor.** El grid de Crate deriva o va desfasado frente al de Mixxx, y la tonalidad tiene un sesgo fuerte (81 % en 7B, 8A y 7A). Propuesta para Decks: inyectar el grid solo si pasa un control de calidad, o dejar los beats a Mixxx y usar Crate para cues, tonalidad y planes. Propuesta para Smart Mix 3: cuantizar el BPM y revisar los detectores de beats y tonalidad. **Actualización:** S8 lo corrige en el servidor con el analizador `smart-mix-audio-v6`; los resultados están en el plan de Smart Mix 3 (SM0.PR0).
2. **MP3 con FFmpeg 9.** La fuente FFmpeg de Mixxx no lee MP3; MAD lo oculta. Afecta a D14. No se avisa a Mixxx (AGENTS.md §9): si hace falta, se arregla aquí.
3. **Base dev sin audio.** El stack de Crate borra pistas al arrancar y al sincronizar. Ya está documentado en el plan.
4. **AutoDJ y Smart Mix.** El hook mínimo funciona, pero el director tiene que vigilar a los decks (paradas, bucles, expulsiones), quantize y sync. Aunque G0 decidiera otra cosa, el director sigue en el hilo principal: las posiciones se leen de `VisualPlayPosition`.

**Lo que G0 tiene que decidir**, con lo que sugiere el spike:

- **Base de upstream.** Decidido por Diego (2026-11-03): `main` (2.7-alpha), donde está hecho todo el spike. Q2 ya no hace falta.
- **Prefijo del keychain de emisión.** Decidido por Diego: `"Crate - "` en vez de `"Mixxx - "` (F1.PR2, Q12).
- **D1** ya está decidido: repositorio independiente. **D7 y D8:** el spike no aporta datos nuevos; S1 implementa D7 tal como lo propone el diseño.
- **D11.** Pendiente de Q13–Q16.
- **D14.** Sigue siendo una opción para FLAC, AAC, ALAC y Opus. MP3 necesita arreglar la fuente FFmpeg o una fuente basada en MAD. La construcción se decide en G3b.
- **Ajustes de fases.** En F4, inyectar el grid con control de calidad (riesgo 1). En F2, keychain asíncrono con alternativa en memoria (Q11). En F1, la ruta de configuración propia (Q12).

## Entorno

| Aspecto | Valor |
| --- | --- |
| Máquina | Intel Core Ultra 9 185H (22 hilos), 30 GiB de RAM más 30 GiB de zram, NVMe con 539 GB libres |
| Sistema | CachyOS (Arch), kernel 7.3.0, GNOME en Wayland, PipeWire 1.6.9 |
| Toolchain | GCC 16.2.1, CMake 4.4.5, Ninja 1.13.2, ccache 4.14.1, Qt 6.12.0, TagLib 2.3.2 |
| Base de Mixxx | `main` en `bc49961` (2.7-alpha) |
| Servidor Crate | Stack dev del monorepo en `main` (`65d944a2`), migración 142 |

**Dependencias:**

- Se usa `tools/archlinux_buildenv.sh` sin su paso de `taglib1` desde AUR: TagLib 2.3.2 del sistema funciona, porque `FindTagLib` pide 1.11 o superior y el código ya soporta TagLib 2.
- El script no instala `cmake` ni `ninja`; se añaden a mano.
- Hubo que actualizar el sistema (`pacman -Syu`) porque la base de datos de paquetes estaba desfasada y el mirror ya no tenía la versión de `cmake` que pedía.

**Opciones de CMake activas que importan al spike:** `FFMPEG` (Q17), `QTKEYCHAIN` (Q11), `QML` (Q16), `KEYFINDER`, `ENGINEPRIME` y `BUILD_TESTING`. DjInterop, KeyFinder y libshout-idjc no están en el sistema y se descargan y compilan durante el build.

## Q1 · Build y tests

| Medida | Valor |
| --- | --- |
| Configure | 4 s |
| Build limpio, ccache vacía, `-j12` | 1.073 s (17 min 53 s) |
| Build limpio, ccache llena, `-j12` | 70 s (99,7 % de aciertos) |
| `build/` (`RelWithDebInfo`) | 4,2 GB; `mixxx` 691 MB y `mixxx-test` 869 MB |
| ccache | 574 MB |
| Avisos del compilador | 9 (4 `-Wunused-result`, 4 `-Wsfinae-incomplete`, 1 `-Wunused-but-set-variable`) |
| `mixxx-test` | 1.303 tests en 105 s: 1.298 correctos, 5 fallos y 38 desactivados, más 1 cuelgue excluido |

Se compiló con `-j12` en lugar de `-j22` porque la máquina tenía 22 GiB de zram ocupados por otras cargas. El tiempo con los 22 hilos queda sin medir.

**Fallos previos (base de referencia):** ninguno toca código de Crate.

| Test | Tipo | Causa probable |
| --- | --- | --- |
| `QmlSkins/QmlStartupSmokeTest.Starts/NewUi` | Violación de segmento que tumba `mixxx-test` | Interfaz QML nueva (`res/qml`) con Qt 6.12. Justo antes avisa: `QQuickStyle::setStyle() must be called before loading QML`. Dato para Q16 |
| `ControllerScriptEngineLegacyTimerTest.beginTimer_singleShotTimer{BindFunction,ArrowFunction,BindFunctionInClass,ArrowFunctionInClass}` | Casi siempre (en la quinta batería pasó `BindFunction`) | Los temporizadores de un solo disparo con `bind` o funciones flecha no se disparan en 35 ms; los simples sí. Probable cambio en `QJSEngine` de Qt 6.12. Solo afecta a scripts de controladoras |
| `TrackMetadataExportTest.keepWithespaceKey` | Depende del orden | Pasa sola; en la batería completa devuelve `B_FLAT_MINOR` en lugar de `B♭m`. Algún test anterior deja cambiada la notación de tonalidad global. Tenerlo en cuenta en Q4 |

**Respuesta:** compila y los tests pasan, salvo los fallos previos de la tabla.

## Arranque aislado (adelanto de Q12)

`build/mixxx --settingsPath <dir>` arranca sin ventana (`QT_QPA_PLATFORM=offscreen`, 25 s):

- Crea `mixxxdb.sqlite`, `mixxx.log`, `analysis/`, `effects/` y `broadcast_profiles/` en el directorio indicado.
- No crea `~/.mixxx` ni nada de Mixxx en `~/.config`, `~/.local/share` o `~/.cache`.
- No se escribió `mixxx.cfg` porque el proceso se cortó por tiempo: se escribe al salir.

Lo que pregunta Q12 (separarse con un nombre de aplicación propio, sin `--settingsPath`) sigue pendiente.

## Servidor Crate para Q2–Q17

**Stack:** el stack dev del monorepo, en `main` (`65d944a2`) y en un worktree aparte, para no tocar ramas con trabajo abierto. Con `make dev-back`, la API migra la base de la revisión 136 a la 142 al arrancar; antes se guardó una copia con `pg_dump`. Caddy no arranca porque le faltan los certificados locales (`make trust-local-ca`), pero el spike usa la API directamente en `:8585`.

**Token:** `crv_` de tipo `virtualdj` con los cuatro scopes de §F0.2, guardado fuera del repositorio en un fichero con permisos 0600. `GET /api/search?scope=local` responde 200 y `scope=auto` responde 403, como prevé S0.1.

**Audio de prueba:** la base dev tiene 136.462 pistas y 136.446 perfiles de mezcla, pero ningún fichero de audio, así que las descargas devuelven 404. Para el spike se importaron 83 pistas de un set real (62 FLAC y 21 MP3), ordenadas en `test-music/<Artista>/<Álbum>/` porque el escáner agrupa por carpeta de artista.

**Riesgos del stack dev sin audio** (fallos del servidor, anotados para el monorepo):

- **Reparación automática:** el primer `library_pipeline` de `main` hace health check y reparación automática. Como no hay ficheros, borró 13.549 pistas por `delete_stale_track`. Se desactiva con `schedules` → `library_pipeline: 0` antes de arrancar los workers.
- **Sync de un álbum que ya existe:** borra las pistas de ese álbum que no están en la carpeta (`library_sync.py`, "Remove deleted tracks"). Lo hace con `delete_track_by_path`, que no deja rastro en `audit_log`. Así se perdieron otras 1.558 pistas.
- **Choque de nombres:** si el álbum ya existe con otra ruta, el sync falla con `UniqueViolation` en `library_albums_artist_name_key`. Si el MusicBrainz ID coincide, reutiliza el álbum, lo que lleva al caso anterior.
- **Mayúsculas:** dos carpetas de un mismo artista que solo difieren en mayúsculas (`3TEETH` y `3teeth`) se funden en un artista, y uno de sus álbumes desaparece.

**Importación final:** la base se restauró desde la copia, con `library_pipeline` y `repair_duplicate_tracks` desactivados antes de arrancar los workers. Las pistas se importaron como artistas nuevos: copias con `ARTIST` y `ALBUMARTIST` prefijados con "Spike · " y sin etiquetas MusicBrainz, AcoustID ni de orden. Solo cambian las etiquetas: el MD5 del audio decodificado es idéntico en las 83. Entran 80 (las 3 restantes son el caso de mayúsculas), y `main` las analiza en unos 10 minutos (Essentia en CPU). La base conserva las 136.462 pistas originales.

**Datos para Q3–Q5:** las 80 pistas tienen perfil de mezcla.

| Formato | Pistas | `full` | `partial` | BPM | En 120–130 BPM |
| --- | --- | --- | --- | --- | --- |
| FLAC | 61 | 38 | 23 | 84–174 | 7 |
| MP3 | 19 | 7 | 12 | 84–171 | 4 |

- `GET /api/tracks/by-entity/{uid}/mix-profile?detail=full` devuelve `beatGridMs` como posiciones absolutas en ms enteros. `delta-ms-v1` es solo la codificación en base de datos. Sin `detail=full` no viene el grid.
- El grid es la salida del detector de beats, no una rejilla uniforme: en una pista de 145 BPM, los primeros intervalos son de 407, 477, 376, 430 y 415 ms. Esto importa para elegir entre `fromBeatPositions` y `fromConstTempo` en Q3.
- La descarga con Range funciona: `GET /api/vdj/tracks/by-entity/{uid}/stream` con `Range: bytes=0-1048575` responde 206 con `audio/flac` en 23 ms.
- El set es post-punk y rock, no la mezcla de house, techno, hip-hop y directos que pide §F0.4. Solo 11 pistas caen en `bpm:120-130`.

**Sesgo de tonalidad (servidor):** 44 de las 80 pistas salen en 7B (fa mayor), y solo aparecen 5 tonalidades. No es del set: en los 136.503 perfiles con tonalidad de la base, 7B, 8A y 7A suman el 81 % (39,9 %, 21,4 % y 20,1 %), todos con `smart-mix-audio-v5`. Q4 mide si Mixxx respeta la tonalidad inyectada, así que el spike puede seguir, pero la detección de tonalidad del analizador de Crate es poco fiable y afecta a la compatibilidad armónica de Smart Mix. El diseño de Smart Mix 3 no lo trata.

**Desvío del plan (corregido):** §F0.2 situaba `POST /api/auth/access-tokens` en `app/crate/api/auth.py`. En `main` está en `app/crate/api/access_tokens.py`, con el esquema en `app/crate/api/schemas/access_tokens.py`. El cuerpo coincide: `name`, `scopes` (de 1 a 10) y `expires_in_days` opcional. El plan ya recoge la ruta buena y el aviso sobre la base sin audio.

## Código del spike (F0.3)

| Paso | Commit | Qué hay | Tests |
| --- | --- | --- | --- |
| S0.1 | `0edf56dc15` | Cliente: búsqueda, perfil, compatibles, planes y descarga con Range | 10 unitarios |
| S0.2–S0.3 | `9621df6f12` | Nodo "Crate (spike)" con vista propia y cargador a deck | 3 unitarios; la vista, a mano |
| S0.4 | `9230689e7e` | Inyección de grid, cues y tonalidad | 8 unitarios y 1 de integración con un FLAC real |
| S0.5 | `43e316b57b` | Director de Smart Mix y CSV de Q7 | 11 unitarios; el pegamento con el motor, a mano |
| S0.6 | `7b389742b4` | Modo `CrateSmartMix` de AutoDJ | 2 en `autodjprocessor_test.cpp` |
| S0.7 | `2e7d34a952` | Nombre y organización por variable de entorno | Medido (Q12) |
| S0.8 | `197805926a` | Prueba de QtKeychain | Medido (Q11) |
| S0.9 | `880f373588` | Fuente progresiva sin motor | 4 unitarios y 1 medida en vivo |
| Arnés | `43c013915d` | Medidas automáticas de Q3–Q5 y Q10 | 2 medidas en vivo |
| Revisión | `5aba6b9098` | Arreglos de la revisión final de toda la rama | 74 de spike y AutoDJ |

La batería completa da los mismos fallos previos de Q1 y ninguno nuevo.

**Parches a ficheros de Mixxx** (frente a `main`): el código propio son 21 ficheros nuevos en `src/crate/spike/` y `src/test/crate/` (+4.206 líneas).

| Fichero | Líneas | Para qué |
| --- | --- | --- |
| `CMakeLists.txt` | +15 / −0 | Fuentes y tests del spike |
| `src/library/library.cpp` | +4 / −0 | Registrar la feature (Q9) |
| `src/library/autodj/autodjprocessor.cpp` | +46 / −14 | Modo `CrateSmartMix` (Q8) |
| `src/library/autodj/autodjprocessor.h` | +12 / −1 | Modo y hook (Q8) |
| `src/library/autodj/dlgautodj.cpp` | +3 / −0 | El modo en el desplegable (Q8) |
| `src/main.cpp` | +8 / −1 | Nombre y organización por variable de entorno (Q12) |
| `src/sources/soundsourceffmpeg.cpp` | +5 / −2 | Hook `openInputContext()` (Q17) |
| `src/sources/soundsourceffmpeg.h` | +4 / −0 | Hook `openInputContext()` (Q17) |
| `src/test/autodjprocessor_test.cpp` | +98 / −0 | 2 tests del modo `CrateSmartMix` |

## Q8 · Enganche mínimo de AutoDJ

**Respuesta:** un modo `TransitionMode::CrateSmartMix` con un hook estático. Al empezar el fundido, AutoDJ llama al hook; si lo acepta, AutoDJ no arranca el deck entrante ni mueve el crossfader, y espera en `FADING` hasta que el deck saliente se para. Después termina como siempre y carga la siguiente pista. No hace falta tocar la máquina de estados.

| Fichero | Qué cambia | Líneas |
| --- | --- | --- |
| `autodjprocessor.h` | Valor del enum (al final, para no mover los números guardados en la configuración), tipo y setter del hook, un miembro | +12 / −1 |
| `autodjprocessor.cpp` | `calculateTransition`: reutiliza `FullIntroOutro` y adelanta el traspaso 30 s. `playerPositionChanged`: llama al hook y se salta su fundido mientras dura. `toggleAutoDJ`: limpia el estado | +46 / −14 (las 14 son el bloque envuelto, sangrado de nuevo) |
| `dlgautodj.cpp` | El modo en el desplegable | +3 |

**Interferencias vistas:** ninguna en los tests. Riesgos para la prueba de F0.4:

- AutoDJ pasa a `FADING` 30 s antes, y la interfaz lo muestra así todo ese rato.
- AutoDJ alterna 1→2 y 2→1, así que el director mueve el crossfader según la orientación del deck saliente.
- Si no hay plan, el director corta al entrante para que AutoDJ no se quede esperando.
- "Fade now" en `CrateSmartMix` también pasa por el hook. Si la cue del planner ya quedó atrás, el director arranca tarde y lleva el entrante a donde debería estar, para no perder la fase.

**Cuidado al parchear ficheros de Mixxx:** `clang-format` sobre el fichero entero reescribe líneas que no se han tocado; en `dlgautodj.cpp` hasta movió un `else` dentro de `VERIFY_OR_DEBUG_ASSERT`. Solo se formatea lo cambiado.

## Q9 · Registro de la feature

**Respuesta:** `library.cpp`, con 4 líneas (include, comentario y `addFeature`). `ExternalTrackCollection` no sirve hoy: `TrackCollectionManager` no tiene cómo registrar colecciones externas (`trackcollectionmanager.cpp:56-60` es un `TODO`). Además repartiría `saveTrack`, purgas y reubicaciones a la colección desde cualquier hilo, y añadiría una entrada al menú de pistas. Se midió leyendo el código, sin construirla.

**Otro hallazgo:** `WTrackTableView` nunca le dice al modelo a qué deck va una carga, y `getTrack()` es síncrono y también se llama al seleccionar. Por eso la feature tiene vista propia (patrón de `RecordingFeature`) y emite `loadTrackToPlayer` cuando termina la descarga. Se pierden las columnas nativas, arrastrar al deck y `LoadSelectedTrack` desde controladoras; recuperarlos exige parchear `wtracktableview.cpp` y `wtrackmenu.cpp`.

## Q11 · QtKeychain y Secret Service

| Escenario | Resultado |
| --- | --- |
| Sesión de GNOME (`gnome-keyring-daemon`, Secret Service) | Funciona: escritura en 22 ms, lectura en menos de 1 ms, borrado bien |
| Sesión D-Bus sin Secret Service (`dbus-run-session`) | La escritura se bloquea hasta que el límite de 10 s del spike la corta |
| Sin bus de sesión alcanzable | Falla en 7 ms con el error 7 (`dbus-launch` no arranca). No hay respaldo en texto plano |

**Respuesta:** funciona en el escritorio de la máquina. Sin keyring, o falla rápido o se bloquea, según haya bus o no. El cliente tiene que lanzar los trabajos de QtKeychain de forma asíncrona y con límite de tiempo, en vez del bucle de eventos anidado de `broadcastprofile.cpp`, y ofrecer guardar la sesión solo en memoria. Si solo se quita `DBUS_SESSION_BUS_ADDRESS`, QtKeychain sigue llegando al keyring por `$XDG_RUNTIME_DIR/bus`.

## Q12 · Configuración separada

Medido en Linux con un `HOME` desechable y sin `--settingsPath`, con y sin `CRATE_SPIKE_APP_NAME` y `CRATE_SPIKE_ORG_NAME`. En los dos casos Mixxx escribe `mixxx.log`, `mixxxdb.sqlite`, `analysis/`, `effects/` y `broadcast_profiles/` en `$HOME/.mixxx`.

**Respuesta:** en Linux no basta con el nombre. La carpeta es `MIXXX_SETTINGS_PATH`, fijada al compilar (`CMakeLists.txt:2065`, `.mixxx/`). En macOS y Windows sale de `AppLocalDataLocation`, que sí sigue el nombre de la aplicación (la organización se deja vacía a propósito, `main.cpp:235`). Hay otros dos identificadores fijos: el nombre de escritorio de Wayland (`org.mixxx.Mixxx`, `main.cpp:229`) y el prefijo `"Mixxx - "` de los servicios del keychain de los perfiles de emisión (`broadcastprofile.cpp:64`), que chocaría con un Mixxx instalado. Mixxx no responde a SIGINT, así que una ejecución cortada por tiempo nunca escribe `mixxx.cfg`.

## Q17 · Reproducción progresiva (prototipo sin motor)

`SoundSourceCrateProgressive` (commit `880f373588`) es un `SoundSourceFFmpeg` con su propio `AVIOContext`:

- La lectura espera los bytes que faltan en vez de devolver una lectura corta o EOF, que acortarían la pista para siempre.
- El seek nunca espera.
- `AVSEEK_SIZE` responde con el tamaño final, que viene del `Content-Length`; sin él FFmpeg no sabe la duración y la apertura falla.

En Mixxx solo cambia un hook virtual, `openInputContext()`, en `SoundSourceFFmpeg` (+9 / −2). Un hilo que copia a velocidad fija hace de descarga.

| Prueba | Resultado |
| --- | --- |
| FLAC, AAC (M4A), ALAC (M4A) y Opus, con la descarga en curso | Salen exactamente las mismas muestras que del fichero completo, sin lecturas cortas |
| Cabeza mínima para abrir sin esperar, con 64 KiB de cola y la copia parada | FLAC 64 KiB (lleva carátula); AAC, ALAC y Opus 16 KiB |
| Seek a una zona sin descargar (M4A) | Espera y devuelve lo mismo que el fichero completo |
| Abortar con un lector esperando | El lector se despierta y termina enseguida |
| 5 FLAC reales (29–81 MiB), 10 s descargados, 40 s a ritmo de reproducción | Abren en 0–1 ms. A 1,5× del bitrate, ninguna espera. A 0,5×, entre 83 y 314 esperas (17–55 s en total) y ninguna lectura corta |
| MP3 (los 3 de prueba de Mixxx y 2 reales) | La fuente FFmpeg de Mixxx no los decodifica ni completos con FFmpeg 9.0.2: `av_seek_frame()` devuelve −1, que el log muestra como `EPERM` pero es el fallo genérico de FFmpeg. Normalmente no se nota porque MAD lee los MP3 antes |

**FFmpeg en los builds:** el de Linux enlaza `libavformat.so.63`, `libavcodec.so.63`, `libavutil.so.61` y `libswresample.so.7` (FFmpeg n9.0.2). Los artefactos de macOS y Windows del CI de upstream no se han revisado.

**Riesgos para meterlo en el motor**, que salen de leer el código:

- **Hilo de lectura:** solo lee el `CachingReaderWorker` de cada deck. No tiene timeout ni se puede cancelar, y la expulsión se procesa entre lecturas. Una lectura bloqueada retrasa la expulsión y la carga siguiente.
- **Cierre de la app:** `quitWait()` espera sin límite. Además, la `Library` (con la descarga) se destruye antes que el `EngineMixer`, así que, sin una señal de aborto, cerrar la app con una lectura esperando se cuelga.
- **Hilo de la interfaz:** también se abre el fichero desde ahí (`wtrackmenu.cpp`, la importación de beats y cues pendiente). Como la descarga corre en ese mismo hilo, bloquear ahí es un interbloqueo.
- **Lecturas en orden:** una lectura más allá de lo descargado bloquea las de zonas ya descargadas del mismo deck, como un hot cue anterior. Reproducir más allá de la descarga da silencio y la posición sigue avanzando.
- **Formatos que leen el final:** el MP3 lee etiquetas ID3v1 y APE del final, y un M4A sin `faststart` tiene el `moov` al final. Hay que descargar primero la cola (`Range: bytes=-N`).
- **Análisis:** el análisis automático al cargar lee la pista entera a la velocidad de la descarga y ocupa un hilo de análisis todo ese tiempo.

**Respuesta parcial:** la lectura progresiva es viable para FLAC, AAC, ALAC y Opus. MP3 necesita antes arreglar la fuente FFmpeg de Mixxx con FFmpeg 9, o una fuente basada en MAD. Faltan las pruebas en un deck real (S0.9 con el motor):

1. registrar el proveedor con prioridad `Highest` solo para la caché del spike;
2. añadir la señal de aborto en el `CachingReaderWorker` y el `AnalyzerThread`, y otra para cuando la app se cierra;
3. no bloquear nunca en el hilo de la interfaz.

## Q3, Q4, Q5 y Q10 · Medidas automáticas

Un arnés sin interfaz (`CrateSpikeHarnessLive`, desactivado por defecto) descarga 10 pistas "Spike ·" con perfil `full` del stack dev e inyecta su perfil como el spike. Después hace dos cosas:

1. Comprueba si los analizadores de Mixxx volverían a correr.
2. Pasa los analizadores de Mixxx sobre el mismo fichero y compara.

Escribe `q3-q5-harness.csv` y `q10-harness.csv` en la caché del spike.

**Sin reanálisis:** en 10 de 10 pistas, `AnalyzerBeats`, `AnalyzerKey` y `AnalyzerSilence` no vuelven a correr sobre la pista inyectada. Para que el dato valga, sobre una pista sin inyectar los tres sí corren con la misma configuración.

**Q3 · Grid frente al de Mixxx** (5 beats repartidos por pista; máximo |diferencia| por pista):

| Grid | Mediana de los máximos | Pistas dentro de 5 ms |
| --- | --- | --- |
| `beatGridMs` en bruto | 160 ms | 0 de 10 |
| Inyectado en modo `auto` (tempo constante si la estabilidad es ≥ 0,95) | 178 ms | 1 de 10 |
| Inyectado en modo `positions` | 59 ms | 0 de 10 |

- La inyección es fiel: el deck muestra exactamente el grid de Crate, con error de 1 frame o menos (test unitario).
- Lo que falla es la calidad del grid de Crate:
  - **El BPM lleva decimales que la pista no tiene** (128,346 frente a 128 de Mixxx). Como tempo constante deriva: un 0,27 % suma casi medio segundo en 3 minutos. Por eso `auto` sale peor.
  - **Los beats detectados bailan unos ±50 ms**, y en algunas pistas la fase va medio beat desplazada (Ceremony, 340 ms a 87 BPM).
- El criterio "error ≤ 5 ms" no se cumple frente a Mixxx. Diego tiene que confirmar en F0.4, sobre la forma de onda, cuál de los dos grids va con la música.
- **Propuesta:** en Decks, inyectar solo si el grid de Crate pasa un control, o dejar que Mixxx analice los beats y usar Crate solo para cues y tonalidad. Para el servidor (Smart Mix 3): cuantizar el BPM y revisar el detector de beats.

**Q4 · Tonalidad:**

- Mixxx respeta la inyectada, porque no la recalcula.
- Solo coincide con la del detector de Mixxx en 3 de 10 pistas. Ese detector es Queen Mary, el que Mixxx usa por defecto, no KeyFinder. Cuadra con el sesgo de tonalidad del servidor: 7B y 8A en casi todas.
- Con el analizador v6 (S8), las mismas 10 pistas coinciden en 6 con Queen Mary y en 8 con KeyFinder.

**Q5 · Cues y silencio:**

- El análisis de silencio no corre.
- El principio del sonido coincide con el de Mixxx con diferencias de 0 a 350 ms; casi siempre menos de un beat.
- El final de Crate (`activeEndMs`) cae entre 1 y 6 s antes que el de Mixxx (−60 dB). Parece un final musical y no un umbral de silencio, así que N60dBSound no debería salir de ahí.

**Q10 · Descargas** (localhost; la red no cuenta):

| Formato | Descargas | Tamaño medio | p50 | p95 | Abrir desde la caché |
| --- | --- | --- | --- | --- | --- |
| FLAC | 20 | 30,7 MB | 44 ms | 83 ms | 0 ms |
| MP3 | 5 | 9,7 MB | 25 ms | 38 ms | 1 ms |

Solo hay 5 MP3 porque la búsqueda solo devolvió 5 pistas "Spike ·" en MP3. "Abrir desde la caché" mide solo la apertura del fichero de audio; no incluye añadirlo a la biblioteca ni cargarlo en un deck. El primer byte llega en unos 10 ms. En una LAN de 1 Gbit, un FLAC de 40 MB serían unos 0,35 s, dentro del criterio de 1,5 s, pero hay que medirlo en la red de verdad. El tiempo hasta pintar la forma de onda necesita la interfaz (F0.4).

## Pendiente

- Q6, Q7, Q13–Q16 y la parte visual de Q3, Q5 y Q10: pruebas de F0.4 (necesitan a Diego para mirar y escuchar).
- Q2 ya no hace falta: G0 eligió `main` (2.7-alpha).
- Q17 en un deck real: descartado del spike. G0 decide D14 con el prototipo sin motor.

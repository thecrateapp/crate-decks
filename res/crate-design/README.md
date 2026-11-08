# Tokens de diseño de Crate Decks

`tokens.json` es la única fuente de los valores visuales de Crate Decks: colores, fuentes, tamaños, radios, geometría de instrumento y las adaptaciones DJ. Desde V1, ningún color, tamaño, radio ni fuente se escribe a mano en QSS, XML de skin ni C++ (AGENTS.md §5.2): todo sale de aquí a través de `tools/crate-design/generate.py`.

Diseño: §14 del [documento de diseño](../../docs/plans/2026-11-02-crate-decks-design.md). Plan: fase V del [plan de implementación](../../docs/plans/2026-11-02-crate-decks-implementation-plan.md).

## Origen

- Los valores se copian a mano del admin de Crate, en el monorepo `thecrateapp/crate`. El commit de origen está en `meta.source.commit` (`8d1430fa`, 2026-11-08), con las rutas que se leyeron.
- **Fuentes de verdad del admin:**
  - `app/shared/ui/tokens/`. `themes.css` se importa después de `semantic.css` y gana donde discrepan (por ejemplo, `--text-subtle` es `#7e8084`, no el blanco al 50 % de `semantic.css`).
  - La geometría de `GenreWheel.tsx`, `genre-wheel.css`, `StackMap.tsx` y `stack-map.css`, y el texto de `MonoLabel.tsx`.
- **Cada token lleva `source`:** el fichero y la variable o regla de la que sale.
- **El PR que cambie un valor** cita el commit de origen y actualiza `meta.source`.

## Formato de `tokens.json`

`tokens` agrupa los tokens por tema. Los grupos solo ordenan: los nombres son únicos en todo el fichero, en `lowerCamelCase`, y las plantillas los usan sin grupo.

| Campo | Significado |
| --- | --- |
| `value` | Color `#rrggbb` en minúsculas, número o texto |
| `alpha` | Opacidad del color, de 0 a 1 |
| `unit` | `px`, `em`, `ms`, `deg`, `dB` o `%`. Solo informa, salvo en QSS (ver abajo) |
| `ref` | Alias de otro token: toma su valor, su unidad y su alfa. Puede nombrar un interruptor de paleta entre llaves: `waveLow{wavePaletteDeckAC}` apunta a `waveLowAccent` o a `waveLowNeutral` según el texto de `wavePaletteDeckAC` |
| `source` | De dónde sale el valor |

## Qué es cada token

| Grupo | Tokens | Uso en Decks |
| --- | --- | --- |
| `color` | `bgApp`, `bgPanel`, `bgRaised`, `bgPopover`, `bgSecondary` | Fondo de ventana; decks, mezclador y biblioteca; franjas elevadas; menús, diálogos y campos; hover fuerte |
| `color` | `border`, `borderSubtle`, `borderHover`, `hoverBg`, `hoverBgStrong`, `scrollbarThumb`, `scrollbarHover` | Líneas de 1 px entre zonas, separadores, hover y scrollbar (blanco frío con alfa) |
| `color` | `textPrimary`, `textSecondary`, `textMuted`, `textSubtle`, `textFaint`, `navIdle` | Texto de mayor a menor peso; `navIdle` es el rótulo de navegación en reposo (blanco al 42 %) |
| `color` | `accent`, `onAccent`, `accentHi`, `accentBg`, `accentBorder`, `accentBorderFocus`, `rowHover`, `rowSelected`, `focusRing` | El único acento: `accent` para acción y foco, `accentHi` para lo activo, y sus mezclas para chip seleccionado, bordes y filas |
| `color` | `warning`, `warningText`, `danger`, `dangerText` | Ámbar para problemas y rojo solo para lo destructivo y el clip. `success` e `info` no se portan: el admin no los usa |
| `color` | `logoStart`, `logoEnd`, `logoShade` | Colores del logo, base de la paleta de onda |
| `iconState` | `iconIdle`, `iconHover`, `iconActive`, `iconDisabled`, `iconOnAccent` | Color de cada estado de los iconos y controles generados |
| `deck` | `deckA` … `deckD` | Color por deck (decidido): A y C en `accentHi`, B y D en `textPrimary` |
| `waveform` | `waveLowAccent` … `waveHighNeutral`, `wavePlayhead`, `waveBeat`, `waveDownbeat`, `waveBg`, `waveOverviewBg`, `waveOverviewPlayed`, `wavePaletteDeckAC`, `wave*DeckAC`, `wave*DeckBD` | Paleta "filtered" de 3 bandas: cian del logo en A y C, grises fríos en B y D. `wavePaletteDeckAC` elige la paleta de A y C (`Accent` o la reserva `Neutral`) |
| `meter` | `vuLevel`, `vuWarning`, `vuClip`, `vuTrack`, `vuClipTrack`, `vuWarningDb`, `vuBarWidth` | VU `accentHi` → `warning` desde −6 dBFS → `danger` en clip |
| `font` | `fontBody`, `fontDisplay`, `fontMono`, `fontWeight*` | Roboto, Bricolage Grotesque y JetBrains Mono, y sus pesos |
| `fontSize` | `fontSizeCounter` … `fontSizeBody`, `fontSizeTitle`, `fontSizeDisplay`, `fontSizeLegend`, `fontSizeTable`, `fontSizeDeckNumber`, `fontSizeDeckNumberCompact` | Escala del admin (8 a 15 px), títulos de 30 y 40 px, leyendas de 9 px y las propuestas DJ |
| `tracking` | `trackingCaps`, `trackingKicker`, `trackingEyebrow`, `trackingTitle`, `trackingNumeral` | Espaciado en `em`: rótulos en mayúsculas, títulos y numerales |
| `height` | `heightControl*`, `heightTableCell`, `heightTrackRow*`, `heightTableHeader`, `heightLibraryRow`, `heightSearch` | Alturas del admin (preferencias y diálogos) y la densidad "cabina" de la biblioteca |
| `layout` | `sidebarWidth`, `sidebarWidthCollapsed`, `topbarHeight`, `activeBarWidth`, `focusRingWidth` | Barra lateral de 240 px, barra superior de 64 px, barra de activo de 2 px y anillo de foco |
| `lane` | `laneMeter`, `laneSlider`, `sliderDot`, `scrollbarWidth` | Carriles de 1 y 2 px, punto de slider y scrollbar de 8 px |
| `radius` | `radiusSm` … `radiusXl`, `radiusPanel`, `radiusPill` | 2–8 px en controles, 14 px en paneles y pastillas |
| `motion` | `motionFast`, `motionNormal`, `motionEaseSnap` | Solo para QML; QSS no anima |
| `iconSize` | `iconNano` … `iconXl` | Escala de iconos del admin (18 px por defecto) |
| `geometry` | Trazos, nodos, marcas, patrones de trazo, opacidades, brillo, `wheelCanvas`, `camelotRing*`, `tickPhrase`, `hotcueRadius` y `levelRingRadius` | La gramática de GenreWheel y StackMap (§14.5 del diseño), más la rueda Camelot, la marca de frase, los hotcues como nodos y el anillo de caché |
| `knob` | `knobSize`, `knobSizeCompact`, `knobSweep`, `knobTickCount`, `knobTickEvery`, `knobTickLong`, `knobTickShort`, `knobArcWidth`, `knobDot` | Knob de instrumento: anillo, marcas, arco de valor y punto |
| `fader` | `faderRail`, `faderTickStep`, `faderHandleWidth`, `faderHandleHeight` | Fader con carril de 1 px, marcas cada 10 % y tirador de contorno |
| `deckDisplay` | `deckNumberFont`, `deckNumberWeight` | Fuente de BPM y tiempo restante en el deck |

## Decisiones de GU1

Diego cerró GU1 el 2026-11-09: aprobó las maquetas de V1.3 tal cual, con todas las recomendaciones (§14.3 del diseño). Los valores de aquí son los de las maquetas y ya no llevan `pending` ni `options`. Las maquetas viven fuera del repositorio (`crate-agents/decks-mockups/`) y son la referencia de píxeles de V2 y V3.

| Tokens | Valor aprobado | Alternativa descartada |
| --- | --- | --- |
| `waveform` | Cian del logo en A y C, grises en B y D | Grises en todos y bandas en todos |
| `wavePaletteDeckAC` | `Accent` | Reserva `Neutral`: grises fríos en todos los decks si en cabina las bandas resultan fuertes. Se cambia en este único token y se regenera |
| `meter` | VU de 4 px; tramo de clip apagado en rojo al 18 % | 3 px |
| `fontSizeTable` | 11 px | 10,5 px (la del admin) |
| `fontSizeDeckNumber*`, `deckNumberFont`, `deckNumberWeight` | Bricolage 700, 34 px (26 con 4 decks), con cifras tabulares | JetBrains Mono 400 |
| `heightLibraryRow` | 30 px | 28 y 32 px |
| `levelRingRadius` | 7 px | 6 y 8 px |
| `knob` | 36 px (30 con 4 decks o a 768 px de alto), 270°, 24 marcas de 4,5 y 2 px con una larga cada 30° | 48 marcas con una larga cada 4, como StackMap |
| `fader` | Carril de 1 px, marcas cada 10 %, tirador de 18 × 10 px | — |
| `iconOnAccent` | Icono en `onAccent` sobre el relleno del color del deck de un botón activo | Contorno `accentHi` con tinte del 10 % |
| `camelotRingA`, `camelotRingB` | Anillos de la rueda Camelot a r 196 y 292 en el lienzo de 760 px de GenreWheel (sus guías están a 132 y 212), para dejar libre la cifra del centro | Las radios de GenreWheel |
| `tickPhrase`, `hotcueRadius` | Regla de frase del deck (beat 3, compás 7, frase 10 px) y hotcues como nodos de r 10 | — |

## Generador

```shell
python3 tools/crate-design/generate.py           # regenera res/skins/CrateDecks/generated/
python3 tools/crate-design/generate.py --check   # falla si lo commiteado no está al día (CI)
python3 -m pytest tools/crate-design             # tests del generador
```

Python 3 estándar, sin dependencias. Las salidas se commitean, y el job `design-tokens` de `crate-decks-build.yml` ejecuta `--check` y los tests.

**Qué escribe** en `res/skins/CrateDecks/generated/`, dentro de la skin que lo usa y se instala con ella (§14.5 del diseño). No se edita a mano; lo marca el fichero `GENERATED`:

| Salida | Desde | Contenido |
| --- | --- | --- |
| `Theme.qml` | `tools/crate-design/templates/Theme.qml.in` | Singleton QML con una propiedad por token, para la etapa 2 (§14.4) |
| `vars.xml` | `tools/crate-design/templates/vars.xml.in` | `<Template>` con los `<SetVariable>` de la skin: superficies, fuentes, color por deck, onda, VU, knobs y faders |
| `icons/<icono>_<estado>.svg` | `tools/crate-design/svg/solar/` | Cada icono Solar, una vez por estado |
| `controls/<control>_<estado>.svg` | `tools/crate-design/svg/controls/*.svg.in` | Controles base (checkbox, radio, punto de slider, nodo), una vez por estado |
| `controls/knob-scale.svg`, `controls/fader-scale-{v,h}.svg`, `controls/fader-handle-{v,h}_<estado>.svg` | Tokens de `knob`, `fader` y `geometry` | Anillo y marcas del knob, carril y marcas del fader, y tirador de contorno (relleno en `active`) |

**Plantillas:** cualquier `<nombre>.<qss|xml|qml|svg>.in` en `tools/crate-design/templates/` produce `generated/<nombre>.<ext>`. V2 añade aquí `style.qss.in` y `default.qss.in`. Los marcadores son `{{token}}` o `{{token.filtro}}`.

| Marcador | QSS | XML y QML | SVG |
| --- | --- | --- | --- |
| Color sin alfa | `#rrggbb` | `#rrggbb` | `#rrggbb` |
| Color con alfa | `rgba(r, g, b, 0–255)` | `#aarrggbb` | Error: usar `.hex` y `.alpha` |
| Número con `px` o `em` | `11px` | `11` | `11` |
| Otro número o texto | Tal cual | Tal cual (escapado en XML) | Tal cual (escapado) |

Filtros: `.hex`, `.alpha` (0–1), `.argb`, `.rgba`, `.number`, `.unit` y `.list` (un patrón de trazo `"9 4"` como `9, 4`).

**Estados** de iconos y controles: `idle` (`iconIdle`), `hover` (`iconHover`), `active` (`iconActive`), `disabled` (`iconDisabled`) y `onaccent` (`iconOnAccent`, para el icono sobre un botón activo relleno). El generador cambia `currentColor` por el color del estado.

**Falla** si una plantilla usa un token que no existe (con fichero y línea), si un token no lo usa ninguna plantilla ni ningún SVG generado (un `ref` cuenta como uso de su destino), si un SVG no tiene `currentColor`, o si un color no es `#rrggbb`. `Theme.qml.in` expone todos los tokens: al añadir uno, se añade también ahí.

**A tener en cuenta en V2:**

- QSS no tiene `letter-spacing` ni `text-transform`. El tracking y las mayúsculas de MonoLabel se aplican con `QFont::setLetterSpacing` y `QFont::setCapitalization` desde C++.
- `QFont` toma tamaños de píxel enteros: 10,5 y 11,5 px necesitan tamaño en puntos o `setPixelSize` redondeado.

## Iconos Solar

- **Origen:** `@solar-icons/react` 2.3.3, el paquete que usa el admin. `tools/crate-design/extract_solar.py` lee sus módulos como texto (sin ejecutarlos) y escribe un SVG por icono en `tools/crate-design/svg/solar/`.
- **Subconjunto:** la lista de `svg/solar/SUBSET`, en contorno, y en relleno (`bold`) solo Play, Pause y Heart, como el admin. Para añadir uno: se añade a `SUBSET`, se ejecuta `extract_solar.py` sobre el paquete descomprimido y se regenera.
- **Solar al 100 %** (§14.3): ningún icono de lucide. Un test falla si aparece un SVG fuera de `svg/solar/` y `svg/controls/`, o si un icono no está en `SUBSET`.
- **Licencia:** el paquete es MIT (`svg/solar/LICENSE`) e incluye los iconos del Solar Icons Set de 480 Design bajo CC BY 4.0 (`svg/solar/LICENSE-THIRD-PARTY`).
  - CC BY 4.0 pide atribución visible y decir qué se cambió. El texto está en `svg/solar/ATTRIBUTION` y en los créditos de `CRATE.md`.
  - Cuando V2 empaquete los iconos, la atribución va también en *About* y en el paquete.

## Fuentes

Las tres fuentes del admin están en `res/fonts/`, como TTF variables oficiales de la misma versión que los woff2 del admin (`app/shared/fonts/`). Qt carga TTF de forma fiable en todas las plataformas; woff2 depende de la versión y la plataforma. Todas son OFL 1.1, con su licencia al lado (`*.OFL.txt`).

| Fichero | Familia | Versión | Ejes | Origen (`google/fonts`) | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `Roboto-Variable.ttf` | Roboto | 3.015 | `wght` 100–900, `wdth` 75–100 | `6183fc0d2636`, `ofl/roboto/Roboto[wdth,wght].ttf` | `d7598e12c5dbef09…` |
| `BricolageGrotesque-Variable.ttf` | Bricolage Grotesque | 1.001 | `opsz` 12–96, `wght` 200–800, `wdth` 75–100 | `b9f6c712059d`, `ofl/bricolagegrotesque/BricolageGrotesque[opsz,wdth,wght].ttf` | `413e7357809ddd12…` |
| `JetBrainsMono-Variable.ttf` | JetBrains Mono | 2.211 | `wght` 100–800 (por defecto 400) | `2e05c1cf00a6`, `ofl/jetbrainsmono/JetBrainsMono[wght].ttf` | `48715a42ec242c21…` |

- **Versiones:** se comprobaron con el campo `fontRevision` y el nombre 5 de los woff2 del admin en `8d1430fa`. Los woff2 del admin son subconjuntos (latín y latín extendido) de estos mismos ficheros.
- **Registro:** `FontUtils::initializeFonts()` registra al arrancar todo lo que hay en `res/fonts/`. En Linux, Mixxx no instala `res/fonts/` (espera los paquetes de la distribución), pero estas tres casi nunca están empaquetadas: `CMakeLists.txt` las instala en todas las plataformas.
- **Ejes variables:** con Qt 6.12, `QFont::setVariableAxis()` aplica `wght` y `opsz`, así que no hacen falta instancias estáticas. Los navegadores ajustan `opsz` al tamaño de letra (`font-optical-sizing: auto`); en Qt hay que fijarlo con `setVariableAxis(QFont::Tag("opsz"), tamaño en px)`, entre 12 y 96.
- **Cifras:** JetBrains Mono y Roboto tienen cifras tabulares por defecto. Las de Bricolage Grotesque son proporcionales; con `QFont::setFeature(QFont::Tag("tnum"), 1)` pasan a tabulares, que es lo que pide §14.3 para el BPM y el tiempo del deck.
- **Test:** `src/test/crate/fontstest.cpp` (`RegistersAdminFonts`, `MonoHasTabularDigits` y `VariableAxesChangeTheGlyphs`) comprueba las familias, que se usan estos ficheros y no otras copias del sistema, las cifras y los ejes.

# Crate Decks — Own brand: nothing of Mixxx reaches the user

> **For agents:** run the tasks in order, each starting with its red test. Stop at the decisions in §7:
> they are Diego's. AGENTS.md applies (no push or PR unless asked, conventional commits, refactors apart).
>
> Design: [2026-11-02-crate-decks-design.md](2026-11-02-crate-decks-design.md) (D2, D3 and §5.2).
> Implementation plan: [2026-11-02-crate-decks-implementation-plan.md](2026-11-02-crate-decks-implementation-plan.md)
> (F1.PR2b renamed only the title, About, the menu bar dialog and Preferences).

**Date:** 2026-11-09. **Base:** `feat/decks-skin` @ `48ac53a9a3`. **Branch:** `feat/decks-own-brand`.

## 1. The rule

Diego, 2026-11-09: *"que se absorba absolutamente todo lo de Mixxx, no puede aparecer nada de Mixxx en ningún sitio"*.

- Every string, dialog, menu, tooltip, link label, error, window title, image and path the user sees names
  Crate Decks or nothing. That includes the terminal (`--help`), the desktop (screensaver inhibitor, PipeWire
  node) and what Decks writes for other programs (stream name, Engine DJ export).
- Mixxx's first-run dialogs are absorbed into Crate's first start, or decided without asking.
- **Unchanged:** identifiers, control groups, config keys and file names keep "Mixxx" (`mixxxdb.sqlite`,
  `mixxx.cfg`, `[Master]`, `MixxxControl` in mapping XML). They are not shown as product names.
- **Only exception:** the credits in *About* ("based on Mixxx", the Mixxx team, the GPL notice), required by
  D2 and by the GPL's notices. The metainfo's credit line is the same credit for software centres. §7 asks
  Diego to confirm both.

Why now: the headless run of the real app on 2026-11-09 showed three Mixxx dialogs before Crate's first start
(music folder, "No Output Devices", "hide the menu bar?") and "A deck is currently playing. Exit Mixxx?" on quit
(`decks-mockups/compare/manual-tests/scrim/`).

## 2. Audit

Measured on the base commit with `lupdate` (every translatable string of `src/` and `res/qml`) and a scan of the
remaining literals, skins, mappings, images and translations. The scripts are reproducible:
`tools/crate-brand/inventory.py` (BR.PR1) keeps the inventory current.

### 2.1. By mechanism

| Mechanism | Count | Where |
| --- | --- | --- |
| `tr()` and `.ui` strings that name Mixxx | **93** in 30 contexts | 25 start-up, errors and command line; 20 main window and menus; 17 Preferences; 9 controllers; 7 library; 6 skin tooltips and widgets; 3 About; 2 broadcast and recording; 2 effects; 1 sampler banks; 1 QML |
| Translations (`res/translations/*.ts`) that name Mixxx | **3,094** messages in 77 locales | 3,071 translate one of the 93 sources; **23** name Mixxx only in the translation (Catalan "Sobre el Mixxx", Finnish "Mixxx-yhteisö", Slovenian key colours…) |
| Hard-coded visible literals (not `tr()`) | **21** | 6 link labels in Preferences (`"Mixxx&nbsp;Forums"`, `"Mixxx Wiki"`…); default stream name; default recordings folder `~/Music/Mixxx/Recordings`; 3 screensaver inhibitor strings; 3 PipeWire node names; 2 Engine DJ export folder names; 4 terminal messages; the library sidebar section "Mixxx" (Crate's own code, from the GU1 mockup) |
| Links to Mixxx sites | **43** uses of 21 URL macros in 17 files | Help menu 4 entries (manual, shortcuts, community, translate); 29 links in Preferences pages; 2 in About (D2); sound error and MIDI error dialogs; `--help`; QML 2; default stream website; MusicBrainz user agent (not visible) |
| Controller mapping data (`res/controllers`) | **109** of 164 mappings | forum links 100, wiki links 11, manual 1, descriptions 18 ("…mapping for Mixxx 2.1"), authors 2. The name "Yaeltex MiniMixxx" is a hardware product and stays |
| Images | 2 + 7 skins | `ic_mixxx_symbolic.svg` marks built-in skins and mappings in Preferences; the Mixxx logo is in the toolbar or launch image of LateNight, Deere, Shade and Tango (and their 64-sampler variants) and LateNightQML; 2 unused Mixxx logos plus a dead QSS rule left in `CrateDecks` |
| First-run and blocking dialogs | 3 + quit | music folder (`coreservices.cpp`), "No Output Devices" and "hide the menu bar?" (`mixxxmainwindow.cpp`); the quit confirmation is a `tr()` string |
| Installed paths shown in the UI | 1 | in installed builds Preferences › Controllers shows the system mapping folder `…/share/mixxx/controllers` |

Not visible and out of scope: 21 log lines, 27 identifiers, keys and file names, 17 QML developer messages,
one drag-and-drop MIME type. Keyboard mappings, notifications, the `.desktop` file and the app icon name nothing
of Mixxx (F1.PR2 already replaced the last two).

### 2.2. By kind (the 93 `tr()` strings)

| Kind | Count | Treatment |
| --- | --- | --- |
| Mixxx as the application ("Exit Mixxx?", "Quits Mixxx") | 73 | Generic rule: Mixxx → Crate Decks |
| Mixxx as a feature or version name ("Mixxx mode", "Mixxx versions before 1.11") | 8 | Curated text where the rule would lie |
| Help references ("Mixxx User Manual", "Mixxx DJ Hardware Guide") | 7 | Neutral label; the target is decision §7.1 |
| Links inside the text (bug tracker, controller engine) | 2 | Curated: the bug tracker is Crate Decks' (CRATE.md) |
| Credits in About | 3 | Kept (D2) |

## 3. Mechanism

### 3.1. A brand translation layer instead of editing Mixxx's strings

`crate::BrandTranslator` is a `QTranslator` installed in `main.cpp` right after the application exists. Qt asks
the most recently installed translator first, so it sees every `tr()` lookup before Mixxx's:

- When neither the source nor Mixxx's translation for the active locale names Mixxx, it returns nothing and
  Qt continues to Mixxx's translator: no cost beyond one substring search (and one extra lookup in non-English
  locales).
- When one of them names Mixxx, it returns the branded text: the curated text from the inventory if there is
  one, else Mixxx's translation (or the source) with the generic rule.
- **Generic rule** (`crate::brandText`): "Mixxx" as a word becomes "Crate Decks" ("Mixxx's" → "Crate Decks'",
  "MIXXX" → "CRATE DECKS"), and "Mixxx 2.1"-style versions drop the number. It never touches file names, URLs,
  paths or longer words (`mixxxdb.sqlite`, `~/.mixxx`, `https://mixxx.org`, `MixxxControl`, `MiniMixxx`).
- Mixxx installs its own translators later, in the `CoreServices` constructor. Each install sends
  `QEvent::LanguageChange` to the application; the brand translator watches for it, moves itself back to the
  front and loads Mixxx's `.qm` for the new locale. No patch in `coreservices.cpp` or `translations.h`.
- `--help` is translated before `CoreServices` exists, so installing in `main.cpp` also brands the terminal.

**Inventory:** `res/crate-brand/mixxx-strings.json` lists every source string that names Mixxx, its files and its
treatment (`rule`, `text` with an optional Spanish text, or `keep` with the reason). It also lists the hard-coded
literals that name Mixxx and are allowed (identifiers and logs). It is in `res/crate.qrc`; the app reads it at
start. `tools/crate-brand/inventory.py` (standard-library Python, like `tools/crate-design/`) regenerates it from
the sources and keeps the decisions already made.

**Drift test:** `CrateBrandTest.InventoryCoversEveryMixxxString` scans the sources and fails when upstream adds
a translatable string or a hard-coded literal that names Mixxx and the inventory does not list. A merge from
upstream therefore stops at the test until someone reviews the new string.

**Why this and not the alternatives:**

| Option | Patched Mixxx files | Translations | Upstream merges | Verdict |
| --- | --- | --- | --- | --- |
| Edit each source string (what F1.PR2b did for 4 surfaces) | ~40 `.cpp`/`.ui` + 77 `.ts` | Lost in every language but the ones we fix by hand | A conflict on every string upstream touches | No |
| Generated `.qm` overrides per locale | 0 | Kept | None, but a build step per locale and a stale `.qm` when upstream changes a string | Close second |
| **Runtime translator plus inventory** | 0 for `tr()` strings (one line in `main.cpp`) | Kept and branded in all 77 locales | None; new strings are branded by the rule at once and flagged by the test | **Chosen** |

### 3.2. Minimal patches where the text is not translatable

Hard-coded link labels are wrapped in `tr()`, so that the layer brands them (a change upstream would also take).
Names that leave the window (stream name, recordings folder, screensaver, PipeWire node, Engine DJ export) come
from `VersionStore::applicationName()` instead of a literal "Mixxx", which keeps Mixxx's behaviour for Mixxx and
gives "Crate Decks" here. Controller mapping names, authors and descriptions pass through `crate::brandText()`
where Preferences shows them.

### 3.3. Dialogs, images and skins

- Mixxx's first-run dialogs are skipped under `__CRATE__` while Crate's first start is pending, and the first
  start offers the same choices in its own pages (BR.PR3).
- Every `QMessageBox` gets a Solar icon from the tokens when Crate styles it (no platform or Mixxx icon), so the
  sound messages and the quit confirmation look like Crate without patching their code.
- `ic_mixxx_symbolic.svg` keeps its name (the code loads it by name) and draws the Crate symbol.
- Mixxx's skins all carry the Mixxx logo. With `CRATE` the skin list offers only `CrateDecks` (§7.4).

## 4. Tasks

Each task starts with the red test named in it. `TEST_GAP:` marks what a test cannot see.

### BR.PR1 · Brand translation layer

| File | Change |
| --- | --- |
| `src/crate/brand/brandtext.{h,cpp}` (new, compiled always, like `appidentity.cpp`) | `crate::brandText()` (the rule) and `crate::BrandInventory` (curated texts and kept strings read from the inventory) |
| `src/crate/brand/brandtranslator.{h,cpp}` (new, compiled always) | `crate::BrandTranslator` and `crate::installBrandTranslator(QCoreApplication*)` |
| `res/crate-brand/mixxx-strings.json` (new), `res/crate.qrc` | Inventory of §3.1 with the treatments of §2.2 |
| `tools/crate-brand/inventory.py`, `tools/crate-brand/test_inventory.py` (new) | Regenerates the inventory, keeping decisions; pytest checks the extraction against `lupdate` when it is installed |
| `src/main.cpp` | `crate::installBrandTranslator(&app)` after `MixxxApplication app(argc, argv)` |
| `CMakeLists.txt` | The new sources (always) and `brandtest.cpp` |
| `.github/workflows/crate-decks-build.yml` | pytest of `tools/crate-brand` |

**Red tests** (`src/test/crate/brandtest.cpp`):
- `CrateBrandTextTest.RenamesTheApplication`, `KeepsFileNamesUrlsAndLongerWords`, `DropsMixxxVersions`.
- `CrateBrandTranslatorTest.WinsOverTranslatorsInstalledLater`: a Spanish Mixxx translator installed after the
  brand one; "A deck is currently playing. Exit Mixxx?" comes back in Spanish with "Crate Decks".
- `UsesCuratedTexts` ("Mixxx DJ Hardware Guide" → "DJ Hardware Guide"), `KeepsTheAboutCredits`,
  `BrandsTranslationsThatNameMixxx` (Catalan "&About").
- `EveryInventoryStringIsBranded`: every entry, in English and in every shipped `.qm`, comes back without Mixxx
  unless it is `keep`.
- `InventoryCoversEveryMixxxString`: the drift test of §3.1.
- `visiblestringstest.cpp` (F1.PR2b) folds into the drift test: it checked four surfaces, this checks all.

### BR.PR2 · Strings outside `tr()` and mapping data

| File | Change |
| --- | --- |
| `src/controllers/dlgprefcontroller.cpp` | The three mapping link labels in `tr()`; mapping name, author and description through `crate::brandText()` |
| `src/controllers/dlgprefcontrollers.cpp`, `src/preferences/dialog/dlgprefsound.cpp` | Link labels in `tr()` |
| `src/preferences/broadcastprofile.cpp` | Default stream name from `applicationName()` |
| `src/preferences/dialog/dlgprefrecord.cpp` | Default recordings folder `~/Music/<application name>/Recordings` |
| `src/util/screensaver.cpp` | Inhibitor reason and application ID from the application identity |
| `src/soundio/pipewireenumerator.cpp` | PipeWire media, node name and nick, and the lookup of its own node, from `applicationName()` |
| `src/library/export/engineprimeexportjob.cpp` | Export folders "<application name> Crates" and "… Playlists" |
| `src/util/cmdlineargs.cpp` | Four terminal messages without "Mixxx" |

**Red tests:** `CrateBrandTest.ControllerMappingInfoIsBranded` (every mapping in `res/controllers`),
`DefaultStreamNameIsTheApplication`, `RecordingsFolderIsNamedAfterTheApplication`, and the hard-coded part of
`InventoryCoversEveryMixxxString`. `TEST_GAP:` the screensaver and PipeWire names need a session bus and a
PipeWire daemon; the drift test sees their literals.

### BR.PR3 · The first start absorbs Mixxx's first-run dialogs

Visual spec: the GU1 mockup `decks-mockups/first-run.html`. The new pages reuse its grammar (logo, MonoLabel
kicker, display title, paragraph, fields and Crate buttons) and only tokens and Solar icons; their mockups
(`first-run-sound.html`, `first-run-music.html`) go to Diego with the captures (§7.5).

1. **Pages.** "Connect your Crate" stays as approved. "Not now" or a connected server moves on to **Sound**
   (main output device, from `SoundManager`; with no device, a warning state with *Search again* and *Continue
   without sound*; *More options* opens Preferences › Sound Hardware) and then **Music on this computer**
   (optional local folders next to the Crate library; *Add folder*, *Skip*, *Finish*).
2. **Music folder:** `coreservices.cpp` skips Mixxx's folder prompt under `__CRATE__`. The music page adds the
   chosen folders with `Library::requestAddDir()` and starts the library scan Mixxx would start.
3. **No output:** `mixxxmainwindow.cpp` skips "No Output Devices" while the first start is pending; the sound
   page owns that case. Later starts keep Mixxx's logic, with Crate's message style (4).
4. **Messages:** `crate::applyApplicationStyle()` gives every `QMessageBox` the Solar icon of its kind.
5. **Menu bar:** `alwaysHideMenuBarDlg()` asks nothing when the skin decides: `CrateDecks` hides it (Alt shows it,
   Preferences › Interface changes it), as the mockups have no menu bar.

**Red tests:**
- `CrateFirstRunTest.PagesFollowTheFirstStart`, `MusicPageAddsTheChosenFolders`, `SoundPageListsOutputs`,
  `SoundPageWithoutOutputsOffersToContinue`.
- `CrateSkinLoadTest.FirstStartAsksNoMixxxQuestions`: a new profile with no folders and no sound API; no modal
  dialog shows while the app starts, and the menu bar ends hidden.
- `CrateDialogsTest.MessageBoxesUseSolarIcons`.

### BR.PR4 · The UI shows no Mixxx

- **Refactor first, own commit:** `OffscreenDecks` and its helpers move from `skinloadtest.cpp` to
  `src/test/crate/offscreendecks.{h,cpp}`.
- `src/test/crate/brandvisibletest.cpp`, `CrateBrandVisibleTest.NoMixxxAnywhere`: starts the real app offscreen
  with `CrateDecks` and walks every widget's text, tooltip, status tip, placeholder, window title, item text
  and model data in the main window, all menus and their actions, every Preferences page, the first-start
  pages, *About* and the quit confirmation. It fails on "Mixxx" in any case, except the About credits (D2) and
  file names.

### BR.PR5 · Images and skins

- `res/images/preferences/{dark,light}/ic_mixxx_symbolic.svg`: the Crate symbol, same size and colours.
- `res/skins/CrateDecks/`: delete the unused `palemoon/style/mixxx_logo*.svg` and the dead `#ToolbarLogo` rule
  (separate cleanup commit).
- `src/skin/skinloader.cpp`: with `CRATE`, the skin list offers only `CrateDecks` (§7.4).
- **Red tests:** `CrateSkinLoadTest.OnlyCrateDecksIsOffered`, `CrateBrandTest.BuiltInMarkerIsTheCrateSymbol`,
  `CrateBrandTest.CrateDecksSkinHasNoMixxxImages`.

### BR.PR6 · The library sidebar's local section

- `src/crate/ui/skin/librarystyle.cpp` names the section of Mixxx's own library **"Local"**, next to
  "Crate · <server>" (§7.3). The GU1 mockups change the same word (`first-run.html`, `lib/decks.js`).
- **Red test:** `CrateSkinLoadTest.SidebarSectionsNameNoMixxx`.

### BR.PR7 · Help and documentation links (blocked on §7.1)

- One table, `crate::helpUrl(kind)`, for the 21 URL macros of `src/defs_urls.h`. Its targets are Diego's choice.
- Until then the labels are neutral ("User manual", "DJ Hardware Guide") and the targets are unchanged.
- **Red test** (when decided): `CrateBrandTest.NoLinkLeadsToMixxx` over every `href` in the inventory, Help menu
  and Preferences.

### BR.PR8 · Installed paths (with F9.PR1)

- Data under `share/crate-decks` instead of `share/mixxx` (`CMakeLists.txt` install dirs,
  `src/preferences/configobject.cpp` resource lookup, AppImage).
- **Red test:** `CrateBrandingTest.ResourcePathNamesNoMixxx` on an installed tree.

### BR.PR9 · Refactor: undo the F1.PR2b string edits (optional, after BR.PR1)

- The layer makes F1.PR2b's edits to 14 Mixxx `.ui`/`.cpp` files and 10 `.ts` files unnecessary. Reverting them
  shortens `CRATE_PATCHES.md`. Behaviour is the same, so it is a refactor in its own PR.

## 5. Mixxx files patched

New entries for `CRATE_PATCHES.md`; the ones in **bold** are outside the closed list of design §5.2 today.

| File | Change | Task |
| --- | --- | --- |
| `src/main.cpp` | Install the brand translator | BR.PR1 |
| `CMakeLists.txt`, `res/crate.qrc` | Sources, test and inventory | BR.PR1 |
| `src/controllers/dlgprefcontroller.cpp`, `src/controllers/dlgprefcontrollers.cpp`, `src/preferences/dialog/dlgprefsound.cpp` | Link labels in `tr()`; mapping info through `brandText()` | BR.PR2 |
| `src/preferences/broadcastprofile.cpp` | Default stream name | BR.PR2 |
| **`src/preferences/dialog/dlgprefrecord.cpp`** | Default recordings folder | BR.PR2 |
| **`src/util/screensaver.cpp`** | Inhibitor reason and ID | BR.PR2 |
| **`src/soundio/pipewireenumerator.cpp`** | PipeWire node names | BR.PR2 |
| **`src/library/export/engineprimeexportjob.cpp`** | Export folder names | BR.PR2 |
| **`src/util/cmdlineargs.cpp`** | Terminal messages | BR.PR2 |
| `src/coreservices.cpp` | Skip the music folder prompt | BR.PR3 |
| `src/mixxxmainwindow.cpp` | Skip "No Output Devices" during the first start; the skin decides the menu bar | BR.PR3 |
| **`res/images/preferences/{dark,light}/ic_mixxx_symbolic.svg`** | Crate symbol | BR.PR5 |
| `src/skin/skinloader.cpp` | Only `CrateDecks` in the skin list | BR.PR5 |
| `src/preferences/configobject.cpp`, `CMakeLists.txt`, `packaging/` | `share/crate-decks` | BR.PR8 |

Of the new ones, all but the icon and the terminal messages read the application identity instead of a literal
"Mixxx", so Mixxx itself would behave the same with them.

## 6. Change to design §5.2 and AGENTS.md §5.2

- **Design D2, "Marca":** nothing of Mixxx is visible (strings, dialogs, links, images, paths) except the credits
  in About; the brand layer is the mechanism.
- **Design §5.2:** a paragraph with the rule and the rows of §5 above.
- **AGENTS.md §5.2, "Nombres":** visible text goes through the brand layer, never by editing Mixxx's strings;
  only what is not translatable is patched; identifiers, control groups, config keys and file names stay.

## 7. Decisions for Diego

1. **Help and documentation links** (43 uses: Help menu, Preferences, mapping forums and wikis). Options:
   (a) hide them until Crate Decks has its own documentation; (b) point them to Crate pages that redirect to
   Mixxx's manual for now; (c) keep Mixxx's documentation with neutral labels. Recommendation: (b), one table
   (`crate::helpUrl`) so the targets can change without touching code. Until he decides, the labels are neutral
   and the targets unchanged (BR.PR7).
2. **About credits and the metainfo line**: keep "based on Mixxx", the Mixxx team and the GPL notice (D2, and
   the GPL requires appropriate copyright notices). Confirm whether *Donate to Mixxx* stays.
3. **Name of the local section** of the library sidebar, "Mixxx" in the GU1 mockups. Proposal: "Local".
4. **Mixxx's skins** (LateNight, Deere, Shade, Tango): all carry the Mixxx logo. Proposal: offer only
   `CrateDecks` (implemented in BR.PR5, a one-line filter to undo). This changes V2's "LateNight stays
   selectable".
5. **New first-start pages** (sound and local music): approve their mockups and captures, like GU1.
6. **Local folders** stay optional next to the Crate library (assumed from the brief; BR.PR3 offers them as an
   optional last page).

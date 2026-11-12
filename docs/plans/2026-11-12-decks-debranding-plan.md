# Crate Decks — Own brand, second pass (BR2): no Mixxx in files, logs, packages or our code

> Addendum to the [own brand plan](2026-11-09-crate-decks-own-brand-plan.md) (BR.PR1–BR.PR7, merged). It covers
> only what that plan left out under the wider rule of 2026-11-12. AGENTS.md applies.

**Date:** 2026-11-12. **Base:** `main` @ `13569619bf`.

## 1. The rule

Diego, 2026-11-12: *"debe desaparecer cualquier referencia a Mixxx que vaya más allá de la atribución de licencia y
decir que Decks es un fork de Mixxx"*. Scope: **everything visible and everything ours**.

- **Must go:** what the user sees (BR already covers the interface); the names of the files and folders Decks writes
  (`mixxx.cfg`, `mixxx.log`, `mixxxdb.sqlite`); log lines; installed paths and package files; help and manual
  links; our own code and comments; documentation, PR texts and commit messages from now on.
- **Stays:** class names, namespaces, CMake targets, control groups and config keys of the inherited code, the
  controller scripting API and any other identifier the user never sees.
- **Allowed:** licence attribution (LICENSE, COPYING, About credits, the CC BY-SA header of `CrateDecks`, which
  derives from LateNight); one plain statement that Crate Decks is a fork of Mixxx (README, About, metainfo);
  developer text about the upstream relationship (sync, `CRATE_PATCHES.md`, `CRATE.md`, `docs/mixxx-merges.md`,
  AGENTS.md §9 with its verbatim copy of upstream's AI policy, and the plans).
- It replaces the line of BR §1 that kept "file names" and the old AGENTS.md §5.2 "Nombres".

## 2. What BR left (measured on the base)

| Category | Left | Mechanism (BR2) |
| --- | --- | --- |
| Files in the settings folder | `mixxx.cfg`, `mixxx.log` (and `.1`–`.10`), `mixxxdb.sqlite` | Names from the application identity: `crate-decks.cfg`, `crate-decks.log`, `crate-decks.sqlite` |
| Visible strings that name those files or `~/.mixxx` | 6 `tr()` strings (`--help`, database errors, developer tools, Crate preferences) in all locales | The brand rule maps Mixxx's file names to Decks' ones, so the translation layer fixes them in every language |
| Reads of a real Mixxx install | `Upgrade::versionUpgrade()` moves `~/.mixxx.cfg` and three other pre-1.7 files from `$HOME`; on macOS and Windows it switches to Mixxx's old folders | Never: Decks reads and writes only its own folder (`crate::kReadsMixxxSettings`, false) |
| Log lines | 14 literals name Mixxx, plus whatever upstream adds | The log handler passes each message through the brand rule; one patch, also for future lines |
| Installed tree | `share/mixxx`, `share/doc/mixxx`, `Mixxx-Manual.pdf` (CI downloads it from mixxx.org), `Mixxx-Keyboard-Shortcuts.pdf`, `mixxx-usb-uaccess.rules`, `mixxx-appimage.desktop`, the 8 Mixxx skins | `share/crate-decks`, `share/doc/crate-decks`, no Mixxx PDFs, renamed rules and desktop file, only `CrateDecks` installed (BR.PR8, done here) |
| Mixxx's skins | Hidden from Preferences but loadable by name from the resource folder (BR §7.4) | Not installed and not loadable from the system folder; they stay in the tree for upstream syncs and tests |
| Our code and comments | "Mixxx" as a word in comments and strings of `src/crate/`, `src/library/crateserver/`, `src/test/crate/`, `res/skins/CrateDecks/`, `res/crate-design/`, `tools/crate-design/` | Reworded ("upstream", "the inherited code"); `tools/crate-brand/inventory.py --check` fails on a new one |
| iOS settings folder | `…/Application Support/Mixxx` | The application name (iOS is not built today) |

Already done by BR and earlier phases, not repeated: the interface strings and their 77 translations, the
first-run dialogs, help links, executable `crate-decks`, settings folder `~/.cratedecks/`, `.desktop`, metainfo,
icons, Wayland app id `app.cratemusic.decks`, keychain prefix, screensaver and PipeWire names. X11 takes the window
class from the executable and the application name (`crate-decks`, `Crate Decks`); Decks registers no D-Bus name of
its own (the screensaver inhibitor uses the application id).

**Not in scope, recorded:** the macOS bundle (`Mixxx.app`, `org.mixxx.mixxx`), the Windows executable and MSI, and
Android are not built today; F9.PR2 and F9.PR3 give them Decks' identity together with the macOS sandbox
migration that the design already lists as blocking. Upstream files that Decks neither installs nor shows
(`res/linux/org.mixxx.Mixxx.*`, `CHANGELOG.md`, `CONTRIBUTING.md`, the Mixxx skins' sources) stay for the syncs.

## 3. Decisions taken here (with the reason)

1. **File names:** `crate-decks.cfg`, `crate-decks.log`, `crate-decks.sqlite`, after the executable. Decks is
   unreleased, so nothing is migrated: a profile made by an earlier build of Decks starts as new. Developers keep
   theirs by renaming the three files.
2. **Upstream's old-settings migrations** never run: they read and delete files of a real Mixxx install.
3. **Logs are branded at the handler,** not line by line: one patched file instead of 14, and it covers lines
   that upstream adds. Identifiers in logs (class names, `mixxx::` loggers) stay, like in code.
4. **Mixxx's skins** are neither installed nor loadable from the system folder: they all carry the Mixxx logo.
   A skin the user copies into their own `skins` folder still loads (their choice, as in Mixxx). The tests that
   guard LateNight load it from a profile's `skins` folder.
5. **Controller mappings:** names, authors and descriptions are already branded where Preferences shows them
   (BR.PR2), and their forum and wiki links lead to Crate's docs (BR.PR7). Their files stay as upstream ships
   them, scripting API included; the hardware "Yaeltex MiniMixxx" keeps its name.
6. **Translations** keep the base name `mixxx_<locale>.qm`: an identifier the user never sees, and renaming it
   would touch every `.ts` file at each sync.

## 4. Tasks

Each starts with its red test; `TEST_GAP:` marks what a test cannot see.

### BR2.PR1 · This plan

This document, AGENTS.md §5.2 "Nombres" and design §5.2.

### BR2.PR2 · Own files and no reads of Mixxx's

| File | Change |
| --- | --- |
| `src/crate/appidentity.h` | `kLogFileName`, `kDatabaseFileName`, `kInstallDataDirName`, `kReadsMixxxSettings` |
| `src/config.h.in` | `MIXXX_SETTINGS_FILE` = `crate-decks.cfg` |
| `src/util/logging.cpp` | Log file name; messages through `crate::brandRule()` |
| `src/dialog/dlgdevelopertools.cpp` | Reads the log by its new name |
| `src/database/mixxxdb.cpp` | Database file name |
| `src/preferences/upgrade.cpp` | Skips the migrations from Mixxx's old folders |
| `src/util/cmdlineargs.cpp` | iOS settings folder named after the application |
| `src/crate/brand/brandtext.cpp` | The rule maps `mixxx.cfg`, `mixxx.log`, `mixxxdb.sqlite` and `~/.mixxx` to Decks' names |

**Red tests:** `CrateBrandTextTest.NamesDecksFiles`; `CrateBrandingTest.SettingsFolderNamesNoMixxx` (a new profile
started offscreen holds no file named after Mixxx, and its log names no Mixxx);
`CrateBrandingTest.UpgradeReadsNoMixxxInstall` (a `$HOME` with Mixxx's pre-1.7 files keeps them).

### BR2.PR3 · Installed tree (BR.PR8)

| File | Change |
| --- | --- |
| `CMakeLists.txt` | Data in `share/crate-decks`, docs in `share/doc/crate-decks`; only `CrateDecks` and the shared QSS of `res/skins`; no Mixxx PDFs; udev rules as `crate-decks-usb-uaccess.rules` |
| `src/preferences/configobject.cpp` | Looks for `../share/crate-decks` |
| `src/skin/skinloader.cpp` | `getSkin()` finds no Mixxx skin in the system folder |
| `packaging/appimage/appimage.cmake`, `packaging/CPackConfig.cmake` | `crate-decks-appimage.desktop` |
| `.github/workflows/crate-decks-build.yml` | No manual download from mixxx.org; installs the build and checks the tree |
| `tools/crate-brand/check_install.py` (new) | Fails on an installed path that names Mixxx, apart from upstream identifiers (`mixxx_*.qm`, `lodash.mixxx.js`, `mixxx-controls.d.ts`, the QML module `Mixxx`, MiniMixxx) |

**Red tests:** `CrateSkinLoadTest.OnlyCrateDecksIsOffered` (LateNight no longer loads by name);
`tools/crate-brand/test_check_install.py`. `TEST_GAP:` the AppImage itself is only checked by its CI job.

### BR2.PR4 · Our code says nothing of Mixxx

- Reword the comments and strings of our directories. The brand layer and its tests name Mixxx on purpose (they
  match upstream's text): they are the only files exempt.
- `tools/crate-brand/inventory.py` scans our directories too; the inventory lists each hit that must stay (an
  object name or text that matches upstream's) with its reason, and `--check` fails on any other.
- Waits for the ST5 stems PRs (#28, #29), which edit many of the same files, so that it does not conflict with them.

**Red test:** `tools/crate-brand/test_inventory.py::test_own_code_names_no_mixxx`.

## 5. Mixxx files patched (new for `CRATE_PATCHES.md`)

Outside the closed list of design §5.2 until this plan: `src/config.h.in`, `src/util/logging.cpp`,
`src/dialog/dlgdevelopertools.cpp`, `src/database/mixxxdb.cpp` (already patched by BR.PR7),
`src/preferences/upgrade.cpp`, `src/preferences/configobject.cpp` (in the list since BR.PR8),
`packaging/appimage/appimage.cmake`, `packaging/CPackConfig.cmake` (already patched by F1). Each change reads the
application identity or is one condition, so a sync conflicts only on those lines.

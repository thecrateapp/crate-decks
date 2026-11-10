# Crate Decks

[![Build](https://github.com/thecrateapp/crate-decks/actions/workflows/develop.yml/badge.svg?branch=main)](https://github.com/thecrateapp/crate-decks/actions/workflows/develop.yml?query=branch%3Amain)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)
[![Version: 0.1.0, unreleased](https://img.shields.io/badge/version-0.1.0%20%28unreleased%29-lightgrey)](#versions)
[![Platform: Linux](https://img.shields.io/badge/platform-Linux-informational)](#platforms)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)](CMakeLists.txt)
[![Qt 6](https://img.shields.io/badge/Qt-6-41CD52)](#build-requirements)

![Crate Decks icon](res/images/icons/128x128/apps/app.cratemusic.decks.png)

**DJ software that uses your Crate server as its library and its brain.**

Crate Decks is a desktop DJ application for [Crate](https://cratemusic.app), the self-hosted music platform.
It browses the library of a Crate server next to the music on your computer, and is being built to load Crate's tracks into the decks with Crate's analysis, recommendations and Smart Mix transitions.

Crate Decks is a fork based on [Mixxx](https://mixxx.org), under the same GPL license.

> **Status: in development, no release yet.**
> The features below are on `main`.
> Crate tracks can be browsed but not yet loaded into a deck: see the [roadmap](#roadmap).

## Versions

| Component | Version |
| --- | --- |
| Crate Decks | `0.1.0`, unreleased. Builds report `0.1.0-dev+g<commit>` until the first `decks-vX.Y.Z` tag |
| Crate server | Crate 3.2.0 or later to connect; the library in the sidebar needs a server release that is not out yet (below) |
| Decks server contract | `contract_version` 1 |

### What each feature needs from the server

Crate Decks reads `GET /api/capabilities` and only turns on what the server announces in its `decks` section.
A server without a feature keeps working: Decks hides or skips what depends on it.

| Server feature | What Decks uses it for | Crate release |
| --- | --- | --- |
| Native `decks` client sign-in (S1) | Sign-in and session | 3.2.0 |
| `decks` capabilities, `min_client_version` and `contract_version` (S2) | Accepting a server when it is added | 3.2.0 |
| `library_cursor` (S3) | The Crate library in the sidebar and its sync | **Not released yet** |
| `stems`, `stem_upload`, `local_refinement` (Smart Mix Stems) | Nothing yet: Decks will use them for stems in the decks | 3.3.0 |

The server side of the contract is documented in the Crate repository, in `docs/technical/crate-decks.md`.

### Platforms

Linux is the only platform built and tested in CI today (Ubuntu 24.04, plus an x86_64 AppImage built on Ubuntu 22.04 as a development artifact).
macOS and Windows packages are planned for the packaging phase and are not available.

## Features

### Crate server connection and session

- **Add a server** from the first start or from *Preferences → Crate*: enter its address, Decks checks the server's capabilities, then you sign in.
- **Sign in** with email and password, or in the browser with OAuth (PKCE and a loopback callback on `127.0.0.1`).
- **Secrets stay in the system keychain** (QtKeychain): only the refresh token is kept, never in the settings file, logs or URLs.
- **Safe addresses:** HTTPS by default; plain HTTP is accepted for this computer, asks for confirmation on the local network and is refused on the internet.
- **Several servers** can be added, each once, with their state on the Crate preferences page.
- **A robust client:** retries with backoff only on 429, 502, 503 and 504, honours `Retry-After`, refreshes the session once on a 401 and replays the waiting requests, and limits concurrent requests per server.

### The CrateDecks skin and design tokens

- **One design language with Crate's admin:** colours, sizes, radii and fonts come from `res/crate-design/tokens.json`, and `tools/crate-design/generate.py` turns them into the skin's QSS, variables and QML theme. `generate.py --check` fails when the generated files drift from the tokens.
- **The CrateDecks skin** is the default and the only one offered: decks, waveforms, mixer, effects, samplers, library, toolbar and dialogs, built from the approved mockups at 1920 × 1080 and 1366 × 768.
- **Solar icons only**, recoloured per state by the generator.
- **The admin's fonts:** Roboto, Bricolage Grotesque and JetBrains Mono, shipped with the application.
- **Details the skin adds:** a toolbar with the Crate server's state, a button that opens Preferences, intro and outro marks on the overview, a dimmed main window behind blocking dialogs, and library cells that paint Camelot keys, profile quality and cache state.

### The Crate library in the sidebar

- **A "Crate · ⟨server⟩" section** in the library sidebar with Library, Playlists, Crates, Genres, Moods, Recent and Likes.
- **Synced into local tables** (`crateserver_*`), page by page with ETags, so an unchanged page is not downloaded again; parsing runs off the main thread.
- **Kept current by the server's events:** Decks listens to the server's cache events and marks the affected containers for the next sync.
- **Track tables** with Camelot, energy, profile quality and cache state, sorted and filtered off the main thread.
- Needs `library_cursor` on the server; loading these tracks into a deck is on the [roadmap](#roadmap).

### Writing to the library during a scan

Writing to the music library while it is being scanned no longer fails with "database is locked".
The scanner commits new tracks in short batches and yields as soon as another connection waits, write transactions take SQLite's write lock up front, and every connection retries every millisecond while the database is busy.

### Its own brand

The interface names Crate Decks everywhere: window titles, menus, dialogs, Preferences, tooltips, the command line, the stream name of new broadcast profiles and the folders Decks creates.
The first start folds the usual setup questions into one Crate flow, the local library's sidebar section is called "Local", and the About dialog keeps only the credits and the license.
A test starts the application without a screen and fails on any visible text that names the upstream project.

### Stems: inference runtime and separator core

The groundwork for separating tracks into stems on your machine is in place, but **it is not wired into the decks yet**.

- **Inference runtime** (built when ONNX Runtime is found, option `CRATE_INFERENCE`): detects the available execution providers and falls back to CPU, with the reason, when a provider is missing or free VRAM is short.
- **Model store:** downloads models from a manifest with resume, SHA-256 verification and size checks, and never stores outside the data directory. The `htdemucs` model exported to ONNX is published as the release [`models-htdemucs-onnx-1`](https://github.com/thecrateapp/crate-decks/releases/tag/models-htdemucs-onnx-1).
- **Separator core:** decodes a track, resamples it to 44.1 kHz stereo and runs `htdemucs` with its STFT, iSTFT and overlap-add around the network, giving four stems. It is checked against reference fixtures (≥ 60 dB SDR per stem).

## Roadmap

Planned, **not done**:

- **Load Crate tracks into the decks** with Crate's analysis, through a local audio cache, and later progressive playback while downloading.
- **DJ search** in the Crate section of the library.
- **Recommendations:** compatible tracks, Radio as an Auto DJ source and Music Paths.
- **Smart Mix in the decks:** an Auto DJ mode and an assistant that run the transition plans of Crate's planner.
- **Stems in the decks:** separation, sync with the server and stem actions in the transitions.
- **Instrument widgets:** the Camelot wheel, the Smart Mix plan view and a level ring.
- **Writing back to Crate:** history, now playing, likes, ratings and playlists; then synced cues and corrections.
- **Packages** for Linux, macOS and Windows, and the first release.

The full plans are in [`docs/plans/`](docs/plans/), in Spanish and English.

## Building

### Build requirements

- CMake 3.22 or later and Ninja.
- A C++20 compiler; CI uses GCC on Ubuntu 24.04.
- Qt 6 (6.2 or later); CI builds with Ubuntu 24.04's Qt 6.4.
- Optional: an [ONNX Runtime](https://onnxruntime.ai) release package for the stems runtime, passed with `-DONNXRUNTIME_ROOT=<path>`.

The setup scripts install every dependency (they use `sudo`):

```shell
tools/archlinux_buildenv.sh setup   # Arch and CachyOS
tools/debian_buildenv.sh setup      # Debian and Ubuntu
```

### Build, test and run

```shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
build/crate-decks --settingsPath "$(mktemp -d)"
```

`--settingsPath` with a throwaway folder keeps a test run away from your real settings.
Without it, Crate Decks keeps its settings and local library in `~/.cratedecks/` on Linux.

## Connecting to a Crate server

1. Start Crate Decks. With no server yet, the first start offers to connect one; later, use *Preferences → Crate → Add server*.
2. Enter the server's address, for example `https://crate.example.com`.
3. Decks checks the server's capabilities. It refuses a server that does not accept Crate Decks or needs a newer version.
4. Sign in with your Crate account, by password or in the browser.

To browse the Crate library in the sidebar, the server must announce `library_cursor` (see [Versions](#versions)).

## Contributing

- Report bugs and ideas in [this repository's issues](https://github.com/thecrateapp/crate-decks/issues).
- Code style, the build in depth and the pre-commit hooks: [CONTRIBUTING.md](CONTRIBUTING.md). Commits follow conventional commits, such as `feat(decks): …`.
- Crate's own code lives in `src/crate/`, `src/library/crateserver/` and `src/test/crate/`. Every change to an inherited file is listed in [CRATE_PATCHES.md](CRATE_PATCHES.md).
- Project notes and rules: [CRATE.md](CRATE.md) and [AGENTS.md](AGENTS.md).

## License and credits

Crate Decks is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License or (at your option) any later version.
See [LICENSE](LICENSE), which keeps the original notice: Copyright (C) 2001-2026 Mixxx Development Team.

- **Mixxx:** Crate Decks builds on the work of the Mixxx contributors.
- **Fonts:** Roboto, Bricolage Grotesque and JetBrains Mono, under the SIL Open Font License 1.1 (`res/fonts/`).
- **Icons:** Solar Icons Set by 480 Design, under CC BY 4.0, recoloured for the skin (`tools/crate-design/svg/solar/`).

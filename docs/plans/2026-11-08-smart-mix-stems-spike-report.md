# Smart Mix Stems · ST0 spike report

> **Input for Diego's STEM-G0 decision. This report does not decide the gate.** The gate decides the separator model and its weights licence, the Crate Decks inference path, and whether the server can separate sessions.

- **Date:** 2026-11-08.
- **Plan and design:** ST0 in [the implementation plan](2026-11-08-smart-mix-stems-implementation-plan.md); STEM-D3, D9 and §6–§9 of [the design](2026-11-08-smart-mix-stems-design.md).
- **Code:** throwaway scripts outside every repository (`crate-agents/stems-spike/`), with torch 2.11.0+cu128 and demucs 4.1.0. Nothing from the spike ships.
- **Machine:** development laptop with an Intel Core Ultra 9 185H (22 threads) and an RTX 4060 Laptop GPU (8 GB). The production server is an i7-7700 without a GPU.
- **Audio:** nine tracks, one per genre: house, techno, drum and bass and other dance from the SM5 electronic set (copied from production read-only with Diego's approval), plus industrial metal, post-punk, noise rock, industrial and hardcore from the development library.
- **Diego's constraint on method:** short measurements and published numbers instead of long batch runs.

## 1. Summary and recommendations

**What STEM-G0 asks, with the recommendation for each:**

| Decision | Recommendation | Evidence |
|---|---|---|
| **Model and weights** | `htdemucs` (four stems) everywhere; `htdemucs_ft` as an option in Decks. Accept the weights' unclear licence, which does not block a free, non-commercial project. Publish the ONNX export as a Decks release asset with attribution and a note on the training data | §3 |
| **Decks inference path** | ONNX Runtime with only the `htdemucs` network in the graph, and STFT, iSTFT, segmentation and overlap-add in C++. Providers: CUDA when the system has CUDA 13 and cuDNN 9, DirectML on Windows, CoreML on macOS, CPU everywhere, and WebGPU evaluated as the driver-only GPU path on Linux. ST4 brings peak VRAM from 3 GB to under 1.5 GB | §5 |
| **Server feasibility** | Go, with the same ONNX graph on ONNX Runtime CPU (no torch in the worker). Estimate: 3–5 hours for a 50-track session on the i7-7700, measured for real in ST2.PR1 | §2, §5 |
| **Stems playback in Decks (amends STEM-D9 and D6)** | Option B: a lossless 4-stem sidecar whose `other` is the residual, so the stems add up to the original. The original stays the track's file. The server stores three lossy stems, and Decks rebuilds the residual locally | §6, §8 |

**Other results:**
- **Speed** (§2, §5): `htdemucs` separates a 5-minute track in about 8 s on a laptop RTX 4060, with ONNX Runtime or PyTorch.
- **Parity** (§5): ONNX Runtime reaches 64–86 dB SDR against PyTorch.
- **Stem levels** (§4):
  - The vocal stem finds voice where YAMNet fails (hardcore, industrial metal, industrial).
  - An absolute level cut does not work for drums and bass.
  - ST3 calibrates the vocal cut against lyrics labels and uses levels relative to each track.
- **NI STEM files written with FFmpeg load in the fork** (§6). NI STEM becomes an optional export for other software.
- **Converting a track to a stem file** (§7) is possible with own code, but option B makes it unnecessary.

**Work this adds to the plan, if approved:**
- **ST4.PR1:**
  - attention export that keeps VRAM under 1.5 GB;
  - CUDA library detection and an optional download of the provider library;
  - a WebGPU evaluation;
  - DirectML and CoreML measured on real machines.
- **ST5:**
  - sidecar writer (resample back to the original rate, integer residual, 8-channel FLAC);
  - PCM fingerprint check;
  - byte-order fix in the stem atom parser;
  - optional NI STEM export.
- **ST6:**
  - the 4–5 patches of §8 instead of the file switch;
  - stems applied on the next load, with an automatic reload when the deck is stopped.
- **ST2:** ONNX Runtime CPU in the analysis worker instead of PyTorch; three AAC stems per track for Decks to download.

## 2. Q1 · Separation speed

| Model | Device | Measured | Per 5-minute track |
|---|---|---|---|
| `htdemucs` | RTX 4060 Laptop, PyTorch | 30–34× realtime on nine tracks of 108–343 s (3.4–10.3 s each) | ~10 s |
| `htdemucs_ft` | RTX 4060 Laptop, PyTorch | 37.8 s for the 312 s house track (8.3× realtime) | ~36 s |
| `htdemucs` | Core Ultra 9 185H, 8 threads, PyTorch | 1.26× realtime on a 30 s clip | ~4 min |

**Server estimate.** The i7-7700 has 4 cores and 8 threads, AVX2 and no GPU, and shares the night window with the Smart Mix backfill. On the laptop's CPU, separation is 1.26× realtime with 8 threads. On the i7-7700 we expect 6–10 minutes per 5-minute track, so a 50-track session takes 5–8 hours: about one night, as the design's budget (§6) requires. This is an estimate, not a measurement on the server.

**Crate Decks.** On a mid-range laptop GPU, `htdemucs` separates a track in about 10 s and `htdemucs_ft` in under a minute. A 300-track crate takes under an hour with `htdemucs`.

## 3. Q2 · Licence of the weights

- **Code:** Demucs is MIT ([facebookresearch/demucs](https://github.com/facebookresearch/demucs), archived; maintained at [adefossez/demucs](https://github.com/adefossez/demucs)).
- **Weights:** the repositories publish no separate licence for the pretrained models. The only licence stated is the repository's MIT licence.
- **Training data:** `htdemucs` was trained on MUSDB18-HQ plus 800 songs held by Meta. The maintained fork warns that "MusDB contains some data that were only usable for research purpose". The six-stem model is usually described as non-commercial.
- **What this means for Crate.** Crate is free and open source with no commercial use, so a non-commercial restriction does not block it. The remaining risk is redistribution: the plan publishes the ONNX exports as release assets of Crate Decks, which makes Crate a distributor of weights with an unclear licence. They would ship with attribution and a note on the training data.

## 4. Q3 · Do the stem levels say what the planner needs?

The levels follow `stem-analysis/1`: one byte per 250 ms, −60 dBFS → 0 and 0 dBFS → 255.

**Vocals against YAMNet.** Share of time with voice, by detector. The stem column uses a level cut of −35 dBFS. "Agree" is the share of windows where both detectors say the same.

| Genre | YAMNet | Vocal stem | Agree | YAMNet only | Stem only |
|---|---|---|---|---|---|
| drum and bass | 10.5 % | 0.3 % | 89.1 % | 10.5 % | 0.3 % |
| hardcore | 31.4 % | 46.0 % | 78.8 % | 3.3 % | 17.9 % |
| house | 87.4 % | 70.8 % | 77.3 % | 19.7 % | 3.1 % |
| industrial metal | 47.3 % | 67.1 % | 66.2 % | 7.0 % | 26.8 % |
| industrial | 47.0 % | 57.8 % | 71.3 % | 8.9 % | 19.7 % |
| noise rock | 80.4 % | 80.0 % | 93.3 % | 3.6 % | 3.1 % |
| other dance | 6.8 % | 20.0 % | 78.3 % | 4.2 % | 17.4 % |
| post-punk | 58.4 % | 57.7 % | 74.4 % | 13.2 % | 12.5 % |
| techno | 91.4 % | 75.1 % | 75.3 % | 20.5 % | 4.2 % |

- The vocal stem finds more voice where YAMNet is known to fail: hardcore, industrial metal and industrial (+11 to +20 points).
- It finds less in house and techno (−16 to −17 points). Without lyrics labels we cannot tell whether YAMNet is reading synth leads as voice or the stem is missing processed vocals.
- **Consequence:** the cut for "vocal stem present" is calibrated against lyrics labels in ST3, as the plan already says. The format stores raw levels, so the calibration needs no format change.

**Drums and bass with an absolute cut.** The same −35 dBFS cut gives values that contradict what the tracks obviously contain: bass present 3.9 % of the time in the drum and bass track, drums 26 % in the techno track and 44 % in the hardcore track.

- An absolute level cut is not a usable presence rule for these stems. ST3 should use levels relative to each track (for example against the stem's own percentiles or against the mix level).
- Demucs was trained mostly on band music. Electronic basses may land partly in `other`. ST3 checks this on the electronic set before the planner trusts the bass stem.

## 5. Q4 · ONNX export of `htdemucs`

**Answer: yes.** The `htdemucs` network exports to ONNX with standard ops only. It runs under ONNX Runtime with near-exact parity to PyTorch and the same GPU speed. The cut is the one [sevagh/demucs.onnx](https://github.com/sevagh/demucs.onnx) (MIT) uses: STFT and iSTFT outside the graph.

**Setup.**
- **Versions:** ONNX Runtime 1.31.1, onnx 1.23.2, torch 2.11.0+cu128.
- **Clip:** a 30 s clip of a techno track.
- **Settings:** the same `apply_model` settings on both sides: `shifts=0`, overlap 0.25.
- **Reference:** PyTorch on CPU in fp32.
- **Load:** the CPU was shared with another agent (load 8–20 on 22 threads), so CPU times are pessimistic.

**Parity** (maximum absolute difference / SDR against the reference):

| Stem | ORT CPU | ORT CUDA | ORT CUDA with the reference STFT in numpy |
|---|---|---|---|
| drums | 6.4e-4 / 85.6 dB | 1.6e-3 / 75.2 dB | 74.3 dB |
| bass | 2.2e-4 / 68.8 dB | 5.4e-4 / 64.3 dB | 64.7 dB |
| other | 6.8e-4 / 75.6 dB | 1.6e-3 / 66.2 dB | 65.1 dB |
| vocals | 5.1e-5 / 82.4 dB | 2.5e-4 / 70.7 dB | 70.6 dB |

ONNX Runtime stays closer to the reference than PyTorch's own CUDA path does (19–56 dB).

**Speed** (one 7.8 s segment, median after warm-up; a 5-minute track is 52 segments):

| Runtime | Per segment | Per 5-minute track |
|---|---|---|
| ORT CUDA, RTX 4060 Laptop | 0.155 s (50× realtime) | ~8 s |
| torch CUDA | 0.153 s | ~8 s |
| ORT CPU, 8 threads | 3.2–4.0 s | ~3–3.5 min (≈1.5× realtime) |
| torch CPU, 8 threads, same load | 5.8–7.5 s | ~5–6.5 min |

**Startup:** session creation takes 1.0 s on CPU and 1.5–1.8 s on CUDA; the first CUDA run takes 1.4 s.

**Model:**
- **`htdemucs`:** 174 MB in fp32 (opset 17; the opset 18 export gives the same result).
- **`htdemucs_ft`:** four graphs of the same shape, one per stem, so 4× the time and about 700 MB.
- **fp16:** a direct conversion produces NaN.
- **GPU nodes:** all 1,187 nodes run on the CUDA provider.

**What Decks implements around the graph** (exact parameters checked against PyTorch with a numpy reference, 1e-6):
- **Graph inputs:**
  - `mix`, the waveform segment (1 × 2 × 343,980 samples at 44.1 kHz);
  - `spec`, its STFT (1 × 4 × 2048 × 336, real and imaginary parts packed as channels).
- **Graph outputs:**
  - the masked spectrogram per stem;
  - a time-domain branch per stem.

  Each stem is the iSTFT of the first plus the second.
- **STFT and iSTFT:**
  - n_fft 4096, hop 1024, periodic Hann window, `normalized` (÷64 and ×64);
  - reflect padding and `center`;
  - the Nyquist bin and two edge frames dropped and restored.
- **The rest of `apply_model`:**
  - normalisation by the mono mix's mean and standard deviation;
  - segments with a stride of 257,985 samples;
  - triangular overlap-add.
- **Why STFT stays outside the graph.** ONNX has no iSTFT op (checked up to opset 28), and the CUDA provider has no STFT kernel, so the C++ code needs an FFT in any case.
- **Test fixtures:** the numpy reference is the source for the C++ golden tests in ST5.PR1.

**What Decks needs, valued and proposed:**

| Need | Finding | Proposal | Phase |
|---|---|---|---|
| Run on 4 GB GPUs | The exported graph materialises the attention matrix: peak 3.07 GB VRAM with `enable_mem_pattern=False` (5.4 GB by default). PyTorch uses 0.9 GB | Export attention as ONNX `Attention` (opset 23) or ORT's fused attention, or split the attention by query chunks in the export. Target: under 1.5 GB. Until then, the runtime picks CPU when free VRAM is short | ST4.PR1 |
| GPU on Linux with NVIDIA without bundling vendor libraries | ORT 1.31's CUDA provider needs CUDA 13 and cuDNN 9 (about 2 GB, not part of the driver) and a 284 MB provider library | Use CUDA when the system has those libraries (checked with `dlopen`), and offer the provider library as an optional download. Evaluate ORT's WebGPU provider (Vulkan), which needs only the driver, as the default GPU path on Linux | ST4.PR1 |
| Windows and macOS GPUs | DirectML and CoreML not tested here (op coverage, 5-D tensors, parity, speed) | Measured in ST4.PR1 on real machines, with the same parity test | ST4.PR1 |
| Smaller download | fp16 needs real mixed-precision work | Ship fp32 first; fp16 later if the download matters | Later |

**The server can use the same graph.** ONNX Runtime on CPU is about 1.8× faster than PyTorch here, under the same load.
- **One implementation of separation:** the server and Decks run the same `htdemucs` graph and the same DSP, so their analyses come from one separator.
- **A smaller worker image:** ONNX Runtime instead of torch.
- **A faster session:** applied to the Q1 estimate, a 50-track session on the i7-7700 drops to about 3–5 hours (still an estimate).

## 6. Q5 · NI STEM files in the fork

**Answer: yes.** A synthetic `.stem.mp4` written with FFmpeg loads in the existing build with `STEM=ON`. It holds a master plus drums, bass, other and vocals, in AAC LC at 256 kbps, 44.1 kHz stereo, with a `moov/udta/stem` atom.

**How it was checked.** The file was bind-mounted over the `stem01` AAC fixture in a private mount namespace and the stem tests were run (`build/mixxx-test`, the `*/AAC_256kbps_VBR_sin` cases). The repository was not edited.
- **Load and playback pass:** `ReadMainMix`, `ReadEachStem`, `OpenStem`, `StemCount`, `VolumeResetOnLoad` and `VuMeter`.
- **Our manifest is read as written:** `FetchStemInfo` and `StemColor` fail only because they expect the fixture's names and colours; the values read are ours.
- **Two tests can't judge our audio:** `Volume` and `Mute` compare engine output against recorded buffers of the fixture's audio, so they fail on any other audio.

**Structure.** The same as the NI fixture:
- five AAC streams;
- the master track enabled and the stems disabled, all in alternate group 1 (FFmpeg does this from `-disposition`);
- the JSON manifest (`version`, `mastering_dsp`, `stems[{name, color}]`) as a raw `stem` box inside `moov/udta`.

The variant with `moov` before `mdat` (faststart) also loads.

**How the inherited code reads it** (`src/track/steminfoimporter.cpp`, `src/sources/soundsourcestem.cpp`):
- **Detection:** the file type comes from the MIME type plus the presence of the `stem` atom (`soundsource.cpp:58-67`).
- **Shape:** exactly one master and four stems, all stereo, same codec and sample rate.
- **The master stream is never decoded.** The deck plays the sum of the four stems (`soundsourcestem.cpp:138-153`).
- **Manifest:** `version` must be at least 1. `mastering_dsp` is ignored, and a missing colour falls back to NI's defaults.
- **Parser:**
  - it reads 64-bit box sizes without byte swapping, so an `mdat` over 4 GiB before `moov` breaks it;
  - it expects a single `udta` in `moov`.

**Engine controls.**

| Group | Keys |
|---|---|
| `[ChannelN]` | `stem_count` (read-only), `load_selected_track_stems` (stem mask) |
| `[ChannelN_StemM]`, M = 1–4 | `volume` (default 1.0), `mute`, `vu_meter`, `vu_meter_left`, `vu_meter_right`, `peak_indicator`, `peak_indicator_left`, `peak_indicator_right`, `color` (read-only; −1 when the track has no stems) |
| `[QuickEffectRack1_[ChannelN_StemM]]` | quick effect per stem (`enabled`, …) |
| `[Mixer Profile]` | `stem_auto_reset` (default on): loading a track resets stem volume, mute and quick effects |

**Other behaviour of the inherited code:**
- **Main decks only:** stem controls exist only there. Samplers and the preview deck play the stems' sum.
- **AutoDJ** loads all stems, and the director can drive them through the controls above (ST6.PR2).
- **Mixxx's own analysis** (BPM, key, gain) reads the stems' sum. It does not matter for Crate tracks, whose analysis comes from the server and is locked on import.

**Recipe for ST5 (C++ with libav, which the fork already links):**
1. **Output and streams:** an MP4 output context with five streams: the original mix as master, then drums, bass, other and vocals.
   - FFmpeg's native `aac` encoder at 256 kbps, `fltp`, stereo;
   - the default disposition only on the master;
   - `moov` at the end.
2. **The atom:** a small post-processing step inserts the `stem` box as the first child of the `udta` FFmpeg writes and updates the `udta` and `moov` sizes. If `moov` sits before `mdat`, it also shifts the `stco`/`co64` offsets. It writes to a temporary file and renames it.
3. **Manifest:** compressor and limiter disabled, so Traktor and Mixxx sound the same. Colours come from `res/crate-design/tokens.json`.

**What Decks needs that the inherited code does not do, and the proposed work:**

| Need | Today | Proposal | Where |
|---|---|---|---|
| A track with stems sounds like the original while no stem is touched | The deck plays four re-encoded stems summed, never the original | **Sidecar stems:** the original stays the track's file, and the stems live beside it in Decks' cache. The deck plays original + Σ (FXᵢ(gᵢ·sᵢ) − sᵢ), which is the original, bit-exact, when no stem is touched. `other` is written as the residual, so the stems add up to the mix. Engine feasibility is analysed in §8 | ST6, replaces the file switch of STEM-D9 if approved |
| Separation finishes while the track is in a deck | Stem count and colours are fixed at load | Valued in §8 (attach live vs on the next load) | ST6 |
| Stem files written by Decks always parse | 64-bit box sizes are not byte-swapped | Fix the byte order in `steminfoimporter.cpp` (a few lines, patch list) and write `moov` at the end | ST5 |
| Exported NI STEM files for other software | — | Same writer as the sidecar, with the master stream. AAC 256 kbps by default; ALAC 24-bit is available too (the inherited tests already cover it) | ST5 |
| Stems in samplers or the preview deck | Main decks only | No Smart Mix need found. Not planned | — |

## 7. Q6 · Converting a track to its stem file

The question comes from STEM-D9 as written: the stem file replaces the track's file. With the sidecar design of §6 and §8 the track never changes file, and this section is only needed if Diego keeps file replacement.

**Answer: Decks implements a single-track relocation in `src/crate/` (`relocateTrack(TrackId, QString)`), on public APIs and without patching any Mixxx file.** The inherited relocation paths cover other cases:
- `TrackDAO::detectMovedTracks` matches the same file name with a duration within 1 s (`trackdao.cpp:1880-1888`), so `song.flac` never matches `song.stem.mp4`.
- `TrackCollectionManager::relocateDirectory` only rewrites the directory prefix.
- `TrackCollection::relocateDirectory` is private.

**How it works.** It runs on the main thread with the scanner stopped:
1. **One SQL transaction** (`TrackCollection::database()` and `SqlTransaction`, both public). It rewrites the track's `track_locations` row in place (location, filename, directory, size and flags) and sets `library.filetype` to `stem.mp4`. Track and location ids do not change. If the new path already has a row, it is merged the way `trackdao.cpp:2002-2042` does it.
2. **Tell the views:** call the public slot `TrackDAO::slotDatabaseTracksRelocated`, which emits `tracksChanged`.
3. **Update cached `Track` objects:** call `GlobalTrackCacheLocker().relocateCachedTracks(&trackDao)`. It only relocates tracks whose old file no longer exists, so the FLAC is moved away first.

**What survives and what is redone:**

| Data | After the switch |
|---|---|
| Cues, hotcues, loops, crates, playlists, history | Kept: keyed by track id |
| BPM, beats, key, ReplayGain, play count | Kept: columns of the same `library` row; beats are not re-analysed, since Mixxx keeps no audio hash |
| Waveform | Regenerated, now with stems |
| Duration, channels, bitrate | Corrected when the file is next opened |
| A deck holding the track | Keeps the FLAC open; stem count and colours are fixed at load. Covered by the "separation finishes in a deck" item of §6 |

**What the conversion must handle:**
- **Sample rate.** Beats and cues are stored in frames, so the stems are written at the original's rate. Demucs works at 44.1 kHz, so the writer resamples back (for example to 48 kHz).
- **Sample alignment.** FFmpeg's AAC priming is compensated on read (`start_pts=0`). A test proves it: a click at a known frame lands on the same frame ±1 in the FLAC and in the stem file.
- **Library folders.** If the FLAC lives in a Mixxx library folder, the next scan would add it back, so the conversion hides it or moves it out. Crate's cache is not a library folder.
- **Tracks first loaded from a `.stem.mp4`** (the cache case in STEM-D9) break nothing. The same relocation applies when the cache moves between `.flac` and `.stem.mp4`, so `crateserver_tracks.mixxx_track_id` stays valid.
- **A `.flac` path holding MP4 data is not an option:** the suffix skips content detection.

**Test for ST6.PR1** (`src/test/crate/stemrelocation_test.cpp`, on `LibraryTest`):
1. Add a FLAC, then set hotcues, a loop, locked beats, a playlist entry and a play count. Keep a live `TrackPointer`.
2. Write the stem file, remove the FLAC and relocate.
3. Assert:
   - the same `TrackId`;
   - the new location, also on the cached pointer;
   - cues, beats, playlist and play count intact;
   - the source opens with four stems and type `stem.mp4`.

A second test checks the click alignment above.

## 8. Sidecar stems in the engine

**The need:** a track with stems sounds like the original while no stem is touched, and keeps its file, cues and beats. **Four options were analysed** on the fork's engine (read-only, file:line evidence in the spike notes):

| Option | How | Patches to Mixxx files | Own code (`src/crate/`) | Effort | Main problem |
|---|---|---|---|---|---|
| **B · Lossless 4-stem sidecar (recommended)** | An 8-channel FLAC beside the original: drums, bass and vocals quantised first, then `other = original − drums − bass − vocals` in integers, so the four stems add up to the original. The deck plays the sidecar through the existing 8-channel stem path | ~40–50 lines in 4–5 files: a hook after `openAudioSource` in the caching reader worker, a public `StemInfo` setter on `Track`, an analyser hook so the waveform is not redone on every load, optionally a pre-allocated stem buffer in `EngineDeck` | ~800 lines with tests: composite source, sidecar store (key, validation, eviction), residual writer, orchestration | 1–1.5 weeks | Not bit-exact (error under about −130 dBFS, transparent). The sidecar is about 2–3× the FLAC (estimate) |
| A · Original plus correction | 10 channels: output = original + Σ (FXᵢ(gᵢ·sᵢ) − sᵢ) | ~200–250 lines in ~10 files, several on the real-time thread (channel limits, `EngineDeck` mix, a RubberBand bug in its generic branch, uneven worker split) | ~1,000 lines | ~3 weeks | With keylock, RubberBand is not linear (PV(o) − PV(v) ≠ PV(o − v)): muting the vocals leaves ghost vocals. Only worth it if stems must be lossy |
| C · A with the NI file's AAC master | As A, with the stem file's master stream | As A, plus `soundsourcestem.cpp` | As A, plus relocation | ~3 weeks | Lossy "original"; a mastered master ≠ Σ stems; track relocation |
| D · Stem file replaces the track (STEM-D9 as written) | `.stem.mp4` becomes the track's file | None in the engine | Writer and relocation (§7) | 1–1.5 weeks | The DJ hears four re-encoded stems instead of the original; track identity changes |

**With option B:**
- **The original stays the track's file and identity.** Library analysis, samplers and the preview deck keep reading it, so §7's relocation is not needed.
- **Keylock:** with RubberBand the sidecar sounds like the sum of four stretched stems, as the inherited stems do today. STEM-G2 includes a listening check with keylock on.
- **Alignment is the critical rule:**
  - Demucs reads the original decoded by Decks' own `SoundSourceProxy`, because MP3 decoders differ in delay.
  - The stems are resampled back to the original's rate before the residual is computed.
  - On load, Decks checks sample rate, frame count and a PCM fingerprint before it uses a sidecar.
- **Server stems (path A of the design)** follow the same rule. The server stores only drums, bass and vocals, lossy. Decks decodes them, computes `other` as the residual against its own decode of the original, and writes its local lossless sidecar. Decoder differences then never reach playback.
- **Separation finishing while the track is in a deck:** the channel layout is fixed per load, and reloading silences the deck until the first chunks arrive.
  - **Proposal:** apply the stems on the next load, and reload automatically, keeping the position, when the deck is stopped. This is own code only.
  - **Not proposed:** a seamless swap would need option A plus about 100–150 lines of real-time cache refresh.
- **NI STEM export** (§6) becomes an optional action for other software, not the playback format.

**What changes in the design if B is approved:**
- **STEM-D9:** the sidecar replaces "the stem file becomes the cache file", and the conversion action disappears.
- **STEM-D6 (server stem files):** three AAC stems (drums, bass, vocals) instead of a five-stream NI STEM file: about 29 MB per 5-minute track instead of about 40. The 50 GiB quota holds about 35 sessions of 50 tracks (estimate).
- **Patch list:** the 4–5 patches above go into §5.2 of the Crate Decks design and `CRATE_PATCHES.md`.

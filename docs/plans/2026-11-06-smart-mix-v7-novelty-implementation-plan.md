# Smart Mix v7 harmony and timbre novelty — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** before the v7 backfill, make the phrase segmentation see section changes of harmony and timbre (SM3-D17) and fix the review findings on SMB.PR2b/PR2c, then measure and close SMB.

**Architecture:** the section novelty in `structure.py` and its Rust twin `structure.rs` gains two terms computed from per-bar means of chroma and MFCC, with a higher boundary cost in the existing dynamic programme. Everything else in the segmentation, the profile contract and the stored model signals stays as it is. The review fixes are independent (Dockerfile, signal store, crate-cli probes, voice calibration check).

**Tech stack:** Python 3.13 (numpy, librosa), Rust 1.88 (`tools/crate-cli`), Docker (analysis worker image), pytest and cargo test.

**Spec:** decision SM3-D17 in `docs/plans/2026-11-02-smart-mix-3-design.md` (Spanish). Context: SM3-D14 (Beat This! + DBN), SM3-D16 (stored model signals), tasks SMB.PR1–PR2c in `docs/plans/2026-11-02-smart-mix-3-implementation-plan.md`.

**Repository:** Crate monorepo worktree `~/Code/Ninja/crate-agents/sm3-beats`, branch `feat/sm3-beats` (HEAD `051e709a` before this plan).

## Global constraints

- Constants, identical in Python and Rust and checked by `test_smart_mix_versions.py`: `HARMONY_WEIGHT = 2.0`, `TIMBRE_WEIGHT = 8.0`, `MFCC_COEFFICIENTS = 13`, `BOUNDARY_COST_DB = 4.0` (was 1.0), `NOVELTY_WINDOW_BARS = 4` (unchanged).
- The analyzer version stays `smart-mix-audio-v7` (not deployed). No API, schema or migration change.
- The new per-bar features are not stored; `track_mix_signals` does not change.
- Python and Rust run the same algorithm with the same constants and the same order of floating-point operations; given the same per-bar features they produce the same phrases.
- Extra analysis cost: at most 0.5 s per track.
- TDD: every task starts with its failing test. Conventional commits in English, body explains why, no AI attribution. Every commit builds.
- Python tests run in Docker, never on the host. Rust checks use the CI toolchain (`rust:1.88-trixie`): `cargo test --locked`, `cargo fmt --check`.
- Never push, open PRs or merge; never touch `~/Code/Ninja/crate`; never use the production server.

## Review focus

- Silent or near-silent bars inside the music (gap bars, zero chroma): harmony and timbre must be 0 there, never NaN, as the v7 novelty already is.
- A track whose MFCC coefficient is constant (standard deviation 0): standardisation divides by 1, no NaN or infinity.
- Frame arrays of different lengths (chroma, MFCC, bands from the same STFT but trimmed differently, or a long track analysed only on its head): per-bar means use the frames that exist in all three; no index error.
- A profile computed without chroma or MFCC (empty arrays, for example an old caller): the novelty is exactly v7's.
- Rust `f32` against Python `float64`: the structure fixture compares phrases, not raw novelty, and real-audio parity keeps its current tolerance.

---

### Task 1: Harmony and timbre novelty in the Python analyzer (SMB.PR2d, part 1)

**Files:**
- Modify: `app/crate/smart_mix/structure.py` (`EnergyFrames`, `_BarFeatures`, `_bar_features`, `_novelty`, module docstring, constants)
- Modify: `app/crate/smart_mix/analyzer.py` (`_frame_features`, `_structure_features`, the call in `analyze_mix_profile`)
- Create: `app/tests/fixtures/smart_mix/structure_v2.json`
- Test: `app/tests/test_smart_mix_structure.py`, `app/tests/test_smart_mix_audio_analysis.py`

**Interfaces:**
- Produces: `EnergyFrames(rms, bands_db, band_hop, chroma=np.empty((0, 12)), mfcc=np.empty((0, 13)))`, frames aligned with `bands_db`; `_harmony_timbre(chroma: np.ndarray, mfcc: np.ndarray, gaps: list[bool]) -> tuple[list[float], list[float]]` (values per bar boundary `0..len`); `_frame_features(audio, sample_rate) -> tuple[onsets, low_onsets, chroma, bands, mfcc]`; constants `HARMONY_WEIGHT`, `TIMBRE_WEIGHT`, `MFCC_COEFFICIENTS`; fixture `structure_v2.json` with per-bar `levels`, `shapes`, `music`, `chroma`, `mfcc`, `bars` and expected `phrases`.

- [ ] **Step 1: Write the failing tests** in `test_smart_mix_structure.py`:
  - `test_harmony_change_at_same_level_is_a_boundary`: 32 music bars with constant level and band shape, chroma one-hot on C for bars 0–15 and on F♯ for 16–31, constant MFCC; assert a phrase starts at bar 16.
  - `test_timbre_change_at_same_level_is_a_boundary`: same, constant chroma, MFCC `[0]*13` for 0–15 and `[5, -3, 2, ...]` for 16–31; assert a phrase starts at bar 16.
  - `test_mfcc_scale_does_not_change_novelty`: `_harmony_timbre(chroma, mfcc * 10, gaps)` equals `_harmony_timbre(chroma, mfcc, gaps)`.
  - `test_gaps_and_edges_have_no_harmony_or_timbre`: boundaries closer than 4 bars to either end, and gap bars, give `(0.0, 0.0)`.
  - `test_zero_chroma_and_flat_mfcc_give_no_nan`: all-zero chroma and a constant MFCC column give finite values only.
  - `test_bar_features_with_fewer_mfcc_frames`: MFCC with fewer frames than the bands gives per-bar means over the common frames, with no index error.
  - `test_without_chroma_and_mfcc_novelty_is_v7`: empty `chroma`/`mfcc` reproduce the phrases of every case in `structure_v1.json`.
  - `test_structure_v2_fixture`: every case in `structure_v2.json` gives its expected phrases.
  And in `test_smart_mix_audio_analysis.py`: `test_frame_features_return_aligned_mfcc` (shape `(frames, 13)`, same `frames` as the bands).
- [ ] **Step 2: Run them and see them fail** (`pytest tests/test_smart_mix_structure.py tests/test_smart_mix_audio_analysis.py -q` in the test container).
- [ ] **Step 3: Implement.**
  - Per-bar `chroma` and `mfcc` means use the same band frames as the band levels: centred inside the bar, without `BAR_EDGE_GUARD_MS` at each end, over the frame range common to bands, chroma and MFCC.
  - In `_novelty`, at every boundary that already gets a v7 value (window and gap rules unchanged), add `HARMONY_WEIGHT * harmony + TIMBRE_WEIGHT * timbre` after the existing `max(0, ...)`.
  - Harmony is `1 - cos` between the mean chroma of the up-to-4 non-gap bars before and after; 0 when either mean has zero norm.
  - Timbre is the Euclidean distance between the mean standardised MFCC before and after, divided by `sqrt(MFCC_COEFFICIENTS)`. Standardise per coefficient over the bars `first..last` (first to last music bar, gaps included); a standard deviation of 0 counts as 1.
  - `BOUNDARY_COST_DB = 4.0`. MFCC in `_frame_features`: `librosa.feature.mfcc(S=mel_db, n_mfcc=13)`, transposed and trimmed to the band frame count.
  - Build `structure_v2.json` from 4–6 synthetic cases (harmony change, timbre change, both, steady groove, a gap before a drop) whose expected phrases come from this implementation and are checked by hand against the case's intent.
  - Update the module docstring (the novelty paragraph) and `docs/technical/smart-mix-operations.md` (structure section: features, weights, boundary cost).
- [ ] **Step 4: Run the tests and see them pass**, plus `test_smart_mix_analysis*.py` and `test_smart_mix_versions.py`. The Python–Rust constant checks for the new constants come in Task 2.
- [ ] **Step 5: Commit** `feat(smart-mix): add harmony and timbre to the section novelty`.

### Task 2: The same novelty in the Rust analyzer (SMB.PR2d, part 2)

**Files:**
- Modify: `tools/crate-cli/src/beat_tracking.rs` (`FrameFeatures`, `frame_features`)
- Modify: `tools/crate-cli/src/structure.rs` (`EnergyFrames`, bar features, novelty, constants)
- Modify: `tools/crate-cli/src/analyze.rs` (the `EnergyFrames` it builds)
- Modify: `app/bin/crate-cli-linux-amd64` (rebuilt, own commit)
- Test: `tools/crate-cli/tests/smart_mix_structure.rs`, `app/tests/test_smart_mix_versions.py`, `app/tests/test_smart_mix_rust_parity.py`

**Interfaces:**
- Consumes: `structure_v2.json` and the constants from Task 1.
- Produces: `FrameFeatures.mfcc: Vec<[f32; 13]>`; `EnergyFrames { …, chroma: &[[f32; 12]], mfcc: &[[f32; 13]] }`; Rust constants `HARMONY_WEIGHT`, `TIMBRE_WEIGHT`, `MFCC_COEFFICIENTS`, `BOUNDARY_COST_DB = 4.0`.

- [ ] **Step 1: Write the failing tests.** In `smart_mix_structure.rs`:
  - `structure_v2_fixture_matches`: every case of `structure_v2.json` gives its expected phrases;
  - `harmony_change_is_a_boundary`, the same case as Task 1;
  - `mfcc_matches_orthonormal_dct`: a fixed mel-dB frame gives the 13 coefficients stored in the fixture, computed with `scipy.fft.dct(x, type=2, norm="ortho")[:13]`.
  
  In `test_smart_mix_versions.py`, the five constants of the global constraints match between Python and Rust.
- [ ] **Step 2: Run them and see them fail** (`cargo test --locked` in `rust:1.88-trixie` with the whole worktree mounted read-only; pytest in the test container).
- [ ] **Step 3: Implement.**
  - `frame_features` computes, per frame, the orthonormal DCT-II of the 128 mel-dB bands it already holds (after the same top-dB clamp the bands use), keeping 13 coefficients.
  - `structure.rs` mirrors Task 1 operation by operation.
  - `analyze.rs` passes `&features.chroma` and `&features.mfcc`.
- [ ] **Step 4: Run Rust and parity tests.** `cargo test --locked`, `cargo fmt --check`, and `test_smart_mix_rust_parity.py` must pass with no change of tolerance.
- [ ] **Step 5: Commit** `feat(crate-cli): add harmony and timbre to the section novelty`.
- [ ] **Step 6: Rebuild the committed binary** with the release recipe the previous tasks used (`docker build --platform linux/amd64 tools/crate-cli`, output to a scratch dir, copy to `app/bin/crate-cli-linux-amd64`). Make the guard test in `test_smart_mix_versions.py` fail on a binary built before this change: for example, the help or version output names the novelty features.
- [ ] **Step 7: Commit** `build(crate-cli): rebuild the Linux worker binary for the harmony and timbre novelty`.
- [ ] **Step 8: Full Python suite in four parts, one after another**, with the split and classifier used for SMB.PR2c. Known failures: `test_content_hash.py::…::test_returns_false_when_hash_matches`, `test_audio_analysis.py::…::test_analyze_short_audio`, `test_stats_dashboard_contract.py::test_built_dashboard_satisfies_the_contract[30d]`, and the environmental ones (no git, make or Docker socket).

### Task 3: Review fixes for SMB.PR2b and SMB.PR2c (SMB.PR2e)

Independent of Tasks 1–2: work on a separate branch `feat/sm3-beats-review` from `051e709a` (its own worktree), to be rebased onto `feat/sm3-beats` afterwards.

**Files:**
- Modify: `app/Dockerfile`, `app/tests/test_analysis_image_contract.py`
- Modify: `app/crate/worker_handlers/analysis.py`, `app/crate/db/repositories/smart_mix_signals.py`, `app/tests/test_smart_mix_track_signals.py`
- Modify: `app/crate/crate_cli.py`, `app/tests/test_crate_cli.py`
- Modify: `docs/technical/smart-mix-operations.md`
- Possibly modify: `app/crate/smart_mix/vocals.py` (only if step 5 says so)

**Interfaces:**
- Produces: `PROBE_RETRY_SECONDS = 60` in `crate_cli.py`. No other interface changes.

- [ ] **Step 1: madmom only with torch.**
  - Failing test in `test_analysis_image_contract.py`: with `PANNS=off`, the analysis image build neither fetches nor compiles madmom and copies no `madmom.LICENSE`. Read the Dockerfile stages, or use `docker buildx build --check` if available.
  - Fix: gate the `madmom-wheel` stage and the licence copy on the same `PANNS` condition as torch (an empty fallback stage is acceptable).
  - Build the image once with `PANNS=off` and once with the default, and check both.
  - Commit `fix(analysis): build madmom only for images that install torch`.
- [ ] **Step 2: Signals stored only for the source they came from.**
  - Failing tests: `test_signal_not_stored_when_source_changed_during_task` (the revision changes between capture and store: nothing is stored, a warning is logged) and `test_older_signal_does_not_overwrite_newer` (an upsert carrying an older `updated_at` than the row's, for another revision, leaves the row as it was).
  - Fix: re-run `smart_mix_source_revision(path)` before each store and compare it with the captured one; guard the upsert.
  - Commit `fix(smart-mix): store model signals only for the source they came from`.
- [ ] **Step 3: Failed probes back off.**
  - Failing tests: `test_failed_probe_is_not_repeated_within_retry_window` and `test_failed_probe_is_repeated_after_retry_window`, with a fake clock and a binary double that counts calls.
  - Fix: the `--help` and `analyze --help` probes remember a failure for `PROBE_RETRY_SECONDS` and answer "unsupported" without running the binary or logging again; a success is still cached as now.
  - Commit `fix(crate-cli): back off for a minute after a failed probe`.
- [ ] **Step 4: Operations note.** In `smart-mix-operations.md`, a file rewritten with the same size, mtime and inode keeps its stored signals and profile; reanalyse the track to force a recompute. Commit with step 2 or on its own as `docs(smart-mix): …`.
- [ ] **Step 5: Voice calibration check.**
  - Recompute the SG1b numbers (358 tracks with LRCLIB synced lyrics, material in `~/Code/Ninja/crate-main/sg1-2026-11-04/calibration/`) with the per-window byte path from SMB.PR2c, at `VOCAL_THRESHOLD = 90`.
  - Baseline: precision 0.88, recall 0.78 against the lyrics.
  - If either moves by more than 0.01, the phrase byte is computed from the float scores when YAMNet has just run, and from the stored bytes only when they are reused. Add a test pinning that, then commit `fix(smart-mix): …`. Otherwise, record the numbers in the report and change nothing.
- [ ] **Step 6: Run the affected test files**, not the full suite: Task 2 step 8 runs it after the rebase.

### Task 4: Measurement, tools and docs (SMB.PR3)

Done by the plan owner after Tasks 1–3 are on `feat/sm3-beats`.

- [ ] **Step 1: Rebase `feat/sm3-beats-review` onto `feat/sm3-beats`.** Resolve the `smart-mix-operations.md` overlap, then run the full suite in four parts.
- [ ] **Step 2: Rebuild the analysis image** and rerun the final pass on the 145 measurement tracks: DBN beats, `beats_per_bar`, Rust analyzer from the image.
- [ ] **Step 3: Evaluate** with `mir_eval` on the 131 phrase-genre tracks with a downbeat. Acceptance (SM3-D17):
  - section F1 at ±3 s ≥ 0.52;
  - precision not below SMB.PR2c's;
  - at most 0.5 s extra per track.
  
  If it fails, stop and report to Diego.
- [ ] **Step 4: Move the evaluation scripts** to `tools/smart-mix-eval/structure/` as an offline tool (All-In-One and madmom runners, Beat This! runner, `mir_eval` evaluation, novelty prototype), with paths as arguments and a README. Not in any production path.
- [ ] **Step 5: Docs.** `docs/technical/smart-mix-operations.md` and `smart-mix-design.md`: model, licences, cost, provenance, stored signals, novelty and the measurement. Update the SMB.PR3 entry in the Smart Mix 3 plan.

## Status (2026-11-06)

- **Tasks 1–2 done** on `feat/sm3-beats`:
  - `91e526f1` (Python novelty), `f8fd1eec` (Rust novelty) and `8df51b55` (rebuilt binary).
  - Deviation: harmony alone is at most 2 dB, under the 4 dB boundary cost, so the pure-harmony test pairs the chord change with a band shift.
- **Task 3 done** on `feat/sm3-beats-review`, rebased without conflicts and fast-forwarded into `feat/sm3-beats`:
  - `d19e4c3a` (madmom only with torch), `3dccb805` (signals only for their source), `4bb7625f` (in-place rewrite note) and `8dc81c8c` (probe back-off).
  - Voice byte path against float at the 90 cut: precision 0.8824 vs 0.8825, recall 0.7832 vs 0.7837, so nothing changes.
  - The step 4 note says to `touch` the file before reanalysing, because a reanalysis with the same identity reuses the stored signals.
- **Task 4, step 3 acceptance: passed.** Measured with the branch's committed Rust binary on the 131 phrase-genre tracks with a downbeat:
  - section F1 at ±3 s is 0.53 (SMB.PR2c: 0.49; required ≥ 0.52);
  - precision is 0.55 (SMB.PR2c: 0.54);
  - the extra cost is about 4 ms (Rust) and 6 ms (Python) per track.
  - At ±0.5 s and ±1 bar, F1 goes 0.33 → 0.36 and 0.43 → 0.47.
  - Phrase boundaries on All-In-One downbeats: 66 tracks (was 61).
  - Arbiter subset (96 tracks), F1 at ±3 s: 0.50 → 0.54.
- **Task 4, steps 4–5 done** (`f337645a`..`98d889ac`, HEAD `98d889ac`):
  - the offline tool `tools/smart-mix-eval/structure/` (runners, `mir_eval` evaluation, novelty prototype, README, 77 tests);
  - docs in `smart-mix-operations.md` and `smart-mix-design.md`;
  - all seven deferred minor review items fixed;
  - full suite: no new failures; `cargo test` 183 passed;
  - image `crate-smb-analysis-worker:final` reproduces the measured profile through the worker path.
- **MFCC deviation floor** (Diego, 2026-11-06): `MFCC_STD_FLOOR = 1.0` in Python and Rust (`ff847868`..`225a2bb8`).
  - Before the floor, per-track MFCC standardisation created spurious timbre boundaries on near-static loops.
  - Floors from 0.25 to 2.0 left the 131-track scores unchanged and removed the loop's boundaries.
  - With 1.0, all 145 measurement profiles are identical to the run without it, and the loop gets no inner boundary.

# Smart Mix three-beat bars — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** stop fast 4/4 tracks (d-beat, thrash, death, metalcore) from losing their bars and phrases because Beat This!'s DBN hears them in three, while tracks really in three keep `time_signature = 3`.

**Status:** MG1 approved by Diego on 2026-11-09 (option D, madmom in production). Tasks 2-4 implemented on `feat/smart-mix-meter-arbiter` (worktree `~/Code/Ninja/crate-agents/smart-mix-meter-arbiter`, from `origin/main` `ca620f1c1`): `f36c5b94d` arbiter, signals and migration 156, `dd9537ca4` `smart-mix-audio-v10`, `c5aed3df3` worker binary. Local, not pushed: stopped at MG2. Task 1's labelled three-beat reference set (All-In-One) is not built; the measurement below is on the evaluation sets only.

**Spec:** SM3-D14 (Beat This! with madmom's DBN) and SM3-D16 (a track the DBN hears in three gets that meter, no downbeat and no phrases) in `docs/plans/2026-11-02-smart-mix-3-design.md`. Analyzer v9 (`smart-mix-audio-v9`, PR #406, deployed 2026-11-09) is the baseline.

**Repository:** Crate monorepo, a new worktree under `~/Code/Ninja/crate-agents/` from `origin/main`, branch `feat/smart-mix-meter-arbiter`.

## 1. The defect

Beat This! runs madmom's `DBNDownBeatTrackingProcessor` with `beats_per_bar=[3, 4]`: it decodes one HMM per bar length and keeps the path with the higher log probability.
Since SM3-D16, a three-beat winner gives the profile `time_signature = 3`, no downbeat, no cue downbeats and no phrases, so the planner crossfades the track.

On the 399 tracks of the stems evaluation sets, the DBN chooses three on 10 tracks (2.5 %).
Nine are fast punk and metal, genres that are almost always in four:

| Track | Group | Tempo (BPM) |
| --- | --- | ---: |
| Sepultura "Arise" | thrash | 168 |
| Cannibal Corpse "Necrogenic Resurrection" | death | 162 |
| Counterparts "A Mass Grave of Saints" | metalcore | 159 |
| Tragedy "Kick and Scream" | crust / d-beat | 158 |
| Doom "Sick Joke '89" (`.m4a`) | crust / d-beat | 166 |
| Anti Cimex "Set me free" | crust / d-beat | 143 |
| Movements "Submerge" | post-hardcore | 152 |
| THRICE "Cold Cash And Cold Hearts" | post-hardcore | 96 |
| Swans "Coward" | live-drums control | 140 |
| Azu Tiwaline "Izen Zaren" | techno (Tuareg-influenced) | 96 |

Nobody has listened to them.
Some may really be in three (6/8 and 12/8 grooves count as three here): the measurement below points at Azu Tiwaline and Movements.

## 2. What was measured (2026-11-09)

A spike on the 10 three-beat tracks and 12 four-beat controls from the same groups (thrash, death, crust, hardcore, grind, black, post-hardcore), all with phrases under v9.
For each track, the log probability of the 3-beat and the 4-beat HMM, per 1,000 frames, as `margin = (log p3 - log p4) / frames * 1000`:

- **Beat This!:** Beat This! `small0` activations through the production DBN settings (55-215 BPM, `transition_lambda=100`, 50 fps).
- **madmom:** madmom's own `RNNDownBeatProcessor` with its DBN (`beats_per_bar=[3, 4]`, 100 fps), an independent model.

| Group | Beat This! margin | madmom margin |
| --- | --- | --- |
| 12 four-beat controls | -15.7 to -295.0 (all < 0) | -10.0 to -68.6 (all < 0) |
| 8 three-beat tracks of fast 4/4 genres | +4.5 to +135.4 | -5.6 to -19.4 (all < 0) |
| Azu Tiwaline, Movements | +64.0, +21.9 | +2.8, +10.7 |
| Doom (`.m4a`) | +27.6 | not run (the spike skipped `.m4a`) |

Scripts and records: `~/Code/Ninja/crate-agents/scratch/v8-phrases/tools/meter_spike.py`, `meas/meter/*.json`, `meas/meter-tracks.tsv` (local scratch, not committed).

Readings:

- The two models agree on every control.
- Where Beat This! says three, madmom says four on eight of the nine tracks it could read and three on the other two, the two most likely to be really in three.
- Beat This!'s own margin does not separate the groups: the doubtful tracks (+22, +64) sit inside the range of the wrong ones (+4.5 to +135).

## 3. Options

| Option | How | Verdict |
| --- | --- | --- |
| A. Margin on Beat This!'s DBN | Keep three only when `log p3 - log p4` clears a threshold | **Rejected by the data:** no threshold separates the doubtful tracks from the wrong ones (§2). Free, so it stays a stored signal |
| B. Tempo prior | Penalise three above some tempo | **Rejected:** wrong three-beat tracks run at 96-168 BPM, the doubtful ones at 96 and 152. Real 6/8 punk is fast too |
| C. Genre prior | Prefer four for punk, metal, techno | **Rejected as the rule:** it ties a content analysis to metadata. Genres are missing or noisy on part of the library, a genre edit would change a profile, and doom, folk metal and post-hardcore have real 6/8 |
| D. Second model as arbiter | When Beat This! picks three, run madmom's RNN with its DBN; keep three only if madmom also picks three. Otherwise take Beat This!'s 4-beat path from the same activations | **Recommended:** it separates the measured groups and costs only on about 2.5 % of tracks |
| E. Phrases in three-beat bars | Let phrases, the planner and Crate Decks count bars of three | Right for tracks really in three, but a contract change across the planner, Admin and Decks. A later decision, independent of D |
| F. Four beats only | Run the DBN with `beats_per_bar=[4]` | **Rejected:** a real waltz or 6/8 would get bars of the wrong length and phrases cut in the wrong places; SM3-D16 chose honesty |

### Cost of D

- madmom's RNN downbeat processor takes about 50-60 s of CPU per 5-minute track on one thread, against 15-25 s for Beat This!. It runs only where Beat This! picks three.
- **Library:** about 2.5 % of about 140,000 tracks is about 3,500 tracks. That is 50-60 hours of CPU once, spread over a backfill with its night window, plus a few minutes per day of imports.
- **Re-analysis:** after Task 3 only the stored three-beat signals are recomputed, not the whole library (§4).

### Gate MG1 (Diego): approved 2026-11-09

- **Licence:** madmom's network weights are CC BY-NC-SA 4.0. Until now the analysis image shipped them unused, and the evaluation used them only offline (`tools/smart-mix-eval/structure/README.md`). D runs them in production.
- **Decision:** Diego approved D and running madmom's CC BY-NC-SA networks in production ("sí a madmom"). Crate is free and open source, not commercial, so a non-commercial licence does not block it. The operations doc records the change with the attribution.
- Option E (phrases in three-beat bars) stays a later, separate decision.

## 4. Design of D

- **One model pass.** `beat_model.py` keeps one Beat This! pass. It replaces `Audio2Beats(dbn=True)` with `Audio2Frames` plus the same madmom DBN settings, decodes both HMMs itself, and keeps both paths and their log probabilities.
- **The rule:**
  - If the 4-beat HMM wins, nothing changes: same beats, downbeats and `beats_per_bar`. This is about 97.5 % of tracks.
  - If the 3-beat HMM wins, the arbiter runs madmom's `RNNDownBeatProcessor` on the decoded signal (a numpy `Signal` at 44.1 kHz, so `.m4a` decoded by ffmpeg works) and its DBN with `beats_per_bar=[3, 4]`.
  - If the arbiter also picks three, the profile stays as today: `time_signature = 3`, no phrases.
  - If the arbiter picks four, the beats and downbeats are Beat This!'s 4-beat path, `beats_per_bar = 4`, and the track gets its bar and phrases.
- **Stored with the signals (SM3-D16),** in two new nullable columns of `track_mix_signals` (one migration):
  - `meter_margin` (Beat This!'s `log p3 - log p4` per 1,000 frames, real);
  - `meter_arbiter` (text: `none` when four won outright, `madmom-dbn-3` or `madmom-dbn-4`).
- **Reuse:**
  - Stored signals with `beats_per_bar = 4` stay valid, whatever the arbiter columns say.
  - Stored signals with `beats_per_bar = 3` and no `meter_arbiter` are stale and run again. These are the about 3,500 tracks.
  - The tracker name `beat-this-small0-dbn` does not change, so the library's 4/4 signals are not invalidated.
- **Failure:** if madmom's RNN fails or is missing, the rule falls back to today's behaviour (three wins) and logs a warning, as Beat This! itself does.
- **Analyzer version:** a new `smart-mix-audio-v10`, because the publisher keeps a same-version profile of equal quality and tracker (see the v9 bump). With the reuse rule, the v10 backfill runs only `crate-cli` on 4/4 tracks, at about 2 s each, and the models on the three-beat ones.

## Global constraints

- TDD: every task starts with its failing test. Conventional commits in English, body explains why, no AI attribution. Every commit builds.
- Python tests run in Docker, in the analysis worker image. Never run full suites during the work; the release branch runs CI.
- madmom runs only in the analysis worker, as Beat This! does; the API image stays without torch or madmom.
- The madmom weights come from the installed package, never downloaded at run time.
- No change to the profile contract, the planner or Crate Decks (option E is out of scope).
- Never use the production server or `~/Code/Ninja/crate`; push, PR and deploy only on Diego's word.

## Tasks

### Task 1: reference set and measurement harness

**Files:**

- Create: `tools/smart-mix-eval/structure/meter_arbiter_eval.py`
- Create: `tools/smart-mix-eval/tests/test_meter_arbiter_eval.py`

- [ ] **Step 1: failing test** for the pure part: given records `{label, bt_margin, mm_margin}`, `confusion(records, rule)` counts the true and false three-beat calls for a rule. Run it: it fails because the module does not exist.
- [ ] **Step 2: implement** the records reader (the spike's JSON shape) and `confusion`.
- [ ] **Step 3: build the reference set.**
  - Take the 10 three-beat tracks and the 12 controls of §2.
  - Add at least 20 tracks really in three or 6/8, found in the development copy by album and track genres and titles (waltz, vals, 6/8, folk metal, doom), plus All-In-One on all of them for its beat positions (`Dockerfile.allin1`, rebuilt since the image is gone).
  - Labels: All-In-One's bar length where it is regular, else "unknown". The list stays in `out/`, by entity UID, never committed.
- [ ] **Step 4: run** the rules A (best threshold), B and D on the set and write the confusion table into this plan.
- [ ] **Step 5: commit** `test(smart-mix-eval): measure meter rules on a labelled three-beat set`.

**Acceptance for D:**

- At least 90 % of the wrong three-beat calls of fast 4/4 genres become four.
- At most 10 % of the labelled three-beat tracks become four.
- No four-beat control changes.

### Task 2: both HMMs from one Beat This! pass (after MG1)

**Files:**

- Modify: `app/crate/smart_mix/beat_model.py`
- Test: `app/tests/test_smart_mix_beat_model.py`

- [ ] **Step 1: failing tests**, with a fake frames model and a fake DBN:
  - (a) a 4-beat winner gives the same `BeatTrack` as today;
  - (b) a 3-beat winner exposes both paths and `meter_margin`;
  - (c) the real checkpoint test (skipped outside the image) still tracks the click track at 4.
- [ ] **Step 2: implement** `Audio2Frames` plus our own decode of the DBN's HMMs, with the same `DBNDownBeatTrackingProcessor` settings as Beat This!'s `Postprocessor`.
- [ ] **Step 3:** run `tests/test_smart_mix_beat_model.py` in the image; parity with today's `Audio2Beats(dbn=True)` on the 22 spike tracks: identical beats where four wins.
- [ ] **Step 4: commit** `feat(smart-mix): keep both bar lengths of the DBN`.

### Task 3: madmom arbiter and the rule

**Files:**

- Create: `app/crate/smart_mix/meter_arbiter.py`
- Modify: `app/crate/smart_mix/beat_model.py`, `app/crate/worker_handlers/analysis.py`
- Create: migration `NNN_track_mix_signals_meter.py` and its schema section
- Modify: `app/crate/db/repositories/smart_mix_signals.py`
- Tests: `app/tests/test_smart_mix_meter_arbiter.py`, `app/tests/test_smart_mix_track_signals.py`

- [ ] **Step 1: failing tests:**
  - `choose_meter(bt_wins_three, arbiter)` for the four cases (four wins; three wins and the arbiter agrees; three wins and the arbiter says four; the arbiter fails);
  - stored signals with `beats_per_bar = 3` and no `meter_arbiter` are not reused, while `beats_per_bar = 4` ones are;
  - the arbiter runs only when three wins (a fake that fails if called on a four-beat track).
- [ ] **Step 2: implement** the arbiter (lazy madmom import, one thread, numpy `Signal` input), the rule, the columns and the reuse check.
- [ ] **Step 3:** run the touched tests plus `test_smart_mix_rust_parity.py` and `test_smart_mix_versions.py` in the image, against Postgres for the signals.
- [ ] **Step 4: commit** `feat(smart-mix): ask madmom before giving a track three-beat bars`.

### Task 4: version, docs and measurement

- [ ] **Step 1: failing tests:** `ANALYZER_VERSION == "smart-mix-audio-v10"`, v9 readable, and the backfill tests for the paused or finished v9 campaign, as in the v9 bump.
- [ ] **Step 2:** bump Python and Rust, rebuild `app/bin/crate-cli-linux-amd64` with the `tools/crate-cli/Dockerfile` toolchain.
- [ ] **Step 3: docs** in `docs/technical/smart-mix-operations.md`:
  - "Analyzer v10" with the measurement;
  - the meter rule under "Beats and downbeats";
  - the madmom licence line, as MG1 decided;
  - "Running the v10 backfill", with its cost: `crate-cli` only for 4/4, models for the about 3,500 three-beat tracks.
- [ ] **Step 4: measure** before and after on the 399 evaluation tracks with stored signals plus the reference set:
  - three-beat calls, phrases found;
  - the four-beat tracks must stay byte-identical.
- [ ] **Step 5: commit** `feat(smart-mix): bump the analyzer to smart-mix-audio-v10` and the binary rebuild.

### Measurement of the implementation (2026-11-09)

The new `beat_model` (Beat This!, both bar paths, madmom arbiter) in the production analysis image, against the stored v9 signals, then `crate-cli` on the tracks whose beats change:

| Tracks | Result |
| --- | --- |
| The 10 the DBN hears in three | 8 take four (`madmom-dbn-4`) and gain 5-19 phrases: Sepultura, Cannibal Corpse, Counterparts, Tragedy, Anti Cimex, Doom (`.m4a`), THRICE, Swans. 2 keep three (`madmom-dbn-3`): Azu Tiwaline, Movements |
| A random sample of 30 four-beat tracks | Beats and downbeats identical to the stored ones on 30 of 30; the arbiter is never asked |
| Tracks without phrases on the 399 | v9 30, v10 22 |

- Swans "Coward" is a live-drums control: its four-beat bars may be less reliable.
- Time per three-beat track: 86-249 s on the development machine, which was shared with other jobs.
- The madmom DBN thresholds its activations at 0.05 before decoding, which the 2026-11-09 spike skipped. The implementation decodes exactly as madmom's processor does (a parity test checks it).

### Production share and the running v9 campaign

- **Share of three-beat bars:** on 2026-11-09 at 20:12 UTC, 537 of the 12,278 stored Beat This! signals in production had bars of three (4.4 %, against 2.5 % in the evaluation sets). That is about 6,100 tracks of the library's about 140,000.
- **Cost per v9 row:**
  - About 95.6 % keep their stored four-beat signals: `crate-cli` only, about 2 s of CPU.
  - About 4.4 % run Beat This! again and the arbiter: about 75 s of CPU or more.
  - That is about 5 s of CPU per row on average.
- **Rows v9 has not reached** (v5 and v6): v10 costs what v9 would have cost, plus the arbiter on 4.4 % of them, about 9 % more.
- **Ship v10 early:** the extra work grows with what v9 has finished.
  - v9 does about 10,500 rows a day once its cheap v8 rows are done (expected by about 00:00 UTC on 2026-11-10).
  - Each day of v9 adds about 15 hours of CPU of v10 redo, a few hours of wall time with the night window.
- **Ship v10 after v9 finishes** (about 2026-11-22 to 25): about 200 hours of CPU over the whole library, about two more days.

### Gate MG2 (Diego)

Release, deploy and the v10 backfill only on Diego's word, after the v9 backfill has finished or been paused.

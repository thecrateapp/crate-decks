# Crate Decks · Gates G4 and G4b report

> **Input for Diego's gates G4 (Android Smart Mix against Decks Auto DJ) and G4b (planner v3 in
> Decks).** Automatic metrics on rendered transitions, as decided on 2026-11-10; nobody listened.
> The agent stops here: nothing below is fixed or changed in the product.
>
> **Decided (Diego, 2026-11-19): fix first, then decide.** G4 and G4b are not approved on this
> report. Findings 1 to 3 are fixed (finding 3: "the director stages the residual"), the harness ran
> again on the same pairs and variants, and §12 gives the numbers before and after the fixes with
> the revised verdict proposals, which replace §10's. §§1–11 are the report as first written, with
> one correction: finding 2's direction (the incoming deck started ahead of the plan, not behind).

- **Date:** 2026-11-19.
- **Plan:** [Crate Decks implementation plan](2026-11-02-crate-decks-implementation-plan.md): Gate G4
  and its decisions of 2026-11-10 and 2026-11-16, "Pendiente para F6.C-PR4 y G4", F6.C-PR2 to
  F6.C-PR4, F6b.PR1 and Gate G4b (2026-11-10 text).
- **Decks code:** `origin/main` (`8625a9abdb`) plus branch `test/decks-g4-reports`, which adds only
  the gated harness `src/test/crate/decksg4render_test.cpp` (skipped without
  `CRATE_DECKS_G4_PAIRS`, so CI never runs it) and this report. No product code changed.
- **Monorepo tools:** branch `tools/decks-g4-metrics` (worktree `crate-agents/decks-g4-tools`, on
  `origin/main` `d9443b459`): `tools/smart-mix-eval/decks_g4/` (survey, pair selection, reference
  plans, limit cases, metrics, tables) and its tests. The metrics are `listen_metrics.py`'s (SM5)
  functions, unchanged; the Android renders are `render_pair.py`'s, unchanged.
- **Server:** the dev stack (`crate-main/crate` at `5e7a9d6ec`, Decks flags for beatmatch and bass
  handoff on), analyzer v10 profiles. Plans were read through the API; nothing was written.
- **Renders, CSVs and measurements** (not committed): `~/Code/Ninja/crate-agents/decks-g4/`
  (`renders/<pair>/<variant>/`, `renders/deviation.csv`, `metrics/metrics.json`, `tables.md`; the
  first full run in `renders-run1/`, `metrics-run1/`;
  `README.md` with the pairs' titles).

## 1. Summary

- **Decks plays the plan as Android does.** On the same 10 plans, Decks with ReplayGain off measures
  the same as the Android renderer on loudness (step, bump and slope the same on 8–9 of 10 pairs),
  voice (9 of 10) and bass (8 of 10), and better or the same on beat clash (5 better, 3 same, 2
  worse). Net on SM5's four: 6 better, 2 same, 2 worse (p 0.29). The differences sit in the pairs
  where Decks started late (finding 1). All 30 Decks runs were played by the director (no fallback
  to Auto DJ's own fade), from the server's pick, with no control overridden.
- **Finding 1, the fixed 30 s handover lead is too short for v3, often.** Measured on the G4 pairs
  (the harness's real Auto DJ handover matched the estimate on 10 of 10) and estimated over 254
  surveyed edges: **35 % of v3 plans start before Auto DJ hands over** (median 11.6 s early, p90
  50.5 s, max 130 s; 18 edges, 7 %, more than 30 s early). v2: 3 % (max 11 s). The director then
  jumps the incoming deck forward by as much: in the G4 pairs by 1.6, 4.4, 44, 46 and 130 s, so the
  listener misses that much of the incoming track's start.
- **Finding 2, on-time starts are off by up to one audio callback.** With the app's default
  1024-frame buffer, the incoming deck starts 1–26 ms (median 18 ms) ahead of where the plan puts it
  against the outgoing deck (the director starts it on an engine position short of the cue), in 14
  on-time starts out of 14; Android's preroll is sample-accurate. The beat
  clock (`beat_distance`) was used in 1 of 20 runs.
- **Finding 3, ReplayGain and the plan's gain staging stack.** The app normalises every track
  (ReplayGain on by default) and then applies the plan's staging (−1.8 to −7.2 dB on these pairs),
  which the server computed for unnormalised tracks. The loudness step gets better than Android's
  (1.9 against 3.2 LU), the dip deeper (6.7 against 5.9 LU mean, up to 3.9 LU deeper on one pair).
- **The first live run's missing bass handoff (open question 2):** by design only beatmatch plans
  carry one, and in "G3 · Auto DJ (industrial EBM)" 1 of 11 edges plans a beatmatch (8.7 % of all
  254 surveyed edges). On top, the server's Decks bass switch only reached `main` at 20:47 that day.
  Decks' side works: in all 5 beatmatch pairs it asked for the handoff and drove both low knobs to
  −24 dB (§6).
- **G4b: v3 measures better than v2 in Decks** (net 7 better, 2 same, 1 worse, p 0.07; loudness
  step 8/2/0, p 0.01; voice on voice 4/5/1), at the cost LV0 already flagged: bass on bass worse
  (0/7/3, 1.0 s against 0.5 s mean). Decks' limit check agrees with the server's `limit_violations`
  on **198 of 198** cases (the 80 real candidates of the G4 menus and 118 variations, 90 of them
  with codes), after 88/88 and 168/168 live; the server's pick was valid on Decks' tracks for 80 of
  80 candidates, and Decks never played another one.
- **Verdict proposals (§10):** G4, approve, with findings 1 to 3 owed before G6 and this harness
  rerun after them. G4b, approve v3 as Decks' planner, with finding 1 as its condition.

## 2. Method

### 2.1 Decks: the real app code, rendered offline

`CrateDecksG4Render.RendersAutoDJTransitions` builds, inside `mixxx-test`, what the app runs for an
Auto DJ transition in "Crate Smart Mix" mode, and records it:

| Part | In the harness | As in the app? |
| --- | --- | --- |
| Engine | `EngineMixer` with two `Deck`s (deck 1 left, deck 2 right), keylock engine, the default mixing EQ (Biquad Full Kill, crossovers 246/2484 Hz) in both racks, rate range 8 % | Yes: real classes, the app's defaults (`DlgPrefMixer`, `DlgPrefDeck`, `DlgPrefEQ`) |
| Crossfader | Additive, curve 1.0, its power calibration | `DlgPrefMixer`'s defaults |
| ReplayGain | On, 0 dB boost, −6 dB without data (`DlgPrefReplayGain`'s defaults); variant `v3-norg` turns it off | Yes; `norg` is for attribution only |
| Tracks | The dev stack's audio files, added to the library; `AnalysisImporter::apply` with the app's default policy (Intro and Outro from `full` profiles, upstream's beats), then upstream's `AnalyzerBeats`, `AnalyzerEbur128` and `AnalyzerSilence` | Yes: what a Crate track gets before it plays (the app reads the same audio from its cache) |
| Auto DJ | The real `AutoDJProcessor` in `TransitionMode::CrateSmartMix`, with Auto DJ's default 10 s transition and the incoming track queued; it loads it into deck 2 and hands the transition over 30 s before its own fade | Yes; only its loads go straight to the decks |
| Adapter, plans | The real `AutoDJSmartMixAdapter`, installed as Auto DJ's director, with a real `PlanClient` signed in to the dev stack; its context (`PlanContext::forServer` from the stack's real capabilities: v3 with the menu, beatmatch, bass handoff, long blends, 10 s); the plan asked ahead and found in the cache at the handover, as `requestAhead()` does | Yes. For v2 the capabilities are edited to list only `smart-mix-v2` |
| Director | The real `SmartMixDirector` with keylock on (the Smart Mix preference's default) | Yes, on a simulated clock (below) |
| Threads and clock | The engine runs on its own thread, one callback of the app's default buffer (1024 frames, 23.2 ms) at a time, so Auto DJ hears positions through queued signals. Between two callbacks the main thread runs the director's 16 ms timer and, while it prepares, its start timer at 1 ms resolution; like the app's, those ticks see the engine position of the last callback, and what they set reaches the engine at the next one | Modelled: the clock is the engine's frames, not a wall clock; no audio device jitter |

Each pair and variant runs twice with the same inputs: once recording the main output (`mix.wav`),
once with the EQs bypassed recording each deck's channel with its pregain and its post-fader gain
divided out (`outgoing.wav`, `incoming.wav`: each deck alone at unity gain, as it played, with keylock
and tempo, on the same clock). The two passes kept the same positions to the buffer through every
transition. `trace.csv` holds, per callback, both decks' positions, rates, pregains, total gains,
crossfader gains and EQ low cuts; `report.json` the director's `Report`, its state changes and the
plan; `deviation.csv` one `Report` row per run.

### 2.2 Android: the SM5 renderer

`render_pair.py` (SM5's renderer, unchanged) renders a plan as the Android engine plays it, through
`listen_metrics.measure_plan`. Two kinds of Android plan:

- **The plan Decks played** (`playedPlan` of each Decks run), so Android and Decks play the same
  plan: the G4 comparison of the two players.
- **Android's own plan for the edge** (`android-listen`): the plan the Android app asks for with the
  production flags on (`source: playlist`, 6 s, beatmatch and bass swap allowed) and the server's
  `listen_planner_version` (`smart-mix-v2` on the dev stack): what an Android listener of this server
  hears for the pair.

The renderer starts the incoming track exactly at the plan's cue: Android's preroll and phase
measurement are taken as perfect. Its excerpt has 12 s on each side of the mix; the Decks excerpts
are cut the same way around the transition as Decks played it (from the incoming deck's start to the
outgoing deck's stop).

### 2.3 Metrics

SM5's (`listen_metrics.py`, LV0 §2.4), on both renders of each pair: beat clash and phase error
(Beat This! on each deck alone), mix beat interval variation, voice on voice (YAMNet), bass on bass
(below 150 Hz), and EBU R128 loudness step, bump, dip and steepest change on the mix. Better and
worse beyond SM5's noise floors, net on SM5's four (beat clash, voice on voice, step, dip), two-sided
sign test. With 10 pairs the sign test only flags large effects; p < 0.1 is marked as LV0 did.

For Decks, "full level" (where clashes and overlaps count) takes each deck's crossfade gain from what
the engine applied (crossfader gain times pregain, with the plan's staging divided out, as SM5's mask
leaves the staging out), and bass on bass applies the EQ low cut the director set. `alignment` reads
from the trace where the incoming deck is against where the plan puts it for the outgoing deck's
position, in output time.

### 2.4 Pairs

`survey.py` asked the dev stack for the plans of all 254 edges between consecutive tracks with audio
in the six "G3 ·" playlists (138 tracks, v10 profiles), in Auto DJ's v3 and v2 contexts, and
estimated Auto DJ's handover from the profiles' Intro and Outro (`calculateTransition`, replicated).
`select_pairs.py` then picks by rule, with no track used twice and `full` profiles first:

| Pair | Why | Outgoing → incoming (uid) | BPM (profile) | Voiced end / start | v3 plan (Auto DJ) | v2 plan (Auto DJ) |
| --- | --- | --- | --- | --- | --- | --- |
| p01 | bass handoff | `02d7186a` → `e74114e7` | 170.0 → 168.0 | 0.5 / 1.0 | beatmatch 10 s, bass swap | beatmatch 10 s, bass swap |
| p02 | bass handoff | `21de1c12` → `2205b4d1` | 179.2 → 179.4 | 0 / 0 | beatmatch 10 s, bass swap | beatmatch 10 s, bass swap |
| p03 | long blend | `28a006aa` → `6fb180bc` | 120.0 → 123.0 | 0 / 0 | beatmatch 32 s dj-blend, bass swap | the same |
| p04 | tempo apart | `cb064389` → `60852b5f` | 124.0 → 164.7 | 0.5 / 1.0 | adaptive 10 s, `incompatible_tempo` | the same |
| p05 | tempo apart | `b73a62c3` → `cfc6eaa1` | 177.9 → 118.1 | 0 / 0 | adaptive 10 s, `incompatible_tempo` | the same |
| p06 | vocal | `0bf3fae8` → `63c6046b` | 127.9 → 164.0 | 0.5 / 1.0 | adaptive 10 s, `incompatible_tempo` | the same |
| p07 | vocal | `0dc51e28` → `ae384fc9` | 187.5 → 126.7 | 1.0 / 0.5 | adaptive 10 s, `incompatible_tempo` | the same |
| p08 | long outro | `19cab1c3` → `48443345` | 137.1 → 110.0 | 1.0 / 0.5 | adaptive 10 s, `incompatible_tempo` | the same |
| p09 | long outro | `07edd51e` → `c7709c78` | 133.0 → 128.0 | 0 / 0 | beatmatch 14.4 s dj-blend, bass swap | beatmatch 28.9 s dj-blend, bass swap |
| p10 | unstable | `6a8952f2` → `da40c60b` | 90.0 → 99.9 | 0 / 1.0 | adaptive 10 s, `low_confidence` | the same |

Rules, in order: two beatmatch plans with a bass swap and an equal-power curve (closest tempo
first); one long blend (longest first); two `incompatible_tempo` fallbacks (largest tempo gap
first); two edges voiced on both sides of the boundary (the two phrases each side, the profiles'
YAMNet bytes; most voiced first); two v3 plans starting more than 10 s before the handover (latest
first); one `low_confidence` or `unstable_tempo` fallback. v3 and v2 differ in their cues on every
pair (by 2–150 s) and in length on p09. The tempo-apart, vocal and long-outro rules all landed on
adaptive fallbacks because 91 % of the surveyed edges are adaptive (§6).

Variants: `decks-v3` (the app's defaults), `decks-v2` (G4b), `decks-v3-norg` (ReplayGain off, to
attribute level differences), `android-v3` and `android-v2` (the plans Decks played, on the Android
renderer), `android-listen`.

## 3. G4: the handover and the start, per run

From `deviation.csv` and the traces (mix pass; times on the outgoing track):

| Pair | Variant | Auto DJ handover (s) | Plan start (s) | Lead short by (s) | Report reason | Late (ms) | Incoming offset at start (ms) |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: |
| p01 | v3 / v2 | 265 | 277 / 285 | 0 / 0 | `completed` | 0 / 0 | 17.4 / 25.9 |
| p02 | v3 / v2 | 79.2 | 94.3 / 99.0 | 0 / 0 | `completed` | 0 / 0 | 8.7 / 14.3 |
| p03 | v3 / v2 | 205 | 200 / 202 | **4.4 / 2.4** | `completed` | 4,420 / 2,420 | 0.0 / 0.0 |
| p04 | v3 / v2 | 172 | 170 / 192 | **1.6** / 0 | `incompatible_tempo` | 1,633 / 0 | 0.0 / 20.9 |
| p05 | v3 / v2 | 314 | 319 / 334 | 0 / 0 | `incompatible_tempo` | 0 / 0 | 19.3 / 18.5 |
| p06 | v3 / v2 | 52.2 | 54.6 / 72.4 | 0 / 0 | `incompatible_tempo` | 0 / 0 | 17.9 / 21.9 |
| p07 | v3 / v2 | 222 | 236 / 243 | 0 / 0 | `incompatible_tempo` | 0 / 0 | 16.9 / 13.8 |
| p08 | v3 / v2 | 382 | 337 / 402 | **45.8** / 0 | `incompatible_tempo` | 45,798 / 0 | 0.0 / 4.0 |
| p09 | v3 / v2 | 203 | 159 / 204 | **44.0** / 0 | `completed` | 43,986 / 0 | 0.0 / 1.1 |
| p10 | v3 / v2 | 343 | 214 / 363 | **129.5** / 0 | `low_confidence` | 129,529 / 0 | 0.0 / 17.6 |

`v3-norg` runs as `v3` to the millisecond. Deviation CSV, all 30 runs: every one mixed (10 per
variant), from candidate 0 (the server's pick), with no control overridden; the reasons are the
plans' own (`completed`, or the fallback reason of a plan that was already a fallback, played as
Android plays it). Beat clock: 1 of 10 v3 runs and 1 of 10 v2 runs (p01). Late starts: v3 5 of 10
(1.6, 4.4, 44.0, 45.8, 129.5 s), v2 1 (2.4 s). The incoming offset is how far the incoming deck is
ahead of the plan against the outgoing deck at the start, in output time (the outgoing deck had not
reached its cue yet when the incoming one started); it stays constant through the
mix (no drift in any run). On late starts the director's jump puts it at 0 because the jump happens
right after a callback; on on-time starts it is 1.1–25.9 ms, median 17.7 ms (14 starts).

## 4. G4: Android against Decks, per pair and in aggregate

### 4.1 Rhythm, voice and bass (A Android, D Decks, D-norg Decks with ReplayGain off; same v3 plan)

| Pair | Beat clash (ms) | Phase error, median (ms) | Voice on voice (ms) | Bass on bass (ms) |
| --- | ---: | ---: | ---: | ---: |
| p01 | 0 / 0 / 0 | 10 / 20 / 20 | 0 / 480 / 480 | 0 / 0 / 0 |
| p02 | 3,340 / 3,340 / 3,340 | 70 / 90 / 90 | 180 / 0 / 0 | 0 / 0 / 0 |
| p03 | 2,550 / 1,000 / 1,000 | 20 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| p04 | 2,810 / 2,350 / 2,350 | 80 / 80 / 80 | 0 / 0 / 0 | 3,010 / 3,100 / 3,100 |
| p05 | 2,630 / 2,290 / 2,290 | 140 / 140 / 140 | 0 / 0 / 0 | 0 / 0 / 0 |
| p06 | 3,340 / 1,780 / 1,780 | 100 / 60 / 60 | 0 / 0 / 0 | 2,960 / 2,960 / 2,960 |
| p07 | 3,340 / 3,330 / 3,330 | 100 / 100 / 100 | 2,200 / 2,210 / 2,210 | 400 / 360 / 360 |
| p08 | 2,790 / 3,320 / 3,320 | 120 / 140 / 140 | 1,440 / 1,590 / 1,590 | 0 / 780 / 780 |
| p09 | 440 / 0 / 0 | 20 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| p10 | 2,680 / 2,860 / 2,850 | 160 / 160 / 160 | 1,620 / 0 / 0 | 1,700 / 3,210 / 3,200 |

### 4.2 Loudness, EBU R128 (A / D / D-norg)

| Pair | Step, abs (LU) | Dip (LU) | Bump (LU) | Steepest change (LU/s) |
| --- | ---: | ---: | ---: | ---: |
| p01 | 1.7 / 0.0 / 1.7 | 9.9 / 11.2 / 10.8 | 0.2 / 0.3 / 0.3 | 2.5 / 2.8 / 2.6 |
| p02 | 2.0 / 2.9 / 1.9 | 6.1 / 6.0 / 6.4 | 1.6 / 1.6 / 1.6 | 2.0 / 2.2 / 2.1 |
| p03 | 6.7 / 1.5 / 5.7 | 6.4 / 9.6 / 7.3 | 0.7 / 0.6 / 0.7 | 1.3 / 1.3 / 1.4 |
| p04 | 0.2 / 0.8 / 0.2 | 5.5 / 4.5 / 4.8 | 0.3 / 1.1 / 0.8 | 1.4 / 1.6 / 1.5 |
| p05 | 2.5 / 0.1 / 2.5 | 6.0 / 6.6 / 6.0 | 1.4 / 1.1 / 1.1 | 3.3 / 2.6 / 3.3 |
| p06 | 1.4 / 0.8 / 1.4 | 4.9 / 5.2 / 4.9 | 0.6 / 0.6 / 0.6 | 1.9 / 1.8 / 1.9 |
| p07 | 0.3 / 0.7 / 0.3 | 6.5 / 8.6 / 6.7 | 2.6 / 1.5 / 2.5 | 3.6 / 3.2 / 3.6 |
| p08 | 4.1 / 0.9 / 8.4 | 5.2 / 7.2 / 4.8 | 2.0 / −0.1 / −0.5 | 2.8 / 1.6 / 3.3 |
| p09 | 6.5 / 1.5 / 6.4 | 3.6 / 7.5 / 4.1 | 0.4 / 2.0 / 2.0 | 3.7 / 2.4 / 2.5 |
| p10 | 6.1 / 9.6 / 5.7 | 4.7 / 0.6 / 2.3 | 0.3 / 0.8 / 0.8 | 1.9 / 1.7 / 1.6 |

### 4.3 Aggregates

Better / same / worse beyond the noise floor, two-sided sign test.

| Metric | Decks against Android (same plan) | Decks with ReplayGain off against Android | Decks against Android's own Listen plan |
| --- | ---: | ---: | ---: |
| Beat clash | 5 / 3 / 2 (2,027 against 2,392 ms) | 5 / 3 / 2 | **1 / 1 / 8 (p 0.04)** (2,027 against 1,550 ms) |
| Phase error, median | 3 / 4 / 3 | 3 / 4 / 3 | 4 / 1 / 5 |
| Beats within 25 ms | 2 / 4 / 4 | 2 / 4 / 4 | **7 / 2 / 1 (p 0.07)** |
| Voice on voice | 1 / 9 / 0 | 1 / 9 / 0 | 1 / 7 / 2 |
| Bass on bass | 0 / 8 / 2 (1,041 against 807 ms) | 0 / 8 / 2 | 3 / 3 / 4 |
| Loudness step | 6 / 1 / 3 (1.9 against 3.2 LU) | 1 / 8 / 1 (3.4 LU) | **9 / 1 / 0 (p < 0.01)** (1.9 against 8.1 LU) |
| Loudness bump | 2 / 6 / 2 | 1 / 8 / 1 | 2 / 3 / 5 |
| Loudness dip | 2 / 2 / 6 (6.7 against 5.9 LU) | 2 / 6 / 2 (5.8 LU) | 3 / 0 / 7 (6.7 against 3.5 LU) |
| Steepest change | 3 / 7 / 0 | 1 / 9 / 0 | **7 / 3 / 0 (p 0.02)** |
| **Net, SM5's four** | **5 / 2 / 3 (p 0.73)** | **6 / 2 / 2 (p 0.29)** | 2 / 4 / 4 (p 0.69) |

Reading:

- **Same plan, ReplayGain off (the player against the player):** Decks reproduces Android. Loudness
  is the same on 8–9 of 10 pairs. Beat clash is better on p03, p05, p06 and p09 and worse on p08 and
  p10, which are late starts: Decks played another part of the incoming track there (finding 1),
  which also explains p08's and p10's bass on bass and p10's voice. On the two beatmatch pairs that
  started on time, Decks' phase error is 10 and 20 ms above Android's (p01 20 against 10 ms, p02 90
  against 70 ms): finding 2's late start, within the 25 ms tolerance on p01. p02 clashes on both
  players (phase 70–90 ms): the plan's own alignment, not Decks'.
- **ReplayGain on (the app's default):** only loudness moves (rhythm, voice and bass are identical
  to the millisecond). The step gets smaller (6 better, 3 worse against Android) and the dip deeper
  (2 better, 6 worse; p03 9.6 against 7.3 LU without ReplayGain, p07 8.6 against 6.7, p09 7.5
  against 4.1): finding 3. Net against Android drops from 6/2/2 to 5/2/3.
- **Against Android's own Listen plan** (6 s, v2, no bass swap on this server, see §11): Decks Auto
  DJ's 10 s plans clash longer (more overlap at full level) but lock more beats, and level far
  better (step 1.9 against 8.1 LU), at a deeper dip. Not the gate's question, but what a listener of
  both apps would hear.

## 5. Open question 1: how often the fixed 30 s handover lead is too short

**Measured.** In the harness's real Auto DJ the handover came where the survey's estimate put it on
10 of 10 pairs (to the second), so the estimate stands for the 254 surveyed edges:

| Planner | Plans starting before the handover | Of those, how early: median / p90 / max | More than 10 s / 30 s early | Lead the plan needs (Auto DJ's fade start minus the plan's start): p50 / p90 / p95 / max |
| --- | ---: | ---: | ---: | ---: |
| `smart-mix-v3` | **89 of 254 (35 %)** | 11.6 / 50.5 / 129.6 s | 46 / 18 | 23.6 / 53.3 / 69.6 / 159.6 s |
| `smart-mix-v2` | 7 of 254 (3 %) | 2.4 / 10.0 / 11.3 s | 1 / 0 | 6.0 / 10.0 / 10.1 / 41.3 s |

**What it does.** The director arms at the handover, finds its cue already passed, starts at once
and jumps the incoming deck forward by the lateness (as designed, so the beats stay aligned): 5 of
the 10 G4 pairs with v3 (1.6, 4.4, 44.0, 45.8 and 129.5 s). On p10 the incoming track starts at
2:39 instead of its cue at 0:30, on p08 at 0:46 instead of 0:01. v3 asks for this more because it ends its mixes on phrases well before the end (LV0 §3: v3
cuts 18–31 s of outro on average); Auto DJ's fade point comes from the Intro and Outro cues, which
for these profiles end at the active end. Nothing else breaks: no deadline, no `insufficient_window`.

**Proposed fix (not implemented).** Arm the director from the cached plan, not a fixed lead: the
adapter knows the plan of the next edge before the handover (it asks ahead), so `TransitionDirector`
can tell Auto DJ the lead it needs (`max(30 s, fade start − SmartMixDirector::startAtMs(plan) +
kPrepareLeadMs + margin)`), and `calculateTransition` uses it instead of
`kTransitionDirectorLeadSeconds`; the 30 s stays the default without a plan. A server-side cap like
Listen's (LV-D9) would also shorten v3's early cues, at the cost of v3's gains.

## 6. Open question 2: why the first live run had no bass handoff

1. **By design, few edges get one.** Both planners give a bass handoff only to beatmatch plans
   (`planner.py`: `bass_handoff="balanced" if context.allow_bass_handoff` only in the beatmatch
   branch; `candidates.py` `_variants`: non-beatmatch geometries try only `"none"`). Over the 254
   surveyed edges, 22 plan a beatmatch (8.7 %, all with the handoff), and 232 fall back to adaptive
   (`incompatible_tempo` 120, `low_confidence` 57, `unstable_tempo` 55), identically in v2 and v3. In
   "G3 · Auto DJ (industrial EBM)", the first live suite's playlist, 1 of 11 edges plans a
   beatmatch (`0fd0e8a9` → `6fb180bc`). A live session of a few transitions there would most likely
   have none.
2. **The server side was arriving that day.** The server's own switch for the Decks bass handoff
   (`429183d32`, 2026-11-15 14:11) started off; it was turned on by default at 14:34 (`aa2d10033`)
   and reached `main` at 20:47 (PR #437). Decks' logging commit (`d7a16cac2b`, "the first live test
   of Crate Smart Mix saw no bass handoff") is from 19:17. A stack running `main` at the time of the
   test announced no `bass_handoff` in `decks.features`, so Decks asked `allowBassHandoff=false`,
   unless it ran the feature branch. There is no log left to tell which.
3. **Decks' side works.** With the app's defaults the default EQ is on on both Auto DJ decks
   (`isDefaultEqOn` true), the adapter asks `allowBassHandoff=true`, every beatmatch plan of the G4
   pairs came with `bassHandoff: balanced`, and the director drove both low knobs to the −24 dB cut
   (`eqmap::lowGainForCutDb`) on all 5 beatmatch pairs, with no bass on bass in any of them. The
   remaining unknown is the session's own EQ preference ("Bypass EQ" or another EQ also drops it);
   the logging added since names which part fails.

## 7. G4b: planner v2 against v3 in Decks

### 7.1 Per pair (Decks, ReplayGain on; v2 / v3)

| Pair | Beat clash (ms) | Phase error (ms) | Voice on voice (ms) | Bass on bass (ms) | Step (LU) | Dip (LU) | Bump (LU) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| p01 | 0 / 0 | 0 / 20 | 0 / 480 | 0 / 0 | 6.6 / 0.0 | 11.7 / 11.2 | 1.1 / 0.3 |
| p02 | 2,320 / 3,340 | 20 / 90 | 0 / 0 | 0 / 0 | 3.8 / 2.9 | 10.1 / 6.0 | 0.1 / 1.6 |
| p03 | 480 / 1,000 | 20 / 0 | 0 / 0 | 0 / 0 | 2.5 / 1.5 | 8.7 / 9.6 | 0.6 / 0.6 |
| p04 | 3,340 / 2,350 | 80 / 80 | 2,860 / 0 | 1,340 / 3,100 | 0.6 / 0.8 | 5.2 / 4.5 | −0.2 / 1.1 |
| p05 | 2,230 / 2,290 | 140 / 140 | 0 / 0 | 0 / 0 | 8.4 / 0.1 | 2.2 / 6.6 | 2.4 / 1.1 |
| p06 | 2,880 / 1,780 | 100 / 60 | 1,740 / 0 | 2,790 / 2,960 | 4.9 / 0.8 | 3.8 / 5.2 | 0.3 / 0.6 |
| p07 | 3,330 / 3,330 | 220 / 100 | 2,850 / 2,210 | 820 / 360 | 8.5 / 0.7 | 2.1 / 8.6 | 0.2 / 1.5 |
| p08 | 2,450 / 3,320 | 140 / 140 | 480 / 1,590 | 0 / 780 | 8.9 / 0.9 | 9.1 / 7.2 | −3.5 / −0.1 |
| p09 | 440 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 1.0 / 1.5 | 9.7 / 7.5 | 2.3 / 2.0 |
| p10 | 2,920 / 2,860 | 100 / 160 | 1,740 / 0 | 0 / 3,210 | 12.2 / 9.6 | 1.0 / 0.6 | −0.1 / 0.8 |

### 7.2 Aggregates (v3 against v2, better / same / worse)

| Metric | Decks (as Decks plays them) | The same plans on the Android renderer |
| --- | ---: | ---: |
| Beat clash | 3 / 4 / 3 (2,027 against 2,039 ms) | 3 / 2 / 5 |
| Phase error, median | 3 / 4 / 3 | 2 / 4 / 4 |
| Mix beat interval CV | 2 / 4 / 4 | 2 / 4 / 4 |
| Voice on voice | 4 / 5 / 1 (428 against 967 ms) | 2 / 8 / 0 |
| Bass on bass | 0 / 7 / 3 (1,041 against 495 ms) | 0 / 8 / 2 |
| Loudness step | **8 / 2 / 0 (p 0.01)** (1.9 against 5.8 LU) | 7 / 1 / 2 (p 0.18) |
| Loudness bump | 2 / 3 / 5 | 3 / 2 / 5 |
| Loudness dip | 4 / 2 / 4 (6.7 against 6.4 LU) | 4 / 0 / 6 |
| Steepest change | 4 / 4 / 2 | 2 / 5 / 3 |
| **Net, SM5's four** | **7 / 2 / 1 (p 0.07)** | 5 / 1 / 4 (p 1.00) |

v3 is better in Decks on net, mostly through the loudness step (its phrase entries keep both tracks
near full level) and voice on voice; it pays in bass on bass (as LV0 found for Listen; the bass swap
does not cover the adaptive fallbacks, where both happen) and in bump. Under LV-D8's reading (not
worse on net or on the dip at p < 0.1), v3 passes: better on net at p 0.07, even on the dip. Two
cautions: 5 of the 10 v3 renders include finding 1's jump, so they are "v3 as Decks plays it today";
and on the Android renderer the same plans are even (5/1/4), so part of v3's gain in Decks comes from
how ReplayGain and v3's staging combine (finding 3).

### 7.3 Decks' limit check against the server's `limit_violations`

- **Before (live, F6b.PR1):** 88 of 88 candidates and 168 of 168, on the two G3 playlists.
- **Re-measured on the G4 pairs** (`limit_cases.py` runs the monorepo's own `limit_violations` and
  `_is_short_cut` through Decks' `make_limit_violation_cases.py` builders; `LimitCheckAgreement`
  runs `TransitionPlan::limitViolations()` and `mixes()` on the same cases): **198 of 198** give Decks
  exactly the server's codes in its order, 90 of them with at least one code; `mixes()` agrees on 198.
  The 80 real candidates of the ten v3 menus: 80 of 80, all without codes.
- **The choice on real decks** (F6b.PR1's `TEST_GAP`): all 80 candidates pass `validate()` on the
  tracks as the engine loaded them, and in 20 of 20 Auto DJ runs Decks played the server's pick
  (`candidateIndex` 0). The case where the next candidate replaces Auto DJ's fallback did not occur.

## 8. Findings, ranked by impact, with proposed fixes

None is implemented; each is owed before G6 if Diego approves (decision of 2026-11-16).

1. **Fixed 30 s handover lead (high; v3).** 35 % of v3 edges start before the handover, 7 % more
   than 30 s before; the incoming deck jumps forward by up to 130 s here. Fix: lead from the cached
   plan (§5). Re-measure: rerun the harness; the "Lead short by" column must read 0.
2. **Start off by up to one callback (medium; every on-time start).** 1–26 ms early, median 18 ms
   at the default buffer; within the 25 ms clash tolerance on most pairs, but it eats the margin of
   every beatmatch. The director decides on the last callback's position, short of the cue, and its
   `play` takes effect at the next callback, which plays the outgoing deck from that position,
   without compensating; the beat clock snaps rarely (1 of 20 runs; on p01
   upstream's grid is at half tempo, 85 against 170 BPM). Fix: in `startIncoming`, seek the incoming
   deck by the time to the callback that will start it (or extrapolate the outgoing position with
   `VisualPlayPosition`'s callback time and rate, as its interpolation does) so the start lands on
   the plan at any buffer size; re-measure with the harness at 256 and 2048 frames too.
3. **ReplayGain on top of the plan's staging (medium; levels).** The server stages gains for
   unnormalised tracks (−1.8 to −7.2 dB here); Decks applies them on top of ReplayGain, which the app
   turns on by default. Dip deeper than without ReplayGain on 5 pairs (up to +3.4 LU) and than
   Android's on 6 (up to +3.9 LU), step flatter. Fix, one of: send the
   server a context flag that playback is normalised (planner skips or recomputes staging), or have
   the director stage only the residual (`plan gain − (ReplayGain difference of the two decks)`,
   attenuating only), or turn staging off when both decks have ReplayGain. Decide with the ear.
4. **Bass on bass with v3 (low; known).** As in LV0: 3 pairs worse, none better; the bass swap only
   exists in beatmatch plans. Not Decks-specific; a bass term for adaptive plans would be server work.
5. **Bump with v3 (low).** 5 pairs worse, 2 better, up to +1.5 LU; small.

## 9. What could not be measured, and what is modelled

- **Android's real start.** The renderer starts the incoming track sample-accurately at the cue.
  Android's preroll and phase measurement on a device are not rendered; if they drift, Android's
  numbers in §4 are optimistic.
- **The app's clock.** The harness clock is the engine's frames: no audio device jitter, no main
  thread stalls, no wall clock between the director's ticks and the callbacks other than the
  modelled 16 ms timer and 1 ms start timer. The buffer is the default 1024 frames; other sizes were
  not rendered.
- **Keylock latency.** The incoming deck plays with keylock (RubberBand); its own processing delay is
  in the audio (Beat This! measures it) but not separated in the trace offsets.
- **Auto DJ's own fade.** No run handed the transition back, so Auto DJ's `FullIntroOutro` fallback
  was not rendered.
- **Android's own Listen plan** used this server's `listen_planner_version` (`smart-mix-v2`) and the
  production context; the dev stack's Android flags are off, so those plans have no bass swap.
  Production Listen (v3 with LV-D9's caps after LV1) was not rendered.
- **Repeatability.** The harness was run twice in full. The director's Reports (`deviation.csv`),
  the starts and the late starts are identical; the end of the transition came one callback later or
  earlier on 7 of 30 runs (the reader thread's timing), which moves a few metrics at the window's
  edge: p07's dip by 2.5 LU, p01's and p10's v2 beat clash by 200 and 380 ms. The tables are the
  second run's, made with the committed harness; no reading in §§4, 7 and 10 changes between runs
  except G4's net with ReplayGain on (5/3/2 in the first run, 5/2/3 here).
- **Ten pairs.** Sign tests over 10 pairs flag only large effects; most comparisons here are not
  significant either way, and the report reads them as "the same" where the counts are even.
- **No listening.** All metrics are proxies (LV0 §9); the renders are there for Diego's ear.

## 10. Verdict proposals (superseded by §12.4)

**G4: approve.** Decks' director, on the real engine with Auto DJ in "Crate Smart Mix", plays the
server's plan as the Android player does: with ReplayGain off the two measure the same on loudness,
voice and bass and Decks is better or even on beat clash (net 6/2/2), and every one of the 30 runs
was played by the director from the server's pick. With the app's ReplayGain on, net is 5/2/3,
the difference being finding 3's deeper dips. Owed before G6, in this order: finding 1 (the
handover lead from the cached plan), finding 2 (start compensation) and a decision on finding 3
(ReplayGain and staging). Then rerun this harness on the same pairs; the gate's evidence is
reproducible from the branch.

**G4b: approve v3 as Decks' planner** (F6b.PR1 as merged). It measures better than v2 in Decks on
net (7/2/1, p 0.07) and on the loudness step (p 0.01), with LV0's known bass-on-bass cost, and the
limit check mirrors the server's on every case (198/198 here, 88/88 and 168/168 before). Condition:
finding 1, which v3 triggers ten times as often as v2.

## 11. Reproduce

```bash
# Monorepo tools (worktree crate-agents/decks-g4-tools, branch tools/decks-g4-metrics)
cd tools/smart-mix-eval/decks_g4
python3 survey.py --email … --password … --music-root <crate-main>/crate/test-music --output <g4>/survey.json
python3 select_pairs.py --survey <g4>/survey.json --output <g4>/pairs.json
python3 fetch_reference_plans.py --email … --password … --pairs <g4>/pairs.json --output <g4>/reference_plans.json
docker run … crate-analysis-worker python limit_cases.py --reference <g4>/reference_plans.json \
  --decks-cases-tool <decks>/src/test/crate/data/transition-plans/make_limit_violation_cases.py \
  --output <g4>/limit_cases.json

# Decks (branch test/decks-g4-reports): renders and the limit check
QT_QPA_PLATFORM=offscreen CRATE_DECKS_G4_PAIRS=<g4>/pairs.json CRATE_DECKS_G4_OUT=<g4>/renders \
  CRATE_DECKS_G4_LIMIT_CASES=<g4>/limit_cases.json CRATE_DECKS_LIVE_URL=http://127.0.0.1:8585 \
  CRATE_DECKS_LIVE_EMAIL=… CRATE_DECKS_LIVE_PASSWORD=… \
  build/mixxx-test --gtest_filter='CrateDecksG4Render.*'

# Metrics (analysis worker image: Beat This!, YAMNet, FFmpeg) and tables
docker run … -v <test-music>:/music:ro crate-analysis-worker python metrics.py --pairs <g4>/pairs.json \
  --runs <g4>/renders --reference <g4>/reference_plans.json --music-root <test-music>=/music \
  --output <g4>/metrics --jobs 6
python3 tables.py --g4 <g4> > <g4>/tables.md
```

The full commands, with the mounts, are in the tools' docstrings. The harness takes about 5 minutes
for 60 runs; the metrics about 2 minutes with 6 jobs.

## 12. After the fixes (2026-11-19)

### 12.1 What changed and how it was measured

Diego's decision of 2026-11-19 (see the note at the top) asked to fix findings 1 to 3 first. Branch
`test/decks-g4-reports`, local commits after `873165d875`; the rules are recorded in the plan's G4
notes ("Implemented (G4 fixes, 2026-11-19)"):

- **Finding 1:** `AutoDJSmartMixAdapter::transitionLeadSeconds()` gives Auto DJ the lead the edge's
  cached plan needs: Auto DJ's fade start less the plan's start, plus the director's 8 s
  preparation and a 2 s margin, within 30 s and 300 s; 30 s without a cached plan. Plans that arrive
  after the handover was placed place it again (`AutoDJProcessor::transitionLeadChanged()`).
- **Finding 2:** `SmartMixDirector::startIncoming()` moves the incoming deck by however far the
  outgoing deck's engine position (where the next callback plays it from) is from the plan's start,
  early or late, and reports it (`Report::startOffsetMs`).
- **Finding 3 (residual):** with ReplayGain on and data for both tracks, the incoming deck stages
  `min(0, plan gain − ReplayGain difference)`; otherwise the plan's gain. The outgoing deck's
  headroom cut stays.

The harness ran again with the same pairs, variants and settings (`renders-after/`,
`metrics-after/`), and v3 and v2 again at 256 and 2048 frames for the start
(`renders-after-256/`, `renders-after-2048/`). The Decks metrics now divide out the director's own
staging (`incomingStagingDb` of each run). The limit check gave 198 of 198 again.

### 12.2 Before and after, finding by finding

#### Finding 1: the handover lead

| Pair | Variant | Handover before → after (s) | Plan start (s) | Lead short by, before → after (s) | Late start, before → after (ms) |
| --- | --- | ---: | ---: | ---: | ---: |
| p01 | v3 | 265 → 265 | 277 | 0.0 → 0.0 | 0 → 0 |
| p01 | v2 | 265 → 265 | 285 | 0.0 → 0.0 | 0 → 0 |
| p02 | v3 | 79.2 → 79.2 | 94.3 | 0.0 → 0.0 | 0 → 0 |
| p02 | v2 | 79.2 → 79.2 | 99.0 | 0.0 → 0.0 | 0 → 0 |
| p03 | v3 | 205 → 190 | 200 | 4.4 → 0.0 | 4,420 → 0 |
| p03 | v2 | 205 → 192 | 202 | 2.4 → 0.0 | 2,420 → 0 |
| p04 | v3 | 172 → 160 | 170 | 1.6 → 0.0 | 1,633 → 0 |
| p04 | v2 | 172 → 172 | 192 | 0.0 → 0.0 | 0 → 0 |
| p05 | v3 | 314 → 309 | 319 | 0.0 → 0.0 | 0 → 0 |
| p05 | v2 | 314 → 314 | 334 | 0.0 → 0.0 | 0 → 0 |
| p06 | v3 | 52.2 → 52.2 | 54.6 | 0.0 → 0.0 | 0 → 0 |
| p06 | v2 | 52.2 → 52.2 | 72.4 | 0.0 → 0.0 | 0 → 0 |
| p07 | v3 | 222 → 222 | 236 | 0.0 → 0.0 | 0 → 0 |
| p07 | v2 | 222 → 222 | 243 | 0.0 → 0.0 | 0 → 0 |
| p08 | v3 | 382 → 327 | 337 | 45.8 → 0.0 | 45,798 → 0 |
| p08 | v2 | 382 → 382 | 402 | 0.0 → 0.0 | 0 → 0 |
| p09 | v3 | 203 → 149 | 159 | 44.0 → 0.0 | 43,986 → 0 |
| p09 | v2 | 203 → 203 | 204 | 0.0 → 0.0 | 0 → 0 |
| p10 | v3 | 343 → 204 | 214 | 130 → 0.0 | 129,529 → 0 |
| p10 | v2 | 343 → 343 | 363 | 0.0 → 0.0 | 0 → 0 |

#### Finding 2: the incoming deck's offset from the plan at the start

| Run | Buffer (frames) | Starts | Offset at start, ms: median / min / max | Within 1 ms |
| --- | ---: | ---: | ---: | ---: |
| before | 1024 | 20 | 14.0 / 0.0 / 25.9 | 6 |
| after | 1024 | 20 | 0.0 / -0.0 / 3.2 | 19 |
| after | 256 | 20 | -0.0 / -0.5 / 0.5 | 20 |
| after | 2048 | 20 | 0.0 / -0.0 / 3.2 | 19 |

#### Finding 3: ReplayGain and staging (v3, ReplayGain on)

| Pair | ReplayGain difference (dB) | Incoming staging: plan → residual (dB) | Step, A / D before / D after (LU) | Dip, A / D before / D after (LU) |
| --- | ---: | ---: | ---: | ---: |
| p01 | -1.7 | -4.7 → -2.9 | 1.7 / 0.0 / 0.0 | 9.9 / 11.2 / 11.2 |
| p02 | -0.9 | -4.0 → -3.1 | 2.0 / 2.9 / 2.9 | 6.1 / 6.0 / 5.7 |
| p03 | 4.3 | -3.8 → -8.1 | 6.7 / 1.5 / 2.4 | 6.4 / 9.6 / 11.0 |
| p04 | -1.0 | -4.2 → -3.2 | 0.2 / 0.8 / 0.7 | 5.5 / 4.5 / 5.1 |
| p05 | -2.5 | -4.1 → -1.7 | 2.5 / 0.1 / 0.1 | 6.0 / 6.6 / 6.0 |
| p06 | 0.6 | -5.3 → -5.9 | 1.4 / 0.8 / 0.8 | 4.9 / 5.2 / 5.7 |
| p07 | -1.1 | -4.4 → -3.4 | 0.3 / 0.7 / 0.7 | 6.5 / 8.6 / 5.5 |
| p08 | -7.5 | -6.0 → 0.0 | 4.1 / 0.9 / 11.3 | 5.2 / 7.2 / 1.3 |
| p09 | 4.8 | -4.0 → -8.8 | 6.5 / 1.5 / 11.4 | 3.6 / 7.5 / 4.0 |
| p10 | -4.0 | -6.0 → -2.0 | 6.1 / 9.6 / 2.1 | 4.7 / 0.6 / 4.7 |


- **Finding 1, done: "Lead short by" reads 0 on all 20 runs** (it was 1.6–129.5 s on 5 v3 runs and
  2.4 s on one v2 run), and no run started late. Auto DJ now hands over 10 s before the plan's start
  when the plan needs it (p10: at 204 s for a start at 214 s, instead of 343 s). Over the 254
  surveyed edges, the new rule gives no late start (the largest lead needed is 170 s, under the 300 s
  ceiling); it lengthens the lead past 30 s on 151 v3 edges (median lead 34 s, max 170 s) and on
  11 v2 edges (max 51 s). Auto DJ then waits in its fading state for that long, with the director
  armed and the incoming deck prepared 8 s before the start.
- **Finding 2, done: the start lands on the plan at every buffer size.** After the fix 19 of 20
  starts are within 1 ms at 1024 and 2048 frames and 20 of 20 at 256 (before: 6 of 20, median
  14 ms, up to 26 ms). The one exception (3.2 ms, p01 v2) is the incoming track's own start: its
  cue is 23 ms in and the start would need 26 ms before it, so it starts at 0.
- **Finding 3, done: the dip with ReplayGain on is back to Android's.** Mean dip 6.0 LU (Android
  5.9, before 6.7); against Decks with ReplayGain off on the same content the dip is now better on
  3 pairs and worse on 2 (before: 1 better, 5 worse). The loudness step is ReplayGain's own: it
  normalises whole tracks, not the outro against the intro, so it moves the step either way (p08
  11.3 LU, p10 2.1 LU, against Android's 4.1 and 6.1); staging cannot change it, because the
  incoming deck is back at unity at the end of the transition. Where ReplayGain raised the incoming
  deck over the outgoing one (p03 +4.3 dB, p09 +4.8 dB), the residual is a deeper cut (−8.1 and
  −8.8 dB) and p03's dip got deeper (11.0 LU).

### 12.3 G4 and G4b after the fixes

Better / same / worse beyond the noise floor (§2.3), 10 pairs:

| Metric | Decks against Android, same plan (ReplayGain on) | The same, ReplayGain off | ReplayGain on against off | G4b: Decks v3 against v2 |
| --- | ---: | ---: | ---: | ---: |
| Beat clash | 2 / 5 / 3 | 3 / 5 / 2 | 0 / 9 / 1 | 3 / 2 / 5 |
| Phase error, median | 3 / 3 / 4 | 3 / 3 / 4 | 0 / 10 / 0 | n/a |
| Voice on voice | 1 / 8 / 1 | 1 / 8 / 1 | 0 / 10 / 0 | 3 / 7 / 0 |
| Bass on bass | 0 / 10 / 0 | 0 / 10 / 0 | 0 / 10 / 0 | 0 / 8 / 2 |
| Loudness step | 5 / 2 / 3 (3.2 against 3.2 LU) | 0 / 10 / 0 | 5 / 2 / 3 | 6 / 2 / 2 (3.2 against 5.7 LU) |
| Loudness bump | 2 / 6 / 2 | 0 / 10 / 0 | 2 / 6 / 2 | n/a |
| Loudness dip | 2 / 5 / 3 (6.0 against 5.9 LU) | 0 / 9 / 1 | 3 / 5 / 2 | 3 / 0 / 7 (6.0 against 5.7 LU) |
| **Net, SM5's four** | **4 / 2 / 4 (p 1.00)** | **4 / 3 / 3 (p 1.00)** | 4 / 5 / 1 (p 0.38) | **3 / 3 / 4 (p 1.00)** |

- **G4: Decks now reproduces the Android renderer on the same plan.** With ReplayGain off, loudness
  step, bump and slope are the same on 10 of 10 pairs, the dip on 9, bass on bass on 10, voice on 8;
  beat clash 3 better, 2 worse. The differences of §4 in the late-start pairs are gone with the late
  starts. With the app's ReplayGain on, net is even (4/2/4): ReplayGain moves the step either way,
  and nothing else.
- **G4b changed.** Before the fixes v3 was better than v2 in Decks on net (7/2/1, p 0.07); after,
  it is even (3/3/4), like the Android renderer's reading of the same plans (5/1/4). Part of v3's
  earlier advantage came from finding 1's jumps (the incoming track started tens of seconds in, at
  full level) and from finding 3. v3 still has a smaller loudness step (6/2/2, 3.2 against 5.7 LU)
  and less voice on voice (3/7/0); it pays in the dip (3/0/7, 6.0 against 5.7 LU, p 0.34) and in
  bass on bass (0/8/2). Under LV-D8's reading (not worse on net or on the dip at p < 0.1) it still
  passes; it is no longer better.

### 12.4 Revised verdict proposals

**G4: approve.** With the three fixes, Decks' Auto DJ in "Crate Smart Mix" plays the server's plan
as the Android player does: no late starts ("Lead short by" 0 on 20 of 20 runs, none expected over
the 254 surveyed edges), the incoming deck on the plan within 1 ms at 256, 1024 and 2048 frames
(one 3.2 ms start clamped at the track's beginning), and the same loudness, bass and voice as the
Android renderer with ReplayGain off. With the app's ReplayGain on the result is even (4/2/4); the
remaining difference is ReplayGain's whole-track normalisation, which Diego may want to hear on
p08 and p10 (§12.2).

**G4b: approve v3 as Decks' planner, as "not worse" rather than "better".** After the fixes v3 and
v2 are even on net in Decks (3/3/4), v3 with a smaller loudness step and less voice on voice and a
deeper dip and more bass on bass, so it passes LV-D8's rule without a measured gain. The limit
check mirrors the server on 198 of 198 cases, and Decks always played the server's pick. If Diego
wants v3 only for a measured gain, the alternative is to keep v2 in Decks' Auto DJ until a larger
set shows one; the menu and limits work of F6b.PR1 stays either way.

### 12.5 Reproduce the after runs

```bash
# Decks (this branch): CRATE_DECKS_G4_BUFFER_FRAMES sets the engine's buffer (default 1024)
CRATE_DECKS_G4_OUT=<g4>/renders-after build/mixxx-test --gtest_filter='CrateDecksG4Render.*'   # + the variables of §11
CRATE_DECKS_G4_BUFFER_FRAMES=256 CRATE_DECKS_G4_VARIANTS=v3,v2 CRATE_DECKS_G4_OUT=<g4>/renders-after-256 …
CRATE_DECKS_G4_BUFFER_FRAMES=2048 CRATE_DECKS_G4_VARIANTS=v3,v2 CRATE_DECKS_G4_OUT=<g4>/renders-after-2048 …
# Metrics for each, then the comparison (monorepo tools)
python3 compare_runs.py --g4 <g4> --buffers 256=renders-after-256:metrics-after-256/metrics.json \
  2048=renders-after-2048:metrics-after-2048/metrics.json
```

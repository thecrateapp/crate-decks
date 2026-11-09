# Smart Mix Stems · ST3 evaluation report

> **Input for Diego's STEM-G1 decision: whether `smart-mix-v3` uses the stems layer by
> default.** This report does not decide the gate.
>
> **STEM-G1 decided (Diego, 2026-11-09): on by mix type, with the vocal fix.** §11 describes the
> final rule (rules revision 3) and measures it; §1–§10 are the evidence it was decided on. §12
> adds machine-drum industrial at Diego's request (rules revision 4) and measures it on analyzer
> v8 with an industrial set and a metal and punk set. §13 keeps the layer to Crate Decks (rules
> revision 5).

- **Date:** 2026-11-09
- **Code:** monorepo branch `feat/smart-mix-stems-st3` on `feat/smart-mix-stems-st1` at
  `3d617852` (ST1, ST2.PR1–PR3), local commits only:
  - ST3.PR1 `5e5b6bbf` (scoring with stems, rules revision 2) and `89db3f44` (the calibrated
    presence cuts, committed after the harness that measures them);
  - ST3.PR2 `e10367dc` (candidate menu and policy limits for Crate Decks);
  - ST3.PR3 `aca8c21b` (harness: separation pool, `run_eval --stems`, calibration,
    `bassOnBassMs`) and `9835f77b` (calibration checks beyond the valley).
- **Plan and design:** ST3 in `2026-11-08-smart-mix-stems-implementation-plan.md`; STEM-D4,
  STEM-D5, STEM-D10, §5 and §9 of `2026-11-08-smart-mix-stems-design.md`. Harness and metrics
  as in the SM5 report (`2026-11-07-smart-mix-3-ai-eval-report.md`).

## 1. Summary

- **Built (ST3.PR1–PR3):** v3 scores its phrase menu with the stems layer when both tracks have
  one (vocal stem instead of YAMNet, bass only where both phrases have a bassline, two small
  bonuses), rules revision 2, byte-identical without stems; Crate Decks gets the menu and the
  policy limits (`source: "decks"` with `localRefinement`); the harness separates, calibrates and
  compares. 4,475 related tests pass; Android and VirtualDJ responses are unchanged.
- **Presence cuts calibrated** (§5): vocals −45 dBFS / 17.9 dB against synced-lyrics labels, with
  balanced accuracy 0.82 on folds by track and 0.79 on 49 held-out tracks, against YAMNet's 0.74
  on the same phrases; bass 17 dB and drums 14 dB below each track's loudest phrase, from level
  distributions without ground truth, which the plans barely depend on.
- **GPU separation matches the CPU path** (§4): SDR 56–73 dB, level bytes off by one in at most
  2.8 % of windows.
- **Profile metrics** (§6): the layer does what it scores (stem bass clash −32 to −55 %, stem
  vocal clash −87 to −97 %, no policy violation), but the two vocal detectors trade places
  (YAMNet's vocal clash +141 to +451 ms), and energy continuity and phrase alignment get a little
  worse.
- **Listening metrics** (§7): bass on bass improves in every run (significantly in five of eight
  comparisons). **As committed, the phrase set in `dj` measures worse on net (25 better / 45
  worse edges, p 0.02)**, from deeper loudness dips and more voice on voice; the electronic set
  is neutral. A variant that takes voice from the stem or YAMNet removes the voice regression,
  keeps the bass gain and is within noise on net everywhere; the deeper dips remain.
- **Recommendation for STEM-G1** (§8): not on as committed. Either a small follow-up (vocal
  presence from either detector, rules revision 3) and on after Diego listens to the dip edges,
  or off by default for now. ST4–ST7 do not depend on the choice.

## 2. What the code does

**Scoring (ST3.PR1, STEM-D5).** When both profiles of an edge carry a stems layer, the v3 phrase
menu is scored with it; otherwise every candidate and score is rules revision 1's, which a golden
file recorded from the unchanged code checks (`candidates_v3_rules1.json`).

- **Vocal penalty:** the same weight (6.0) and reference, counting phrases where the vocal stem
  is present instead of YAMNet's byte (`stem_vocal_overlap_ms`).
- **Bass component:** one minus the share of the mix with both basses at full level (crossfade
  gain plus the handoff shelf) while both phrases have a bassline (`stem_bass_overlap_ms`). A mix
  where one side has no bassline scores like a handoff.
- **Two bonuses of 0.05 each:** the share of the mix before the handoff where the incoming plays
  drums without bass or vocals (`drums_only_entry`), and the share where the outgoing plays a
  phrase without vocals (`vocal_free_exit`).
- `PLANNER_POLICY_V3`: `rules_revision` 2 and `stems_layer: true`; STEM-G1 decides that flag.
  Plans say `stemsUsed: true` only when used, so v3 responses without stems, v2 and every Android
  and VirtualDJ response are byte-identical. The plan batch reads the stem analyses of its tracks
  in one bounded query and keys the plan cache on the analyses that apply to both tracks.

**Presence per phrase (§5).** A phrase has a stem when its mean level byte is at least the
stem's floor and at most the stem's drop below the track's loudest phrase of that stem. The cuts
are calibrated in §5 of this report.

**Menu and limits for Crate Decks (ST3.PR2, STEM-D10).** With `plannerVersion: "smart-mix-v3"`,
`context.source: "decks"` and `context.localRefinement: true`, each plan also carries its
`candidates` (the menu with every score breakdown, stems terms included) and `policyLimits` (the
policy resolved for the edge). `limit_violations` is the reference check Decks mirrors: on
hundreds of plans around every bound it gives exactly the codes of `policy_violations`.
Capabilities advertise `decks.features: local_refinement`. Android and VirtualDJ plans and cache
keys do not change (tests, `test_vdj_contract_baseline` included).

## 3. Data

| Set                                                                               |    Tracks | Edges | Use                                                 | Stem layers                                                                                       |
| --------------------------------------------------------------------------------- | --------: | ----: | --------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| **Electronic** (SM5 R2.3): drum and bass, house, techno, other dance, cross-genre |       120 |   112 | comparison, bass and drums calibration              | 104 of the 105 edge tracks (one duration differs by more than 2 %): 111 edges with layers on both |
| **Phrase set** (SM5 primary): alternative, post-punk, industrial, rock            |        64 |   184 | comparison, bass and drums calibration              | all 64: 184 edges                                                                                 |
| **Lyrics** (SG1b): 20 genres with LRCLIB synced lyrics                            | 97 of 275 |     — | vocal calibration (980 labelled phrases)            | —                                                                                                 |
| **Lyrics, held out**                                                              |        49 |     — | check of the fixed vocal cut (453 labelled phrases) | —                                                                                                 |

- **Profiles:** analyzer v7 with phrases, read-only from the SM5 evaluation copy
  (`sm5-eval-postgres`). No v8 profiles exist there, so the comparison pairs v3 with and without
  the layer on the same v7 profiles.
- **Lyrics subset:** the 275 SG1b tracks were ordered round-robin by genre and separated in that
  order, so any prefix is balanced. Calibration used the first 97 (all that existed when the cuts
  were fixed). Tracks separated afterwards are a held-out check (§5.4).
- **Audio:** local copies made for SM5 and SG1b. No production access, nothing copied to `/tmp`,
  no audio, stems or analyses committed.

## 4. Separation and CPU/GPU parity

Every analysis comes from the server's own code (`crate.smart_mix.separation`, separator
`htdemucs-onnx-1`, the model file with SHA-256 `808790c0…6a08`) through `separate_pool.py`. To
fit 281 tracks into a day, the graph ran on ONNX Runtime 1.31.1's CUDA provider (CUDA 13 and
cuDNN 9 from pip wheels, two inter-op threads per session); STFT, iSTFT, segmentation,
overlap-add and the level bytes stay the same numpy as on the server.

- **Throughput** with two jobs on a laptop shared with C++ builds: a median of 43–50 s of
  separation per 5 minutes of audio, about 1.8 GB of GPU memory for both sessions, and no
  failures. This is not a Crate Decks budget measurement (the DSP is CPU-bound numpy).
- **Parity** (`separate_pool.py parity`), CUDA against the CPU path on three tracks (techno,
  Skinny Puppy's "Assimilate", industrial metal):

| Track            | SDR drums / bass / other / vocals (dB) | Level bytes that differ (of the 250 ms windows) | Largest difference |
| ---------------- | -------------------------------------- | ----------------------------------------------- | -----------------: |
| techno           | 64.1 / 66.0 / 67.0 / 56.2              | 4 / 33 / 8 / 41 of 1,632                        |                  1 |
| "Assimilate"     | 73.1 / 60.2 / 64.9 / 68.2              | 5 / 46 / 10 / 16 of 1,667                       |                  1 |
| industrial metal | 68.7 / 63.7 / 71.5 / 69.2              | 2 / 13 / 2 / 7 of 907                           |                  1 |

At most 2.8 % of the windows differ, by one byte, so a phrase mean (dozens of windows) moves by
a fraction of a byte: presence can only change for a phrase sitting exactly on a cut. The GPU
analyses stand in for the server's.

## 5. Calibration of the presence cuts

`calibrate_stems.py` on the three sets: 3,370 phrases of 265 tracks. Output in
`out/st3/calibration/`.

### 5.1 Vocals: against the lyrics labels

A phrase has voice when the synced lyrics cover a quarter of it and none when they do not touch
it, as SG1b. The cut is chosen by balanced accuracy on four folds of tracks and measured on the
fifth.

| Rule                                                   | Precision | Recall | Specificity | Balanced accuracy |
| ------------------------------------------------------ | --------: | -----: | ----------: | ----------------: |
| Stem, 5-fold cross-validation                          |      0.91 |   0.90 |        0.74 |          **0.82** |
| Stem, the fixed cut (−45 dBFS, 17.9 dB), all 97 tracks |      0.92 |   0.88 |        0.78 |              0.83 |
| YAMNet byte ≥ 90 (SG1b), same phrases                  |      0.87 |   0.84 |        0.64 |          **0.74** |

- The folds pick drops of 17.9 or 20.9 dB and floors from −50 to −45.6 dBFS; the fit on every
  track is −47.5 dBFS and 17.9 dB.
- Per genre (fitted cut), the stem beats YAMNet on 14 of the 20 genres. It is weaker on
  post-punk (0.85 against 0.89), shoegaze (0.68 against 0.78), hip-hop (0.91 against 1.00) and
  post-hardcore and punk rock (0.96 against 1.00); both fail pop (one class only). Its largest
  gains are on the genres SG1b found weak for YAMNet: black metal 0.85 against 0.56, death metal
  0.87 against 0.53, industrial metal 0.79 against 0.67, synthpop 0.65 against 0.49, trip-hop
  0.83 against 0.69, and electronic 0.76 against 0.69.
- **The floor.** The labels come from sung songs and barely hold instrumentals, so they cannot
  see a floor that lets leakage through on an instrumental. Each floor, with its best drop on the
  labels, was also checked against YAMNet's stored byte on the electronic and phrase sets:

| Floor (dBFS) | Drop (dB) | Lyrics BA | Agreement with YAMNet (BA) | Tracks YAMNet hears as instrumental with stem vocals | Their voiced phrases |
| -----------: | --------: | --------: | -------------------------: | ---------------------------------------------------: | -------------------: |
|          −50 |      20.9 |     0.832 |                      0.729 |                                             35 of 66 |           181 of 557 |
|      **−45** |  **17.9** | **0.830** |                      0.739 |                                             30 of 66 |           130 of 557 |
|          −40 |      20.9 |     0.815 |                      0.770 |                                             24 of 66 |            78 of 557 |
|          −35 |      60.0 |     0.788 |                      0.759 |                                             17 of 66 |            48 of 557 |
|          −30 |      60.0 |     0.738 |                      0.707 |                                              9 of 66 |            26 of 557 |

The rule fixed before the final run was the highest floor within 0.01 balanced accuracy of the
best fit: **−45 dBFS with a drop of 17.9 dB** (bytes 64 and 76). −40 dBFS is the cautious
alternative: it agrees more with YAMNet (0.770) and voices six fewer of the tracks YAMNet hears
as instrumental, for 0.015 less balanced accuracy on the labels. YAMNet is the weaker detector
on the labels, so its agreement is a check, not a target.

### 5.2 Bass and drums: no labels

Nothing labels bass or drums per phrase, so the cut comes from the distribution of each phrase's
level below its track's loudest phrase. Two estimators were tried, each with a bootstrap over
tracks (200 draws) and per set, and both were also run on the lyrics tracks' vocal levels, where
the labels can judge them:

| Stem           | Valley, pooled | Valley, bootstrap p5 / p50 / p95 | Valley per set: electronic / lyrics / phrase |            Mixture cut (bootstrap p5–p95) |
| -------------- | -------------: | -------------------------------: | -------------------------------------------: | ----------------------------------------: |
| Bass           |         −14.25 |      −33.75 / **−17.25** / −9.75 |                     −33.75 / −14.25 / −15.75 |                       −4.9 (−6.6 to −4.2) |
| Drums          |         −14.25 |     −35.25 / **−14.25** / −14.25 |                     −14.25 / −14.25 / −14.25 |                       −4.8 (−5.7 to −4.2) |
| Vocals (check) |         −24.75 |          −24.75 / −12.75 / −0.75 |                                            — | −9.9 on the lyrics tracks (−12.0 to −8.0) |

- **Neither estimator recovers the labels on vocals:** the labels' best drop is 17.9 dB, the
  valley's median 12.75 dB and the mixture's 9.9 dB. The mixture is stable but sits at the edge
  of the loudest phrases (it models the tight cluster near 0 dB), so it was not used.
- **Drums:** the valley is the same in all three sets and in 95 % of the draws. **Drop 14 dB**
  (byte 60).
- **Bass:** the valley is unstable. The electronic set has a dip near −34 dB that the other sets
  do not, and the draws spread from 10 to 34 dB. The rule fixed beforehand, the bootstrap
  median, gives **17 dB** (byte 72). It is the least supported cut in this report; §5.3 shows
  how little the plans depend on it.
- **Floors** stay at −40 dBFS (byte 85): seven to nine of the 265 tracks have their loudest
  bass phrase under it, four or five their loudest drums phrase (3 dB histogram bins).
- **Phrase kinds** agree in direction, not as ground truth: at a drop of 18 dB, bass is present in
  84 % of body and 91 % of peak phrases against 44 % of breaks and 51 % of intros; at 15 dB,
  drums in 94 % of body and 95 % of peak phrases against 40 % of breaks.

### 5.3 Sensitivity of the plans to the cuts

`run_eval.py` (context `dj`) with one cut moved, against the plans of the calibrated cuts:

| Cut moved                                      | Electronic: plans that change (of 112) | Phrase set: plans that change (of 184) |
| ---------------------------------------------- | -------------------------------------: | -------------------------------------: |
| Bass drop 12 dB / 21 dB                        |                                 10 / 2 |                                  1 / 1 |
| Drums drop 11 dB / 17 dB                       |                                  1 / 0 |                                  1 / 0 |
| Vocal floor −50 dBFS / −40 dBFS (drop 20.9 dB) |                                  8 / 6 |                                23 / 28 |

The weakly supported bass and drums cuts barely move the plans; the vocal cut, which the labels
support, is the one that matters.

### 5.4 The vocal cut on held-out tracks, and where the two detectors disagree

The separation kept running after the cuts were fixed: 49 more lyrics tracks (453 labelled
phrases) never took part in choosing them.

| Labelled phrases              | Rule              | Precision | Recall | Specificity | Balanced accuracy |
| ----------------------------- | ----------------- | --------: | -----: | ----------: | ----------------: |
| Calibration (97 tracks, 980)  | Stem (fixed cut)  |      0.92 |   0.88 |        0.78 |          **0.83** |
|                               | YAMNet ≥ 90       |      0.87 |   0.84 |        0.64 |              0.74 |
|                               | Either says voice |      0.86 |   0.95 |        0.58 |              0.76 |
|                               | Both say voice    |      0.93 |   0.78 |        0.84 |              0.81 |
| **Held out (49 tracks, 453)** | Stem (fixed cut)  |      0.92 |   0.80 |        0.79 |          **0.79** |
|                               | YAMNet ≥ 90       |      0.90 |   0.75 |        0.74 |              0.74 |
|                               | Either says voice |      0.89 |   0.87 |        0.65 |              0.76 |
|                               | Both say voice    |      0.94 |   0.67 |        0.87 |              0.77 |

The stem holds up on unseen tracks, with a smaller lead (0.79 against 0.74). **Where the two
detectors disagree, the stem's mistakes are almost all misses:**

| Labelled phrases where they disagree | Stem says voice, YAMNet none: stem right | Stem says none, YAMNet voice: stem right |
| ------------------------------------ | ---------------------------------------: | ---------------------------------------: |
| Calibration (190)                    |                          75 of 91 (82 %) |                          52 of 99 (53 %) |
| Held out (92)                        |                          44 of 53 (83 %) |                          14 of 39 (36 %) |

So a phrase the stem calls voiced almost always is, but a phrase it calls free of voice while
YAMNet hears one is voiced about half the time or more. This is the case §6 measures as a rise
in YAMNet's vocal clash.

## 6. Results: profile metrics

`run_eval.py`, v3 without the layer (`smart-mix-v3-nostems`, the menus of rules revision 1)
against v3 with it, on the same profiles. Stem clashes use the calibrated presence; YAMNet's
vocal clash is the SM5 metric. Means over all edges.

**Electronic** (112 edges, 111 with layers on both; the layer changes 41 plans in `dj` and 35 in
`listen`):

| Metric (mean)                      | dj: v3 | dj: + stems |        Δ | listen: v3 | listen: + stems |        Δ |
| ---------------------------------- | -----: | ----------: | -------: | ---------: | --------------: | -------: |
| Policy violations                  |      0 |           0 |        0 |          0 |               0 |        0 |
| Start and end on a phrase boundary |  7.1 % |       7.1 % |        0 |      0.0 % |           0.0 % |        0 |
| Boundaries aligned (of 4)          | 52.2 % |      52.0 % |  −0.2 pp |     46.4 % |          46.4 % |        0 |
| **Stem bass clash, ms**            |  1,065 |         479 | **−586** |        595 |             324 | **−270** |
| Bass overlap by gain (SM5), ms     |  2,606 |       2,606 |        0 |      2,000 |           2,000 |        0 |
| **Stem vocal clash, ms**           |    356 |          48 | **−309** |        167 |               5 | **−162** |
| **YAMNet vocal clash (SM5), ms**   |     36 |         344 | **+308** |          4 |             147 | **+143** |
| Harmonic clash, ms                 |  2,127 |       2,127 |        0 |      1,000 |           1,000 |        0 |
| Energy step (0–1)                  | 0.0373 |      0.0416 |  +0.0043 |     0.0413 |          0.0448 |  +0.0035 |
| Incoming music before the mix, ms  | 16,595 |      17,367 |     +772 |     16,050 |          16,726 |     +676 |

**Phrase set** (184 edges, all with layers on both; the layer changes 94 plans in `dj` and 91 in
`listen`):

| Metric (mean)                      | dj: v3 | dj: + stems |          Δ | listen: v3 | listen: + stems |        Δ |
| ---------------------------------- | -----: | ----------: | ---------: | ---------: | --------------: | -------: |
| Policy violations                  |      0 |           0 |          0 |          0 |               0 |        0 |
| Start and end on a phrase boundary | 15.8 % |      12.5 % |    −3.3 pp |      0.0 % |           0.0 % |        0 |
| Boundaries aligned (of 4)          | 52.4 % |      48.2 % |    −4.2 pp |     38.9 % |          34.6 % |  −4.2 pp |
| **Stem bass clash, ms**            |  1,376 |         929 |   **−447** |      1,185 |             729 | **−456** |
| Bass overlap by gain (SM5), ms     |  2,152 |       2,152 |          0 |      2,000 |           2,000 |        0 |
| **Stem vocal clash, ms**           |  1,627 |         163 | **−1,464** |        457 |              14 | **−443** |
| **YAMNet vocal clash (SM5), ms**   |     70 |         521 |   **+451** |          0 |             141 | **+141** |
| Harmonic clash, ms                 |  4,470 |       4,068 |       −402 |      1,424 |           1,424 |        0 |
| Energy step (0–1)                  | 0.0476 |      0.0541 |    +0.0065 |     0.0441 |          0.0489 |  +0.0048 |
| Duration, ms                       | 14,144 |      13,755 |       −389 |      6,000 |           6,000 |        0 |
| Incoming music before the mix, ms  | 15,097 |      12,898 |     −2,199 |     13,261 |          10,505 |   −2,756 |

- **The layer does what it scores:** stem bass clash falls by 32–55 %, stem vocal clash by
  87–97 %, with no policy violation and no change in the gain-only bass overlap.
- **The two vocal detectors trade places.** v3 avoids YAMNet's voiced phrases and walks into the
  stem's; v3 with stems does the opposite. §5.4 says which side to believe: the stem's voiced
  calls are right 82–83 % of the time, but most of the new YAMNet clash comes from phrases the
  stem calls free of voice, where it is right only about half the time.
- **Costs:** energy continuity is a little worse on every run (+0.004 to +0.007), and phrase
  alignment loses 3–4 points on the phrase set. The bonuses and the bass component move the mix
  to phrases that score better on stems and worse on those terms.
- On the phrase set the mix starts earlier in the incoming track (−2.2 to −2.8 s): drums-only
  and vocal-free phrases sit near intros.

## 7. Results: automatic listening metrics

`listen_metrics.py`, the SM5 metrics (SM5 report §2.4) plus **bass on bass**: low end below
150 Hz of each rendered deck, with the crossfade and the bass handoff shelf applied, both decks
at full level while each plays within 12 dB of its own loud low end. It reads the audio, not the
stems, so it is an independent check of the bass term. **Voice on voice is YAMNet on each deck,
the detector the layer replaces** (§9), so it cannot referee the vocal term on its own.

Three arms per set and context: v3, v3 with the stems layer as committed, and a variant measured
for this report, "**either**": a phrase has voice when the vocal stem **or** YAMNet's stored byte
says so (§5.4), everything else as committed. The variant is a harness patch, not committed code.
Every render succeeded (0 failures on 1,776 plan measurements).

**Electronic**, means over all 112 edges (phase error and locked share over beatmatch edges):

| Metric                                    | dj: v3 | dj: + stems | dj: either | listen: v3 | listen: + stems | listen: either |
| ----------------------------------------- | -----: | ----------: | ---------: | ---------: | --------------: | -------------: |
| Beat clash at full level, ms              |  2,478 |       2,488 |      2,438 |      1,121 |           1,140 |          1,118 |
| Beat phase error, median ms               |     39 |          38 |         34 |         47 |              42 |             39 |
| Beats within 25 ms                        |   0.77 |        0.78 |       0.80 |       0.73 |            0.76 |           0.79 |
| Voice on voice at full level (YAMNet), ms |    274 |     **446** |        253 |        138 |             170 |            165 |
| Bass on bass at full level, ms            |  1,121 |     **946** |        995 |        838 |             792 |            808 |
| Loudness step across the mix, \|LU\|      |   2.79 |        2.83 |       2.93 |       3.12 |            3.17 |           3.33 |
| Loudness bump, LU                         |   1.13 |        0.96 |       1.02 |       0.83 |            0.77 |           0.78 |
| Loudness dip, LU                          |   7.81 |        7.95 |       7.85 |       5.80 |            5.93 |           5.81 |
| Steepest short-term change, LU/s          |   2.46 |        2.58 |       2.56 |       2.77 |            2.81 |           2.79 |
| Mix beat interval CV                      |  0.044 |       0.046 |      0.047 |      0.036 |           0.036 |          0.036 |

**Phrase set**, means over all 184 edges:

| Metric                                    | dj: v3 | dj: + stems | dj: either | listen: v3 | listen: + stems | listen: either |
| ----------------------------------------- | -----: | ----------: | ---------: | ---------: | --------------: | -------------: |
| Beat clash at full level, ms              |  3,954 |       3,731 |      3,666 |      1,468 |           1,408 |          1,401 |
| Beat phase error, median ms               |     60 |          64 |         60 |         61 |              58 |             60 |
| Beats within 25 ms                        |   0.44 |        0.42 |       0.43 |       0.45 |            0.49 |           0.50 |
| Voice on voice at full level (YAMNet), ms |    956 |   **1,074** |        931 |        280 |             247 |            208 |
| Bass on bass at full level, ms            |  1,513 |   **1,380** |      1,355 |      1,399 |           1,250 |          1,236 |
| Loudness step across the mix, \|LU\|      |   2.84 |        2.91 |       2.80 |       3.08 |            3.14 |           3.11 |
| Loudness bump, LU                         |   0.57 |        0.48 |       0.49 |       0.35 |            0.19 |           0.20 |
| Loudness dip, LU                          |   6.82 |    **7.64** |       7.67 |       4.83 |        **5.56** |           5.74 |
| Steepest short-term change, LU/s          |   2.31 |        2.52 |       2.49 |       2.70 |            2.96 |           2.89 |
| Mix beat interval CV                      |  0.076 |       0.073 |      0.074 |      0.068 |           0.071 |          0.070 |

Paired, changed edges only, better / worse beyond SM5's noise floor (480 ms for bass on bass, as
for voice); net = more of the metrics better than worse; two-sided sign test where p < 0.1:

| Comparison                       | Changed |       Beat clash |      Voice on voice |          Bass on bass | Loudness step |           Loudness dip |      Net, SM5's four |      Net, with bass | Clearly worse / better¹ |
| -------------------------------- | ------: | ---------------: | ------------------: | --------------------: | ------------: | ---------------------: | -------------------: | ------------------: | ----------------------: |
| Electronic dj: + stems vs v3     |      41 |          12 / 13 |              5 / 12 |   **14 / 3 (p 0.01)** |       13 / 13 |                14 / 18 |     11 / 18 (p 0.26) |    13 / 16 (p 0.71) |                 17 / 14 |
| Electronic dj: either vs v3      |      41 |          10 / 13 |               9 / 5 |   **14 / 4 (p 0.03)** |       12 / 16 |                15 / 16 |              14 / 14 |    17 / 13 (p 0.58) |                 17 / 16 |
| Electronic dj: either vs + stems |      15 |            4 / 5 | **11 / 1 (p 0.01)** |                 1 / 3 |         3 / 6 |                  7 / 7 |       7 / 2 (p 0.18) |      8 / 3 (p 0.23) |                   5 / 8 |
| Electronic listen: + stems vs v3 |      35 |            6 / 6 |               1 / 3 |                11 / 6 |       14 / 12 |                11 / 18 |      9 / 11 (p 0.82) |             10 / 10 |                  10 / 8 |
| Electronic listen: either vs v3  |      36 |            5 / 7 |               3 / 5 |                11 / 6 |       13 / 16 |                11 / 15 |     11 / 14 (p 0.69) |             11 / 12 |                  11 / 6 |
| **Phrase dj: + stems vs v3**     |      94 |          32 / 30 |             17 / 24 |       18 / 8 (p 0.08) |       31 / 32 |   **27 / 47 (p 0.03)** | **25 / 45 (p 0.02)** |    27 / 40 (p 0.14) |                 43 / 38 |
| Phrase dj: either vs v3          |      75 |          27 / 23 |             16 / 15 |   **16 / 4 (p 0.01)** |       28 / 22 | **18 / 42 (p < 0.01)** |     21 / 32 (p 0.17) |    23 / 25 (p 0.89) |                 31 / 30 |
| Phrase dj: either vs + stems     |      26 |            9 / 9 | **10 / 1 (p 0.01)** |                 5 / 1 |        11 / 5 |                 8 / 11 |      15 / 6 (p 0.08) | **16 / 4 (p 0.01)** |                 13 / 16 |
| Phrase listen: + stems vs v3     |      91 | 31 / 18 (p 0.09) |               9 / 9 | **29 / 8 (p < 0.01)** |       28 / 37 |       30 / 47 (p 0.07) |     27 / 33 (p 0.52) |    35 / 27 (p 0.37) |                 35 / 20 |
| Phrase listen: either vs v3      |      80 | 28 / 16 (p 0.10) |               9 / 3 | **27 / 5 (p < 0.01)** |       23 / 34 |   **23 / 44 (p 0.01)** |     24 / 28 (p 0.68) |    32 / 22 (p 0.22) |                 32 / 14 |

¹ Edges with at least one metric worse (better) by more than 2 s of beat clash, voice or bass on
bass, or 3 LU. On the phrase set in `dj`, 25 of the 43 clearly worse edges are loudness dips.

- **Bass on bass improves in every run**, significantly (p < 0.05) in five of the eight
  comparisons with v3:
  the bass term works on the audio, not just on the separated stems.
- **Voice on voice gets worse with the layer as committed** (electronic dj 274 → 446 ms; phrase
  dj 956 → 1,074 ms; 6 electronic edges clearly worse, none clearly better). Taking voice from
  either detector removes that regression (11 / 1 and 10 / 1 against the committed layer,
  p ≈ 0.01) and keeps the bass gain. YAMNet referees here, but §5.4 says the stem's misses are
  real about half the time or more, so this is not only YAMNet grading its own replacement.
- **The loudness dip is the regression that remains:** on the phrase set the mixes dip deeper
  (6.82 → 7.64 LU in `dj`, 4.83 → 5.56 LU in `listen`), with either vocal rule. It is not the two
  bonuses: with them at zero (either variant, `dj`) the phrase set still has 14 better / 32 worse
  dips (p 0.01) and the net against the either variant is 9 / 9. The bass and vocal terms move the
  mix into sparser phrases (the profile metrics show the same as a larger energy step and fewer
  aligned boundaries). Whether that dip is worse to a listener is what these metrics cannot say:
  an intro laid over an outro dips by design.
- **Beat clash, phase and loudness steps are unchanged** within noise; on the phrase set beat
  clash leans better (−223 ms in `dj`, 31 / 18 in `listen`).
- **Net:** with the layer as committed, the phrase set in `dj` (the Crate Decks context) **measures
  worse** on SM5's four metrics: 25 better / 45 worse edges, p 0.02. Counting bass on bass it is
  27 / 40 (p 0.14). Every other comparison, and every comparison of the either variant, is
  within noise on net.

## 8. Recommendation (input for STEM-G1)

Diego's position (STEM-D5) is to keep the layer unless it measures worse. **As committed, it
measures worse** in the case that matters most to Crate Decks: the phrase set in `dj`, 25 better
against 45 worse edges on SM5's metrics (p 0.02), from deeper loudness dips and more voice on
voice. Elsewhere it is neutral on net, and it reduces bass on bass everywhere.

1. **Do not turn on rules revision 2 as committed.**
2. **Preferred: a small follow-up, then on.** Make the planner's vocal presence "the stem or
   YAMNet" where the profile has YAMNet's byte (a rules revision 3, its golden and tests), then
   enable it. Measured here as the either variant, it keeps the bass gain, removes the voice
   regression and is within noise on net in all four runs. It is not better on SM5's four
   metrics; what it buys is less time with two basslines at full level.
3. **Before enabling it, listen to the dips.** The phrase set still dips deeper with either
   vocal rule and with no bonuses. A short check by Diego on a real setup of 10–15 of the edges
   whose dip grew most would tell whether that is an intro-over-outro blend (fine) or a hole in
   the mix (not fine). If it is a hole, the bass component's weight or the "no bassline counts
   as a handoff" rule is where to look.
4. If Diego prefers not to spend more on it now: **off by default** (`stems_layer = False` with
   a rules revision bump). The menu for Crate Decks (PR2) and the presence flags in the profile
   do not depend on the flag, so ST4–ST7 can go ahead either way.

Other points for the gate:

- **Vocal cut:** −45 dBFS / 17.9 dB (the labels' choice), or −40 dBFS / 20.9 dB, which voices six
  fewer of the tracks YAMNet hears as instrumental at 0.015 less balanced accuracy (§5.1).
- **Bass and drums cuts** have no ground truth (§5.2). The plans barely depend on them (§5.3);
  hand labels on a few dozen tracks would settle them if they ever matter.
- **STEM-D5's energy proxy** (drums plus bass around the cue) is not in the plan nor the code:
  implement it later or drop it from the design.

## 9. Deviations, mismatches and open questions

- **No `decks` context yet (S5).** PR2 adds `source: "decks"` and `context.localRefinement` as the
  request for the menu, standing in for S5's context and `features.local_refinement`
  (coordinator's decision). Until S5, `decks` plans like `playlist` and its bass handoff follows
  the Android flag. Android and VirtualDJ responses and cache keys are unchanged.
- **STEM-D5 lists a fourth signal the plan does not:** "drums plus bass as the energy proxy around
  the cue". ST3.PR1 names four terms and none of them is that, so it is not implemented. Open:
  add it in a later rules revision, or drop it from the design.
- **The calibrated cuts are a commit of their own** (`89db3f44`) after the harness, though they
  belong to PR1: calibrating them needs PR3's tools.
- **The calibration tool grew beyond the plan** (`9835f77b`: bootstrap, mixture cut, the YAMNet
  floor check), because the valley the plan's method relied on moved by 20 dB between subsets.
- **SM4's refinement worker builds its menu without the stems layer.** SM4 is off after SG3, so
  nothing serves it; if it returns, its menus will differ from the API's on edges with stems.
- **Profiles are v7.** The evaluation copy has no v8 profiles; both arms read the same v7
  profiles, so the comparison is paired.
- **Lyrics set:** 97 of the 275 SG1b tracks for calibration and 49 held out, in genre-balanced
  order; the rest were not separated, to keep the shared laptop's load down.
- **Separation on CUDA** instead of the server's CPU path, as the brief allows, with the parity
  check of §4.
- **No automatic referee for the vocal term.** `voiceOnVoiceMs` is YAMNet on the rendered decks,
  the detector the layer replaces; the lyrics labels (§5.1, §5.4) are the only independent
  evidence, and no evaluation track has them. `bassOnBassMs` does not read the stems and is the
  independent check of the bass term.
- **The bass and drums cuts have no ground truth** (§5.2); §5.3 shows the plans hardly depend on
  them.
- **The "either" variant of §7 is a harness patch** (`out/st3/tools/either_vocals.py`, with an
  option to zero the two bonuses), measured to inform this gate. It is not in the planner; making
  it the rule needs code, a rules revision and tests.
- **The branch has the layer on** (`PLANNER_POLICY_V3.stems_layer = True`, rules revision 2). If
  STEM-G1 turns it off, the flag goes to `False` and the rules revision is bumped again, since
  plans change.

## 10. Reproduce

Outputs live in the worktree `~/Code/Ninja/crate-agents/stems-st3/tools/smart-mix-eval/out/st3/`
(git-ignored): `stems/<set>/` (analyses; `lyrics-cal/` is the frozen calibration subset),
`calibration/`, `eval/<set>/<context>/{v3,v3-stems}/`, `variants/{either,either-nobonus}/`,
`sensitivity/`, `listen/<set>/listen-final.{json,md}` (the three arms of §7),
`listen/<set>/listen-nobonus.{json,md}` and `logs/`. Every tool ran in Docker against the SM5
evaluation copy, read-only.

```bash
# From tools/smart-mix-eval. Separation: the spike venv with onnxruntime-gpu and the CUDA wheels
# on LD_LIBRARY_PATH, CRATE_HTDEMUCS_ONNX pointing at htdemucs-onnx-1.onnx.
python separate_pool.py separate --tracks out/st3/sets/electronic.tsv \
  --output out/st3/stems/electronic --scratch out/st3/scratch \
  --music-root /music=<electronic audio> --provider cuda --jobs 2 --threads 4
python separate_pool.py parity --tracks out/st3/sets/parity.tsv --output out/st3/parity \
  --scratch out/st3/scratch --threads 8

# Calibration (image crate-ollama-test:local, SMART_MIX_EVAL_DSN at the evaluation copy).
python calibrate_stems.py --stems out/st3/stems/lyrics-cal --labels labels.csv \
  --yamnet phrase_windows.csv --stems out/st3/stems/electronic --stems out/st3/stems/spike \
  --dataset <electronic>/dataset.jsonl --groups <electronic>/groups.tsv \
  --dataset baselines/dev-2026-11-06-sm2/spike/dataset.jsonl --output out/st3/calibration

# Plans, per set and context (dj, listen).
python run_eval.py --dataset <set>/dataset.jsonl --planner smart-mix-v3-nostems \
  --context dj --stems out/st3/stems/<set> --output out/st3/eval/<set>/dj/v3
python run_eval.py --dataset <set>/dataset.jsonl --planner smart-mix-v3 --context dj \
  --stems out/st3/stems/<set> --output out/st3/eval/<set>/dj/v3-stems \
  --compare out/st3/eval/<set>/dj/v3/report.json

# Listening metrics (image crate-stems-analysis:pr2, TMPDIR on disk).
python listen_metrics.py --plans v3-dj=out/st3/eval/<set>/dj/v3/report.json \
  --plans v3+stems-dj=out/st3/eval/<set>/dj/v3-stems/report.json \
  --plans v3-listen=out/st3/eval/<set>/listen/v3/report.json \
  --plans v3+stems-listen=out/st3/eval/<set>/listen/v3-stems/report.json \
  --compare v3+stems-dj:v3-dj --compare v3+stems-listen:v3-listen \
  --music-root /music=<set audio> --output out/st3/listen/<set> --jobs 4
```

The sensitivity runs (§5.3), the held-out vocal check (§5.4) and the either variant (§7) are
small scripts on top of these tools, kept with the outputs (`out/st3/tools/`), not committed. The
variant adds `--plans either-dj=out/st3/variants/either/<set>/dj/report.json` (and `listen`) to the
listening run; the cache measures only the plans it has not seen.

## 11. STEM-G1: the final rule, measured

**Decision (Diego, 2026-11-09):** keep the stems layer on, by mix type, with voice from either
detector. Implemented as `smart-mix-v3` rules revision 3 in the monorepo branch
`feat/smart-mix-stems-st3`: `d0912cf1` (album genre read) and `94190340` (the rule).

### 11.1 The rule

- **Voice:** a phrase has voice for the planner when the vocal stem or YAMNet's stored byte says
  so (the "either" variant of §7, now planner code). The bonuses, the bass term and the cuts
  (vocals −45 dBFS / 17.9 dB, bass 17 dB, drums 14 dB) are unchanged.
- **Mix type:** the layer scores an edge only when **both tracks' genre beatmatch priors are known
  and at least 0.5** (`stems_layer_min_genre_prior`). The prior is the existing
  `transition_fit.genre_beatmatch_prior` of the track's album genres (taxonomy names with their
  weights, the source SM4 reads its genre from). Every other edge plans exactly as rules
  revision 1 (tests on the revision 1 golden with stems on both tracks).
- **Why 0.5, on each track:** in the prior table 0.5 is where the smooth genres end (rock, punk
  and metal 0.1–0.15, indie and ambient 0.2, pop and soul 0.4, trip-hop and IDM 0.45) and the
  beatmatch ones begin (dub and R&B 0.5, hip-hop and synthwave 0.6, dance 0.75, house, techno
  and drum and bass 0.95). The rule asks it of both tracks, not of their mean
  (`pair_genre_prior`), because a mean lets a dance track carry a punk one over the line:
  Carpenter Brut → Ho99o9 (0.95 and 0.39) averages 0.67.

| Rule (both priors known) | Electronic edges on (of 112) | Phrase-set edges on (of 184) |
| ------------------------ | ---------------------------: | ---------------------------: |
| Mean ≥ 0.6               |                          112 |                            3 |
| Mean ≥ 0.7               |                          101 |                            1 |
| Mean ≥ 0.75              |                           92 |                            1 |
| Mean ≥ 0.85              |                           52 |                            0 |
| **Each ≥ 0.5 (chosen)**  |                      **112** |                        **1** |

- **Unknown genres keep the layer off.** No profile signal was adopted: the electronic tracks all
  have album genres, 27 of the 64 phrase-set tracks have none and 11 more only genres the table
  does not know (alternative, industrial, minimal wave, new retro wave), and nothing measured on
  the profile was shown to separate the two sets. Per-artist genres could not be checked: the
  phrase set's artists are renamed in the development copy.
- **Industrial is the borderline.** "Industrial" alone has no prior, so Godflesh and Author &
  Punisher stay off; industrial metal and industrial rock read as metal and rock (0.1, 0.15),
  off. EBM and electro-industrial tagged electronic read 0.8 and are in, like Front 242 and
  Nitzer Ebb in the electronic set. On the phrase set that leaves one edge in: GosT → 3TEETH
  (darksynth into electro-industrial, both tagged electronic).
- **The cache key** carries the stem analyses only for edges the gate passes, so a genre that
  moves an edge into or out of the family changes its key; the album genres are read (one
  bounded query) only for edges with stems on both tracks. v2, Android and VirtualDJ plans and
  keys are unchanged.

### 11.2 Edges that get the layer

| Set        | Edges | With stems on both tracks | In the family | Planned with the layer | Plans that differ from v3 (`dj` / `listen`) |
| ---------- | ----: | ------------------------: | ------------: | ---------------------: | ------------------------------------------: |
| Electronic |   112 |                       111 |           111 |              111 / 110 |                                     41 / 36 |
| Phrase set |   184 |                       184 |             1 |                  1 / 1 |                                       1 / 1 |

As expected, the electronic plans are exactly the either variant's (0 differences in either
context), and the phrase set equals v3 except GosT → 3TEETH.

### 11.3 Profile metrics, v3 against the final rule

| Metric (mean)                      | Electronic dj: v3 |   final | Electronic listen: v3 |   final | Phrase dj: v3 |  final | Phrase listen: v3 |  final |
| ---------------------------------- | ----------------: | ------: | --------------------: | ------: | ------------: | -----: | ----------------: | -----: |
| Policy violations                  |                 0 |       0 |                     0 |       0 |             0 |      0 |                 0 |      0 |
| Start and end on a phrase boundary |             7.1 % |   7.1 % |                 0.0 % |   0.0 % |        15.8 % | 15.8 % |             0.0 % |  0.0 % |
| Boundaries aligned (of 4)          |            52.2 % |  50.7 % |                46.4 % |  45.5 % |        52.4 % | 52.4 % |            38.9 % | 38.9 % |
| Stem bass clash, ms                |             1,065 | **623** |                   595 | **396** |         1,376 |  1,376 |             1,185 |  1,185 |
| Bass overlap by gain (SM5), ms     |             2,606 |   2,606 |                 2,000 |   2,000 |         2,152 |  2,152 |             2,000 |  2,000 |
| Stem vocal clash, ms               |               356 |      48 |                   167 |       5 |         1,627 |  1,627 |               457 |    457 |
| YAMNet vocal clash (SM5), ms       |                36 |  **36** |                     4 |   **4** |            70 |     70 |                 0 |      0 |
| Harmonic clash, ms                 |             2,127 |   2,127 |                 1,000 |   1,000 |         4,470 |  4,470 |             1,424 |  1,424 |
| Energy step (0–1)                  |            0.0373 |  0.0479 |                0.0413 |  0.0464 |        0.0476 | 0.0478 |            0.0441 | 0.0443 |

### 11.4 Listening metrics, v3 against the final rule

Electronic, means over all 112 edges (phase error and locked share over beatmatch edges):

| Metric                                    | dj: v3 | dj: final | listen: v3 | listen: final |
| ----------------------------------------- | -----: | --------: | ---------: | ------------: |
| Beat clash at full level, ms              |  2,478 |     2,438 |      1,121 |         1,118 |
| Beat phase error, median ms               |     39 |        34 |         47 |            39 |
| Beats within 25 ms                        |   0.77 |      0.80 |       0.73 |          0.79 |
| Voice on voice at full level (YAMNet), ms |    274 |       253 |        138 |           165 |
| Bass on bass at full level, ms            |  1,121 |   **995** |        838 |           808 |
| Loudness step across the mix, \|LU\|      |   2.79 |      2.93 |       3.12 |          3.33 |
| Loudness bump, LU                         |   1.13 |      1.02 |       0.83 |          0.78 |
| Loudness dip, LU                          |   7.81 |      7.85 |       5.80 |          5.81 |
| Steepest short-term change, LU/s          |   2.46 |      2.56 |       2.77 |          2.80 |
| Mix beat interval CV                      |  0.044 |     0.047 |      0.036 |         0.036 |

Paired, changed edges only (better / worse beyond the noise floor):

| Comparison        | Changed | Beat clash | Voice on voice |        Bass on bass | Loudness step | Loudness dip |  Net, SM5's four |   Net, with bass | Clearly worse / better |
| ----------------- | ------: | ---------: | -------------: | ------------------: | ------------: | -----------: | ---------------: | ---------------: | ---------------------: |
| Electronic dj     |      41 |    10 / 13 |          9 / 5 | **14 / 4 (p 0.03)** |       12 / 16 |      15 / 16 |          14 / 14 | 17 / 13 (p 0.58) |                17 / 16 |
| Electronic listen |      36 |      5 / 7 |          3 / 5 |              11 / 6 |       13 / 16 |      11 / 15 | 11 / 14 (p 0.69) |          11 / 12 |                 11 / 6 |
| Phrase dj         |       1 |      1 / 0 |          0 / 0 |               0 / 0 |         1 / 0 |        0 / 1 |            1 / 0 |            1 / 0 |                  1 / 1 |
| Phrase listen     |       1 |      0 / 0 |          0 / 0 |               0 / 0 |         1 / 0 |        0 / 1 |            0 / 0 |            0 / 0 |                  1 / 1 |

- **Electronic:** the final rule keeps bass on bass lower (significant in `dj`) and voice on voice
  where v3 had it, and is within noise on net in both contexts. Energy continuity and boundary
  alignment cost a little more than with the committed layer (energy step +0.011 in `dj`).
- **Phrase set:** only GosT → 3TEETH changes. Its loudness step improves by 6.5 LU and its beat
  clash by 0.96 s in `dj`, but **its loudness dip grows by 17.6 LU in `dj` and 13.1 LU in
  `listen`**: the layer moves the outgoing cue from 2:38 into GosT's outro at 3:07. It heads the
  list prepared for Diego's listening
  (`~/Code/Ninja/crate-agents/handoffs/2026-11-09/stem-g1-listen-dips.md`), with the 14 rock
  and post-punk edges whose dip grew most under the committed layer, input for a future
  revision.

### 11.5 Open

- STEM-D5's energy proxy (drums plus bass around the cue) stays an open question.
- The loudness dips (§7, §11.4): whether a deeper dip is an intro over an outro or a hole, which
  Diego's listening of the prepared edges can tell.
- The gate's genre source is the album taxonomy. Per-artist genres, which radio uses, could not
  be measured on the phrase set; tracks without album genres stay off.

## 12. Analyzer v8 and machine-drum industrial (rules revision 4)

**Request (Diego, 2026-11-09, after STEM-G1):** "Godflesh, Ministry, Fear Factory, Author and
Punisher deberían poder mezclarse bien, tienen baterías/baterías electrónicas marcadas." Rules
revision 3 kept them out. Their genre priors read as metal (0.1), "industrial" has no prior,
Fear Factory's albums split industrial metal with death or groove metal, and Author & Punisher's
albums have no genres. Rules revision 4 (`c9e86ff9`) adds a family of the gate's own, and this
section measures it on the analyzer production runs (v8).

### 12.1 The rule

- A track's gate prior is 1.0 when its **top artist genre or top album genre** is of the
  machine-drum industrial family: industrial, industrial metal and rock, EBM, electro-industrial,
  aggrotech, dark electro, Neue Deutsche Härte. Otherwise it is its album genres' beatmatch
  prior, as in revision 3. Both tracks still need 0.5.
- The plan batch reads album and artist genres in one bounded query. The **shared prior table
  does not change**: compatible tracks and radio keep their priors.
- **Why genres and not a steadiness signal.** On the development library (136,512 tracks, v5
  profiles, read-only), the share of tracks with tempo stability ≥ 0.9 ranks the families as
  expected, but it does not separate tracks. Per artist, Godflesh (0.87) and Ministry (0.88)
  are as steady as EBM, while Fear Factory (0.64) and Author & Punisher (0.47) would stay out
  and The Clash (0.70) or pop-punk (0.76) would come in. Per artist the genres are clean: the
  four artists' top artist genre is industrial metal or industrial.

| Artist's top genre (development library) | Tracks | Revision 3 | Revision 4 | Tempo stability ≥ 0.9 |
| ---------------------------------------- | -----: | ---------: | ---------: | --------------------: |
| Industrial family                        |  5,967 |     34.4 % |      100 % |                  75 % |
| Electronic dance                         | 14,366 |     76.8 % |     76.8 % |                  80 % |
| Extreme metal                            | 13,658 |      0.0 % |      0.5 % |                  32 % |
| Other metal                              |  9,411 |      0.0 % |      0.0 % |                  52 % |
| Punk and hardcore                        | 33,298 |      0.3 % |      1.0 % |                  56 % |
| Rock                                     | 32,088 |      0.6 % |      1.1 % |                  57 % |

What gets in from extreme metal, punk and rock are albums tagged industrial: Killing Joke, Code
Orange, Voivod, Blut Aus Nord (drum-machine black metal), Morbid Angel's industrial album, Gary
Numan, Bowie's _Outside_, HEALTH.

### 12.2 Data: four sets on analyzer v8

Prepared by a separate agent (`handoffs/2026-11-09/eval-v8-data.md`): v8 profiles from
production's own analysis worker image (`3965fa86`) in a copy of the evaluation database
(`crate_eval_v8`; v7 untouched), stems for every track, artist genres copied for the real
artists of the phrase set.

| Set                                | Tracks | Edges | Edges with phrases on both | Stems on both | In the family | Plans changed (`dj` / `listen`) |
| ---------------------------------- | -----: | ----: | -------------------------: | ------------: | ------------: | ------------------------------: |
| Electronic                         |    120 |   112 |                        112 |           111 |           111 |                         36 / 34 |
| Phrase set                         |     64 |   184 |          **150** (v7: 184) |           150 |             3 |                           3 / 3 |
| Industrial                         |     71 |    60 |                         60 |            60 |            60 |                         31 / 32 |
| Metal and punk (12 subgenres × 12) |    144 |   125 |                        124 |           122 |         **0** |                       **0 / 0** |

- **Industrial set:** Godflesh 8, Ministry 8, Fear Factory 8, Author & Punisher 8 (machine-drum
  eras), the family (3TEETH, Black Magnet, Static-X, Nine Inch Nails, Skinny Puppy, Front Line
  Assembly, Front 242, Nitzer Ebb, Youth Code, Combichrist, Revolting Cocks) and six live-drum
  controls (Swans, Einstürzende Neubauten, Strapping Young Lad).
- **Metal and punk:** hardcore, crust/d-beat, thrash, death, black, doom/sludge, grind,
  metalcore, pop-punk, ska-punk, punk rock/Oi! and post-hardcore, 12 tracks each.
- On the phrase set the family is now NIN ↔ Fear Factory (two edges, through the copied artist
  genres) and GosT → 3TEETH; the Godflesh edges of §11 lost their phrases on v8.

### 12.3 The gate on v8

- **The four artists are in:** every track of theirs in an edge passes (Godflesh 7, Author &
  Punisher 6, Fear Factory 5, Ministry 3), and so does the rest of the family.
- **Metal and punk are out:** 0 of 125 tracks, so all 125 edges plan byte for byte as v3 in
  both contexts (three control pairs rendered for §12.6 are bit-identical).
- **The live-drum controls are in, and too few to say more.** Only two of the six reach edges
  (four edges): Einstürzende Neubauten «Feurio! (Turen offen)» and Strapping Young Lad
  «Monument». Their top artist genres are industrial and industrial metal, so the gate lets
  them in, and three of the four edges change. A steadiness signal would not have kept them
  out either: on v8 their tempo stability is 1.0 and 0.999 (the machine and marked tracks have a
  median of 0.998 and 1.0). Telling a live drummer from a machine needs more than six controls
  and a signal this data does not have.

### 12.4 Profile metrics, v3 against revision 4 (v8)

**Electronic**

| Metric (mean)                      | dj: v3 | dj: final | listen: v3 | listen: final |
| ---------------------------------- | -----: | --------: | ---------: | ------------: |
| Policy violations                  |    0 % |       0 % |        0 % |           0 % |
| Start and end on a phrase boundary | 30.4 % |    30.4 % |      0.0 % |         0.0 % |
| Boundaries aligned (of 4)          | 65.0 % |    63.2 % |     45.5 % |        43.8 % |
| Stem bass clash, ms                |    685 |       408 |        577 |           396 |
| Bass overlap by gain (SM5), ms     |  2,107 |     2,107 |      2,000 |         2,000 |
| Stem vocal clash, ms               |    569 |        48 |        167 |             5 |
| YAMNet vocal clash (SM5), ms       |     71 |        71 |          5 |             5 |
| Harmonic clash, ms                 |  3,463 |     3,245 |      1,000 |         1,000 |
| Energy step (0–1)                  | 0.0354 |    0.0427 |     0.0407 |        0.0458 |

**Phrase set**

| Metric (mean)                      | dj: v3 | dj: final | listen: v3 | listen: final |
| ---------------------------------- | -----: | --------: | ---------: | ------------: |
| Policy violations                  |    0 % |       0 % |        0 % |           0 % |
| Start and end on a phrase boundary | 15.8 % |    15.8 % |      0.0 % |         0.0 % |
| Boundaries aligned (of 4)          | 48.8 % |    48.5 % |     35.9 % |        35.7 % |
| Stem bass clash, ms                |  1,820 |     1,793 |      1,147 |         1,120 |
| Bass overlap by gain (SM5), ms     |  2,625 |     2,625 |      2,000 |         2,000 |
| Stem vocal clash, ms               |  2,221 |     2,178 |        507 |           493 |
| YAMNet vocal clash (SM5), ms       |    104 |       104 |          0 |             0 |
| Harmonic clash, ms                 |  4,622 |     4,622 |      1,424 |         1,424 |
| Energy step (0–1)                  | 0.0495 |    0.0498 |     0.0508 |        0.0522 |

**Industrial**

| Metric (mean)                      | dj: v3 | dj: final | listen: v3 | listen: final |
| ---------------------------------- | -----: | --------: | ---------: | ------------: |
| Policy violations                  |    0 % |       0 % |        0 % |           0 % |
| Start and end on a phrase boundary | 51.7 % |    43.3 % |      0.0 % |         0.0 % |
| Boundaries aligned (of 4)          | 75.0 % |    68.3 % |     45.8 % |        39.6 % |
| Stem bass clash, ms                |    667 |       119 |      1,133 |           367 |
| Bass overlap by gain (SM5), ms     |  1,400 |     1,400 |      2,000 |         2,000 |
| Stem vocal clash, ms               |  1,678 |       252 |        367 |             0 |
| YAMNet vocal clash (SM5), ms       |      0 |         0 |          0 |             1 |
| Harmonic clash, ms                 |  5,974 |     5,246 |      1,333 |         1,333 |
| Energy step (0–1)                  | 0.0451 |    0.0629 |     0.0424 |        0.0679 |

**Metal/punk**

| Metric (mean)                      | dj: v3 | dj: final | listen: v3 | listen: final |
| ---------------------------------- | -----: | --------: | ---------: | ------------: |
| Policy violations                  |    0 % |       0 % |        0 % |           0 % |
| Start and end on a phrase boundary |  1.6 % |     1.6 % |      0.0 % |         0.0 % |
| Boundaries aligned (of 4)          | 44.8 % |    44.8 % |     40.0 % |        40.0 % |
| Stem bass clash, ms                |  1,916 |     1,916 |      1,120 |         1,120 |
| Bass overlap by gain (SM5), ms     |  3,028 |     3,028 |      2,000 |         2,000 |
| Stem vocal clash, ms               |    345 |       345 |        135 |           135 |
| YAMNet vocal clash (SM5), ms       |    258 |       258 |         37 |            37 |
| Harmonic clash, ms                 |  2,635 |     2,635 |      1,328 |         1,328 |
| Energy step (0–1)                  | 0.0462 |    0.0462 |     0.0489 |        0.0489 |

- **Industrial:** stem bass clash falls by 82 % in `dj` (667 → 119 ms) and stem vocal clash by
  85 %, harmonic clash by 0.7 s; energy continuity is worse (0.045 → 0.063) and phrase
  alignment loses 8 points, more than on electronic music.
- **Electronic** on v8 behaves as on v7 (§11.3). **Metal and punk** are identical to v3.

### 12.5 Listening metrics, v3 against revision 4 (v8, changed edges)

Measured only on the edges whose plan changes (the rest are identical to v3 by construction).

**Industrial:**

| Metric (mean over changed edges) | dj: v3 | dj: rev. 4 | listen: v3 | listen: rev. 4 |
| -------------------------------- | -----: | ---------: | ---------: | -------------: |
| Beat clash at full level, ms     |  2,771 |      2,276 |        792 |            963 |
| Voice on voice (YAMNet), ms      |    948 |        272 |        271 |            131 |
| Bass on bass, ms                 |  1,000 |        602 |      1,341 |            837 |
| Loudness step, \|LU\|            |   2.37 |       2.95 |       3.31 |           4.18 |
| Loudness dip, LU                 |   6.08 |       7.30 |       4.53 |           5.47 |
| Beats within 25 ms (beatmatch)   |   0.82 |       0.82 |       0.83 |           0.71 |

**Electronic:**

| Metric (mean over changed edges) | dj: v3 | dj: rev. 4 | listen: v3 | listen: rev. 4 |
| -------------------------------- | -----: | ---------: | ---------: | -------------: |
| Beat clash at full level, ms     |  3,142 |      2,975 |      1,355 |          1,307 |
| Voice on voice (YAMNet), ms      |    716 |        522 |        177 |            255 |
| Bass on bass, ms                 |  1,218 |        916 |        774 |            697 |
| Loudness step, \|LU\|            |   3.13 |       4.18 |       2.80 |           3.68 |
| Loudness dip, LU                 |   7.91 |       7.50 |       6.28 |           6.21 |
| Beats within 25 ms (beatmatch)   |   0.67 |       0.74 |       0.60 |           0.69 |

**Phrase set:**

| Metric (mean over changed edges) | dj: v3 | dj: rev. 4 | listen: v3 | listen: rev. 4 |
| -------------------------------- | -----: | ---------: | ---------: | -------------: |
| Beat clash at full level, ms     |  4,973 |      2,500 |      1,447 |            667 |
| Voice on voice (YAMNet), ms      |    640 |        480 |        377 |            320 |
| Bass on bass, ms                 |  1,090 |        560 |      1,870 |            963 |
| Loudness step, \|LU\|            |   4.90 |       3.48 |       4.77 |           3.67 |
| Loudness dip, LU                 |   4.43 |       5.63 |       2.23 |          10.30 |
| Beats within 25 ms (beatmatch)   |   0.49 |       0.49 |       0.29 |           0.96 |

Paired, changed edges (better / worse beyond the noise floor; sign test where p < 0.1):

| Set, context         | Changed | Beat clash |  Voice on voice |      Bass on bass |   Loudness step |    Loudness dip |   Net, SM5's four | Net, with bass | Clearly worse / better |
| -------------------- | ------: | ---------: | --------------: | ----------------: | --------------: | --------------: | ----------------: | -------------: | ---------------------: |
| Industrial, `dj`     |      31 |      7 / 5 | 10 / 1 (p 0.01) |    6 / 0 (p 0.03) |          7 / 13 |          9 / 18 |           12 / 12 |        15 / 12 |                11 / 11 |
| Industrial, `listen` |      32 |      4 / 9 |           4 / 0 | 14 / 1 (p < 0.01) |          9 / 17 | 6 / 21 (p 0.01) | 4 / 20 (p < 0.01) |         7 / 15 |                 12 / 7 |
| Electronic, `dj`     |      36 |     13 / 9 |          10 / 5 |    9 / 2 (p 0.07) | 7 / 16 (p 0.09) |         16 / 14 |           15 / 11 |        16 / 10 |                17 / 15 |
| Electronic, `listen` |      34 |      6 / 5 |           2 / 4 |            10 / 6 |         11 / 17 |         10 / 13 |           10 / 13 |        11 / 12 |                 10 / 5 |
| Phrase set, `dj`     |       3 |      3 / 0 |           1 / 0 |             1 / 0 |           1 / 1 |           1 / 1 |             3 / 0 |          3 / 0 |                  1 / 2 |
| Phrase set, `listen` |       3 |      2 / 0 |           0 / 0 |             2 / 0 |           1 / 2 |           1 / 2 |             1 / 1 |          2 / 0 |                  2 / 1 |

- **Industrial, `dj` (the Crate Decks context):** the layer does what Diego asked for, without
  measuring worse: voice on voice falls on 10 edges and rises on 1 (p 0.01), bass on bass falls on
  6 and rises on none (p 0.03), net 12 / 12 on SM5's four metrics and 15 / 12 with bass. The cost
  is loudness: steps worse on 13 edges against 7, dips on 18 against 9, and in the profile metrics
  energy continuity and phrase alignment.
- **Industrial, `listen`** (6 s, no bass handoff) **measures worse:** 4 better against 20 worse on
  SM5's four (p < 0.01), from loudness dips (6 / 21, p 0.01) and steps (9 / 17); bass on bass
  still improves (14 / 1). The cause is not measured; Listen's 6 s blend without a bass handoff
  is where the dips concentrate on the earlier sets too (§7).
- **Electronic** on v8 matches v7 (§11.4): within noise on net (15 / 11 in `dj`, 10 / 13 in
  `listen`), bass on bass lower (9 / 2 in `dj`), loudness steps leaning worse (7 / 16, p 0.09).
- **Phrase set:** three edges; better on net in `dj` (3 / 0). GosT → 3TEETH still dips deeply in
  `listen` (+17.7 LU).
- **Metal and punk:** no plan changes, so nothing to measure.

### 12.6 Listening playlist

`~/Code/Ninja/crate-agents/listening/stem-g1-v8/` (`dj` context, A v3, B revision 4, same render
chain and gain, `playlist.m3u8`, README in Spanish): 47 pairs, only edges whose plan changes
except the controls. The 31 industrial edges come first, those with Godflesh, Ministry, Fear
Factory or Author & Punisher at the head, the three live-drum control edges marked; then 10
electronic edges (the five that improve most and the five that worsen most), the three phrase-set
edges, and three metal and punk controls (black, hardcore, ska-punk) where A and B are the same
file bit for bit.

### 12.7 Open

- **Analyzer v8 leaves 45 of the 399 tracks without phrases** (no downbeat anchor, structure
  confidence 0), steady ones included (Ministry «Just One Fix», stability 0.999): 11
  electronic, 5 of the phrase set that had phrases on v7, 17 industrial (Ministry 5, Fear
  Factory 3), 12 metal and punk. On the phrase set **34 of the 184 edges lose their phrases**,
  so the menu falls back to v2's bar heuristic and the stems layer cannot act. This is the
  analyzer production runs and deserves its own look outside ST3. `analyze_pool.py` also counts
  a profile with an empty phrase list as ready.
- **Live drums inside the industrial family** (§12.3): the gate cannot tell them apart; a larger
  control set, or a per-track drum signal, would be needed.
- The loudness dips (§7, §11.4, §12.5) and STEM-D5's energy proxy stay open.

## 13. Stems for Crate Decks only (rules revision 5)

**Decision (Diego, 2026-11-09):** "los stems que vayan solo si el cliente es Decks". The reason is
§12.5: on the industrial set, Listen's context (6 s, no bass handoff) measured worse with the layer
(4 better against 20 worse edges on SM5's four metrics, from loudness dips), while the `dj`
context Crate Decks uses did not.

- **The rule:** the stems layer scores an edge only when the request comes from Crate Decks
  (`context.source = "decks"`, the source of PR2's menu contract; `stems_layer_sources`) **and**
  revision 4's mix-family gate passes. Listen, Android playlist and radio, VirtualDJ and every
  other source get the v3 menu without stems, byte for byte rules revision 1's (tests on the
  revision 1 golden with stems on both tracks and in-family genres).
- **The cache key** already carries the context's source (the whole context but a few ignored
  fields), so it needs no new field; it carries the stem analyses only when the gate, client
  included, passes. The batch reads genres only for Crate Decks edges with stems on both tracks,
  so other requests make no extra query. v2, Android and VirtualDJ responses and keys do not
  change.
- **The harness** now asks as Decks does in its `dj` context (`source: "decks"`; it was
  `playlist`), so `dj` measures what Decks gets. Spot check on the four v8 sets with revision 5:

| Set                    | `dj` plans that differ from revision 4 | `listen` plans that differ from v3 | Edges planned with stems in `listen` |
| ---------------------- | -------------------------------------: | ---------------------------------: | -----------------------------------: |
| Electronic (112 edges) |                                      0 |                                  0 |                                    0 |
| Phrase set (184)       |                                      0 |                                  0 |                                    0 |
| Industrial (60)        |                                      0 |                                  0 |                                    0 |
| Metal and punk (125)   |                                      0 |                                  0 |                                    0 |

The `dj` re-measure on the four sets gives profile metrics identical to revision 4's, and the
listening metrics depend only on the plans, so §12.4–§12.5 stand for revision 5 in `dj`. In
`listen` every v3 plan is v3's own: the regression of §12.5 is gone by construction.

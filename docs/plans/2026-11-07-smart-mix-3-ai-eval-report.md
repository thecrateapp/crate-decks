# Smart Mix 3 · SM5 AI evaluation report

> **Revision 2 (2026-11-07). Input for Diego's SG3 decision, which after revision 1 was "need
> more data". This report does not decide SG3.** Revision 2 fixes the SM4 bass handoff defect,
> tests one guidance change, adds an electronic edge set and reruns Gemini 3.8 Flash on both
> sets. Revision 1 follows unchanged after the revision 2 sections.

- **Date:** 2026-11-07 (revision 1 and revision 2)
- **Code:** monorepo branch `feat/sm5-ai-eval` from `origin/main` `d0316e5a`, local commits only.
  Revision 2 adds `8972fcd1` (bass fix, `sm3-refine-2`), `5259974a` and `9875e91f` (pool
  analysis and grouped pairing), `4f88f89b` (guidance experiment, `sm3-refine-3`) and
  `3ace93c8` (docs).
- **Plan and design:** SM5 in `2026-11-02-smart-mix-3-implementation-plan.md`; metrics and
  criteria in §10 of `2026-11-02-smart-mix-3-design.md` (SM3-D12).
- **Changes to the plan decided by Diego:** Gemini only, and for revision 2 3.8 Flash only;
  automatic listening metrics instead of the 30 blind pairs; an electronic set built from
  production tracks copied read-only by the coordinator with Diego's approval.

## R2.1 Summary of revision 2

- **The bass handoff defect is fixed.** Every candidate now lists the `bass_handoff` answers it
  accepts (`bassOk`), from the same rule the validation enforces, which stays strict. Flash's
  rejection rate on the phrase set falls from **43.5 % to 1.6 %**; on the electronic set it is
  **0 %**. The rejection criterion now passes.
- **The guidance experiment (`sm3-refine-3`) helps the profile metrics but not the audio.**
  Saying that the score already weighs alignment, energy, key and voice, and dropping "avoid
  vocals over vocals" and "mix outros into intros", halves the changes (24 instead of 46 on the
  phrase set) and the vocal clash criterion passes. Phrase alignment and energy continuity still
  fail on the phrase set, and energy continuity fails on the electronic set with both prompts.
- **The electronic set does not change the picture.** On 112 house, techno, drum and bass and
  other dance edges, v3 is clearly better than v2 on the audio (59 edges net better, 28 net
  worse, p = 0.001), while Flash's changes make the audio **worse** with `sm3-refine-2` (5 net
  better, 16 net worse, p = 0.03) and lean worse with `sm3-refine-3` (2 and 8, p = 0.11), mostly
  through bigger loudness steps.
- **Recommendation for SG3 after revision 2: no-go for enabling AI refinement.** With the defect
  fixed and the guidance tightened, Flash agrees with v3 on 87 % of edges and its remaining
  changes do not improve any automatic measurement on either set. If the line is kept for later
  (another model, set history, a listening check), keep `sm3-refine-3` rather than
  `sm3-refine-2` (§R2.6).
- **Side finding about the analyzer, not the AI:** 87 of the 120 electronic tracks get a
  `partial` v7 profile (mean BPM confidence 0.77, tempo stability 0.71), so v2 and v3 refuse to
  beatmatch 73 of the 112 electronic edges (`low_confidence` 46, `unstable_tempo` 24). Where
  they do beatmatch, the decks' beats agree well (the per-edge phase error has a median of 20 ms;
  30 of 38 edges within 25 ms), unlike the live-drum phrase set (40 ms).
- **Cost of revision 2:** 1.01 USD of the 2 USD allowed (592 Flash calls); total of the
  evaluation 2.66 USD.

## R2.2 What changed in the code

- **Fix (`8972fcd1`, `sm3-refine-2`).** `crate.smart_mix.refinement.allowed_bass_handoffs`
  (`keep` and `none` always; `balanced` only on a beatmatch candidate in a context that allows
  the handoff) is the single rule: the prompt lists it per candidate as `bassOk`, and
  `apply_refinement` rejects anything outside it. Tests first: the prompt lists `["keep",
"none"]` for every crossfade candidate (it failed before), the replayed revision-1 answer
  (`balanced` on a crossfade) is still rejected, and for every candidate of the planner's edge
  cases the values the prompt offers are exactly the values the validation accepts.
  `PROMPT_VERSION` became `sm3-refine-2`; capabilities serve it, the capability contract tests
  and `docs/technical/smart-mix-operations.md` follow.
- **Guidance experiment (`4f88f89b`, `sm3-refine-3`), its own commit so it can be dropped.**
  "Guidance: the score already weighs phrase alignment, energy continuity, key, stability, bass
  and vocal overlap, so candidate 0 is the best by those. Choose another only for what the score
  cannot see: genre conventions (…) and the history's energy arc." The genre conventions are
  kept word for word.
- **Input budget.** `bassOk` adds about 300 bytes on the longest synthetic edge, which takes the
  runtime's byte-count admission bound to about 6,300, past the 6,000 the SM4 budget test
  allowed; the test now allows 6,500. Gemini counted about 1,800 real input tokens per
  transition (1,694 before). Design §8 says 3–6 K tokens; this is a deviation to confirm.

## R2.3 The electronic set

- **Tracks:** 120 production tracks copied read-only by the coordinator into the worktree's
  `out/electronic/audio/` (git-ignored, never committed), 30 per bucket: `house` (house, deep,
  acid), `techno` (techno, minimal, dub techno), `drum-and-bass` (drum and bass, jungle),
  `other-dance` (dubstep, electro, breakbeat, trance, dance). Their library rows were already in
  `sm5-eval-postgres` (copied from dev), with v5 profiles.
- **Profiles:** v7 computed locally by `analyze_pool.py` with the worker's own
  `compute_smart_mix_profile` handler in `crate-smb-analysis-worker:final` (Beat This! with its
  DBN, YAMNet, `crate-cli`), written only to `sm5-eval-postgres`. All 120 ended ready (phrases,
  current, usable): 33 `full`, 87 `partial`.
- **Edges (`build_pool_dataset.py --groups`):** each track's best partner by
  `score_compatibility` inside its bucket with a ΔBPM band, the 25 best per bucket, plus the 12
  best pairs across buckets (`cross`, no track used twice on a side): **112 edges**, 50 / 41 / 21
  in ΔBPM 0–2 / 2–4 / 4–6 %. The dataset lives in `out/electronic/dataset/` and is not committed.
- **Modes:** v3 beatmatches 39 edges and crossfades 73 (`low_confidence` 46, `unstable_tempo` 24,
  `incompatible_tempo` 3), the same split as v2. The menu still has eight candidates per edge.

## R2.4 Results: what Flash did

|                                  | Phrase set, rev. 1 (`sm3-refine-1`) |       Phrase set, `sm3-refine-2` |         Phrase set, `sm3-refine-3` | Electronic, `sm3-refine-2` |         Electronic, `sm3-refine-3` |
| -------------------------------- | ----------------------------------: | -------------------------------: | ---------------------------------: | -------------------------: | ---------------------------------: |
| Edges / calls                    |                           184 / 184 |                        184 / 184 |                          184 / 184 |                  112 / 112 |                          112 / 112 |
| **Rejection rate**               |                              43.5 % | **1.6 %** (3 `response_invalid`) | **0.5 %** (1 `length_not_allowed`) |                  **0.0 %** | **2.7 %** (3 `length_not_allowed`) |
| Plans changed                    |                         30 (16.3 %) |                      46 (25.0 %) |                        24 (13.0 %) |                27 (24.1 %) |                        14 (12.5 %) |
| Accepted with the planner's plan |                                  74 |                              135 |                                159 |                         85 |                                 95 |
| Latency p50 / p95 / max          |                   3.0 / 4.4 / 5.1 s |                3.1 / 4.4 / 6.5 s |                  3.6 / 5.0 / 9.6 s |          3.0 / 4.5 / 6.4 s |                  2.8 / 3.9 / 5.0 s |
| Tokens in / out (thinking)       |                      1,694 / 97 (0) |                   1,780 / 96 (0) |                     1,804 / 90 (0) |             1,788 / 96 (0) |                     1,812 / 94 (0) |
| Cost per transition              |                         0.00164 USD |                      0.00170 USD |                        0.00169 USD |                0.00170 USD |                        0.00171 USD |

The three `response_invalid` answers exceeded the schema's length limits (`reason` or
`notes`). On the electronic set the changes touch neither harmony nor duration: the model starts
the incoming track earlier (15–21 s less of its intro skipped on the changed edges), which is
what moves the energy step and the loudness.

## R2.5 Results: §10 objective metrics

Means; served = the model's plan where accepted, the deterministic plan otherwise.

**Phrase set (184 edges):**

| Metric (§10)                       |    v2 |     v3 | v3 + Flash rev. 1 | v3 + Flash `sm3-refine-2` | v3 + Flash `sm3-refine-3` |
| ---------------------------------- | ----: | -----: | ----------------: | ------------------------: | ------------------------: |
| Policy violations                  |     0 |      0 |                 0 |                         0 |                         0 |
| Start and end on a phrase boundary | 0.0 % | 15.8 % |            13.0 % |                    12.5 % |                    14.1 % |
| Boundaries aligned                 | 8.0 % | 52.5 % |            51.9 % |                    53.1 % |                    51.9 % |
| Bass overlap, ms                   | 2,152 |  2,152 |             2,152 |                     2,152 |                     2,152 |
| Harmonic clash, ms                 | 7,557 |  4,470 |             4,141 |                     4,214 |                     4,441 |
| Energy step                        | 0.092 | 0.0476 |            0.0492 |                    0.0544 |                    0.0505 |
| Vocal clash, ms                    | 2,481 |     70 |               155 |                       179 |                    **70** |

**Electronic set (112 edges):**

| Metric (§10)                       |     v2 |     v3 | v3 + Flash `sm3-refine-2` | v3 + Flash `sm3-refine-3` |
| ---------------------------------- | -----: | -----: | ------------------------: | ------------------------: |
| Policy violations                  |      0 |      0 |                         0 |                         0 |
| Start and end on a phrase boundary |  0.0 % |  7.1 % |                     7.1 % |                     7.1 % |
| Boundaries aligned                 | 10.3 % | 52.2 % |                    53.4 % |                    52.2 % |
| Bass overlap, ms                   |  2,607 |  2,606 |                     2,606 |                     2,606 |
| Harmonic clash, ms                 |  2,344 |  2,127 |                     2,127 |                     2,127 |
| Energy step                        |  0.126 | 0.0373 |                    0.0426 |                    0.0402 |
| Vocal clash, ms                    |    177 |     36 |                        71 |                    **36** |

By stratum, served minus deterministic (start+end on phrase / energy step / vocal clash ms):

| Stratum                   | Edges | `sm3-refine-2`          | `sm3-refine-3`       |
| ------------------------- | ----: | ----------------------- | -------------------- |
| Phrase set, ΔBPM 0–2 %    |    63 | −4.8 pp / +0.010 / +127 | −3.2 pp / +0.003 / 0 |
| Phrase set, ΔBPM 2–4 %    |    59 | −5.1 pp / +0.008 / +136 | −1.7 pp / +0.002 / 0 |
| Phrase set, ΔBPM 4–6 %    |    62 | 0 / +0.003 / +65        | 0 / +0.004 / 0       |
| Electronic, ΔBPM 0–2 %    |    50 | 0 / +0.004 / +80        | 0 / +0.003 / 0       |
| Electronic, ΔBPM 2–4 %    |    41 | 0 / +0.005 / 0          | 0 / +0.002 / 0       |
| Electronic, ΔBPM 4–6 %    |    21 | 0 / +0.009 / 0          | 0 / +0.004 / 0       |
| Electronic, house         |    25 | 0 / +0.008 / 0          | 0 / +0.006 / 0       |
| Electronic, techno        |    25 | 0 / +0.009 / +160       | 0 / +0.005 / 0       |
| Electronic, drum and bass |    25 | 0 / +0.001 / 0          | 0 / +0.001 / 0       |
| Electronic, other dance   |    25 | 0 / +0.005 / 0          | 0 / +0.001 / 0       |
| Electronic, cross-bucket  |    12 | 0 / +0.002 / 0          | 0 / 0.000 / 0        |

## R2.6 Results: automatic listening metrics

Same metrics as revision 1 (§2.4). Means over all edges; then the edges whose plan changed.

| Metric                                  | Phrase: v3 | Phrase: `-2` | Phrase: `-3` | Electronic: v2 | Electronic: v3 | Electronic: `-2` | Electronic: `-3` |
| --------------------------------------- | ---------: | -----------: | -----------: | -------------: | -------------: | ---------------: | ---------------: |
| Beat clash at full level, ms            |      3,954 |        3,870 |        3,943 |          2,631 |          2,478 |            2,437 |            2,449 |
| Beat phase error, median ms (beatmatch) |         60 |              |              |             44 |             39 |               34 |               34 |
| Beats within 25 ms (beatmatch)          |      0.435 |              |              |           0.70 |           0.77 |             0.79 |             0.79 |
| Voice on voice at full level, ms        |        956 |          781 |          926 |            638 |            274 |              274 |              283 |
| Loudness step across the mix, \|LU\|    |       2.84 |         2.93 |         3.03 |           3.62 |           2.79 |             2.93 |             2.90 |
| Loudness bump, LU                       |       0.57 |         0.64 |         0.60 |           1.17 |           1.13 |             1.05 |             1.05 |
| Loudness dip, LU                        |       6.82 |         6.84 |         6.72 |           8.55 |           7.81 |             8.02 |             7.99 |
| Steepest short-term change, LU/s        |       2.31 |         2.41 |         2.35 |           2.99 |           2.46 |             2.48 |             2.48 |
| Mix beat interval CV                    |      0.076 |        0.077 |        0.077 |          0.061 |          0.044 |            0.041 |            0.047 |

Paired, changed edges only (better / worse beyond the noise floor; net = more of the four metrics
better than worse):

| Comparison                       | Changed | Beat clash | Voice on voice | Loudness step | Loudness dip | Net better / worse | Sign test p | No metric worse | Clearly worse¹ |
| -------------------------------- | ------: | ---------: | -------------: | ------------: | -----------: | -----------------: | ----------: | --------------: | -------------: |
| Phrase: v3 vs v2                 |     158 |    79 / 54 |        56 / 22 |       71 / 40 |      80 / 44 |           102 / 37 |     < 0.001 |            29 % |             40 |
| Phrase: `sm3-refine-2` vs v3     |      46 |    15 / 13 |         11 / 5 |       12 / 22 |      18 / 17 |            11 / 16 |        0.44 |            13 % |             17 |
| Phrase: `sm3-refine-3` vs v3     |      24 |      6 / 8 |          5 / 1 |        5 / 15 |        9 / 4 |              4 / 8 |        0.39 |            13 % |              7 |
| Electronic: v3 vs v2             |     108 |    40 / 32 |         24 / 4 |       51 / 24 |      51 / 35 |            59 / 28 |       0.001 |            40 % |             35 |
| Electronic: `sm3-refine-2` vs v3 |      27 |      5 / 5 |          1 / 1 |        4 / 15 |       7 / 11 |         **5 / 16** |    **0.03** |            15 % |              6 |
| Electronic: `sm3-refine-3` vs v3 |      14 |      3 / 2 |          0 / 1 |        2 / 10 |        5 / 8 |              2 / 8 |        0.11 |             0 % |              6 |

¹ More than 2 s of beat clash or voice on voice, or more than 3 LU, on at least one metric.

The automatic stand-in for the blind criterion (the AI wins or ties in ≥ 60 % of the changed
pairs) is far from met with either prompt on either set.

## R2.7 §10 criteria, pass or fail (revision 2)

| Criterion (§10)                    | Target              | Phrase, `-2`              | Phrase, `-3`              | Electronic, `-2`          | Electronic, `-3`          |
| ---------------------------------- | ------------------- | ------------------------- | ------------------------- | ------------------------- | ------------------------- |
| Policy violations                  | 0                   | yes                       | yes                       | yes                       | yes                       |
| Phrase alignment                   | ≥ v3                | **no** (12.5 % vs 15.8 %) | **no** (14.1 % vs 15.8 %) | yes                       | yes                       |
| Bass overlap                       | ≤ v3                | yes                       | yes                       | yes                       | yes                       |
| Harmonic clash                     | ≤ v3                | yes                       | yes                       | yes (equal)               | yes (equal)               |
| Energy continuity                  | ≤ v3                | **no** (0.0544 vs 0.0476) | **no** (0.0505 vs 0.0476) | **no** (0.0426 vs 0.0373) | **no** (0.0402 vs 0.0373) |
| Vocal clash                        | ≤ v3                | **no** (179 vs 70 ms)     | yes (equal)               | **no** (71 vs 36 ms)      | yes (equal)               |
| Rejection rate                     | ≤ 10 %              | yes (1.6 %)               | yes (0.5 %)               | yes (0.0 %)               | yes (2.7 %)               |
| Latency p95 (cloud)                | ≤ 30 s              | yes (4.4 s)               | yes (5.0 s)               | yes (4.5 s)               | yes (3.9 s)               |
| Blind listening, automatic reading | wins or ties ≥ 60 % | **no** (13 %)             | **no** (13 %)             | **no** (15 %)             | **no** (0 %)              |

## R2.8 Recommendation after revision 2 (input for SG3)

**No-go for enabling AI refinement**, now on firmer ground than revision 1:

1. The defect that made revision 1 fail the rejection criterion is fixed and the criterion passes,
   so the remaining failures are about the choices themselves.
2. With the tighter guidance the served plan is v3's on 87 % of edges, and what the model changes still
   makes energy continuity worse on both sets and phrase alignment worse on the phrase set.
3. The audio metrics never favour the AI: on the electronic set, the genre the guidance names,
   `sm3-refine-2` is significantly worse than v3 and `sm3-refine-3` leans worse; on the phrase set
   both are indistinguishable from v3 or lean worse. v3 itself beats v2 significantly on both sets.
4. Cost and latency stay fine (0.0017 USD and p95 under 5 s per transition), so they are not the
   reason.

If Diego keeps the line open: drop `sm3-refine-2` and keep `sm3-refine-3` (commit `4f88f89b`)
as the prompt, and the next useful experiment is not another prompt on pairwise edges but the
set history (`setHistory`), the one input the score cannot see and the model could use; it needs
an evaluation set built from real sets, not pairs.

## R2.9 Cost of revision 2

| Run                        |     Calls |        USD |
| -------------------------- | --------: | ---------: |
| Phrase set, `sm3-refine-2` |       184 |     0.3118 |
| Phrase set, `sm3-refine-3` |       184 |     0.3114 |
| Electronic, `sm3-refine-2` |       112 |     0.1906 |
| Electronic, `sm3-refine-3` |       112 |     0.1918 |
| **Revision 2**             |   **592** | **1.0057** |
| Revision 1                 |       524 |     1.6582 |
| **Whole evaluation**       | **1,116** | **2.6639** |

Ledgers: `out/refine-cache/ledger.jsonl` (revision 1) and `out/refine-cache/ledger-round2.jsonl`
(revision 2), capped at 2 USD for the round.

## R2.10 Deviations, mismatches and open questions (revision 2)

- **Input budget test raised from 6,000 to 6,500** (byte-count bound) by the `bassOk` list; real
  tokens about 1,800 per transition. Confirm or ask for a more compact encoding.
- **Electronic profiles:** 73 % `partial`, so v3 crossfades 65 % of electronic edges. This is an
  analyzer question for S8/SMB, not for SG3, and it limits how much any refinement can do on
  electronic music: the menu of a crossfade edge cannot become a long beatmatched blend.
- **`analyze_pool.py` writes profiles**; it refuses non-loopback databases and the shared `crate`
  database. The first run lost 14 of 120 tracks to forked workers sharing the parent's
  connections; fixed in `9875e91f` and rerun.
- **Variant A on the electronic set** ran from a temporary detached worktree at `5259974a`
  (removed afterwards), sharing the same `out/`, cache and ledger.
- **Containers kept running for the next round:** `sm5-eval-postgres` (127.0.0.1:55473, volume
  `sm5-eval-pgdata`, now also holding the 120 electronic v7 profiles), `sm5-eval-redis`
  (127.0.0.1:56380), `sm5-test-postgres`, `sm5-test-redis` (network `sm5-test`).
- **Open:** keep or drop the `sm3-refine-3` commit; whether the electronic `partial` rate deserves
  its own look before Crate Decks relies on beatmatch for electronic music.

---

# Revision 1

## 1. Summary

- **Recommendation: no-go for enabling AI refinement as merged in SM4.** The deterministic v3
  planner carries the gains; the AI line can stay closed (SM6 documents only) or get one more,
  cheap iteration with a fixed prompt if Diego wants it (§6). This is input for SG3, not the
  decision.
- **Both Gemini models fail the §10 criteria on the phrase set.** Rejection rate 43.5 % (3.8
  Flash) and 26.8 % (3.1 Pro preview) against a 10 % target, and the served plans are worse than
  the deterministic ones in phrase alignment, energy continuity and vocal clash; they are better
  in harmonic clash. Policy violations (0), bass overlap (equal) and latency (p95 4.4 s and
  11.1 s) pass.
- **Most rejections come from one SM4 prompt defect:** the models ask for a `balanced` bass
  handoff on crossfade candidates, which the validation rejects (77 of Flash's 80 rejections, 41
  of Pro's 48). On beatmatch edges the rejection rate is 1.2 % for both. With the defect
  corrected offline (cached answers, no new calls) rejections fall to 1.6 % and 3.9 %, but the
  phrase alignment, energy and vocal clash criteria still fail.
- **Automatic listening metrics see no reliable gain.** On the edges whose plan the model
  changed, beat clash and voice on voice measured on the audio go down slightly (Flash: 9 edges
  better, 3 worse on voice) and loudness is a wash; no difference is distinguishable from noise
  (sign test p ≥ 0.14). The same metrics show v3 clearly better than v2 (102 edges net better,
  37 net worse, p < 0.001).
- **Cheap and fast enough:** Flash costs 0.0016 USD per transition (1.64 USD per 1,000, double
  from 2027-01-01) with p95 4.4 s; Pro 0.0062 USD with p95 11.1 s and 2.7 % failures (3 timeouts
  at 45 s). Total spend of the evaluation: **1.66 USD** of the 5 USD cap.
- **The models are consistent:** a 60-edge rerun of Flash served the same plan on 58 edges, and
  Flash and Pro agree on 135 of the 169 edges both accepted. The changes are a systematic
  preference (intros and instrumental phrases, shorter mixes outside electronic music), not
  noise; the metrics just do not reward it.

## 2. Method

### 2.1 Pipeline

`run_eval.py --planner smart-mix-v3 --refine <provider>/<model>` (new, `tools/smart-mix-eval/`)
plans every edge with v3, then sends the edge's candidate menu to the model through the SM4
pieces, unchanged:

- input, system prompt and response schema: `crate.llm.prompts.smart_mix_transition`
  (`PROMPT_VERSION = "sm3-refine-1"`), genre sent (the server default), no set history (the
  edges are pairs, not sets);
- request: `crate.ai.transport.generate` with the profile the admin's default-model flow binds
  to `smart_mix.transition.refine`: 45 s timeout, 1,024 output tokens (thinking included, per
  Google's docs), low reasoning;
- answer: the runtime's JSON parsing and schema validation (`response_invalid` on failure) and
  `crate.smart_mix.refinement.apply_refinement` with its closed rejection codes. Menus with a
  single candidate are not sent (`single_candidate`), as in the task.

What only the server does is left out: binding, invocation ledger and daily budgets in
PostgreSQL, Redis, the refinement table and queue wait. **Latency is the provider round trip
measured around the transport call**; the `ai` lane adds queue wait and a few database writes.

The plan served for an edge is the model's when its answer is accepted and the deterministic v3
plan otherwise (what a client plays). Every report keeps both, so the comparison is paired on
the same edges and the same profiles.

Spend control: every answer and failure is cached on disk by request hash (reruns are free), a
shared ledger refused any call whose worst case would take the total over 5 USD, call starts
were spaced by 1 s with two calls in flight, and failed calls were never retried.

### 2.2 Data

The development database (`crate-dev-postgres`) is behind the branch's migrations (its
`alembic_version` 144 belongs to another numbering and lacks `beat_tracker`, so origin/main's
ORM cannot read profiles). It was not migrated. The tools read **`sm5-eval-postgres`**, a local
copy built from the branch's migrations (head 153) with the library, profile, genre, taxonomy
tables and the Gemini connection row copied read-only from dev (no secrets: the connection's key
is an environment variable). The deterministic v3 report on the copy is identical to the SM2
baseline.

| Set                                                             |                                        Edges | Profiles                                                                |                                                   Menus sent to the model | Audio               | Genre strata                                                                                                              |
| --------------------------------------------------------------- | -------------------------------------------: | ----------------------------------------------------------------------- | ------------------------------------------------------------------------: | ------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| **Phrase set** (primary): `baselines/dev-2026-11-06-sm2/spike/` | 184 (63 / 59 / 62 in ΔBPM 0–2 / 2–4 / 4–6 %) | analyzer v7 with phrases, energy curve and per-phrase voice (64 tracks) |                                         184 (182 of 8 candidates, 2 of 4) | yes (`test-music/`) | the pool set has none; edges grouped by the root of the genre the prompt sent for the outgoing track (78 edges `unknown`) |
| **SM0 set** (secondary): `baselines/dev-2026-11-04/`            |           300 (100 per band, 15 root genres) | v5, no phrases (bar heuristic menus)                                    | 96 (the beatmatch edges; the 204 crossfade edges have a single candidate) | no                  | yes                                                                                                                       |

Limits of the data:

- The phrase set is the only one where v3 has real phrase menus, and it is small and narrow: 64
  tracks of the spike pool, mostly alternative, post-punk, industrial and rock; **no house,
  techno or drum and bass**, the genres the prompt's guidance names. Both genres are known on
  only 62 of its 184 edges.
- The design asks for 300 edges stratified by genre with phrases. The development library has
  only 80 analyzer-v7 profiles, so that set cannot be built until the v7 backfill; a larger set
  on the production library needs Diego's authorization.
- Context `dj` (Crate Decks: beatmatch, long blends and bass handoff allowed).

### 2.3 Models

From the `model_catalog` of connection `f0028100-…` ("Environment gemini", `secret_source =
environment`):

| Model                    | Role                                                                   | Crate catalog                                                                       | Reasoning                               | Price (USD per million tokens: input / output incl. thinking) |
| ------------------------ | ---------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | --------------------------------------- | ------------------------------------------------------------- |
| `gemini-3.8-flash`       | the model every dev binding uses, fast and cheap                       | validated, in Crate's manifest                                                      | `low` (the action's own preference)     | 0.75 / 3.75 until 2026-12-31; **1.50 / 7.50 from 2027-01-01** |
| `gemini-3.1-pro-preview` | stronger model: the only current Pro text model in Google's price list | **pending, not validated**: no manifest entry, so the admin could not bind it today | `low` (explicit; its default is `high`) | 2.00 / 12.00 (prompts up to 200K tokens)                      |

Prices: <https://ai.google.dev/gemini-api/docs/pricing> (redirects to
<https://aistudio.google.com/docs/pricing>), read 2026-11-07, paid tier, standard. Thinking
levels and the fact that `maxOutputTokens` includes thinking:
<https://ai.google.dev/gemini-api/docs/thinking>, read 2026-11-07.

### 2.4 Automatic listening metrics

`listen_metrics.py` (new) renders each plan with `render_pair.py` (the mix, and each deck alone
on the mix's clock at unity gain) and measures 36–60 s of audio around the transition. "Full
level" is the harness's own definition: both crossfade gains at half amplitude or more.

| Metric                                      | Measures                                                                                                                                                           | Better         |
| ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------- |
| Beat clash (ms)                             | Beat This! (`small0`, DBN) on each stem; full-level time while the incoming beat nearest to each outgoing beat is more than 25 ms off (phase folded into one beat) | lower          |
| Phase error median (ms), beats within 25 ms | the same comparison over the whole mix; meaningful for beatmatch plans                                                                                             | lower / higher |
| Mix beat interval CV                        | Beat This! on the mix: variation of the beat intervals inside the mix (flams and trainwrecks make them irregular)                                                  | lower          |
| Voice on voice (ms)                         | YAMNet (the `yamnet-2` port) on each stem: full-level time with a voiced 0.96 s patch on both decks (patch byte ≥ `VOCAL_THRESHOLD` 90, SG1b)                      | lower          |
| Loudness step (\|LU\|)                      | EBU R128 short-term loudness (FFmpeg `ebur128`): median 6 s after the mix minus 6 s before                                                                         | lower          |
| Loudness bump, dip (LU)                     | momentary loudness during the mix above the louder side, below the quieter side                                                                                    | lower          |
| Steepest change (LU/s)                      | largest change of short-term loudness over 1 s, from 3 s before to 3 s after the mix                                                                               | lower          |

Paired comparisons count edges that got better, stayed the same (within a noise floor: 100 ms of
clash, 5 ms of phase, 0.05 of locked share, 0.02 of CV, one YAMNet patch of 480 ms, 0.5 LU) or
got worse. These are proxies for rhythm, voice and level, not for taste. Beat This! resolves beats
to about 20 ms, so the 25 ms tolerance is about one frame. Only the phrase set has audio.

## 3. Results

### 3.1 What the models did

Phrase set (184 edges, every menu sent), SM4 as merged:

|                                                                |                                                              `gemini-3.8-flash` |                                                `gemini-3.1-pro-preview` |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------: | ----------------------------------------------------------------------: |
| Calls / answers                                                |                                                                       184 / 184 |                                                               184 / 179 |
| Accepted (`ready`)                                             |                                                                             104 |                                                                     131 |
| Rejected                                                       | 80: `bass_handoff_not_allowed` 77, `length_not_allowed` 2, `response_invalid` 1 | 48: `bass_handoff_not_allowed` 41, `low_confidence` 6, `plan_invalid` 1 |
| Transport failures                                             |                                                                               0 |               5: `AI_PROVIDER_TIMEOUT` 3 (45 s), `AI_PROVIDER_FAILED` 2 |
| **Rejection rate** (over answers)                              |                                                                      **43.5 %** |                                                              **26.8 %** |
| Rejection rate on beatmatch edges / crossfade edges            |                                             1.2 % (1 of 85) / 79.8 % (79 of 99) |                                     1.2 % (1 of 82) / 48.5 % (47 of 97) |
| Plans changed (served ≠ deterministic)                         |                                                                     30 (16.3 %) |                                                             37 (20.1 %) |
| Accepted with the planner's own plan                           |                                                                              74 |                                                                      94 |
| Candidate chosen when accepted (index: count)                  |                                            0: 74, 1: 18, 2: 9, 3: 1, 4: 1, 6: 1 |                                          0: 96, 1: 22, 2: 9, 3: 2, 4: 2 |
| Phrase shifts and length steps accepted                        |                                                                               0 |                                                                3 shifts |
| Confidence, mean                                               |                                                                            0.81 |                                                                    0.81 |
| Latency p50 / p95 / max                                        |                                                               3.0 / 4.4 / 5.1 s |                                                     4.6 / 11.1 / 45.0 s |
| Tokens per transition: input / output with thinking (thinking) |                                                                  1,694 / 97 (0) |                                               1,694 / 194 (87; max 796) |
| Cost per transition                                            |                                                                     0.00164 USD |                                                             0.00619 USD |

SM0 set (300 edges, v5 profiles without phrases): only the 96 beatmatch edges have a menu (the
bar heuristic's other windows and bass/curve variants); the 204 crossfade edges have a single
candidate. Flash accepted 95 answers (rejection 1.0 %, one `length_not_allowed`), changed 16
plans and **all 16 halve a long blend** (25–48 s down to 12–24 s), in hip-hop, pop, metal, punk
and alternative edges, which is the prompt's genre guidance ("hip-hop and pop short mixes").
Latency p50/p95 3.2/4.7 s, 1,164 input and 97 output tokens, 0.00124 USD per transition.

What drives the choices: the reasons cite vocals or instrumental sections in 21 of Flash's 30
changes and intros in 17, in line with the system prompt ("avoid vocals over vocals, mix outros
into intros"). The profile-based vocal clash nevertheless never improved on a changed edge
(Flash: 26 equal, 4 worse; Pro: 32 equal, 5 worse); the audio measurement in §3.5 does see a
small reduction.

### 3.2 §10 objective metrics: served against deterministic

Means over the 184 phrase edges; served = the model's plan where accepted, the deterministic
plan otherwise. "Changed" rows are the edges whose served plan differs.

| Metric (§10)                       |     v2 | v3 deterministic | v3 + Flash | v3 + Pro | Flash, changed edges (Δ) | Pro, changed edges (Δ) |
| ---------------------------------- | -----: | ---------------: | ---------: | -------: | -----------------------: | ---------------------: |
| Policy violations                  |      0 |                0 |          0 |        0 |                        0 |                      0 |
| Start and end on a phrase boundary |  0.0 % |           15.8 % |     13.0 % |   12.5 % |                 −16.7 pp |               −16.2 pp |
| Boundaries aligned (of 4)          |  8.0 % |           52.5 % |     51.9 % |   52.7 % |                  −3.3 pp |                +1.4 pp |
| Bass overlap, ms                   |  2,152 |            2,152 |      2,152 |    2,152 |                        0 |                      0 |
| Harmonic clash, ms                 |  7,557 |            4,470 |      4,141 |    4,142 |                   −2,022 |                 −1,635 |
| Energy step (0–1)                  |  0.092 |            0.048 |      0.049 |    0.048 |                   +0.010 |                 +0.003 |
| Vocal clash, ms (profiles)         |  2,481 |               70 |        155 |      179 |                     +523 |                   +541 |
| Transition duration, ms            | 19,549 |           14,144 |     13,715 |   13,598 |                   −2,631 |                 −2,715 |

The models trade phrase alignment, energy continuity and vocal clash (the components v3's score
already weighs) for less harmonic clash. On the SM0 set the served plans equal the deterministic
ones on every §10 metric except a mean 1.0 s shorter duration (the 16 halved blends) and 0.2 pp
more aligned grid boundaries.

### 3.3 By ΔBPM band and by genre

Served minus deterministic on the phrase set, by ΔBPM band (all edges of the band):

| Band  | Edges | Model | Start+end on phrase | Boundaries aligned | Harmonic clash ms | Energy step | Vocal clash ms | Duration ms |
| ----- | ----: | ----- | ------------------: | -----------------: | ----------------: | ----------: | -------------: | ----------: |
| 0–2 % |    63 | Flash |             −4.8 pp |            −1.2 pp |            −1,016 |      +0.005 |           +127 |      −1,427 |
| 0–2 % |    63 | Pro   |             −4.8 pp |            −1.2 pp |            −1,016 |      +0.004 |           +127 |      −1,771 |
| 2–4 % |    59 | Flash |             −3.4 pp |            −1.7 pp |               +56 |      +0.005 |              0 |        +185 |
| 2–4 % |    59 | Pro   |             −5.1 pp |             0.0 pp |               +59 |      +0.004 |            +68 |        +188 |
| 4–6 % |    62 | Flash |              0.0 pp |            +1.2 pp |                 0 |      −0.005 |           +124 |           0 |
| 4–6 % |    62 | Pro   |              0.0 pp |            +2.0 pp |                 0 |      −0.006 |           +129 |           0 |

The 4–6 % band is all crossfades, where most answers were rejected (bass defect), so little
changed there. The harmonic gain is concentrated in the 0–2 % band.

By genre (root genre of the genre slug the prompt sent for the outgoing track; the pool set has
no other genre data, and 78 of its 184 edges have none):

| Genre       | Edges | Flash: start+end / harmonic ms / energy / vocal ms | Pro: start+end / harmonic ms / energy / vocal ms |
| ----------- | ----: | -------------------------------------------------- | ------------------------------------------------ |
| alternative |    36 | −2.8 pp / −522 / +0.004 / 0                        | −2.8 pp / −522 / +0.001 / 0                      |
| electronic  |    26 | 0 / +653 / +0.006 / 0                              | 0 / +653 / +0.006 / 0                            |
| other       |    26 | −3.9 pp / 0 / +0.002 / +154                        | −7.7 pp / 0 / 0.000 / +154                       |
| punk        |     9 | 0 / 0 / +0.003 / 0                                 | 0 / 0 / −0.009 / 0                               |
| metal       |     6 | 0 / 0 / 0 / 0                                      | 0 / 0 / −0.014 / +667                            |
| rock        |     3 | 0 / −7,967 / +0.018 / +1,333                       | same as Flash                                    |
| unknown     |    78 | −3.9 pp / −448 / −0.002 / +99                      | −3.9 pp / −446 / +0.001 / +103                   |

On the SM0 set (15 root genres, about 20 edges each, 96 menus) the only differences are the
halved blends: mean duration −5.7 s in hip-hop, −4.3 s in pop, −2.8 s in metal, −2.5 s in punk,
−2.2 s in the 0–2 % band and −0.9 s in the 2–4 % band; every other §10 metric is unchanged in
every genre and band.

Group sizes are small (genres of 3–36 edges); read these as directions, not estimates.

### 3.4 Counterfactual: the bass handoff defect fixed

**Not SM4 behaviour; an offline what-if.** The cached answers were validated again with one
change: a `balanced` request on a candidate that is not beatmatch is read as `keep` (what a
prompt that explained the rule, or a lenient validation, would give). No call was made.

|                                                 | Flash as merged | Flash, bass fixed | Pro as merged | Pro, bass fixed |
| ----------------------------------------------- | --------------: | ----------------: | ------------: | --------------: |
| Rejection rate                                  |          43.5 % |         **1.6 %** |        26.8 % |       **3.9 %** |
| Plans changed                                   |              30 |                61 |            37 |              56 |
| Start and end on a phrase boundary (v3: 15.8 %) |          13.0 % |            13.0 % |        12.5 % |          12.5 % |
| Boundaries aligned (v3: 52.5 %)                 |          51.9 % |            53.4 % |        52.7 % |          52.9 % |
| Harmonic clash, ms (v3: 4,470)                  |           4,141 |             4,141 |         4,142 |           4,142 |
| Energy step (v3: 0.048)                         |           0.049 |             0.056 |         0.048 |           0.053 |
| Vocal clash, ms (v3: 70)                        |             155 |               199 |           179 |             179 |

Fixing the defect would pass the rejection criterion, but the newly accepted crossfade choices
make energy continuity and vocal clash worse still. The prompt's guidance, not only the bass
rule, would need work.

### 3.5 Automatic listening metrics

Rendered phrase-set transitions (184 edges per variant, 424 distinct plans, none failed). Means
over all edges, then paired comparisons. Lower is better except "beats within 25 ms".

| Metric                                       |    v2 |    v3 | v3 + Flash | v3 + Pro | v3 + Flash, bass fixed | v3 + Pro, bass fixed |
| -------------------------------------------- | ----: | ----: | ---------: | -------: | ---------------------: | -------------------: |
| Beat clash at full level, ms                 | 6,060 | 3,954 |      3,802 |    3,716 |                  3,768 |                3,663 |
| Beat phase error, median ms (beatmatch only) |    53 |    60 |         60 |       58 |                     60 |                   58 |
| Beats within 25 ms (beatmatch only)          | 0.477 | 0.435 |      0.433 |    0.434 |                  0.433 |                0.434 |
| Mix beat interval CV                         | 0.080 | 0.076 |      0.076 |    0.074 |                  0.075 |                0.074 |
| Voice on voice at full level, ms             | 2,717 |   956 |        789 |      847 |                    778 |                  815 |
| Loudness step across the mix, \|LU\|         |  3.36 |  2.84 |       2.92 |     2.89 |                   2.96 |                 2.95 |
| Loudness bump during the mix, LU             |  0.45 |  0.57 |       0.57 |     0.53 |                   0.64 |                 0.59 |
| Loudness dip during the mix, LU              |  8.26 |  6.82 |       6.53 |     6.55 |                   6.83 |                 6.74 |
| Steepest short-term change, LU/s             |  2.43 |  2.31 |       2.38 |     2.38 |                   2.47 |                 2.44 |

Paired, on the edges whose plan changed (better / worse edges beyond the noise floor):

| Comparison                    | Changed edges | Beat clash | Voice on voice | Loudness step | Loudness dip | Net better / net worse edges¹ | Sign test p |
| ----------------------------- | ------------: | ---------: | -------------: | ------------: | -----------: | ----------------------------: | ----------: |
| v3 vs v2                      |           158 |    79 / 54 |        56 / 22 |       71 / 40 |      80 / 44 |                      102 / 37 |     < 0.001 |
| v3 + Flash vs v3              |            30 |     13 / 8 |          9 / 3 |       10 / 11 |      14 / 12 |                        11 / 7 |        0.48 |
| v3 + Pro vs v3                |            37 |    15 / 11 |          8 / 4 |       13 / 10 |      14 / 15 |                       15 / 10 |        0.42 |
| v3 + Flash, bass fixed, vs v3 |            61 |    22 / 19 |         15 / 5 |       18 / 26 |      23 / 25 |                       20 / 21 |         1.0 |
| v3 + Pro, bass fixed, vs v3   |            56 |    22 / 12 |         12 / 5 |       16 / 22 |      18 / 23 |                       20 / 17 |        0.74 |

¹ An edge is net better when more of these four metrics improved than worsened beyond their
noise floors, and net worse in the opposite case.

**Closest automatic reading of the SG3 listening criterion** (the AI wins or ties in ≥ 60 % of
pairs and no pair is rated below 2): counting a changed edge as a win or tie when none of the
four metrics got worse beyond its noise floor, Flash reaches 13 % (4 of 30) and Pro 16 % (6 of
37); 13 and 11 of those edges are clearly worse on at least one metric (more than 2 s of beat
clash or voice on voice, or more than 3 LU). The same reading for v3 against v2 gives 29 % and
25 %, so the bar is strict for any plan change; the useful signal is the comparison: v3's changes
are net positive and significant, the AI's are not.

Side finding, not about the AI: on rendered **beatmatch** transitions, Beat This! puts the two
decks' beats a median 40 ms apart (v2 and v3 alike), and only 34 of v3's 85 beatmatch edges have
a median phase error of 25 ms or less. The phrase set is mostly live-drum rock and post-punk,
where a constant tempo ratio drifts, and Beat This! resolves about 20 ms, so this needs a look
by ear before drawing conclusions; it matters for Crate Decks' beatmatch, not for SG3.

### 3.6 Stability

- **Rerun:** 60 phrase edges sent to Flash again with a separate cache (same prompt, new calls,
  0.10 USD). Same status on 56, same candidate on 55, same candidate and adjustments on 39 (the
  bass request flips), **same served plan on 58**; 14 of the 15 changes of the first run
  repeated.
- **Across models:** on the 169 edges both models accepted (bass fixed), Flash and Pro served
  the same plan on 135; 35 of the 40 edges both changed got the identical new plan.

## 4. Cost

Ledger of every paid call (`out/refine-cache/ledger.jsonl`); failed calls are counted at their
worst case because the provider reported no usage.

| Run                                                               |   Calls |        USD |
| ----------------------------------------------------------------- | ------: | ---------: |
| Flash pilot (5) and phrase set (179 more)                         |     184 |     0.3010 |
| Flash SM0 set                                                     |      96 |     0.1188 |
| Flash stability rerun                                             |      60 |     0.0989 |
| Pro pilot (5) and phrase set (179 more), 5 failures at worst case |     184 |     1.1395 |
| **Total**                                                         | **524** | **1.6582** |

Per 1,000 transitions at today's prices: **1.64 USD** with 3.8 Flash (3.28 USD from
2027-01-01) and **6.19 USD** with 3.1 Pro preview. Input is about 1,700 tokens per phrase
transition (well under the design's 3–6 K estimate) and 1,160 without phrases.

## 5. §10 criteria, pass or fail

Phrase set, served against deterministic v3 (`yes` = meets the §10 target):

| Criterion (§10)            | Target                           | 3.8 Flash                                                | 3.1 Pro preview                 | Flash, bass fixed* | Pro, bass fixed* |
| -------------------------- | -------------------------------- | -------------------------------------------------------- | ------------------------------- | ------------------ | ---------------- |
| Policy violations          | 0                                | yes (0)                                                  | yes (0)                         | yes (0)            | yes (0)          |
| Phrase alignment           | ≥ v3                             | **no** (13.0 % vs 15.8 %)                                | **no** (12.5 % vs 15.8 %)       | **no**             | **no**           |
| Bass overlap               | ≤ v3                             | yes (equal)                                              | yes (equal)                     | yes                | yes              |
| Harmonic clash             | ≤ v3                             | yes (4,141 vs 4,470 ms)                                  | yes (4,142 vs 4,470 ms)         | yes                | yes              |
| Energy continuity          | smoother or equal                | **no** (0.049 vs 0.048)                                  | **no** (0.0482 vs 0.0476)       | **no**             | **no**           |
| Vocal clash                | ≤ v3                             | **no** (155 vs 70 ms)                                    | **no** (179 vs 70 ms)           | **no**             | **no**           |
| Rejection rate             | ≤ 10 %                           | **no** (43.5 %)                                          | **no** (26.8 %)                 | yes (1.6 %)        | yes (3.9 %)      |
| Latency p95 (cloud)        | ≤ 30 s                           | yes (4.4 s)                                              | yes (11.1 s)                    | yes                | yes              |
| Cost                       | informative                      | 0.0016 USD                                               | 0.0062 USD                      |                    |                  |
| Blind listening (replaced) | AI wins or ties ≥ 60 %, none < 2 | **no** on the automatic reading (13 %; 13 clearly worse) | **no** (16 %; 11 clearly worse) | **no** (15 %)      | **no** (14 %)    |

\* Offline what-if, not SM4 as merged (§3.4).

SM0 set (Flash, menus without phrases): every objective criterion passes (rejection 1.0 %,
latency p95 4.7 s, vocal clash not available), but the only effect is 16 shorter blends; there
is no audio for a listening reading.

## 6. Recommendation (input for SG3)

**No-go for activating AI refinement as merged (SM4, `sm3-refine-1`).**

Reasoning:

1. The criteria Diego set in §10 are not met by either model: three objective criteria fail on
   the served plans (phrase alignment, energy continuity, vocal clash), the rejection rate fails
   by a wide margin, and the automatic stand-in for the blind test does not show the AI winning.
2. The defect behind most rejections is real and cheap to fix, but fixing it offline does not
   rescue the outcome: the extra accepted choices make energy and vocal clash worse, and the audio
   metrics stay at "no detectable difference".
3. The models are consistent and follow the prompt's guidance faithfully (intros, instrumental
   phrases, shorter blends outside electronic music). The disagreement is between that guidance
   and what v3's score and the metrics value. A better prompt might change the picture; a better
   model alone does not (Pro behaves like Flash at four times the cost).
4. The value is already in v3: against v2 it improves phrase alignment, harmonic clash, energy
   continuity and vocal clash (bass overlap is equal) and, on the audio, beat clash, voice on
   voice and loudness steps, with significance. Keeping v3 deterministic as the
   plan clients get loses nothing measured here.
5. Cost and latency are not the obstacle: Flash would cost about 1.6 USD per 1,000 transitions
   with p95 4.4 s.

If Diego prefers **"need more data"** over closing the line, the cheapest decisive next step,
in this order:

1. Fix the bass rule in SM4 (prompt or per-candidate `bass`) and decide whether the "mix outros
   into intros" guidance stays.
2. Rerun Flash on the phrase set (about 0.30 USD) and on a phrase set with electronic music
   (house, techno, drum and bass with v7 profiles), where long phrase-aligned blends and the
   prompt's genre guidance matter most and where this evaluation has no data.
3. Decide SG3 on that rerun, with a short listening check of the changed pairs by Diego if the
   automatic metrics move.

Not recommended: Pro (3.1 preview) as the default model. It is not in Crate's manifest, it
needs its reasoning pinned to `low` to fit the 1,024-token cap, it timed out 3 times at 45 s, it
costs four times as much and it chose like Flash.

## 7. Deviations from the plan, mismatches with the code, open questions

**Deviations from the plan (all known before the run):**

- Gemini only, and automatic listening metrics instead of the 30 blind pairs (Diego's
  decisions). The SG3 listening criterion ("the AI wins or ties in ≥ 60 % of the pairs and none
  is rated below 2") therefore has no direct equivalent here; §3.5 gives the closest automatic
  reading.
- `--refine` takes `<provider>/<model>` plus `--refine-connection <id>` instead of a connection
  alone: a connection holds a catalog of models, not one model.
- 184 phrase edges from 64 tracks instead of 300 stratified edges with phrases: the development
  library has only 80 analyzer-v7 profiles (§2.2).

**Mismatches with the code or the environment, and what I did:**

1. **SM4 prompt defect, the main finding.** The menu shows each candidate's `mode` and `bass`,
   and `rules.bass = true` says the context allows the bass handoff, but nothing says that
   `balanced` only works in beatmatch. `apply_refinement` (design §9.5) rejects it on any other
   candidate. Both models asked for it on crossfade candidates in most crossfade edges. Fix
   options, for SM4: say it in the system prompt; send `rules.bass` per candidate (or omit
   `bass_handoff` from crossfade candidates); or have the validation read `balanced` on a
   crossfade candidate as `keep`. Not changed here: SM5 measures SM4 as merged.
2. **Development database behind the branch's migrations** (no `beat_tracker`; its alembic
   revision 144 follows another numbering). I did not migrate it; I built `sm5-eval-postgres`
   from the branch's migrations and copied the needed tables read-only (§2.2). It is still
   running, with volume `sm5-eval-pgdata`, until you say it can go.
3. **`gemini-3.1-pro-preview` is not in Crate's model manifest** (`crate/ai/capabilities.py`
   lists 3.8 Flash and the 2.5 models only), so the catalog marks it pending and the admin
   could not bind it. I evaluated it with an explicit `low` reasoning; the report marks its
   profile as not validated. `gemini-2.5-pro`, the only validated Pro, is deprecated and routed
   to 3.8 Flash from 2026-12-01.
4. **Gemini counts thinking in `maxOutputTokens`.** Flash at `low` reported no thinking tokens.
   Pro at `low` thought up to 796 tokens and its longest answer used 893 of the action's 1,024;
   at its default `high` it would truncate. A Pro binding needs the reasoning pinned to `low` or
   a larger output cap.
5. **Pro timed out 3 times at the action's 45 s** and failed twice more (2.7 % of calls). The
   `ai` lane would store these as `failed` and serve the deterministic plan.
6. **The worker's `_genres` and error mapping are private**; the eval mirrors them (same query,
   same `resolve_genre_slug`, same `FAILED_CODES`) and a test pins the codes and key variable
   names to the server's.
7. **Gemini 3.8 Flash's price doubles on 2027-01-01** (0.75/3.75 → 1.50/7.50 per million). The
   costs in §4 use today's price.

**Open questions for Diego:**

- Fix the bass handoff defect in SM4 and rerun SM5 (cost about 0.30 USD for Flash on the phrase
  set) before deciding SG3, or decide on these numbers?
- Should the prompt's guidance ("mix outros into intros", "avoid vocals over vocals") stay?
  Flash's reasons cite intros in 17 and vocals or instrumental phrases in 21 of its 30 changes,
  each a candidate v3's score had ranked lower, and 22 of those 30 changes raise the energy
  step.
- A phrase set with electronic music (house, techno, drum and bass) needs v7 profiles for those
  tracks: in the development library after the v7 backfill, or a production sample with your
  authorization.
- Keep `sm5-eval-postgres` (and the `sm5-test-*` pair) for the rerun, or remove them?
- DeepSeek: the same command with `--refine deepseek/<model>` once its connection exists; the
  price table in `refine.PRICES` needs its entry.

## 8. Reproduce

Outputs live in the worktree `~/Code/Ninja/crate-agents/sm5-eval/tools/smart-mix-eval/out/`
(git-ignored): `sm5/spike/<model>/report.{json,md}`, `sm5/sm0/gemini-3.8-flash/`,
`sm5/spike/listen/listen.{json,md}`, the answer cache `refine-cache/` and the spend ledger
`refine-cache/ledger.jsonl`. Rerunning any refinement command below reads the cache and pays
nothing.

```bash
# From tools/smart-mix-eval, with SMART_MIX_EVAL_DSN pointing at the migrated copy and the
# provider key in the environment (never printed).
python run_eval.py --dataset baselines/dev-2026-11-06-sm2/spike/dataset.jsonl \
  --planner smart-mix-v3 --refine gemini/gemini-3.8-flash \
  --refine-connection f0028100-fb55-58c5-8a93-bb95dee25e3f \
  --output out/sm5/spike/gemini-3.8-flash --concurrency 2 --max-usd 5
python run_eval.py ... --refine gemini/gemini-3.1-pro-preview --refine-reasoning low \
  --output out/sm5/spike/gemini-3.1-pro-preview
python run_eval.py --dataset baselines/dev-2026-11-04/dataset.jsonl ... \
  --output out/sm5/sm0/gemini-3.8-flash
python listen_metrics.py --plans smart-mix-v2=out/sm5/spike/smart-mix-v2/report.json \
  --plans smart-mix-v3=out/sm5/spike/gemini-3.8-flash/report.json:deterministic \
  --plans v3+flash=out/sm5/spike/gemini-3.8-flash/report.json \
  --plans v3+pro=out/sm5/spike/gemini-3.1-pro-preview/report.json \
  --compare smart-mix-v3:smart-mix-v2 --compare v3+flash:smart-mix-v3 \
  --compare v3+pro:smart-mix-v3 --output out/sm5/spike/listen --jobs 10
```

The bass-fixed what-if (§3.4) is a throwaway wrapper, not committed
(`out/sm5/scripts/counterfactual_bass.py`): it rewrites `balanced` to `keep` on non-beatmatch
candidates before `apply_refinement` and runs `run_eval.main` with `--max-usd 0`, so it can only
read the cache.

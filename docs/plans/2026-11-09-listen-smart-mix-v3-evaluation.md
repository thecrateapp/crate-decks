# Listen on Smart Mix v3 · LV0 evaluation report

> **Draft, incomplete.** The `<!-- T:… -->` and `<!-- P:… -->` markers are tables and text
> still to fill; the handoff `crate-agents/handoffs/2026-11-10/lv0.md` says how.
>
> **Input for Diego's gate LV-G1: whether `smart-mix-v3` is good enough for Listen offline.**
> This report proposes a verdict (§7); it does not decide the gate.

- **Date:** 2026-11-10 (measured 2026-11-09 and 2026-11-10)
- **Plan:** LV0 of [Listen on Smart Mix v3](2026-11-09-listen-smart-mix-v3-plan.md), with Diego's
  decisions LV-D8 to LV-D12.
- **Code:** monorepo branch `feat/listen-smart-mix-v3-eval` on `origin/main` (`3965fa86f`), local
  commits only: `d1fe14359` (Listen's three contexts and the planner `smart-mix-v3-listen-caps`)
  and `8ca2a1fc8` (`listen_metrics` measures plans that render alike once); its 33 harness tests
  pass. v3 is `origin/main`'s, rules revision 1: the menu rules revision 5 serves every source but
  `decks`, byte for byte (stems evaluation §13), so Listen's v3 has no stems layer. None of the
  48 v3 reports has a plan with `stemsUsed` (Task 2, step 3).
- **Profiles:** analyzer v9 (branch `fix/smart-mix-v8-anchor-m4a` at `af44c95b2`: the bar anchor
  and m4a decoding) in `crate_eval_v9`, a copy of `crate_eval_v8`. `crate_eval` and
  `crate_eval_v8` were not written.

## 1. Summary

<!-- P:summary -->

## 2. Method

### 2.1 Listen's contexts

What the Android app sends, with its default 6 s "Transición máxima" (`run_eval.py` `CONTEXTS`):

| Context | Beatmatch and tempo | Bass swap | Long blends | Who |
| --- | --- | --- | --- | --- |
| `listen-crossfade` | no | no | no | the default user: beatmatch is an opt-in |
| `listen-beatmatch` | yes | no | no | beatmatch turned on |
| `listen-bass` | yes | yes | no | beatmatch and bass swap turned on |

All three are `source: playlist`, `automatic`, 6,000 ms, `userCueProfile: default`. None allows
long blends: with v3 the user's maximum always wins (LV-D10). Production has the three Android
flags on, so real users reach all three. Radio, shuffle and manual queues plan as `playlist`
(only `album` and `decks` differ in the planner).

### 2.2 Planners

- **v2:** `smart-mix-v2`, what Listen plays today.
- **v3:** `smart-mix-v3` as the API serves Listen: the first candidate of the phrase menu.
- **v3 with caps (LV-D9):** `smart-mix-v3-listen-caps`, the planner LV1 would give Listen. It
  takes v3's menu in rank order and serves the first candidate that skips at most 10 s of the
  incoming intro and cuts at most 20 s of the outgoing outro, else v2's own plan (it is on every
  menu). Both are `music_left_out`: from the start of the incoming music to where the deck
  starts, and from the end of the mix to the end of the outgoing music.
- **For the tuning,** the same rule with caps of 5/10 s and 20/40 s. The plan asked for their
  plans only; their listening metrics cost 143 new renders (every other plan was already
  measured), so they were measured too (§5).

### 2.3 Data: the four sets on analyzer v9

The edges are those of the v8 report (stems evaluation §12.2, `eval-v8/sets/<set>/dataset.jsonl`),
unchanged, so v8 and v9 compare on the same edges. The 399 tracks were recomputed with
`analyze_pool.py --force` in the production analysis worker image, with the branch's archived
code and its `crate-cli` binary; stored model signals were reused, and only the m4a files ran
Beat This! again. The profiles read `smart-mix-audio-v8`: the version bump came later. Analyzer
v9 as merged into `origin/main` (`ca620f1c1`, not deployed yet) differs from `af44c95b2` only in
that label (`ANALYZER_VERSION` in Python and Rust, and the binary rebuilt for it), so these are
v9 profiles.

| Set | Tracks | Edges | Profiles that changed | Tracks without phrases, v8 → v9 | Edges planned on phrases |
| --- | ---: | ---: | ---: | ---: | ---: |
| Electronic | 120 | 112 | 6 | 11 → 6 | 112 / 112 |
| Phrase set | 64 | 184 | 3 | 5 → 2 | 170 / 184 (v8: 150) |
| Industrial | 71 | 60 | 8 | 17 → 9 | 60 / 60 |
| Metal and punk | 144 | 125 | 3 | 13 → 13 | 122 / 125 |

The m4a fix shows on the five m4a tracks: their profiles now have the file's real length (for
example 186 s instead of 372 s).

### 2.4 Metrics and the gate rule

- **Plans** (`run_eval.py`): how many edges change against v2 (the render differs), the music
  left out, the share of the four mix boundaries on a phrase boundary, and policy violations.
- **Listening** (`listen_metrics.py`, SM5 §2.4): beat clash, voice on voice (YAMNet), loudness
  step and loudness dip, rendered as the Android engine plays each plan. Paired on the edges
  whose render changes; better or worse beyond the noise floor; net on SM5's four metrics; a
  two-sided sign test; clearly worse above 2 s or 3 LU (SM5 R2.6).
- **Bass on bass:** ST3's metric (stems-st3 `listen_metrics.py`, `046bf6702`): each deck rendered
  alone, its level below 150 Hz, the plan's crossfade gain and bass handoff shelf applied, and
  the time both basses play at full level. `listen_metrics` on `main` does not have it yet, and
  adding it there would measure every plan again with Beat This! and YAMNet, so a scratch tool
  (`bass_metrics.py`, §10) copies ST3's two functions verbatim and renders only the two decks of
  each plan, keyed like `listen_metrics`. Paired with ST3's floor (480 ms) and its "net with
  bass" on the five metrics, as in the stems report. It is not part of the gate rule.
- **LV-G1 (LV-D8):** stop if v3 measures worse than v2 in any context or set, on net (SM5's four)
  or on the loudness dip, with the sign test at p < 0.1. The planner judged is v3 with the caps,
  the one Listen would get (plan, Task 2).
- **Coverage:** <!-- P:coverage -->

## 3. Results: plans

<!-- T:plans -->

<!-- P:plans -->

### 3.1 Music left out against the caps

<!-- T:capscheck -->

<!-- P:capscheck -->

## 4. Results: listening metrics

### 4.1 Means over all edges

<!-- T:means -->

### 4.2 v3 with caps against v2, changed edges

Better / worse beyond the noise floor; in bold where the sign test gives p < 0.1.

<!-- T:listen-v3caps -->

<!-- P:listen-v3caps -->

### 4.3 v3 without caps against v2, changed edges

<!-- T:listen-v3 -->

<!-- P:listen-v3 -->

## 5. Tuning the caps

Plans, `listen-crossfade` (the other contexts give the same picture):

<!-- T:caps -->

Listening, paired against v2 on the changed edges (changed edges; net on SM5's four; loudness
dip):

<!-- T:tuning -->

<!-- P:tuning -->

## 6. Latency

`crate.api.smart_mix._plan_transition_edges` in-process (the API's plan path without HTTP or
auth), 32 edges chained from the electronic set, 20 runs each, on `crate_eval_v9`, with the plan
cache in a Redis of its own. Uncached: every run used a `preferredDurationMs` not seen before.
The laptop had a load average of about 17 from other agents' builds.

| Context | Planner | Uncached p50 / p95, ms | Cached p50 / p95, ms |
| --- | --- | ---: | ---: |
| `listen-crossfade` | smart-mix-v2 | 19 / 21 | 11 / 14 |
| `listen-crossfade` | smart-mix-v3 | 70 / 87 | 11 / 16 |
| `listen-beatmatch` | smart-mix-v2 | 23 / 26 | 13 / 16 |
| `listen-beatmatch` | smart-mix-v3 | 78 / 105 | 12 / 15 |

**The caps' share.** v3 already builds its whole menu (`plan_transition_v3`), so the caps add
only the walk over it: `music_left_out` for up to eight candidates and, where none keeps the
caps, v2's plan computed again (the harness does; Task 5b can take it from the menu). Timed
apart on the same 32 edges, profiles loaded once, 20 runs (`caps_cost.py`, load average about
15):

| Context | v3 planning alone, p50 / p95, ms | Caps' walk, p50 / p95, ms | Edges served v2's plan |
| --- | ---: | ---: | ---: |
| `listen-crossfade` | 67 / 88 | 5.8 / 7.5 | 7 / 32 |
| `listen-beatmatch` | 102 / 112 | 8.1 / 9.7 | 8 / 32 |

v3 costs about four times v2 uncached and the same cached. With the caps, its uncached p95 is
about 95 ms for a default user and 115 ms with beatmatch: under the 150 ms budget of the Smart
Mix design and far under Listen's 1 s deadline floor. The measurement runs the API's function,
not an HTTP request against the dev stack the plan names (Task 2, step 5): HTTP, auth and
serialisation are left out.

## 7. Verdict proposal for LV-G1

<!-- P:verdict -->

## 8. Listening list

<!-- P:listening -->

## 9. Limits and open points

- **The sets are compatible-track edges**, each seed's best partner within 6 % ΔBPM, not Listen
  queues in any order. Real playlists will have more crossfades between unrelated tracks.
- **The renders assume a perfect beatmatch**; the phone engages it only with a phase lock and
  otherwise crossfades on the same cues (LV-G2).
- **Bass on bass comes from a scratch tool** (§2.4), not from `main`'s `listen_metrics`; it will
  be the harness's own once ST3 is merged.
- **The v9 profiles carry the v8 label** (§2.3); their content is the merged analyzer's.
- The listening metrics are proxies for rhythm, voice, bass and level, not for taste.

## 10. Reproduce

Tools not committed, in `~/Code/Ninja/crate-agents/scratch/listen-v3/` and
`~/Code/Ninja/crate-agents/eval-v9/tools/`:

```bash
# v9 profiles (crate_eval_v9 = TEMPLATE crate_eval_v8), one set at a time, 4 jobs
eval-v9/tools/analyze-all.sh
# Plans: 4 sets x 3 contexts x (v2, v3, v3 caps 10/20, 5/10, 20/40)
scratch/listen-v3/plans.sh
# Latency, and the caps' share of it
scratch/listen-v3/latency.sh
scratch/listen-v3/caps_cost.sh
# Listening metrics (15 variants per set, one set at a time), then bass on bass
scratch/listen-v3/listen.sh electronic phrase industrial metalpunk
scratch/listen-v3/bass.sh electronic phrase industrial metalpunk
# Tables
python3 scratch/listen-v3/lv0_tables.py <worktree>/tools/smart-mix-eval/out/listen-v3 --json summary.json
python3 scratch/listen-v3/md_tables.py summary.json <plans|capscheck|means|listen-v3caps|listen-v3|caps|tuning>
# Listening list: pick, render, then FLAC, playlist and README
python3 scratch/listen-v3/select_listening.py <worktree>/tools/smart-mix-eval/out/listen-v3 scratch/listen-v3/jobs.json
scratch/listen-v3/render.sh
```

Outputs in the worktree `~/Code/Ninja/crate-agents/listen-v3-lv0/tools/smart-mix-eval/out/listen-v3/`
(git-ignored): `<set>/listen-<context>/<planner>/report.{json,md}`, `<set>/listen/listen.{json,md}`
and `<set>/listen/bass.json`.

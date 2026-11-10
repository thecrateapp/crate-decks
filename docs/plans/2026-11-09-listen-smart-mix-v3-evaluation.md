# Listen on Smart Mix v3 · LV0 evaluation report

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

- **Proposal for LV-G1: go, with caps of 10 s of skipped intro and 20 s of cut outro** (LV-D9's
  starting values). Diego decides the gate; §7 has the reasoning.
- **v3 with the caps passes Diego's rule (LV-D8) in all 12 set and context cells:** it is never
  worse than v2 on net (SM5's four metrics) or on the loudness dip at p < 0.1. It is better on
  the dip in 9 cells and on net in 4 (electronic crossfade and the phrase set's three contexts);
  in the other cells neither direction is significant.
- **The caps do their job.** With them, v3 changes 35–53 % of the edges in the crossfade context
  (v3 alone: 76–96 %), cuts 4.4–7.2 s of outro and skips 1.3–1.8 s of intro on average (v3 alone:
  18–31 s and 8–17 s), and never exceeds a cap with one of its own candidates. No mix is longer
  than the user's 6 s in any planner (LV-D10).
- **Flags outside the rule, for Diego's ear:**
  - **bass on bass is worse in 10 of the 12 cells** (p < 0.01 in 7), about 0.1–0.3 s more per
    transition on average; only the bass swap context of electronic and industrial is clear. The
    net with bass (five metrics) is never significantly worse;
  - **beat clash is worse in metal and punk with crossfade** (13 better, 26 worse, p 0.053).
- **Tuning:** caps of 5/10 s change almost nothing and are worse on net in electronic beatmatch
  (p 0.07), which would fail the rule. Caps of 20/40 s pass and gain more, but cut 13–18 s of
  outro on average (p90 30–36 s) and add beat clash in two more cells.
- **Latency:** with the caps, v3's uncached p95 is about 95 ms for 32 edges by default and 115 ms
  with beatmatch, under the 150 ms budget and far under Listen's 1 s floor.
- **Listening list:** 40 pairs (v2 against v3 with the caps) in
  `~/Code/Ninja/crate-agents/listening/listen-v3-lv0/` (§8).

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
- **Coverage:** every edge of the four sets (481) was planned in the three contexts with
the five planners (v2, v3, and v3 with caps of 10/20, 5/10 and 20/40 s) and measured: 0 listening
failures, bass on bass on every measured edge, 0 policy violations and 0 mixes longer than 6 s.
The beatmatch contexts mix 46, 34, 65 and 22 % of the edges with beatmatch (electronic, phrase
set, industrial, metal and punk), the same share in every planner; the crossfade context has
none.

## 3. Results: plans

| Set | Context | Planner | Changed vs v2 | Outro cut, s (mean / p90 / max) | Intro skipped, s (mean / p90 / max) | Boundaries on a phrase | Served: v3's first / another candidate / v2 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Electronic | crossfade | v2 | – | 0.4 / 0.0 / 20.7 | 0.3 / 0.6 / 7.2 | 10 % | – |
| Electronic | crossfade | v3 | 107 / 112 | 24.8 / 45.3 / 68.8 | 16.5 / 42.5 / 67.8 | 48 % | – |
| Electronic | crossfade | v3, caps 10/20 s | 59 / 112 | 7.2 / 17.6 / 20.7 | 1.3 / 5.7 / 8.8 | 29 % | 30 / 53 / 29 |
| Electronic | beatmatch | v2 | – | 0.4 / 0.4 / 20.7 | 0.3 / 0.7 / 7.2 | 8 % | – |
| Electronic | beatmatch | v3 | 105 / 112 | 25.2 / 49.1 / 75.0 | 16.1 / 42.5 / 67.8 | 46 % | – |
| Electronic | beatmatch | v3, caps 10/20 s | 58 / 112 | 7.2 / 17.6 / 20.7 | 1.4 / 5.7 / 8.8 | 27 % | 32 / 53 / 27 |
| Electronic | bass swap | v2 | – | 0.4 / 0.4 / 20.7 | 0.3 / 0.7 / 7.2 | 8 % | – |
| Electronic | bass swap | v3 | 105 / 112 | 25.2 / 49.1 / 75.0 | 16.1 / 42.5 / 67.8 | 46 % | – |
| Electronic | bass swap | v3, caps 10/20 s | 55 / 112 | 6.8 / 17.3 / 20.7 | 1.3 / 5.7 / 8.8 | 26 % | 32 / 44 / 36 |
| Phrase set | crossfade | v2 | – | 0.0 / 0.0 / 0.3 | 0.1 / 0.2 / 1.0 | 10 % | – |
| Phrase set | crossfade | v3 | 139 / 184 | 20.5 / 51.4 / 153.7 | 10.0 / 30.0 / 60.7 | 42 % | – |
| Phrase set | crossfade | v3, caps 10/20 s | 73 / 184 | 5.5 / 17.8 / 19.9 | 1.4 / 6.9 / 9.5 | 26 % | 81 / 78 / 25 |
| Phrase set | beatmatch | v2 | – | 0.1 / 0.3 / 0.6 | 0.1 / 0.5 / 1.0 | 10 % | – |
| Phrase set | beatmatch | v3 | 135 / 184 | 20.0 / 50.9 / 153.7 | 9.7 / 30.0 / 60.7 | 40 % | – |
| Phrase set | beatmatch | v3, caps 10/20 s | 72 / 184 | 5.5 / 17.8 / 19.9 | 1.5 / 6.9 / 9.5 | 25 % | 84 / 75 / 25 |
| Phrase set | bass swap | v2 | – | 0.1 / 0.3 / 0.6 | 0.1 / 0.5 / 1.0 | 10 % | – |
| Phrase set | bass swap | v3 | 135 / 184 | 20.0 / 50.9 / 153.7 | 9.7 / 30.0 / 60.7 | 40 % | – |
| Phrase set | bass swap | v3, caps 10/20 s | 71 / 184 | 5.4 / 17.8 / 19.9 | 1.5 / 6.9 / 9.5 | 25 % | 84 / 73 / 27 |
| Industrial | crossfade | v2 | – | 0.3 / 0.0 / 11.1 | 0.1 / 0.1 / 0.7 | 8 % | – |
| Industrial | crossfade | v3 | 56 / 60 | 30.7 / 60.8 / 144.4 | 14.0 / 32.0 / 67.0 | 48 % | – |
| Industrial | crossfade | v3, caps 10/20 s | 21 / 60 | 4.9 / 15.3 / 18.9 | 1.8 / 7.9 / 10.0 | 22 % | 14 / 27 / 19 |
| Industrial | beatmatch | v2 | – | 0.5 / 0.5 / 11.1 | 0.2 / 0.6 / 0.7 | 6 % | – |
| Industrial | beatmatch | v3 | 55 / 60 | 30.2 / 60.8 / 144.4 | 14.0 / 32.0 / 67.0 | 46 % | – |
| Industrial | beatmatch | v3, caps 10/20 s | 19 / 60 | 4.5 / 15.3 / 18.9 | 1.7 / 7.9 / 10.0 | 20 % | 14 / 28 / 18 |
| Industrial | bass swap | v2 | – | 0.5 / 0.5 / 11.1 | 0.2 / 0.6 / 0.7 | 6 % | – |
| Industrial | bass swap | v3 | 55 / 60 | 30.2 / 60.8 / 144.4 | 14.0 / 32.0 / 67.0 | 46 % | – |
| Industrial | bass swap | v3, caps 10/20 s | 18 / 60 | 4.4 / 15.3 / 18.9 | 1.6 / 7.6 / 10.0 | 19 % | 14 / 21 / 25 |
| Metal and punk | crossfade | v2 | – | 0.4 / 0.0 / 35.4 | 0.4 / 0.4 / 8.0 | 7 % | – |
| Metal and punk | crossfade | v3 | 103 / 125 | 19.0 / 44.7 / 82.7 | 8.3 / 25.7 / 71.5 | 42 % | – |
| Metal and punk | crossfade | v3, caps 10/20 s | 64 / 125 | 6.2 / 15.3 / 35.4 | 1.8 / 6.6 / 9.5 | 28 % | 64 / 46 / 15 |
| Metal and punk | beatmatch | v2 | – | 0.5 / 0.2 / 35.4 | 0.4 / 0.5 / 8.5 | 6 % | – |
| Metal and punk | beatmatch | v3 | 99 / 125 | 18.3 / 44.0 / 96.9 | 8.4 / 28.3 / 71.5 | 39 % | – |
| Metal and punk | beatmatch | v3, caps 10/20 s | 63 / 125 | 6.1 / 15.3 / 35.4 | 1.8 / 6.7 / 9.5 | 26 % | 67 / 42 / 16 |
| Metal and punk | bass swap | v2 | – | 0.5 / 0.2 / 35.4 | 0.4 / 0.5 / 8.5 | 6 % | – |
| Metal and punk | bass swap | v3 | 99 / 125 | 18.3 / 44.0 / 96.9 | 8.4 / 28.3 / 71.5 | 39 % | – |
| Metal and punk | bass swap | v3, caps 10/20 s | 62 / 125 | 6.0 / 15.3 / 35.4 | 1.8 / 6.4 / 9.5 | 26 % | 67 / 39 / 19 |

- **v3 alone changes most edges and leaves out a lot of music.** It changes 76–96 % of the edges
  and cuts 18–31 s of the outgoing outro on average (p90 44–61 s, up to 154 s on the phrase set),
  and skips 8–17 s of the incoming intro (p90 26–43 s, up to 72 s): the risk the plan names. It
  puts 39–48 % of the mix boundaries on a phrase, against v2's 6–10 %.
- **With the caps, the cut shrinks to what LV-D9 allows.** v3 changes 30–53 % of the edges, cuts
  4.4–7.2 s of outro and skips 1.3–1.8 s of intro on average, and keeps 19–29 % of the boundaries
  on a phrase. In the crossfade context it serves v3's first candidate on 23–51 % of the edges, a
  lower-ranked candidate on 37–47 %, and v2's own plan on 12–32 % (industrial needs v2 most).
- **The contexts barely differ.** Beatmatch and bass swap change one to four fewer edges than
  crossfade; the beatmatch share is set by the tracks, not by the planner.

### 3.1 Music left out against the caps

| Set | Context | v2: over 10 s intro / 20 s outro | v3: over 10 s intro / 20 s outro / either | v3 with caps: over either (all v2's own plan) | v3 with caps: intro skipped, s (p50 / p90 / max) | Outro cut, s (p50 / p90 / max) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Electronic | crossfade | 0 / 1 | 61 / 60 / 82 | 1 | 0.1 / 5.7 / 8.8 | 6.8 / 17.6 / 20.7 |
| Electronic | beatmatch | 0 / 1 | 60 / 58 / 80 | 1 | 0.4 / 5.7 / 8.8 | 6.8 / 17.6 / 20.7 |
| Electronic | bass swap | 0 / 1 | 60 / 58 / 80 | 1 | 0.4 / 5.7 / 8.8 | 6.0 / 17.3 / 20.7 |
| Phrase set | crossfade | 0 / 0 | 57 / 73 / 103 | 0 | 0.1 / 6.9 / 9.5 | 0.0 / 17.8 / 19.9 |
| Phrase set | beatmatch | 0 / 0 | 53 / 72 / 100 | 0 | 0.1 / 6.9 / 9.5 | 0.2 / 17.8 / 19.9 |
| Phrase set | bass swap | 0 / 0 | 53 / 72 / 100 | 0 | 0.1 / 6.9 / 9.5 | 0.2 / 17.8 / 19.9 |
| Industrial | crossfade | 0 / 0 | 28 / 36 / 46 | 0 | 0.1 / 7.9 / 10.0 | 0.0 / 15.3 / 18.9 |
| Industrial | beatmatch | 0 / 0 | 28 / 36 / 46 | 0 | 0.3 / 7.9 / 10.0 | 0.4 / 15.3 / 18.9 |
| Industrial | bass swap | 0 / 0 | 28 / 36 / 46 | 0 | 0.3 / 7.6 / 10.0 | 0.3 / 15.3 / 18.9 |
| Metal and punk | crossfade | 0 / 1 | 29 / 46 / 61 | 1 | 0.1 / 6.6 / 9.5 | 3.8 / 15.3 / 35.4 |
| Metal and punk | beatmatch | 0 / 1 | 29 / 43 / 58 | 1 | 0.3 / 6.7 / 9.5 | 3.4 / 15.3 / 35.4 |
| Metal and punk | bass swap | 0 / 1 | 29 / 43 / 58 | 1 | 0.3 / 6.4 / 9.5 | 3.0 / 15.3 / 35.4 |

No candidate served by the capped planner exceeds either cap (the table script asserts it). The
one edge over a cap in electronic and in metal and punk is v2's own plan, served because no
candidate fits: v2 itself cuts 20.7 s and 35.4 s of outro there. Without the caps, v3 exceeds one
of them on 49–77 % of the edges.

## 4. Results: listening metrics

### 4.1 Means over all edges

| Set | Context | Beat clash, ms (v2 / v3 / v3 caps) | Voice on voice, ms | Bass on bass, ms | Loudness step, LU | Loudness dip, LU |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Electronic | crossfade | 1606 / 1429 / 1540 | 251 / 136 / 214 | 441 / 836 / 547 | 4.19 / 3.04 / 4.04 | 7.21 / 5.72 / 6.78 |
| Electronic | beatmatch | 1166 / 1118 / 1175 | 276 / 131 / 213 | 455 / 834 / 564 | 4.20 / 3.05 / 4.04 | 7.23 / 5.82 / 6.84 |
| Electronic | bass swap | 1166 / 1118 / 1171 | 276 / 131 / 208 | 207 / 412 / 271 | 4.20 / 3.06 / 4.10 | 8.30 / 6.81 / 7.93 |
| Phrase set | crossfade | 1612 / 1661 / 1618 | 193 / 249 / 190 | 778 / 1315 / 1049 | 4.62 / 3.17 / 3.98 | 6.94 / 4.99 / 6.29 |
| Phrase set | beatmatch | 1174 / 1362 / 1231 | 211 / 246 / 209 | 780 / 1312 / 1051 | 4.60 / 3.22 / 3.99 | 6.95 / 5.04 / 6.29 |
| Phrase set | bass swap | 1174 / 1362 / 1242 | 211 / 246 / 209 | 427 / 854 / 653 | 4.60 / 3.22 / 3.99 | 7.57 / 5.58 / 6.85 |
| Industrial | crossfade | 1462 / 1399 / 1497 | 239 / 214 / 257 | 671 / 1212 / 780 | 4.48 / 3.44 / 4.80 | 6.88 / 4.67 / 6.14 |
| Industrial | beatmatch | 914 / 953 / 967 | 274 / 254 / 279 | 696 / 1204 / 795 | 4.44 / 3.43 / 4.68 | 6.79 / 4.59 / 6.06 |
| Industrial | bass swap | 914 / 953 / 939 | 274 / 254 / 295 | 235 / 396 / 223 | 4.45 / 3.44 / 4.66 | 7.75 / 5.32 / 7.02 |
| Metal and punk | crossfade | 1559 / 1731 / 1663 | 415 / 441 / 413 | 828 / 1269 / 992 | 3.89 / 2.93 / 3.66 | 6.70 / 5.15 / 6.28 |
| Metal and punk | beatmatch | 1475 / 1610 / 1566 | 411 / 439 / 415 | 834 / 1287 / 1000 | 3.89 / 2.90 / 3.66 | 6.70 / 5.17 / 6.24 |
| Metal and punk | bass swap | 1475 / 1610 / 1550 | 411 / 439 / 415 | 609 / 969 / 729 | 3.89 / 2.90 / 3.72 | 6.92 / 5.41 / 6.46 |

### 4.2 v3 with caps against v2, changed edges

Better / worse beyond the noise floor; in bold where the sign test gives p < 0.1.

| Set | Context | Changed | Beat clash | Voice on voice | Bass on bass | Loudness step | Loudness dip | Net, SM5's four | Net, with bass | Clearly worse |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Electronic | crossfade | 59 | 19 / 20 | 4 / 2 | **1 / 11 (p < 0.01)** | 25 / 23 | **33 / 17 (p 0.03)** | **30 / 16 (p 0.05)** | 25 / 19 | 15 |
| Electronic | beatmatch | 58 | 16 / 17 | 6 / 2 | **1 / 12 (p < 0.01)** | 25 / 22 | 31 / 20 | 26 / 17 | 22 / 21 | 15 |
| Electronic | bass swap | 55 | 16 / 16 | 6 / 1 | 1 / 6 | 23 / 21 | 29 / 19 | 24 / 16 | 22 / 19 | 14 |
| Phrase set | crossfade | 73 | 18 / 24 | 5 / 3 | **3 / 36 (p < 0.01)** | **34 / 18 (p 0.04)** | **47 / 16 (p < 0.01)** | **40 / 15 (p < 0.01)** | 28 / 24 | 13 |
| Phrase set | beatmatch | 72 | 15 / 24 | 5 / 4 | **2 / 34 (p < 0.01)** | **33 / 18 (p 0.05)** | **47 / 16 (p < 0.01)** | **38 / 15 (p < 0.01)** | 28 / 25 | 15 |
| Phrase set | bass swap | 71 | 14 / 24 | 5 / 4 | **1 / 27 (p < 0.01)** | **33 / 18 (p 0.05)** | **45 / 18 (p < 0.01)** | **35 / 16 (p 0.01)** | 28 / 24 | 13 |
| Industrial | crossfade | 21 | 5 / 8 | 2 / 3 | **2 / 10 (p 0.04)** | 8 / 10 | **13 / 5 (p 0.10)** | 8 / 7 | 5 / 8 | 5 |
| Industrial | beatmatch | 19 | 4 / 7 | 2 / 3 | **2 / 9 (p 0.07)** | 7 / 9 | **12 / 4 (p 0.08)** | 8 / 5 | 4 / 6 | 4 |
| Industrial | bass swap | 18 | 4 / 6 | 1 / 3 | 2 / 2 | 7 / 8 | **12 / 4 (p 0.08)** | 8 / 5 | 7 / 4 | 5 |
| Metal and punk | crossfade | 64 | **13 / 26 (p 0.05)** | 5 / 5 | **4 / 18 (p < 0.01)** | 22 / 20 | 29 / 17 | 20 / 22 | 17 / 26 | 5 |
| Metal and punk | beatmatch | 63 | 14 / 24 | 5 / 5 | **4 / 18 (p < 0.01)** | 22 / 19 | **30 / 16 (p 0.05)** | 21 / 19 | 18 / 23 | 5 |
| Metal and punk | bass swap | 62 | 14 / 23 | 5 / 5 | **4 / 15 (p 0.02)** | 21 / 19 | **30 / 15 (p 0.04)** | 20 / 18 | 18 / 21 | 5 |

- **The gate's two readings hold in every cell.** No cell is worse on net or on the dip at
  p < 0.1. The dip is better in 9 cells (electronic crossfade, the phrase set's three contexts,
  industrial's three at p 0.08–0.10, and metal and punk with beatmatch or bass swap); the other
  three lean better without significance (31/20, 29/19, 29/17). Net is better in electronic
  crossfade and the phrase set; metal and punk with crossfade is the closest to even (20/22).
- **Loudness step** is better in the phrase set (p 0.04–0.05) and even elsewhere. **Voice on
  voice** differs on at most 10 edges per cell, with no signal.
- **Bass on bass is the cost.** It is worse in 10 of 12 cells. The phrase entries that avoid the
  dip keep both tracks near full level, so both basses overlap longer: the means go from 441 to
  547 ms (electronic crossfade), 778 to 1,049 ms (phrase set), 671 to 780 ms (industrial) and 828
  to 992 ms (metal and punk). The bass swap context roughly halves it and takes away the
  significance in electronic (1/6) and industrial (2/2). The net with bass is never significantly
  worse: its lowest reading is metal and punk with crossfade, 17/26 (p 0.22).
- **Beat clash** is worse in metal and punk with crossfade, 13/26 (p 0.053): without beatmatch,
  two full-energy phrases of fast drums overlap for 6 s. It leans worse, without significance, in
  the other metal and punk contexts and in the phrase set.
- **Clearly worse edges** (any of SM5's four beyond 2 s or 3 LU): 13–15 per context in electronic
  and the phrase set, 4–5 in industrial and in metal and punk. The listening list starts with the
  deepest of them (§8).

### 4.3 v3 without caps against v2, changed edges

| Set | Context | Changed | Beat clash | Voice on voice | Bass on bass | Loudness step | Loudness dip | Net, SM5's four | Net, with bass | Clearly worse |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Electronic | crossfade | 107 | 40 / 29 | 10 / 4 | **7 / 45 (p < 0.01)** | **55 / 36 (p 0.06)** | **66 / 28 (p < 0.01)** | **62 / 21 (p < 0.01)** | **50 / 30 (p 0.03)** | 20 |
| Electronic | beatmatch | 105 | 34 / 25 | **11 / 2 (p 0.02)** | **8 / 43 (p < 0.01)** | **55 / 35 (p 0.04)** | **63 / 29 (p < 0.01)** | **58 / 20 (p < 0.01)** | **48 / 29 (p 0.04)** | 20 |
| Electronic | bass swap | 105 | 34 / 25 | **11 / 2 (p 0.02)** | **4 / 21 (p < 0.01)** | **55 / 35 (p 0.04)** | **63 / 29 (p < 0.01)** | **58 / 20 (p < 0.01)** | **53 / 24 (p < 0.01)** | 22 |
| Phrase set | crossfade | 139 | 36 / 46 | **7 / 16 (p 0.09)** | **10 / 74 (p < 0.01)** | **81 / 28 (p < 0.01)** | **93 / 26 (p < 0.01)** | **85 / 25 (p < 0.01)** | **60 / 38 (p 0.03)** | 15 |
| Phrase set | beatmatch | 135 | **29 / 53 (p 0.01)** | 8 / 14 | **9 / 71 (p < 0.01)** | **78 / 30 (p < 0.01)** | **85 / 26 (p < 0.01)** | **70 / 29 (p < 0.01)** | 50 / 43 | 16 |
| Phrase set | bass swap | 135 | **29 / 53 (p 0.01)** | 8 / 14 | **6 / 56 (p < 0.01)** | **78 / 30 (p < 0.01)** | **84 / 30 (p < 0.01)** | **67 / 32 (p < 0.01)** | 49 / 43 | 15 |
| Industrial | crossfade | 56 | 20 / 18 | 5 / 6 | **4 / 29 (p < 0.01)** | **31 / 15 (p 0.03)** | **36 / 12 (p < 0.01)** | **31 / 11 (p < 0.01)** | 27 / 17 | 10 |
| Industrial | beatmatch | 55 | 16 / 18 | 4 / 6 | **4 / 27 (p < 0.01)** | **30 / 15 (p 0.04)** | **39 / 10 (p < 0.01)** | **31 / 11 (p < 0.01)** | 26 / 17 | 11 |
| Industrial | bass swap | 55 | 16 / 18 | 4 / 6 | **2 / 9 (p 0.07)** | **30 / 15 (p 0.04)** | **42 / 10 (p < 0.01)** | **31 / 11 (p < 0.01)** | **30 / 13 (p 0.01)** | 13 |
| Metal and punk | crossfade | 103 | **25 / 43 (p 0.04)** | 12 / 15 | **7 / 42 (p < 0.01)** | **48 / 29 (p 0.04)** | **57 / 20 (p < 0.01)** | 47 / 35 | 36 / 41 | 9 |
| Metal and punk | beatmatch | 99 | 27 / 38 | 13 / 14 | **5 / 42 (p < 0.01)** | **45 / 25 (p 0.02)** | **57 / 19 (p < 0.01)** | **49 / 26 (p 0.01)** | 35 / 35 | 8 |
| Metal and punk | bass swap | 99 | 27 / 38 | 13 / 14 | **4 / 33 (p < 0.01)** | **45 / 25 (p 0.02)** | **57 / 18 (p < 0.01)** | **49 / 26 (p 0.01)** | 39 / 33 | 8 |

Without the caps v3 gains more: net is better in 11 of 12 cells (all but metal and punk with
crossfade, 47/35) and the dip in all 12. It also pays more: beat clash is worse in the phrase set
with beatmatch or bass swap (29/53, p 0.01) and in metal and punk with crossfade (25/43, p 0.04),
voice on voice in the phrase set with crossfade (7/16, p 0.09), and bass on bass in all 12 cells.
It would pass the gate's rule too, but it leaves out the music the caps exist to keep (§3).

## 5. Tuning the caps

Plans, `listen-crossfade` (the other contexts give the same picture):

| Set | Context | Planner | Changed vs v2 | Outro cut, s (mean / p90 / max) | Intro skipped, s (mean / p90 / max) | Boundaries on a phrase | Served: v3's first / another candidate / v2 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Electronic | crossfade | v3, caps 5/10 s | 14 / 112 | 1.4 / 6.9 / 20.7 | 0.3 / 0.6 / 7.2 | 13 % | 13 / 36 / 63 |
| Electronic | crossfade | v3, caps 10/20 s | 59 / 112 | 7.2 / 17.6 / 20.7 | 1.3 / 5.7 / 8.8 | 29 % | 30 / 53 / 29 |
| Electronic | crossfade | v3, caps 20/40 s | 95 / 112 | 18.1 / 34.4 / 39.5 | 5.3 / 15.5 / 19.0 | 43 % | 61 / 42 / 9 |
| Phrase set | crossfade | v3, caps 5/10 s | 11 / 184 | 0.5 / 0.0 / 9.1 | 0.2 / 0.2 / 4.0 | 12 % | 49 / 84 / 51 |
| Phrase set | crossfade | v3, caps 10/20 s | 73 / 184 | 5.5 / 17.8 / 19.9 | 1.4 / 6.9 / 9.5 | 26 % | 81 / 78 / 25 |
| Phrase set | crossfade | v3, caps 20/40 s | 126 / 184 | 14.2 / 31.6 / 39.1 | 4.2 / 12.8 / 17.8 | 38 % | 135 / 43 / 6 |
| Industrial | crossfade | v3, caps 5/10 s | 1 / 60 | 0.5 / 0.3 / 11.1 | 0.1 / 0.1 / 0.7 | 8 % | 4 / 29 / 27 |
| Industrial | crossfade | v3, caps 10/20 s | 21 / 60 | 4.9 / 15.3 / 18.9 | 1.8 / 7.9 / 10.0 | 22 % | 14 / 27 / 19 |
| Industrial | crossfade | v3, caps 20/40 s | 44 / 60 | 16.3 / 35.7 / 37.6 | 6.4 / 17.4 / 19.5 | 38 % | 34 / 19 / 7 |
| Metal and punk | crossfade | v3, caps 5/10 s | 20 / 125 | 1.5 / 7.0 / 35.4 | 0.5 / 1.3 / 8.0 | 13 % | 35 / 50 / 40 |
| Metal and punk | crossfade | v3, caps 10/20 s | 64 / 125 | 6.2 / 15.3 / 35.4 | 1.8 / 6.6 / 9.5 | 28 % | 64 / 46 / 15 |
| Metal and punk | crossfade | v3, caps 20/40 s | 92 / 125 | 13.2 / 30.3 / 39.9 | 3.7 / 9.9 / 18.2 | 38 % | 95 / 26 / 4 |

Listening, paired against v2 on the changed edges (changed edges; net on SM5's four; loudness
dip):

| Set | Context | Caps 5/10 s: changed, net, dip | Caps 10/20 s | Caps 20/40 s | No caps (v3) |
| --- | --- | ---: | ---: | ---: | ---: |
| Electronic | crossfade | 14; 3 / 7; 8 / 3 | 59; **30 / 16 (p 0.05)**; **33 / 17 (p 0.03)** | 95; **55 / 18 (p < 0.01)**; **60 / 21 (p < 0.01)** | 107; **62 / 21 (p < 0.01)**; **66 / 28 (p < 0.01)** |
| Electronic | beatmatch | 13; **1 / 7 (p 0.07)**; 6 / 4 | 58; 26 / 17; 31 / 20 | 93; **49 / 19 (p < 0.01)**; **57 / 22 (p < 0.01)** | 105; **58 / 20 (p < 0.01)**; **63 / 29 (p < 0.01)** |
| Electronic | bass swap | 13; 2 / 7; 7 / 3 | 55; 24 / 16; 29 / 19 | 90; **47 / 19 (p < 0.01)**; **58 / 23 (p < 0.01)** | 105; **58 / 20 (p < 0.01)**; **63 / 29 (p < 0.01)** |
| Phrase set | crossfade | 11; 2 / 5; 5 / 2 | 73; **40 / 15 (p < 0.01)**; **47 / 16 (p < 0.01)** | 126; **73 / 23 (p < 0.01)**; **77 / 25 (p < 0.01)** | 139; **85 / 25 (p < 0.01)**; **93 / 26 (p < 0.01)** |
| Phrase set | beatmatch | 11; 2 / 6; 4 / 2 | 72; **38 / 15 (p < 0.01)**; **47 / 16 (p < 0.01)** | 122; **60 / 26 (p < 0.01)**; **72 / 24 (p < 0.01)** | 135; **70 / 29 (p < 0.01)**; **85 / 26 (p < 0.01)** |
| Phrase set | bass swap | 11; 1 / 6; 4 / 3 | 71; **35 / 16 (p 0.01)**; **45 / 18 (p < 0.01)** | 121; **58 / 30 (p < 0.01)**; **71 / 29 (p < 0.01)** | 135; **67 / 32 (p < 0.01)**; **84 / 30 (p < 0.01)** |
| Industrial | crossfade | 1; 0 / 0; 0 / 0 | 21; 8 / 7; **13 / 5 (p 0.10)** | 44; **23 / 9 (p 0.02)**; **28 / 10 (p < 0.01)** | 56; **31 / 11 (p < 0.01)**; **36 / 12 (p < 0.01)** |
| Industrial | beatmatch | 1; 0 / 0; 0 / 0 | 19; 8 / 5; **12 / 4 (p 0.08)** | 43; **21 / 8 (p 0.02)**; **31 / 8 (p < 0.01)** | 55; **31 / 11 (p < 0.01)**; **39 / 10 (p < 0.01)** |
| Industrial | bass swap | 1; 0 / 0; 0 / 0 | 18; 8 / 5; **12 / 4 (p 0.08)** | 41; **20 / 8 (p 0.04)**; **30 / 8 (p < 0.01)** | 55; **31 / 11 (p < 0.01)**; **42 / 10 (p < 0.01)** |
| Metal and punk | crossfade | 20; 8 / 7; 9 / 4 | 64; 20 / 22; 29 / 17 | 92; 39 / 32; **45 / 21 (p < 0.01)** | 103; 47 / 35; **57 / 20 (p < 0.01)** |
| Metal and punk | beatmatch | 20; 8 / 6; 9 / 4 | 63; 21 / 19; **30 / 16 (p 0.05)** | 89; **41 / 24 (p 0.05)**; **47 / 19 (p < 0.01)** | 99; **49 / 26 (p 0.01)**; **57 / 19 (p < 0.01)** |
| Metal and punk | bass swap | 20; 7 / 6; 8 / 4 | 62; 20 / 18; **30 / 15 (p 0.04)** | 89; **39 / 23 (p 0.06)**; **46 / 18 (p < 0.01)** | 99; **49 / 26 (p 0.01)**; **57 / 18 (p < 0.01)** |

- **Caps of 5/10 s** change 1–20 edges per cell: v3 rarely has a phrase entry within 5 s of the
  intro, so it serves v2's plan (63 of 112 electronic edges) or a low-ranked candidate. In
  electronic and the phrase set net leans worse, and in electronic beatmatch it is worse at p 0.07
  (1/7): this value would fail LV-D8.
- **Caps of 10/20 s** pass in every cell (§4.2).
- **Caps of 20/40 s** pass too, with net better in 11 of 12 cells and the dip in all 12. But they
  cut 13–18 s of outro on average (p90 30–36 s, up to 40 s) and skip up to 19.5 s of intro, beat
  clash becomes worse in metal and punk with crossfade (22/39, p 0.04) and in the phrase set with
  beatmatch or bass swap (29/47, p 0.05), and bass on bass is worse in 11 of 12 cells.
- **Reading.** The gains grow with the caps, and so does what the listener loses: more of the
  outro, more beat clash in fast crossfades, more bass on bass. The metrics do not price the music
  left out, which is what the caps were set for (plan, Risks). 10/20 s is the value that keeps the
  rule with margin while no edge loses more than 20 s of outro or 10 s of intro.

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

**Proposal: go, with caps of 10 s of skipped intro and 20 s of cut outro.** For Task 5b that is
`listen_max_skipped_intro_ms = 10000` and `listen_max_unplayed_outro_ms = 20000`, the plan's
starting values.

- **Rule (LV-D8):** v3 with the caps is not worse than v2 on net or on the dip at p < 0.1 in any
  of the 12 cells; it is better on the dip in 9 and on net in 4 (§4.2).
- **Latency:** uncached p95 about 95 ms (crossfade) and 115 ms (beatmatch) for 32 edges with the
  caps, under the 150 ms budget and Listen's 1 s floor (§6).
- **Flags outside the rule, none of them blocking under LV-D8:**
  1. **Bass on bass** is worse in 10 of 12 cells, including industrial with crossfade (2/10,
     p 0.04) and with beatmatch (2/9, p 0.07); about 0.1–0.3 s more per transition on average,
     and the bass swap context takes most of it away. If Diego's ear confirms it, a bass term for
     non-Decks sources gets its own plan and measurement (out of scope here).
  2. **Beat clash** in metal and punk with crossfade, 13/26 (p 0.053).
  3. **The deep dips** v3 adds on some edges (for example 16.9 LU against v2's 1.2 LU on pair 01):
     the first 15 pairs of the list.
- **Other values Diego can pick:** 20/40 s gains more on the metrics but cuts up to 40 s of outro
  and adds beat clash in two more cells; 5/10 s is not an option, since it fails the rule in
  electronic beatmatch.
- **If go:** LV1 starts with these caps (Task 3 onwards). If Diego's listening of §8 disagrees
  with the metrics, the caps change or the plan stops here.

## 8. Listening list

40 pairs, 80 FLAC files (400 MB), in `~/Code/Ninja/crate-agents/listening/listen-v3-lv0/`, with
`playlist.m3u8` and a README in Spanish that gives each pair's plans and metrics. A is v2, B is
v3 with the caps of 10/20 s, both rendered as the Android engine plays them, from about 20 s
before the mix to about 20 s after it, without normalising.

| Pairs | What | Why |
| --- | --- | --- |
| 01–15 | The 5 edges per context where B's loudness dip is deepest against A | The dip guard (plan, "Guarding against the loudness dips") |
| 16–20 | The 5 that skip the most intro with the caps (crossfade) | The intro cap |
| 21–25 | The 5 that cut the most outro with the caps (crossfade) | The outro cap |
| 26–37 | One changed edge per set and context, picked with a fixed seed | What a listener usually gets |
| 38–40 | 3 edges where B is A's plan; A and B are bit-identical | Controls |

For the flags of §7: metal and punk with crossfade are pairs 02, 18, 19, 21 and 35 (beat clash);
industrial pairs 16, 17, 32 and 33 (bass on bass without bass swap), and 12 and 34 with it. The
plan's "5 long blends in the beatmatch contexts" are replaced by the sample: with LV-D10, v3 has
no long blends in Listen (0 mixes over 6 s).

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

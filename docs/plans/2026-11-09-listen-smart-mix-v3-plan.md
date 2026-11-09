# Listen on Smart Mix v3 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **For agents:** execute the phases in order. Each code task starts with the failing test it names. Do not pass a gate (LV-G1, LV-G2) without Diego's explicit approval. If the code does not match this plan (paths, line numbers, APIs), stop, report and propose the change to the plan. Line numbers are those of `origin/main` at `3965fa86f` and of `feat/smart-mix-stems-st3` at `c9e86ff97`; Listen and Android are identical on both. Revalidate them before use.

**Goal:** Listen's Android app plans its Smart Mix transitions with `smart-mix-v3` instead of `smart-mix-v2`, behind a server switch, with v2 as the fallback for older servers and as the rollback.

**Architecture:**

- The server recommends a planner for Listen in `/api/capabilities` (`smart_mix.listen_planner_version`): `smart-mix-v2` unless the switch `CRATE_LISTEN_SMART_MIX_V3_ENABLED` is on. The API default stays v2.
- Listen resolves its planner from that field, v2 when the field is absent, uses it for its plan batches and hands it to the native background continuation.
- The plan format does not change: each v3 plan keeps `plannerVersion: 2`, so the native engine executes v3 plans as they are. Only the native continuation client learns which planner to ask for, and the server's continuation endpoint learns to plan with v3.

**Tech stack:** server in Python 3.13, FastAPI and Pydantic (`app/crate`); Listen in React, TypeScript and Vitest (`app/listen/src`, shared with `app/listen-desktop`); Android in Java with JUnit 4 (`app/listen/android`); evaluation harness `tools/smart-mix-eval` (Python, FFmpeg, Beat This! and YAMNet in Docker).

**Spec:**

- [Smart Mix 3 design](2026-11-02-smart-mix-3-design.md), SM3-D3: the v3 planner, and v2 as the API default "until clients migrate".
- [SM5 evaluation](2026-11-07-smart-mix-3-ai-eval-report.md), §3.5 and R2.6: v3 clearly better than v2, measured in the `dj` context only.
- [Smart Mix Stems design](2026-11-08-smart-mix-stems-design.md), STEM-D5, and the [stems evaluation](2026-11-09-smart-mix-stems-evaluation.md), §12.5 and §13: rules revision 5 serves the stems layer to Crate Decks only.
- This plan's decisions, LV-D1–LV-D7 (§2).

## 1. What the code does today

### 1.1 Listen client (`app/listen/src`)

- **Planner:** `lib/smart-mix.ts:17-18` fixes `SMART_MIX_PLANNER_VERSION = "smart-mix-v2"` and policy version 2.
  - Plans are requested only when Smart Mix is available, the native crossfade is on and the server's default `planner_version` is `smart-mix-v2` (`:326-329`). Otherwise every edge gets a local `capability_unavailable` plan with duration 0, which plays gapless.
  - The response is dropped unless its `plannerVersion` is v2 (`:414-418`), and each plan unless its policy version is 2 (`:425-428`).
  - Local fallbacks carry policy version 2 (`:572`). Server plans are copied field by field (`:591-613`), so extra fields such as v3's `structureUsed` never reach the engine.
- **Request context** (`:383-397`):
  - `source` from `transitionSource`: album, playlist, radio, shuffle or manual (`:615-628`);
  - `automatic: true`; `offline: true` when the next three tracks are downloaded (`contexts/player-engine-adapter.ts:352-363`);
  - `preferredDurationMs` clamped to 12 s (`MAX_TRANSITION_DURATION_MS`, `:19`, `:630-633`);
  - `userCueProfile: "default"`; beatmatch and tempo adjustment from the beatmatch opt-in; `allowBassHandoff` and `allowLongBeatmatch` only when on.
- **Batching, caching and fallbacks:**
  - up to 32 edges from the current track (`:182`, `:508-526`);
  - one in-flight request per generation and a settled cache keyed by server, queue revision, edges, source, shuffle, offline, network, duration and the whole capabilities object (`:486-506`);
  - a 1 s deadline, stretched to the slack before the next transition up to 10 s (`:179-181`, `:243-262`);
  - `planner_timeout` and `planner_unavailable` fall back to a crossfade and are retried (`:183-219`, `contexts/player-engine-adapter.ts:276-302`);
  - known album sequences are gapless without a request (`:528-563`), and an offline network gets local `offline` plans (`:340-348`).
- **Capabilities** are read once per app start (`app-shell/AuthenticatedApp.tsx:25-57`): `planner_version` and the four Android flags. `planner_versions` is ignored.
- **Targets:** only `android-native` requests plans (`contexts/player-engine-adapter.ts:281-283`). Web and desktop use `legacySmartTransitionSeconds` (`contexts/player-smart-transition.ts:108`); iOS has no plan executor.
- **Effective modes** (`lib/smart-mix.ts:108-127`, `lib/player-playback-prefs.ts:240-292`): Smart Mix is on by default at 6 s (12 s at most); beatmatch and bass swap are opt-in and off by default; long blends follow beatmatch. **A default Listen user asks for 6 s crossfades without beatmatch.**
- **UI:** `components/player/PlayerMixIndicator.tsx` shows the mode (gapless, crossfade, beatmatch) and the bass swap from the native mix state. Nothing in it depends on the planner.
- **Telemetry:** nothing about transitions reaches the server. The native engine logs QoE lines with the tag `CrateSmartMixQoE` (`NativeMixAudioProcessor.java:45`).

### 1.2 Android native engine (`app/listen/android/app/src/main/java/app/cratemusic/crate`)

- **What it accepts:** `NativeTransitionPlan.fromJson` (`:75-185`) accepts only `plannerVersion` 2 (`:10`, `:90-95`):
  - modes gapless, adaptive and beatmatch;
  - non-negative cues and phase offset; tempo ratio 0.94–1.06; handoff progress and confidence in [0, 1];
  - curves `equal-power` and `dj-blend`; bass handoff `none` and `balanced`.
  - Unknown fields are ignored. An invalid plan is dropped with a log (`CrateNativePlaybackService.java:1766-1777`) and its edge falls back to the crossfade.
- **What it executes** (`NativeMixController.java:215-290`, `NativeMixTrigger.java`): every field above.
  - The outgoing cue, clamped to fit before the end; the incoming cue; the gains; the handoff progress.
  - The duration, with no upper limit: v2's long blends of up to 48 s already play.
  - Beatmatch with a preroll and a phase lock (at least 3 samples within 10 ms); without the lock it is a crossfade at speed 1 on the same cues.
  - `dj-blend` only with beatmatch engaged, as a progress-based envelope that works at any duration (`NativeMixAudioProcessor.java:288-299`); the bass handoff only in beatmatch.
- **v3 plans:** same fields and limits (the v3 policy copies v2's safety limits, `app/crate/smart_mix/policy.py:171-181`), plus `structureUsed`. `stemsUsed`, `candidates`, `policyLimits` and `refinement` are for Crate Decks or AI requests only. **Nothing in a v3 plan is beyond the engine: native execution does not change.**
- **Background planning:** `NativeContinuationClient.java` calls `/api/playback/transition-plans` and `/api/playback/continuation` itself with the constant `PLANNER_VERSION = "smart-mix-v2"` (`:23`, `:312`, `:369`, `:428`) and drops responses of another planner (`:439`). **This is what must change in native code.**

### 1.3 Server (`app/crate`)

- **Plan batch:** accepts `smart-mix-v2 | smart-mix-v3`, v2 by default (`api/schemas/smart_mix.py:11`, `:383-394`). Each plan keeps `plannerVersion: 2`. v3 adds `structureUsed`; without phrases on both tracks it plans as v2, its own plan first (`smart_mix/candidates.py` module docstring).
- **Capabilities** (`api/capabilities.py:77-96`): `planner_version` v2, also `vdj.planner_version`; `planner_versions` lists both; `android_long_beatmatch` equals `android_beatmatch`.
- **Continuation:** accepts only `smart-mix-v2` (`api/schemas/smart_mix.py:686-699`) and plans with the default planner (`api/smart_mix.py:313`). A v3 continuation request gets a 422 today.
- **Cache keys** carry the planner identifier, its rules revision, the request context with its source and, only for edges the stems gate passes, the stem analyses (`api/smart_mix.py:785-830`). v2 and v3, Listen and Decks never share a key.
- **v3 for Listen's sources:** rules revision 5 (ST3.PR6) gives every source but `decks` the revision 1 menu, without stems. Within Listen's limits:
  - a crossfade keeps the preferred duration (12 s at most);
  - a beatmatch lasts the preferred duration or, with `allowLongBeatmatch`, 8, 16 or 32 bars up to 48 s as a `dj-blend`;
  - tempo within ±4 % (6 % hard), and the same gain and true-peak limits as v2.
- **A wasted query:** every v3 batch reads the stem analyses of its tracks (`_applicable_stem_analyses`, `api/smart_mix.py:457-482`) before the source gate drops them. Each Listen batch on v3 would pay it.
- **What v3 does with Listen's context today** (stems evaluation §12, `listen` context: 6 s, beatmatch without long blends; reports in `tools/smart-mix-eval/out/st3/v8r5/` of the ST3 worktree): every mix lasts 6 s, and v3 leaves a mean of 17.6–30.2 s of the outgoing track unplayed and skips 8.6–16.1 s of the incoming one, depending on the set. The v2 numbers for the same context were never measured.

## 2. Decisions

- **LV-D1. A capability made for Listen.** Listen takes its planner from a new field, `smart_mix.listen_planner_version`, and not from `planner_versions`. Servers on `origin/main` today list v3 but reject it in the continuation, and self-hosted servers update on their own schedule. Only a server that announces the field accepts v3 on both endpoints.
- **LV-D2. A server switch.** `CRATE_LISTEN_SMART_MIX_V3_ENABLED`, an environment flag like the Android ones, off by default until LV-G2. LV4 turns it on in production and later flips the code default.
  - **Rollback:** turn it off. Clients go back to v2 at their next capabilities read (app start or login).
  - The plan API keeps accepting v3, so a client still on v3 never fails.
- **LV-D3. The defaults do not move.** `smart_mix.planner_version` and `vdj.planner_version` stay `smart-mix-v2`.
  - An APK released before this plan treats any other default as "Smart Mix unavailable" and plays gapless (`lib/smart-mix.ts:326-337`).
  - VirtualDJ's contract negotiator refuses a planner change.
- **LV-D4. One plan format.** Native execution does not change. The planner travels from JS to native in the continuation config; a missing or unknown value means v2.
- **LV-D5. Plain v3 for Listen.** No stems, AI refinement, candidate menu or set history: Listen relies on rules revision 5 and never sends `aiRefinement`, `localRefinement` or `setHistory`.
- **LV-D6. Measure first.** SM5 compared v3 with v2 only in the `dj` context, and the harness's `listen` context (beatmatch without long blends) matches no real Listen request. LV0 measures Listen's three real contexts on the four v8 sets before any product code.
- **LV-D7. No UI change.** v3 adds no mode, so `PlayerMixIndicator` keeps working, and there is no new copy.

## Global Constraints

- **Repository:** `thecrateapp/crate` (monorepo), following its root `AGENTS.md` and `app/listen/AGENTS.md`. Every Listen change also runs `app/listen-desktop`'s typecheck.
- **Worktrees** under `~/Code/Ninja/crate-agents/`, from `origin/main` once ST3 is merged through ST3.PR6 (rules revision 5). Never `~/Code/Ninja/crate`. Branches `feat/listen-smart-mix-v3-<part>`.
- **Push, PRs, deploys, releases and production access** only when Diego asks. The production switch and the app release are Diego's.
- **Fixed values:**
  - switch `CRATE_LISTEN_SMART_MIX_V3_ENABLED`, false by default until Task 13;
  - capability `smart_mix.listen_planner_version`: one of `planner_versions`, null while Smart Mix is off;
  - planner identifiers `smart-mix-v2` and `smart-mix-v3`; every plan keeps `plannerVersion: 2`;
  - `smart_mix.planner_version` and `vdj.planner_version` stay `smart-mix-v2`;
  - Listen's `preferredDurationMs` stays at 12,000 ms at most.
- **Crate Decks, VirtualDJ and v2 stay byte-identical:** no change under `app/crate/smart_mix/` (planner, candidates, policy), and the golden fixtures in `app/tests/fixtures/smart_mix/` stay as they are.
- **Resources** (`~/Code/Ninja/crate-agents/handoffs/2026-11-09/crash-1129/README.md`):
  - `free -g` with at least 6 GB available before a heavy job;
  - one `listen_metrics.py` at a time, `--jobs 4`, with `TMPDIR` and outputs on disk (`~/Code/Ninja/crate-agents/scratch/listen-v3/` and the worktree's ignored `out/`), never audio in `/tmp`;
  - Gradle builds under the shared lock `flock ~/Code/Ninja/crate-agents/locks/cxx-build.lock` with `--max-workers=4`;
  - stop what you start.
- **Tests:** focused files only, never full suites.
  - Python: `cd app && ../.venv/bin/python -m pytest <files> -q`, with the worktree's virtualenv set up as `docs/technical/00b-development-setup.md` says.
  - Listen: `cd app/listen && npx vitest run <files>`, then `npm run --workspace=app/listen typecheck`, `npm run --workspace=app/listen lint` and `npm run --workspace=app/listen-desktop typecheck`.
  - Android: `cd app/listen/android && ./gradlew :app:testDebugUnitTest --tests '<class>' --no-daemon --max-workers=4`. It needs JDK 21 and Android SDK 36, which this laptop does not have today (Task 10, step 3), or CI.
- **Commits:** conventional commits without AI attribution, with the why in the body.

## Review Focus

1. **A new APK on a server that lists v3 without `listen_planner_version`** (`origin/main` today, self-hosted installs): it must plan with v2 and never send v3 to `/api/playback/continuation`, which answers 422 there. Tests in Tasks 6, 8 and 9.
2. **The switch changes between two capabilities reads:** the queue is replanned and the native continuation follows the new planner. Tests in Tasks 7 and 8.
3. **An old APK against a server with the switch on:** its v2 requests and continuations are answered as before. Tests in Tasks 3 and 4.
4. **A beatmatch user gets a long v3 phrase blend** (more than 12 s, `dj-blend`): the client keeps it intact and the native engine accepts it. Tests in Tasks 7 and 9.
5. **Album playback** stays gapless under v3. Test in Task 7.

## Phase map

```mermaid
flowchart TD
  ST3[ST3.PR6 merged: stems for Crate Decks only] --> LV0
  LV0[LV0 · Measure v2 against v3 in Listen's contexts] --> G1{{LV-G1 · offline go}}
  G1 --> LV1[LV1 · Server: capability, switch, v3 continuation]
  LV1 --> LV2[LV2 · Listen and Android: the recommended planner]
  LV2 --> LV3[LV3 · Phone listening build]
  LV3 --> G2{{LV-G2 · default-on}}
  G2 --> LV4[LV4 · Release, switch on, default on]
```

| Phase | Result | PRs | Size |
| --- | --- | --- | --- |
| LV0 | Listen's contexts in the harness; report with v2 against v3, latency and a listening list | 1 (harness) + report here | 1–2 days, mostly compute |
| LV1 | Capability, switch, v3 continuation, no stem reads for Listen | 1 | 1 day |
| LV2 | Listen and Android use the recommended planner | 1 | 2 days |
| LV3 | Debug APK on Diego's phone against the dev stack | none | 1 day plus Diego's session |
| LV4 | Releases, production switch, code default | 1 | Diego's schedule |

---

## LV0 · Measure v2 against v3 in Listen's contexts

### Task 1: Listen's real contexts in the harness

**Files:**

- Modify: `tools/smart-mix-eval/run_eval.py:180-206` (`CONTEXTS`)
- Test: `tools/smart-mix-eval/tests/test_run_eval.py`

**Interfaces:**

- Produces: `CONTEXTS["listen-crossfade"]`, `CONTEXTS["listen-beatmatch"]` and `CONTEXTS["listen-bass"]`, usable as `run_eval.py --context <name>`. `CONTEXTS["listen"]` stays as it is, for comparison with the ST3 reports.

- [ ] **Step 1: Write the failing tests**

```python
def test_listen_contexts_mirror_listen_requests() -> None:
    crossfade = CONTEXTS["listen-crossfade"]
    # What Listen sends by default: 6 s, no beatmatch (an opt-in).
    assert crossfade == TransitionContext(
        source="playlist",
        automatic=True,
        offline=False,
        preferred_duration_ms=6_000,
        user_cue_profile="default",
        allow_beatmatch=False,
        allow_tempo_adjustment=False,
    )
    # Beatmatch on: Listen also allows long blends (android_long_beatmatch).
    assert CONTEXTS["listen-beatmatch"] == replace(
        crossfade,
        allow_beatmatch=True,
        allow_tempo_adjustment=True,
        allow_long_beatmatch=True,
    )
    assert CONTEXTS["listen-bass"] == replace(
        CONTEXTS["listen-beatmatch"], allow_bass_handoff=True
    )
    assert CONTEXTS["listen"].allow_beatmatch
    assert not CONTEXTS["listen"].allow_long_beatmatch


@pytest.mark.parametrize(
    "context", ["listen-crossfade", "listen-beatmatch", "listen-bass"]
)
def test_listen_contexts_get_no_stems(context: str) -> None:
    ...
```

The body of `test_listen_contexts_get_no_stems` is that of `test_the_dj_context_is_crate_decks_and_listen_gets_no_stems` with `CONTEXTS[context]` in place of `CONTEXTS["listen"]`: with stems on both tracks and in-family priors, `stemsUsed` and `stemsGate` are false and the plan equals the plan without stem analyses.

- [ ] **Step 2: Run them and check they fail**

Run: `cd app && ../.venv/bin/python -m pytest ../tools/smart-mix-eval/tests/test_run_eval.py -q -k listen`
Expected: FAIL with `KeyError: 'listen-crossfade'`.

- [ ] **Step 3: Add the three contexts to `CONTEXTS`**, with a comment each saying which Listen settings they stand for.

- [ ] **Step 4: Run the tests and check they pass**

Same command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/smart-mix-eval/run_eval.py tools/smart-mix-eval/tests/test_run_eval.py
git commit -m "feat(smart-mix-eval): add Listen's real request contexts"
```

### Task 2: Run the comparison and write the report

No product code. The output is a report in this repository and a listening list.

**Inputs:**

- Sets and audio roots: `~/Code/Ninja/crate-agents/handoffs/2026-11-09/eval-v8-data.md` §3. Datasets in `~/Code/Ninja/crate-agents/eval-v8/sets/<set>/dataset.jsonl`, for the sets `electronic`, `phrase`, `industrial` and `metalpunk`.
- Database: `postgresql://crate:crate@127.0.0.1:55473/crate_eval_v8` (container `sm5-eval-postgres`), read-only.
- Stems, only to prove that Listen gets none: `stems-st3/tools/smart-mix-eval/out/st3/stems/{electronic,spike}` and `eval-v8/stems/{industrial,metalpunk}`, mounted read-only.

- [ ] **Step 1: Pre-flight.** Run `free -g` (at least 6 GB available) and check with `docker ps` that no other `listen_metrics.py` is running and that `sm5-eval-postgres` is up.

- [ ] **Step 2: Plan every set in every context with both planners**, from the Task 1 worktree, with the README's Docker form (`crate-analysis-worker:v8-3965fa86`, the worktree mounted at `/workspace`, the sets at `/eval-v8:ro`):

```bash
for S in electronic phrase industrial metalpunk; do
  for C in listen-crossfade listen-beatmatch listen-bass; do
    python run_eval.py --dataset /eval-v8/sets/$S/dataset.jsonl --planner smart-mix-v2 \
      --context $C --output out/listen-v3/$S/$C/v2
    python run_eval.py --dataset /eval-v8/sets/$S/dataset.jsonl --planner smart-mix-v3 \
      --context $C --stems <stems of $S> --output out/listen-v3/$S/$C/v3 \
      --compare out/listen-v3/$S/$C/v2/report.json
  done
done
```

- [ ] **Step 3: Check that no Listen plan used stems.**

Run: `jq '[.edges[] | select(.stemsUsed == true)] | length' out/listen-v3/*/*/v3/report.json`
Expected: `0` for each of the 12 reports. Any other value: stop, rules revision 5 is not in the tree.

- [ ] **Step 4: Listening metrics, one set at a time**, in `crate-stems-analysis:pr2` with `TMPDIR=~/Code/Ninja/crate-agents/scratch/listen-v3/tmp`:

```bash
python listen_metrics.py \
  --plans v2-crossfade=out/listen-v3/$S/listen-crossfade/v2/report.json \
  --plans v3-crossfade=out/listen-v3/$S/listen-crossfade/v3/report.json \
  --plans v2-beatmatch=out/listen-v3/$S/listen-beatmatch/v2/report.json \
  --plans v3-beatmatch=out/listen-v3/$S/listen-beatmatch/v3/report.json \
  --plans v2-bass=out/listen-v3/$S/listen-bass/v2/report.json \
  --plans v3-bass=out/listen-v3/$S/listen-bass/v3/report.json \
  --compare v3-crossfade:v2-crossfade --compare v3-beatmatch:v2-beatmatch \
  --compare v3-bass:v2-bass \
  --music-root /music=<audio root of $S> --output out/listen-v3/$S/listen --jobs 4
```

- [ ] **Step 5: Latency of a Listen batch.** On a dev stack with v8 profiles on at least 33 tracks (the one in `~/Code/Ninja/crate-agents/listening/android-local/`), with a script kept in `~/Code/Ninja/crate-agents/scratch/listen-v3/` and not committed:
  - post a 32-edge batch in the `listen-crossfade` context, 20 times per planner, each time with a `preferredDurationMs` not used before (6001, 6002…), so that every request misses the plan cache; then 20 times with one value, cached;
  - log in as the dev test user and never print the session;
  - record p50 and p95 per planner, cached and uncached.

- [ ] **Step 6: Write the report** `docs/plans/2026-11-<dd>-listen-smart-mix-v3-evaluation.md` in this repository, in the format of the SM5 and stems reports. Per set and context:
  - profile metrics: beatmatch rate; share of mixes longer than 12 s; mean duration; mean, median, p90 and maximum of unplayed outgoing and skipped incoming music; phrase alignment;
  - listening metrics: means, then v3 against v2 on paired edges (beat clash, voice on voice, bass on bass, loudness step, loudness dip; net on SM5's four; sign test; clearly worse edges);
  - the latency of step 5 against the design budget (uncached p95 under 150 ms) and Listen's deadline floor (1 s).

- [ ] **Step 7: Prepare the listening list.** At most 40 edges, rendered as A (v2) and B (v3) with `render_pair.py --report … --edge …` into `~/Code/Ninja/crate-agents/listening/listen-v3/`, with `playlist.m3u8` and a README in Spanish like `stem-g1-v8/`:
  - per context, the 5 edges where v3's loudness dip is deepest compared with v2;
  - the 5 with the most skipped intro and the 5 with the most unplayed outro;
  - in the beatmatch contexts, 5 long blends;
  - 3 unchanged controls.

- [ ] **Step 8: Commit the report** (this repository, only that file)

```bash
git commit -m "docs(plans): measure Smart Mix v3 in Listen's contexts" -- docs/plans/2026-11-<dd>-listen-smart-mix-v3-evaluation.md
```

**Gate LV-G1 (Diego): whether v3 is good enough for Listen offline.** The agent stops and presents the report, the listening list and this proposed reading:

- **Go** when, in each of the three contexts and on each of the four sets, v3 is not significantly worse than v2 (sign test p < 0.1) on net or on the loudness dip, and Diego accepts the unplayed and skipped distributions.
- **Stop** otherwise. A fix to v3 for non-Decks sources (a dip term, a cap on skipped music) gets its own plan and its own measurement; it is not added here.
- The uncached p95 for 32 edges must stay under 1 s. Above the 150 ms budget, the report says by how much.

---

## LV1 · Server: capability, switch and v3 continuation

One PR: `feat(smart-mix): let Listen opt into smart-mix-v3`.

### Task 3: `listen_planner_version` behind the switch

**Files:**

- Modify: `app/crate/config.py:61-66`, `app/crate/api/schemas/capabilities.py:51-72`, `app/crate/api/capabilities.py:77-96`
- Modify: `docs/technical/smart-mix-operations.md`, the flags paragraph after the consumer table (about line 1478) and the section "Planner smart-mix-v3" (line 1291)
- Test: `app/tests/test_capabilities_smart_mix_v3.py`

**Interfaces:**

- Produces: `crate.config.listen_smart_mix_v3_enabled() -> bool`, reading `CRATE_LISTEN_SMART_MIX_V3_ENABLED` with default false.
- Produces: `SmartMixCapabilities.listen_planner_version: str | None` in the JSON of `/api/capabilities`, consumed by Task 6.

- [ ] **Step 1: Write the failing tests**, with the file's `smart_mix_on` fixture and `_structure_snapshot(monkeypatch, None)`:

```python
def test_listen_planner_defaults_to_v2(test_app, monkeypatch, smart_mix_on) -> None:
    monkeypatch.delenv("CRATE_LISTEN_SMART_MIX_V3_ENABLED", raising=False)
    smart_mix = test_app.get("/api/capabilities").json()["smart_mix"]
    assert smart_mix["listen_planner_version"] == "smart-mix-v2"


def test_listen_planner_v3_behind_the_switch(test_app, monkeypatch, smart_mix_on) -> None:
    monkeypatch.setenv("CRATE_LISTEN_SMART_MIX_V3_ENABLED", "true")
    body = test_app.get("/api/capabilities").json()
    assert body["smart_mix"]["listen_planner_version"] == "smart-mix-v3"
    assert "smart-mix-v3" in body["smart_mix"]["planner_versions"]
    # Older APKs and VirtualDJ read these two: they never move (LV-D3).
    assert body["smart_mix"]["planner_version"] == "smart-mix-v2"
    assert body["vdj"]["planner_version"] == "smart-mix-v2"


def test_no_listen_planner_while_smart_mix_is_off(test_app, monkeypatch) -> None:
    monkeypatch.delenv("CRATE_SMART_MIX_ENABLED", raising=False)
    monkeypatch.setenv("CRATE_LISTEN_SMART_MIX_V3_ENABLED", "true")
    smart_mix = test_app.get("/api/capabilities").json()["smart_mix"]
    assert smart_mix["listen_planner_version"] is None
```

- [ ] **Step 2: Run them and check they fail**

Run: `cd app && ../.venv/bin/python -m pytest tests/test_capabilities_smart_mix_v3.py -q`
Expected: FAIL with `KeyError: 'listen_planner_version'`.

- [ ] **Step 3: Implement.** In `config.py`, `listen_smart_mix_v3_enabled()`. In the schema, the field with this description: "Planner Listen's native engine asks for, in its plan batches and its background continuation: smart-mix-v3 with CRATE_LISTEN_SMART_MIX_V3_ENABLED, smart-mix-v2 otherwise. Always one of planner_versions; null while Smart Mix is off." In `capabilities.py`, `_listen_planner(available: bool) -> str | None`, which returns `PLANNER_POLICY_V3.identifier` or `PLANNER_IDENTIFIER`. In the operations doc, the switch beside the Android flags (default, effect, rollback) and one line in "Planner smart-mix-v3" on how Listen chooses it.

- [ ] **Step 4: Run the tests and check they pass**

Run: `cd app && ../.venv/bin/python -m pytest tests/test_capabilities_smart_mix_v3.py tests/test_capabilities.py tests/test_capabilities_decks.py -q`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/crate/config.py app/crate/api/schemas/capabilities.py app/crate/api/capabilities.py \
  app/tests/test_capabilities_smart_mix_v3.py docs/technical/smart-mix-operations.md
git commit -m "feat(smart-mix): recommend a planner to Listen behind a switch"
```

### Task 4: The continuation plans with the planner it is asked for

**Files:**

- Modify: `app/crate/api/schemas/smart_mix.py:686-699` (`PlaybackContinuationRequest.planner_version`)
- Modify: `app/crate/api/smart_mix.py:313` (`playback_continuation`)
- Test: `app/tests/test_playback_continuation_api.py`, `app/tests/test_smart_mix_openapi.py`

**Interfaces:**

- Produces: `POST /api/playback/continuation` accepts `plannerVersion: "smart-mix-v2" | "smart-mix-v3"` (still required) and plans its edges with it. The response shape does not change. Consumed by Task 9.

- [ ] **Step 1: Write the failing tests**, with the file's helpers (`_no_planner_io`, `_continuation_user`, `_radio_row`, `_CONTEXT`). With no profiles, a v3 plan carries `structureUsed: false` and a v2 plan does not carry the key, which tells the planners apart:

```python
@pytest.mark.parametrize(
    ("planner", "structure_key"), [("smart-mix-v2", False), ("smart-mix-v3", True)]
)
def test_continuation_plans_with_the_requested_planner(
    test_app, monkeypatch, planner, structure_key
):
    ...  # as test_continuation_returns_native_tracks_and_chained_plans
    assert response.status_code == 200
    assert all(
        ("structureUsed" in plan) is structure_key for plan in response.json()["plans"]
    )
    assert all(plan["plannerVersion"] == 2 for plan in response.json()["plans"])


def test_continuation_rejects_an_unknown_planner(test_app, monkeypatch):
    ...  # same request with "plannerVersion": "smart-mix-v9"
    assert response.status_code == 422
```

In `test_smart_mix_openapi.py`, extend `test_smart_mix_openapi_offers_planner_v3`: the `plannerVersion` of `PlaybackContinuationRequest` lists both planners.

- [ ] **Step 2: Run them and check they fail**

Run: `cd app && ../.venv/bin/python -m pytest tests/test_playback_continuation_api.py tests/test_smart_mix_openapi.py -q`
Expected: FAIL. The v3 case gets a 422, and the OpenAPI enum has only v2.

- [ ] **Step 3: Implement.** `planner_version: PlannerIdentifier = Field(alias="plannerVersion")`, and `_plan_transition_edges(edges, planner_version=payload.planner_version)`.

- [ ] **Step 4: Run the tests and check they pass**

Same command, plus `tests/test_playback_continuation_session_ids.py tests/test_radio_continuation_contract.py`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/crate/api/schemas/smart_mix.py app/crate/api/smart_mix.py \
  app/tests/test_playback_continuation_api.py app/tests/test_smart_mix_openapi.py
git commit -m "feat(smart-mix): plan native continuations with the requested planner"
```

### Task 5: No stem reads for batches the layer does not serve

**Files:**

- Modify: `app/crate/api/smart_mix.py:381-396` (`_plan_transition_edges`) and `:457-482` (`_applicable_stem_analyses`)
- Test: `app/tests/test_smart_mix_api_stems.py`

**Interfaces:**

- Produces: `_applicable_stem_analyses(entity_uids, profiles, policy, *, edges: list[TransitionEdgeRequest]) -> dict[str, StemAnalysis]`, which returns `{}` without a query unless some edge's `context.source` is in `policy.stems_layer_sources`.

- [ ] **Step 1: Write the failing test and extend the guard**, with the file's `batch` fixture, which records every stem read in `batch["stem_reads"]`:

```python
LISTEN_SOURCES = ["playlist", "radio", "shuffle", "manual", "infinite"]


@pytest.mark.parametrize("source", LISTEN_SOURCES)
def test_listen_batches_read_no_stem_analyses(batch, source) -> None:
    batch["analyses"] = {uid: [_analysis(uid)] for uid in REVISIONS}
    context = {**_edge()["context"], "source": source}

    plan = batch["post_edge"]({**_edge(), "context": context})

    assert batch["stem_reads"] == []
    assert "stemsUsed" not in plan
```

Then parametrize `test_only_crate_decks_gets_the_stems_layer` with `LISTEN_SOURCES`: it covers playlist, radio and infinite today, and Listen also sends shuffle and manual. This guard passes at once: it pins rules revision 5 for every Listen source.

- [ ] **Step 2: Run them and check the new test fails**

Run: `cd app && ../.venv/bin/python -m pytest tests/test_smart_mix_api_stems.py -q -k "listen_batches or only_crate_decks"`
Expected: `test_listen_batches_read_no_stem_analyses` FAILS (one stem read per batch); the guard passes.

- [ ] **Step 3: Implement** the `edges` keyword and its early return.

- [ ] **Step 4: Run the tests and check they pass, and that the goldens are untouched**

Run: `cd app && ../.venv/bin/python -m pytest tests/test_smart_mix_api_stems.py tests/test_smart_mix_api.py tests/test_candidates_stems.py -q && git diff --exit-code origin/main -- app/tests/fixtures/smart_mix/ app/crate/smart_mix/`
Expected: PASS and an empty diff.

- [ ] **Step 5: Commit**

```bash
git add app/crate/api/smart_mix.py app/tests/test_smart_mix_api_stems.py
git commit -m "perf(smart-mix): skip stem reads for batches the layer does not serve"
```

---

## LV2 · Listen and Android: the recommended planner

One PR: `feat(listen): plan Smart Mix with the planner the server recommends`.

### Task 6: Capabilities carry the planners

**Files:**

- Modify: `app/listen/src/lib/smart-mix.ts:44-79` (`SmartMixCapabilities`, `DISABLED_SMART_MIX_CAPABILITIES`, `setSmartMixCapabilities`)
- Modify: `app/listen/src/app-shell/AuthenticatedApp.tsx:14-57`
- Modify: `app/listen/src/contexts/player-engine-adapter.ts:338-350` (`sameCapabilities`)
- Test: `app/listen/src/lib/smart-mix.test.ts`, `app/listen/src/app-shell/AuthenticatedApp.test.tsx`

**Interfaces:**

- Produces in `lib/smart-mix.ts`:
  - `export type SmartMixPlannerIdentifier = "smart-mix-v2" | "smart-mix-v3"`;
  - `SmartMixCapabilities.plannerVersions?: string[]` and `listenPlannerVersion?: string | null`, normalised by `setSmartMixCapabilities` to `[]` and `null`;
  - `export function resolvePlannerVersion(capabilities: SmartMixCapabilities): SmartMixPlannerIdentifier | null`, which returns `listenPlannerVersion` when the client supports it and `plannerVersions` lists it, otherwise `plannerVersion` when the client supports it, otherwise null.

- [ ] **Step 1: Write the failing tests.** In `smart-mix.test.ts`, with `V3_CAPABILITIES = { ...ENABLED_CAPABILITIES, plannerVersions: ["smart-mix-v2", "smart-mix-v3"], listenPlannerVersion: "smart-mix-v3" }`:

```ts
describe("resolvePlannerVersion", () => {
  it("uses the planner the server recommends for Listen", () => {
    expect(resolvePlannerVersion(V3_CAPABILITIES)).toBe("smart-mix-v3");
  });
  it("stays on smart-mix-v2 on a server without a Listen recommendation", () => {
    expect(
      resolvePlannerVersion({
        ...ENABLED_CAPABILITIES,
        plannerVersions: ["smart-mix-v2", "smart-mix-v3"],
      }),
    ).toBe("smart-mix-v2");
  });
  it("ignores a recommendation the server does not list or the client does not know", () => {
    expect(
      resolvePlannerVersion({ ...V3_CAPABILITIES, plannerVersions: ["smart-mix-v2"] }),
    ).toBe("smart-mix-v2");
    expect(
      resolvePlannerVersion({
        ...V3_CAPABILITIES,
        plannerVersions: ["smart-mix-v2", "smart-mix-v9"],
        listenPlannerVersion: "smart-mix-v9",
      }),
    ).toBe("smart-mix-v2");
  });
  it("has no planner while Smart Mix is off", () => {
    expect(
      resolvePlannerVersion({ ...ENABLED_CAPABILITIES, plannerVersion: null }),
    ).toBeNull();
  });
});
```

In `AuthenticatedApp.test.tsx`, with its API mock: `planner_versions` and `listen_planner_version` reach `setAndroidNativeSmartMixCapabilities` as `plannerVersions` and `listenPlannerVersion`; a response without them gives `[]` and `null`.

- [ ] **Step 2: Run them and check they fail**

Run: `cd app/listen && npx vitest run src/lib/smart-mix.test.ts src/app-shell/AuthenticatedApp.test.tsx`
Expected: FAIL. `resolvePlannerVersion` is not exported, and the new fields are missing.

- [ ] **Step 3: Implement** the type, the fields, `resolvePlannerVersion`, the parsing in `AuthenticatedApp.tsx` (also in its `catch` branch) and the two comparisons in `sameCapabilities`.

- [ ] **Step 4: Run the tests and check they pass**

Same command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/listen/src/lib/smart-mix.ts app/listen/src/lib/smart-mix.test.ts \
  app/listen/src/app-shell/AuthenticatedApp.tsx app/listen/src/app-shell/AuthenticatedApp.test.tsx \
  app/listen/src/contexts/player-engine-adapter.ts
git commit -m "feat(listen): read the planner the server recommends for Listen"
```

### Task 7: Plan batches use the resolved planner

**Files:**

- Modify: `app/listen/src/lib/smart-mix.ts:17`, `:148-170`, `:326-329`, `:379`, `:414-418`
- Test: `app/listen/src/lib/smart-mix.test.ts`

**Interfaces:**

- Consumes: `resolvePlannerVersion` (Task 6).
- Produces: `TransitionPlanBatchResponse.plannerVersion: SmartMixPlannerIdentifier`; `TransitionPlanResponse.structureUsed?: boolean`, never copied into `EngineTransitionPlan`. The constant `SMART_MIX_PLANNER_VERSION` disappears; `SMART_MIX_PLANNER_POLICY_VERSION` (2) stays.

- [ ] **Step 1: Write the failing tests**, with a local helper `planInput(capabilities)` that returns the input of `requests only current→N+1 and N+1→N+2` with `currentIndex: 0`:

```ts
it("requests smart-mix-v3 when the server recommends it and installs its plans", async () => {
  const request = vi.fn().mockResolvedValue({
    plannerVersion: "smart-mix-v3",
    plans: [{ ...serverPlan(TRACKS[0]!, TRACKS[1]!), structureUsed: true }],
  });
  const plans = await new SmartMixTransitionPlanner(request).plan(planInput(V3_CAPABILITIES));
  expect(request.mock.calls[0]![2]).toMatchObject({ plannerVersion: "smart-mix-v3" });
  expect(plans[0]).toMatchObject({ plannerVersion: 2, durationMs: 4000, outgoingCueMs: 176000 });
  expect(plans[0]).not.toHaveProperty("structureUsed");
  expect(plans[0]!.fallbackReason).toBeUndefined();
});

it("drops a response from another planner than the one it asked for", async () => {
  // Asked smart-mix-v3, answered smart-mix-v2.
  // Every requested edge: fallbackReason "missing_profile", confidence 0.
});

it("a planner change starts a new planning generation", async () => {
  // Same input with ENABLED_CAPABILITIES, then with V3_CAPABILITIES.
  // Two requests; the second asks for smart-mix-v3.
});

it("keeps known album sequences gapless with smart-mix-v3", async () => {
  // A guard that passes at once. Album source, same album and artist:
  // no request, mode "gapless", fallbackReason "album_sequence".
});

it.each(["smart-mix-v2", "smart-mix-v3"] as const)(
  "keeps a %s long beatmatch dj-blend intact",
  async (planner) => {
    // Plan: mode "beatmatch", durationMs 32_000, curve "dj-blend",
    // incomingTempoRatio 1.02, beatPhaseOffsetMs 120.
    // Installed with the same values: nothing clamps a server plan to 12 s.
  },
);
```

Update the existing literals that reach the request so that the v2 tests still describe a v2 server.

- [ ] **Step 2: Run them and check they fail**

Run: `cd app/listen && npx vitest run src/lib/smart-mix.test.ts`
Expected: FAIL. The client still asks for `smart-mix-v2` and drops the v3 response.

- [ ] **Step 3: Implement.** `capabilityAvailable` requires `resolvePlannerVersion(input.capabilities) !== null`; the request body and the response check use the resolved identifier; the per-plan check stays at policy version 2.

- [ ] **Step 4: Run the tests and check they pass**

Run: `cd app/listen && npx vitest run src/lib/smart-mix.test.ts src/contexts/player-engine-adapter.test.ts`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/listen/src/lib/smart-mix.ts app/listen/src/lib/smart-mix.test.ts
git commit -m "feat(listen): plan Smart Mix with the recommended planner"
```

### Task 8: The native continuation context carries the planner

**Files:**

- Modify: `app/listen/src/lib/playback-engine.ts:87-95` (`EngineContinuationContext`)
- Modify: `app/listen/src/contexts/player-engine-adapter.ts:321-333`
- Modify: `app/listen/src/lib/android-native-engine.ts:82-100` (the `setContinuation` options), `:187-205` (`continuationWithModes`), `:616-637` (`configureContinuation`)
- Test: `app/listen/src/contexts/player-engine-adapter.test.ts`, `app/listen/src/lib/android-native-engine.test.ts`

**Interfaces:**

- Consumes: `resolvePlannerVersion` and `SmartMixPlannerIdentifier` (Task 6).
- Produces: `EngineContinuationContext.plannerVersion: SmartMixPlannerIdentifier`, set to `resolvePlannerVersion(capabilities) ?? "smart-mix-v2"`, and sent as `plannerVersion` in `setContinuation`. Consumed by Task 9.

- [ ] **Step 1: Write the failing tests**

```ts
// player-engine-adapter.test.ts
it("hands the resolved planner to the native continuation", async () => {
  // Capabilities resolving to smart-mix-v3, target "android-native".
  expect(snapshot.continuation?.plannerVersion).toBe("smart-mix-v3");
});
it("hands smart-mix-v2 to the native continuation without a Listen recommendation", async () => {
  expect(snapshot.continuation?.plannerVersion).toBe("smart-mix-v2");
});

// android-native-engine.test.ts
it("continuationWithModes follows a planner change", () => {
  const context = { ...continuationContext, plannerVersion: "smart-mix-v3" as const };
  expect(continuationWithModes(context, capabilitiesResolvingV2)).toEqual({
    ...context,
    plannerVersion: "smart-mix-v2",
  });
  expect(continuationWithModes(context, capabilitiesResolvingV3)).toBeNull();
});
it("configures the native continuation with the planner", async () => {
  expect(nativePlaybackMock.setContinuation).toHaveBeenCalledWith(
    expect.objectContaining({ plannerVersion: "smart-mix-v3" }),
  );
});
```

The two capabilities objects of the third test keep the context's beatmatch, bass and long modes, so only the planner differs. Add `plannerVersion` to the existing `setContinuation` expectations (`android-native-engine.test.ts:832`, `:884`, `:920`).

- [ ] **Step 2: Run them and check they fail**

Run: `cd app/listen && npx vitest run src/contexts/player-engine-adapter.test.ts src/lib/android-native-engine.test.ts`
Expected: FAIL. The context has no `plannerVersion`.

- [ ] **Step 3: Implement.** Set the field in `toStartupEngineQueueSnapshot`; compare it in `continuationWithModes` and return the updated context when it changes; pass it in `configureContinuation`; add it to the plugin's option type.

- [ ] **Step 4: Run the tests and the checks**

Run: `cd app/listen && npx vitest run src/contexts/player-engine-adapter.test.ts src/lib/android-native-engine.test.ts src/lib/android-native-engine.mix-state.test.ts src/lib/smart-mix.test.ts && cd ../.. && npm run --workspace=app/listen typecheck && npm run --workspace=app/listen lint && npm run --workspace=app/listen-desktop typecheck`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/listen/src/lib/playback-engine.ts app/listen/src/contexts/player-engine-adapter.ts \
  app/listen/src/contexts/player-engine-adapter.test.ts app/listen/src/lib/android-native-engine.ts \
  app/listen/src/lib/android-native-engine.test.ts
git commit -m "feat(listen): hand the planner to the native continuation"
```

### Task 9: Native background planning asks for the configured planner

**Files:**

- Modify: `app/listen/android/app/src/main/java/app/cratemusic/crate/NativeContinuationClient.java`: `:23`, `Config` (`:30-135`), `continuationBody` (`:299-321`), `parseContinuation` (`:324-385`), `requestBody` (`:405-430`), `parsePlans` (`:432-460`)
- Test: `app/listen/android/app/src/test/java/app/cratemusic/crate/NativeContinuationClientTest.java`, `NativeTransitionPlanTest.java`

**Interfaces:**

- Consumes: the `plannerVersion` option of `setContinuation` (Task 8), which reaches `Config.fromJson` through `CrateNativePlaybackPlugin.java:318` unchanged.
- Produces:
  - `DEFAULT_PLANNER_VERSION = "smart-mix-v2"`, in place of `PLANNER_VERSION`;
  - `Config.plannerVersion` (`final String`), the last parameter of the full constructor; the shorter constructors pass the default;
  - `static String supportedPlanner(@Nullable String requested)`, which returns `smart-mix-v2` or `smart-mix-v3` and the default for anything else;
  - `static List<JSONObject> parsePlans(String body, String plannerVersion, List<NativeTrack> tracks, int fromIndex, int edgeCount)`.

- [ ] **Step 1: Write the failing tests**

```java
@Test
public void requestsTheConfiguredPlanner() throws Exception {
    NativeContinuationClient.Config config = NativeContinuationClient.Config.fromJson(
        new JSONObject("{\"token\":\"crv_x\",\"plannerVersion\":\"smart-mix-v3\"}"));
    assertEquals("smart-mix-v3",
        NativeContinuationClient.requestBody(config, tracks, 0, 4).getString("plannerVersion"));
    assertEquals("smart-mix-v3",
        NativeContinuationClient.continuationBody(config, tracks, 10).getString("plannerVersion"));
}

@Test
public void fallsBackToV2ForAMissingOrUnknownPlanner() throws Exception {
    for (String payload : new String[] {
        "{\"token\":\"crv_x\"}",
        "{\"token\":\"crv_x\",\"plannerVersion\":\"smart-mix-v9\"}",
    }) {
        assertEquals("smart-mix-v2",
            NativeContinuationClient.Config.fromJson(new JSONObject(payload)).plannerVersion);
    }
}

@Test
public void acceptsOnlyTheResponseOfTheRequestedPlanner() throws Exception {
    // With the plan JSON of mapsServerPlansBackToRuntimeTrackIds:
    // a v2 response parsed for smart-mix-v3 gives no plans; a v3 response gives one.
}

@Test
public void installsTheContinuationPlansOfTheConfiguredPlanner() throws Exception {
    // parseContinuation with a v3 config and one plan: plans.size() == 1.
}
```

In `NativeTransitionPlanTest.java`, a guard that passes at once: `acceptsAV3LongBlendWithExtraFields`. A payload with `plannerVersion` 2, mode beatmatch, `durationMs` 32000, curve `dj-blend`, `structureUsed` true and `stemsUsed` true parses, keeps 32000 ms, and `isDjBlend()` is true. Update `ignoresAResponseFromAnotherPlannerVersion` to the new `parsePlans` signature.

- [ ] **Step 2: Run them and check they fail**

Run: `cd app/listen/android && flock ~/Code/Ninja/crate-agents/locks/cxx-build.lock ./gradlew :app:testDebugUnitTest --tests 'app.cratemusic.crate.NativeContinuationClientTest' --tests 'app.cratemusic.crate.NativeTransitionPlanTest' --no-daemon --max-workers=4`
Expected: compilation FAILS on `plannerVersion` and the new `parsePlans` signature.

- [ ] **Step 3: Implement** the field, `supportedPlanner`, its use in `fromJson`, the three request bodies and the response check. `parseContinuation` wraps the plans with `config.plannerVersion`.

- [ ] **Step 4: Run the tests and check they pass**

The same command, plus `--tests 'app.cratemusic.crate.NativeContinuationServiceContractTest'`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/listen/android/app/src/main/java/app/cratemusic/crate/NativeContinuationClient.java \
  app/listen/android/app/src/test/java/app/cratemusic/crate/NativeContinuationClientTest.java \
  app/listen/android/app/src/test/java/app/cratemusic/crate/NativeTransitionPlanTest.java
git commit -m "feat(listen): ask the configured planner in native background planning"
```

---

## LV3 · Phone listening build

Nothing here is committed. Everything lives under `~/Code/Ninja/crate-agents/listening/`.

### Task 10: Phone access, listening build and dev stack

- [ ] **Step 1: Ask Diego** which server the phone uses (decision D4): the local dev stack through `adb reverse` (the default, `listening/android-local/README.md`), or a test server he names. Never production.

- [ ] **Step 2: Make the phone reachable.** On 2026-11-09 `adb devices -l` was empty and `lsusb` did not list the phone.
  - **USB:** a data cable and another port; on the phone, USB mode "File transfer", developer options and USB debugging on, and the RSA prompt accepted. When `lsusb` lists it and adb does not, the udev rules need `sudo`: ask Diego.
  - **Without USB (Android 11 or later, same Wi-Fi):** Developer options → Wireless debugging → Pair device with pairing code. Then `adb pair <ip>:<pairing port> <code>` and `adb connect <ip>:<port>`. `adb reverse tcp:8585 tcp:8585` works the same way, and the API stays on `localhost`.
  - Expected: `adb devices -l` lists the phone.

- [ ] **Step 3: Toolchain.** JDK 21 and the Android SDK (platform 36, build-tools, platform-tools) in user space under `~/Android`, without `sudo`. About 3 GB: only with Diego's approval. `adb` is already in `/usr/bin`.

- [ ] **Step 4: Build "Crate DBG" from the LV2 branch**, with `free -g` checked first:

```bash
cd app/listen && NODE_ENV=development VITE_ALLOW_INSECURE_LOOPBACK=true \
  VITE_CRATE_OAUTH_SCHEME=cratemusic-dbg npx vite build --mode capacitor && npx cap sync android
cd android && flock ~/Code/Ninja/crate-agents/locks/cxx-build.lock \
  ./gradlew assembleDebug --no-daemon --max-workers=4
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

`VITE_CRATE_SMART_MIX_LOCAL_TEST` stays unset: it would force the crossfade length and hide the user's settings.
Expected: the server screen of "Crate DBG" offers "Desarrollo local" and accepts `http://localhost:8585`. If it does not, the build did not get `DEV`: stop and report.

- [ ] **Step 5: Turn the switch on in the dev stack.** Add `CRATE_LISTEN_SMART_MIX_V3_ENABLED: "true"` to `listening/android-local/docker-compose.android-local.yaml`, beside the Android flags, and recreate the API container.
Expected: `curl -s http://localhost:8585/api/capabilities | jq .smart_mix.listen_planner_version` prints `"smart-mix-v3"`.

- [ ] **Step 6: Check what the phone sends.**
  - In `chrome://inspect`, the WebView's Network tab shows the plan batch with `"plannerVersion":"smart-mix-v3"` and plans with `structureUsed`.
  - With a radio playing and the screen off, the API log shows `POST /api/playback/continuation` with 200, never 422.

### Task 11: Session material for Diego

- [ ] **Step 1: Write** `~/Code/Ninja/crate-agents/listening/listen-v3/README.md` in Spanish, with:
  - the setup, adapted from `android-local/README.md` §6;
  - the session matrix below;
  - a sheet to mark each transition as fine, worse than v2 or failed, with its time.

| Session | Settings | Queue | What it checks |
| --- | --- | --- | --- |
| 1 | Default: Smart Mix on, 6 s, beatmatch off | Playlists 33, 34 and 35, from track 1, no shuffle | What most users will hear |
| 2 | Beatmatch on | Same | Long phrase blends, phase lock |
| 3 | Beatmatch and bass swap on | Playlist 33 | Bass handoff on v3's cues |
| 4 | Default | A radio, screen off, at least 3 transitions | Native continuation on v3 |
| 5 | Default | Shuffle, an album, downloaded tracks in airplane mode | Shuffle source, gapless album, offline fallback |
| 6 | Default, switch off and app restarted | Playlist 34 | Rollback to v2, and A/B by ear |

- [ ] **Step 2: Add** the laptop A/B list of Task 2, step 7, for the worst dips and skips.

**Gate LV-G2 (Diego): Listen on v3 by default.** The agent stops and presents the filled sheet. Proposed criteria:

- in session 1, no transition clearly worse than v2;
- no execution failure in any session: silence, double audio, a track starting mid-word, a missing bass after a handoff, a stall;
- session 4 keeps extending the radio on v3;
- session 6 goes back to v2.

Diego decides default-on (D5).

---

## LV4 · Release, switch on, default on

### Task 12: Release order and the production switch

Agents prepare; Diego decides and runs each step.

- [ ] **Step 1: Server release** with LV1, the switch off (normal release process).
- [ ] **Step 2: Android release** (`v*` tag) with LV2, with the gates of `docs/technical/listen-mobile-release-gates.md`. Add one row to the device matrix: "Smart Mix v3: crossfade, beatmatch, radio with the screen off, switch off".
- [ ] **Step 3: Production switch:** `CRATE_LISTEN_SMART_MIX_V3_ENABLED=true`, then the API restarts.
Expected: `/api/capabilities` shows `listen_planner_version: "smart-mix-v3"`. APKs from before LV2 keep asking for v2.
- [ ] **Step 4: Soak** for as long as Diego decides, listening on his phone. **Rollback:** switch off and restart the API. Clients go back to v2 at their next app start, and no app release is needed.

### Task 13: v3 by default in the code

PR: `feat(smart-mix): recommend smart-mix-v3 to Listen by default`.

**Files:**

- Modify: `app/crate/config.py` (`listen_smart_mix_v3_enabled`, `default=True`), `docs/technical/smart-mix-operations.md`, `docs/releases/<version>.md`
- Modify: `docs/plans/2026-11-02-smart-mix-3-design.md` in this repository: the line under SM3-D3 says Listen migrated, with the date.
- Test: `app/tests/test_capabilities_smart_mix_v3.py`

- [ ] **Step 1: Write the failing tests.** `test_listen_planner_v3_by_default`: with the switch unset, `listen_planner_version` is `"smart-mix-v3"`. `test_listen_planner_v2_when_switched_off`: with `CRATE_LISTEN_SMART_MIX_V3_ENABLED=false`, it is `"smart-mix-v2"`. Rename Task 3's `test_listen_planner_defaults_to_v2` to the second one.
- [ ] **Step 2: Run them and check they fail.** `cd app && ../.venv/bin/python -m pytest tests/test_capabilities_smart_mix_v3.py -q`. Expected: the default test FAILS.
- [ ] **Step 3: Implement** the default and the docs.
- [ ] **Step 4: Run the tests and check they pass.** Same command. Expected: PASS.
- [ ] **Step 5: Commit:** `git commit -m "feat(smart-mix): recommend smart-mix-v3 to Listen by default"` with the three files.

---

## Guarding against the loudness dips without stems

- **What was measured:** on the industrial set, Listen's context with the stems layer was worse than v3 (stems evaluation §12.5: 4 better and 20 worse edges, with dips on 6 better and 21 worse). Rules revision 5 takes the layer away from Listen (§13), so Listen's v3 plans are the revision 1 menu.
- **What remains open:** the stems report found the dips concentrated in Listen's 6 s blends without a bass handoff in v3 itself (§7), and v3 against v2 has only been measured in `dj`. There, SM5 found v3 better on dips (80 better and 44 worse edges on the phrase set).
- **How this plan guards against them:**
  1. LV0 measures the paired loudness dip and step of v3 against v2 per set and context, with the sign test, and lists the edges where v3 dips deepest (Task 2).
  2. LV-G1 stops the plan if v3 dips significantly more than v2 in any context and set. The fix (a dip term for non-Decks sources) gets its own plan and measurement.
  3. The deepest-dip edges go into Diego's A/B list, and the phone sessions cover the same playlists (Task 11).
  4. Task 5 pins that no Listen source gets stems, so the §12.5 regression cannot come back through a change of source.

## Risks

| Risk | Signal | Action |
| --- | --- | --- |
| v3 is not better than v2 in Listen's short crossfades; SM5 only measured `dj` | LV0 report | Stop at LV-G1 (LV-D6) |
| Loudness dips without stems | Paired dip worse with p < 0.1 | Previous section |
| v3 cuts more music: up to 30 s unplayed and 16 s skipped on average in `listen`; in the dev playlists, Melvins → Pallbearer enters at 1:12, and e0096 leaves at 2:52 where v2 leaves at 4:05 | LV0 distributions, Diego's ear | Decision D2 at LV-G1 |
| Beatmatch users get long phrase blends (up to 48 s) much more often than with v2, beyond the "Maximum transition" they chose | LV0 share of mixes longer than 12 s | Decision D3 |
| The harness renders beatmatch as perfect; the phone only engages it with a phase lock and otherwise crossfades on v3's cues | Phone sessions 2 and 3 | LV-G2 |
| The v8 sets are compatible-track edges, not Listen queues (any order, radio) | Phone sessions on real playlists | LV-G2; a Listen-queue set is out of scope |
| Analyzer v8 finds no phrases on 45 of 399 tracks | `structureUsed` false | None: v3 plans as v2 there |
| Uncached v3 latency was never benchmarked (SM2.PR2's 150 ms budget) | Task 2, step 5 | LV-G1; the deadline stretches to 10 s ahead of a transition |
| Someone moves `smart_mix.planner_version` to v3 | Old APKs play gapless | LV-D3 and Task 3's test |
| A new APK on an older server | v3 sent to a server that rejects it | LV-D1 and Tasks 6, 8 and 9 |
| The phone stays invisible to adb | Task 10, step 2 | Wireless debugging; otherwise LV-G2 waits |
| A rollback reaches a client only at its next app start | Long sessions stay on v3 | Accepted; the server keeps accepting v3 |
| ST3.PR6 is not merged when LV1 starts | Task 2, step 3 counts stems | Do not start LV1 |

## Out of scope

- Web, desktop (Tauri) and iOS: they do not execute plans.
- Stems, AI refinement, the candidate menu and set history for Listen.
- Changing the API default planner, VirtualDJ's planner or removing v2.
- Any change to v3's scoring or limits for Listen. If LV-G1 asks for one, it gets its own plan.
- Native execution changes beyond the continuation's planner.
- Server metrics per planner, and refreshing capabilities while the app runs.
- A harness set built from real Listen queues.
- Smart Mix's R1 physical gate (an external capture for clipping and underruns), which stays open.

## Open decisions for Diego

- **D1 (LV-G1):** the reading of the offline comparison; the proposal is in Task 2.
- **D2 (LV-G1):** whether Listen needs a cap on the intro v3 skips and the outro it leaves unplayed, once the v2 and v3 distributions are known.
- **D3 (LV-G1):** long blends for beatmatch users. Either keep them (as v2 already does, now more often), or ask for them only when the user's maximum is 12 s.
- **D4 (LV3):** the phone setup (local dev stack through adb or a test server), the `sudo` udev rules if USB needs them, and installing JDK and the Android SDK in user space (about 3 GB).
- **D5 (LV-G2, LV4):** default-on, the soak time in production, and when the code default changes for self-hosted servers.
- **For the report:** which Android flags production has on (`CRATE_ANDROID_BEATMATCH_ENABLED`, `CRATE_ANDROID_BASS_HANDOFF_ENABLED`). They decide which of Listen's three contexts real users can reach.

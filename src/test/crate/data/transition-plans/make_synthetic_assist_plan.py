#!/usr/bin/env python3
"""Writes synthetic-assist-long-blend.json, a SYNTHETIC transition plan for
transitionplan_test.cpp: the longest blend the assist mode of Decks may
get (S5, plan F6.S-PR1), with the `policyLimits` the server sends with it.

No dev stack serves the assist mode yet, so it cannot be exported like the
real-*.json plans. It follows the server's code instead (Crate monorepo,
branch feat/smart-mix-decks-context):

- the plan fields: `TransitionPlan.to_dict` in app/crate/smart_mix/models.py,
  as `_plan_edge` in app/crate/api/smart_mix.py serves a plan of the decks
  context with `assist`, without `localRefinement`: every decks plan carries
  `policyLimits` (Diego, 2026-11-15, option B), so there is no candidate
  menu;
- `policyLimits`: `policy_limits()` and `PolicyLimits.to_dict` in
  app/crate/smart_mix/evaluation.py, with `PLANNER_POLICY_V3` of
  app/crate/smart_mix/policy.py. `maximumLongBeatmatchMs` is
  `long_beatmatch_ceiling_ms(policy, context)` for a decks context with
  `assist`: `assist_maximum_long_beatmatch_ms`, 96 000 ms (decided by Diego
  on 2026-11-15). The constants below were checked against that branch.

The edge, its tempo ratio, phase offset and gains are those of the real
beatmatch plan (real-beatmatch.json) and the durations those of its tracks
(real-tracks.json), so the plan fits the same tracks.

    python3 make_synthetic_assist_plan.py [output directory]

Only the standard library. The output is deterministic.
"""

import json
import sys
from pathlib import Path

# PLANNER_POLICY_V3 (policy.py, F6.S-PR1)
RULES_REVISION = 6
MAXIMUM_TEMPO_ADJUSTMENT = 0.04  # min(maximum, hard maximum)
MINIMUM_TRANSITION_MS = 1_000
MAXIMUM_TRANSITION_MS = 12_000
ASSIST_MAXIMUM_LONG_BEATMATCH_MS = 96_000
MAX_LOUDNESS_ADJUSTMENT_DB = 6.0
MINIMUM_OUTGOING_BODY_RATIO = 0.5
MINIMUM_OUTGOING_BODY_MS = 30_000
MINIMUM_INCOMING_BODY_MS = 10_000
PHRASE_LENGTHS = [8, 16, 32]


def main() -> None:
    here = Path(__file__).parent
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else here
    real = json.loads((here / "real-beatmatch.json").read_text(encoding="utf-8"))
    durations = json.loads((here / "real-tracks.json").read_text(encoding="utf-8"))["durationMs"]
    outgoing_ms = durations[real["outgoingTrackEntityUid"]]
    incoming_ms = durations[real["incomingTrackEntityUid"]]

    duration_ms = ASSIST_MAXIMUM_LONG_BEATMATCH_MS
    plan = {
        "plannerVersion": 2,
        "outgoingTrackEntityUid": real["outgoingTrackEntityUid"],
        "incomingTrackEntityUid": real["incomingTrackEntityUid"],
        "mode": "beatmatch",
        "durationMs": duration_ms,
        # One second before the end, so a longer blend still fits the track.
        "outgoingCueMs": outgoing_ms - duration_ms - 1_000,
        "incomingCueMs": real["incomingCueMs"],
        "incomingTempoRatio": real["incomingTempoRatio"],
        "beatPhaseOffsetMs": real["beatPhaseOffsetMs"],
        "handoffProgress": 0.5,
        "outgoingGainDb": real["outgoingGainDb"],
        "incomingGainDb": real["incomingGainDb"],
        "curve": "dj-blend",
        "bassHandoff": "balanced",
        "confidence": real["confidence"],
    }
    limits = {
        "rulesRevision": RULES_REVISION,
        "beatmatchAllowed": True,
        "tempoAdjustmentAllowed": True,
        "bassHandoffAllowed": True,
        "longBeatmatchAllowed": True,
        "longBeatmatchEligible": True,
        "maximumTempoAdjustment": MAXIMUM_TEMPO_ADJUSTMENT,
        "minimumTransitionMs": MINIMUM_TRANSITION_MS,
        "maximumTransitionMs": MAXIMUM_TRANSITION_MS,
        "maximumLongBeatmatchMs": ASSIST_MAXIMUM_LONG_BEATMATCH_MS,
        "maximumBoostDb": MAX_LOUDNESS_ADJUSTMENT_DB,
        "maximumCutDb": MAX_LOUDNESS_ADJUSTMENT_DB,
        # The plan's gains are its tracks' peak-limited ones.
        "outgoingPeakLimitedGainDb": real["outgoingGainDb"],
        "incomingPeakLimitedGainDb": real["incomingGainDb"],
        "outgoingEarliestCueMs": min(int(outgoing_ms * MINIMUM_OUTGOING_BODY_RATIO),
                                     MINIMUM_OUTGOING_BODY_MS),
        "outgoingLatestEndMs": outgoing_ms + 1,
        "incomingLatestEndMs": incoming_ms - MINIMUM_INCOMING_BODY_MS + 1,
        "phraseLengths": PHRASE_LENGTHS,
    }
    payload = {**plan, "structureUsed": True, "policyLimits": limits}
    text = json.dumps(payload, indent=1, ensure_ascii=False) + "\n"
    (out / "synthetic-assist-long-blend.json").write_text(text, encoding="utf-8")
    print("Wrote synthetic-assist-long-blend.json", file=sys.stderr)


if __name__ == "__main__":
    main()

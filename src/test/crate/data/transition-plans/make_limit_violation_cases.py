#!/usr/bin/env python3
"""Writes limit-violations.json: the server's own answer to the limits check
that Decks mirrors (plan F6b.PR1), for transitionplan_test.cpp.

`limit_violations(plan, limits)` in app/crate/smart_mix/evaluation.py of the
Crate monorepo (STEM-D10, `aea3d084e`) is the reference check of a plan that
mixes against the `policyLimits` of its edge. This script runs that very
function, not a copy, over:

- every candidate of the two real menus of real-v3-menu-response.json,
  against the limits of their edge (the server serves only valid ones);
- variations of the first candidate of each menu and of its limits, each
  built to cross one bound or to sit right on it.

For each case it records the codes the server gives, in its order, and
whether the plan mixes: neither gapless nor the planner's short cut
(`_is_short_cut` with `PLANNER_POLICY_V3`), the plans `policy_violations`
checks against the limits.

It needs the monorepo's `app` directory on the path (only the standard
library otherwise):

    PYTHONPATH=<crate monorepo>/app python3 make_limit_violation_cases.py [output directory]

The output is deterministic.
"""

import copy
import json
import sys
from pathlib import Path

from crate.smart_mix.evaluation import PolicyLimits, _is_short_cut, limit_violations
from crate.smart_mix.models import TransitionPlan
from crate.smart_mix.policy import PLANNER_POLICY_V3

PLAN_FIELDS = {
    "plannerVersion": "planner_version",
    "outgoingTrackEntityUid": "outgoing_track_entity_uid",
    "incomingTrackEntityUid": "incoming_track_entity_uid",
    "mode": "mode",
    "durationMs": "duration_ms",
    "outgoingCueMs": "outgoing_cue_ms",
    "incomingCueMs": "incoming_cue_ms",
    "incomingTempoRatio": "incoming_tempo_ratio",
    "beatPhaseOffsetMs": "beat_phase_offset_ms",
    "handoffProgress": "handoff_progress",
    "outgoingGainDb": "outgoing_gain_db",
    "incomingGainDb": "incoming_gain_db",
    "curve": "curve",
    "bassHandoff": "bass_handoff",
    "confidence": "confidence",
    "fallbackReason": "fallback_reason",
}
LIMIT_FIELDS = {
    "rulesRevision": "rules_revision",
    "beatmatchAllowed": "beatmatch_allowed",
    "tempoAdjustmentAllowed": "tempo_adjustment_allowed",
    "bassHandoffAllowed": "bass_handoff_allowed",
    "longBeatmatchAllowed": "long_beatmatch_allowed",
    "longBeatmatchEligible": "long_beatmatch_eligible",
    "maximumTempoAdjustment": "maximum_tempo_adjustment",
    "minimumTransitionMs": "minimum_transition_ms",
    "maximumTransitionMs": "maximum_transition_ms",
    "maximumLongBeatmatchMs": "maximum_long_beatmatch_ms",
    "maximumBoostDb": "maximum_boost_db",
    "maximumCutDb": "maximum_cut_db",
    "outgoingPeakLimitedGainDb": "outgoing_peak_limited_gain_db",
    "incomingPeakLimitedGainDb": "incoming_peak_limited_gain_db",
    "outgoingEarliestCueMs": "outgoing_earliest_cue_ms",
    "outgoingLatestEndMs": "outgoing_latest_end_ms",
    "incomingLatestEndMs": "incoming_latest_end_ms",
    "phraseLengths": "phrase_lengths",
}


def server_plan(plan: dict) -> TransitionPlan:
    return TransitionPlan(**{snake: plan.get(camel) for camel, snake in PLAN_FIELDS.items()})


def server_limits(limits: dict) -> PolicyLimits:
    values = {snake: limits[camel] for camel, snake in LIMIT_FIELDS.items()}
    values["phrase_lengths"] = tuple(values["phrase_lengths"])
    return PolicyLimits(**values)


def case(name: str, plan: dict, limits: dict) -> dict:
    checked = server_plan(plan)
    return {
        "name": name,
        "plan": plan,
        "policyLimits": limits,
        "codes": list(limit_violations(checked, server_limits(limits))),
        "mixes": checked.mode.value != "gapless"
        and not _is_short_cut(checked, PLANNER_POLICY_V3),
    }


def varied(plan: dict, limits: dict, plan_changes=None, limit_changes=None):
    plan = copy.deepcopy(plan)
    limits = copy.deepcopy(limits)
    plan.update(plan_changes or {})
    limits.update(limit_changes or {})
    return plan, limits


def beatmatch_variations(plan: dict, limits: dict) -> list:
    duration = plan["durationMs"]
    peak_in = limits["incomingPeakLimitedGainDb"]
    latest_out = limits["outgoingLatestEndMs"]
    latest_in = limits["incomingLatestEndMs"]
    ratio = plan["incomingTempoRatio"]
    return [
        ("too-short", {"durationMs": limits["minimumTransitionMs"] - 1}, {}),
        ("shortest", {"durationMs": limits["minimumTransitionMs"]}, {}),
        ("longest-blend", {"durationMs": limits["maximumLongBeatmatchMs"],
                           "outgoingCueMs": latest_out - limits["maximumLongBeatmatchMs"]}, {}),
        ("beyond-the-ceiling", {"durationMs": limits["maximumLongBeatmatchMs"] + 1,
                                "outgoingCueMs": latest_out - limits["maximumLongBeatmatchMs"] - 1},
         {}),
        ("long-equal-power", {"curve": "equal-power"}, {}),
        ("long-unsteady", {}, {"longBeatmatchEligible": False}),
        ("long-not-allowed", {}, {"longBeatmatchAllowed": False}),
        ("tempo-beyond-the-limit", {"incomingTempoRatio": 1.0 + limits["maximumTempoAdjustment"]
                                    + 0.001}, {}),
        ("tempo-on-the-limit", {"incomingTempoRatio": round(
            1.0 + limits["maximumTempoAdjustment"], 6)}, {}),
        ("tempo-adjustment-not-allowed", {}, {"tempoAdjustmentAllowed": False}),
        ("tempo-under-the-floor", {"incomingTempoRatio": 1.0005},
         {"tempoAdjustmentAllowed": False}),
        ("beatmatch-not-allowed", {}, {"beatmatchAllowed": False}),
        ("bass-handoff-not-allowed", {}, {"bassHandoffAllowed": False}),
        ("cut-too-deep", {"outgoingGainDb": -limits["maximumCutDb"] - 0.5}, {}),
        ("cut-to-the-peak-limit", {"incomingGainDb": -8.0},
         {"incomingPeakLimitedGainDb": -8.0}),
        ("above-the-peak-limit", {"incomingGainDb": round(peak_in + 0.5, 6)}, {}),
        ("boost-too-high", {"incomingGainDb": limits["maximumBoostDb"] + 0.5},
         {"incomingPeakLimitedGainDb": 12.0}),
        ("outgoing-past-its-end", {"outgoingCueMs": latest_out - duration + 1}, {}),
        ("outgoing-to-its-end", {"outgoingCueMs": latest_out - duration}, {}),
        ("outgoing-too-early", {"outgoingCueMs": limits["outgoingEarliestCueMs"] - 1}, {}),
        ("incoming-past-its-end", {"incomingCueMs": int(latest_in - duration * ratio) + 1}, {}),
        ("several", {"incomingTempoRatio": 1.05, "outgoingGainDb": -7.0},
         {"bassHandoffAllowed": False, "beatmatchAllowed": False}),
    ]


def adaptive_variations(plan: dict, limits: dict) -> list:
    return [
        ("adaptive-tempo", {"incomingTempoRatio": 1.01}, {}),
        ("adaptive-too-long", {"durationMs": limits["maximumTransitionMs"] + 1}, {}),
        ("short-cut", {"durationMs": 350, "fallbackReason": "insufficient_window"}, {}),
        ("not-quite-a-short-cut", {"durationMs": 351, "fallbackReason": "insufficient_window"},
         {}),
        ("gapless", {"mode": "gapless", "durationMs": 0, "fallbackReason": None}, {}),
    ]


def main() -> None:
    here = Path(__file__).parent
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else here
    answer = json.loads((here / "real-v3-menu-response.json").read_text(encoding="utf-8"))
    cases = []
    for served, kind, variations in zip(answer["plans"], ("beatmatch", "adaptive"),
                                        (beatmatch_variations, adaptive_variations)):
        limits = served["policyLimits"]
        for candidate in served["candidates"]:
            cases.append(case(f"{kind}-candidate-{candidate['index']}", candidate["plan"],
                              limits))
        first = served["candidates"][0]["plan"]
        for name, plan_changes, limit_changes in variations(first, limits):
            cases.append(case(name, *varied(first, limits, plan_changes, limit_changes)))
    text = json.dumps({"cases": cases}, indent=1, ensure_ascii=False) + "\n"
    (out / "limit-violations.json").write_text(text, encoding="utf-8")
    print(f"Wrote limit-violations.json ({len(cases)} cases)", file=sys.stderr)


if __name__ == "__main__":
    main()

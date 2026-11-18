#!/usr/bin/env python3
"""Exports real smart-mix-v3 plans with their candidate menu from a Crate dev
stack as anonymised fixtures for transitionplan_test.cpp and planclient_test.cpp
(plan F6b.PR1).

It signs in like Decks, reads one playlist and asks
`POST /api/playback/transition-plans` with `smart-mix-v3` for the edges between
consecutive tracks with a mix profile, in Auto DJ's context with
`localRefinement` (STEM-D10): each plan then carries its `candidates` (best
first, the first one is the plan) and its `policyLimits`. It keeps:

- real-v3-menu-response.json: the whole answer for two edges, as a single
  request: the first `beatmatch` plan whose menu has at least three
  candidates, then the first `adaptive` plan with a `fallbackReason` whose
  menu has at least two.
- real-v3-menu-tracks.json: the profile duration of each track of those
  edges, which the plans do not carry and their validation needs.

Anonymised as export_real_transition_plans.py does: every uid is replaced by
a fixed one per role, and any field that could name something is dropped.
The numbers stay as the planner wrote them.

Never point it at production: only a dev stack (loopback or a private
address) is accepted.

    CRATE_DECKS_EXPORT_URL=http://127.0.0.1:8585 \
    CRATE_DECKS_EXPORT_EMAIL=... CRATE_DECKS_EXPORT_PASSWORD=... \
    python3 export_real_candidate_menus.py [output directory]

CRATE_DECKS_EXPORT_PLAYLIST names the playlist (default
"G3 · Electronic (Q3 Q5 grids)"). Only the standard library. The output is
deterministic: the same server data gives the same bytes.
"""

import json
import os
import sys
from pathlib import Path

from export_real_transition_plans import (
    EDGES_PER_REQUEST,
    Anonymiser,
    Server,
    check_dev_stack,
    edge,
    fail,
)

DEFAULT_PLAYLIST = "G3 · Electronic (Q3 Q5 grids)"
PLANNER = "smart-mix-v3"
# Auto DJ's context (AutoDJSmartMixAdapter::contextFor) on a server with
# every Decks feature: 10 s transitions, long blends and the menu.
AUTO_DJ_CONTEXT = {
    "preferredDurationMs": 10000,
    "allowLongBeatmatch": True,
    "localRefinement": True,
}


def main() -> None:
    url = os.environ.get("CRATE_DECKS_EXPORT_URL", "")
    email = os.environ.get("CRATE_DECKS_EXPORT_EMAIL", "")
    password = os.environ.get("CRATE_DECKS_EXPORT_PASSWORD", "")
    if not (url and email and password):
        fail("Set CRATE_DECKS_EXPORT_URL, CRATE_DECKS_EXPORT_EMAIL and "
             "CRATE_DECKS_EXPORT_PASSWORD")
    check_dev_stack(url)
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent

    server = Server(url)
    server.sign_in(email, password)
    tracks = server.playlist_tracks(os.environ.get("CRATE_DECKS_EXPORT_PLAYLIST",
                                                   DEFAULT_PLAYLIST))
    uids = [track["entity_uid"] for track in tracks
            if track.get("profile_quality") in ("full", "partial")]
    pairs = list(zip(uids, uids[1:]))
    beatmatch = fallback = None
    for start in range(0, len(pairs), EDGES_PER_REQUEST):
        chunk = pairs[start:start + EDGES_PER_REQUEST]
        answer = server.plans(PLANNER, [edge(a, b, **AUTO_DJ_CONTEXT) for a, b in chunk])
        for pair, plan in zip(chunk, answer["plans"]):
            menu = plan.get("candidates") or []
            if beatmatch is None and plan["mode"] == "beatmatch" and len(menu) >= 3:
                beatmatch = pair
            if (fallback is None and plan["mode"] == "adaptive"
                    and plan.get("fallbackReason") and len(menu) >= 2):
                fallback = pair
        if beatmatch and fallback:
            break
    if beatmatch is None or fallback is None:
        fail("The playlist has no beatmatch edge with a menu, or no fallback edge with one")

    answer = server.plans(PLANNER, [edge(*beatmatch, **AUTO_DJ_CONTEXT),
                                    edge(*fallback, **AUTO_DJ_CONTEXT)])
    if [plan["mode"] for plan in answer["plans"]] != ["beatmatch", "adaptive"]:
        fail("The edges were planned differently the second time")

    anonymiser = Anonymiser()
    anonymiser.add_uid(beatmatch[0], "v3-beatmatch-outgoing")
    anonymiser.add_uid(beatmatch[1], "v3-beatmatch-incoming")
    anonymiser.add_uid(fallback[0], "v3-fallback-outgoing")
    anonymiser.add_uid(fallback[1], "v3-fallback-incoming")
    durations = {uid: server.duration_ms(uid) for uid in (*beatmatch, *fallback)}

    files = {
        "real-v3-menu-response.json": anonymiser.mapping(answer),
        "real-v3-menu-tracks.json": {"durationMs": anonymiser.mapping(durations)},
    }
    for name, payload in files.items():
        text = json.dumps(payload, indent=1, ensure_ascii=False) + "\n"
        (out / name).write_text(text, encoding="utf-8")
        print(f"Wrote {name}", file=sys.stderr)


if __name__ == "__main__":
    main()

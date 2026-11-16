#!/usr/bin/env python3
"""Exports real transition plans from a Crate dev stack as anonymised fixtures
for transitionplan_test.cpp and planclient_test.cpp.

It signs in like Decks, reads one playlist and asks
`POST /api/playback/transition-plans` for the edges between consecutive tracks
with a mix profile, in the `decks` context, until it has one plan of each
kind:

- real-beatmatch.json: the first `beatmatch` plan with a beat phase offset, so
  the tests see how the incoming start moves with it.
- real-adaptive-fallback.json: the first automatic `adaptive` plan with a
  `fallbackReason` (the planner could not beatmatch the edge).
- real-adaptive.json: the manual `adaptive` plan (`automatic: false`) of the
  same edge as the beatmatch one.
- real-gapless.json: the `gapless` plan of that edge in an automatic `album`
  context.
- real-batch-response.json: the whole answer for those edges, in that order,
  as a single request.
- real-tracks.json: the profile duration of each track of those edges, which
  the plan does not carry and its validation needs.

Anonymised: every uid is replaced by a fixed one per role. Plans carry no
names, files or people; any field that could is dropped anyway. The numbers
stay as the planner wrote them.

Never point it at production: only a dev stack (loopback or a private
address) is accepted.

    CRATE_DECKS_EXPORT_URL=http://127.0.0.1:8585 \
    CRATE_DECKS_EXPORT_EMAIL=... CRATE_DECKS_EXPORT_PASSWORD=... \
    python3 export_real_transition_plans.py [output directory]

CRATE_DECKS_EXPORT_PLAYLIST names the playlist (default
"G3 · 500 tracks (open time)") and CRATE_DECKS_EXPORT_PLANNER the planner
(default "smart-mix-v3"). Only the standard library. The output is
deterministic: the same server data gives the same bytes.
"""

import ipaddress
import json
import os
import re
import sys
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

DEFAULT_PLAYLIST = "G3 · 500 tracks (open time)"
DEFAULT_PLANNER = "smart-mix-v3"
NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/thecrateapp/crate-decks/test")
PRIVATE_WORDS = {"title", "artist", "album", "path", "file", "filename", "user", "owner",
                 "email", "name", "url", "cover"}
HEADERS = {
    "X-Crate-App": "decks/0.1.0",
    "X-Device-Fingerprint": "crate-decks-fixture-export",
    "Content-Type": "application/json",
}
# What PlanClient sends by default (planclient.h, PlanContext).
DECKS_CONTEXT = {
    "source": "decks",
    "automatic": True,
    "offline": False,
    "preferredDurationMs": 8000,
    "userCueProfile": "default",
    "allowBeatmatch": True,
    "allowTempoAdjustment": True,
    "allowBassHandoff": True,
}
EDGES_PER_REQUEST = 32


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    sys.exit(1)


def check_dev_stack(url: str) -> None:
    """Only loopback and private addresses: never a public server."""
    host = urllib.parse.urlsplit(url).hostname or ""
    if host == "localhost":
        return
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        fail(f"{host} is not a dev stack address (use an IP on loopback or the LAN)")
    if not (address.is_loopback or address.is_private):
        fail(f"{host} is not a dev stack address")


class Server:
    def __init__(self, url: str) -> None:
        self.url = url.rstrip("/")
        self.token = ""

    def request(self, method: str, path: str, body=None):
        headers = dict(HEADERS)
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.url + path, data=data, headers=headers,
                                         method=method)
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read() or b"null")

    def sign_in(self, email: str, password: str) -> None:
        answer = self.request("POST", "/api/auth/login", {"email": email, "password": password})
        self.token = answer.get("token") or ""
        if not self.token:
            fail("The sign-in gave no access token")

    def playlist_tracks(self, name: str) -> list:
        tree = self.request("GET", "/api/decks/containers")
        for container in tree["containers"]:
            if container["kind"] == "playlist" and container["name"] == name:
                path = f"/api/decks/containers/playlist/{container['id']}/tracks?limit=500"
                return self.request("GET", path)["tracks"]
        fail(f"No playlist named {name!r}")
        return []

    def duration_ms(self, uid: str) -> int:
        return self.request("GET", f"/api/tracks/by-entity/{uid}/mix-profile")["durationMs"]

    def plans(self, planner: str, edges: list) -> dict:
        return self.request("POST", "/api/playback/transition-plans",
                            {"plannerVersion": planner, "edges": edges})


def edge(outgoing: str, incoming: str, **context) -> dict:
    return {
        "outgoingTrackEntityUid": outgoing,
        "incomingTrackEntityUid": incoming,
        "context": {**DECKS_CONTEXT, **context},
    }


def is_private(key: str) -> bool:
    words = re.findall(r"[a-z]+|[A-Z][a-z]*", key)
    return any(word.lower() in PRIVATE_WORDS for word in words)


class Anonymiser:
    def __init__(self) -> None:
        self.uids = {}

    def add_uid(self, real: str, role: str) -> None:
        self.uids.setdefault(real.lower(), str(uuid.uuid5(NAMESPACE, "transition-plan/" + role)))

    def value(self, value):
        if isinstance(value, dict):
            return self.mapping(value)
        if isinstance(value, list):
            return [self.value(item) for item in value]
        if isinstance(value, str) and value.lower() in self.uids:
            return self.uids[value.lower()]
        return value

    def mapping(self, object_: dict) -> dict:
        result = {}
        for key, value in object_.items():
            if is_private(key):
                print(f"Dropped the field {key}", file=sys.stderr)
                continue
            result[self.uids.get(key.lower(), key)] = self.value(value)
        return result


def main() -> None:
    url = os.environ.get("CRATE_DECKS_EXPORT_URL", "")
    email = os.environ.get("CRATE_DECKS_EXPORT_EMAIL", "")
    password = os.environ.get("CRATE_DECKS_EXPORT_PASSWORD", "")
    if not (url and email and password):
        fail("Set CRATE_DECKS_EXPORT_URL, CRATE_DECKS_EXPORT_EMAIL and "
             "CRATE_DECKS_EXPORT_PASSWORD")
    check_dev_stack(url)
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent
    planner = os.environ.get("CRATE_DECKS_EXPORT_PLANNER", DEFAULT_PLANNER)

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
        answer = server.plans(planner, [edge(a, b) for a, b in chunk])
        for pair, plan in zip(chunk, answer["plans"]):
            if (beatmatch is None and plan["mode"] == "beatmatch"
                    and plan["beatPhaseOffsetMs"] > 0):
                beatmatch = pair
            if (fallback is None and plan["mode"] == "adaptive"
                    and plan.get("fallbackReason")):
                fallback = pair
        if beatmatch and fallback:
            break
    if beatmatch is None or fallback is None:
        fail("The playlist has no beatmatch edge with a phase offset, or no fallback edge")

    edges = {
        "beatmatch": edge(*beatmatch),
        "adaptive-fallback": edge(*fallback),
        "adaptive": edge(*beatmatch, automatic=False),
        "gapless": edge(*beatmatch, source="album"),
    }
    answer = server.plans(planner, list(edges.values()))
    if len(answer["plans"]) != len(edges):
        fail("The server did not answer every edge")
    expected_modes = {"beatmatch": "beatmatch", "adaptive-fallback": "adaptive",
                      "adaptive": "adaptive", "gapless": "gapless"}
    for (role, _), plan in zip(edges.items(), answer["plans"]):
        if plan["mode"] != expected_modes[role]:
            fail(f"The {role} edge was planned as {plan['mode']}")

    anonymiser = Anonymiser()
    anonymiser.add_uid(beatmatch[0], "beatmatch-outgoing")
    anonymiser.add_uid(beatmatch[1], "beatmatch-incoming")
    anonymiser.add_uid(fallback[0], "fallback-outgoing")
    anonymiser.add_uid(fallback[1], "fallback-incoming")
    durations = {uid: server.duration_ms(uid) for uid in (*beatmatch, *fallback)}

    files = {f"real-{role}.json": anonymiser.mapping(plan)
             for role, plan in zip(edges, answer["plans"])}
    files["real-batch-response.json"] = anonymiser.mapping(answer)
    files["real-tracks.json"] = {"durationMs": anonymiser.mapping(durations)}
    for name, payload in files.items():
        text = json.dumps(payload, indent=1, ensure_ascii=False) + "\n"
        (out / name).write_text(text, encoding="utf-8")
        print(f"Wrote {name}", file=sys.stderr)


if __name__ == "__main__":
    main()

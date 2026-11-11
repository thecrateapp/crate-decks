#!/usr/bin/env python3
"""Exports real mix profiles from a Crate dev stack as anonymised fixtures for
analysisimporter_test.cpp and profilestore_test.cpp (the real-*.json files).

make_mix_profiles.py writes fixtures that follow the server's schema; these
come from the analyzer itself, so the tests also see what it really writes. It
signs in like Crate Decks, reads one playlist and picks, in playlist order:

- real-full-whole-bpm.json: the first `full` profile at a whole BPM with a
  steady tempo (tempoStability of at least 0.99).
- real-full-fractional-bpm.json: the first `full` one at a BPM that is neither
  whole nor half, with a steady tempo.
- real-full-tracked.json: the first `full` one whose tempo is not steady
  (tempoStability below 0.5): tracked beats rather than a constant grid.
- real-partial.json: the first `partial` one.
- real-batch-response.json: the S4 answer of `POST /api/decks/mix-profiles`
  for [whole BPM, partial, a uid the server does not know].

Anonymised: every uid is replaced by a fixed one per role, every revision is
re-hashed, and any field that could name a track, a file or a person is
dropped. The numbers stay as the analyzer wrote them.

Never point it at production: only a dev stack (loopback or a private
address) is accepted.

    CRATE_DECKS_EXPORT_URL=http://127.0.0.1:8585 \
    CRATE_DECKS_EXPORT_EMAIL=... CRATE_DECKS_EXPORT_PASSWORD=... \
    python3 export_real_mix_profiles.py [output directory]

CRATE_DECKS_EXPORT_PLAYLIST names the playlist (default
"G3 · Electronic (Q3 Q5 grids)"). Only the standard library. The output is
deterministic: the same server data gives the same bytes.
"""

import hashlib
import ipaddress
import json
import os
import re
import sys
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

DEFAULT_PLAYLIST = "G3 · Electronic (Q3 Q5 grids)"
NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/thecrateapp/crate-decks/test")
UNKNOWN_UID = "00000000-0000-4000-8000-000000000000"
# Words of a field name (camelCase or snake_case) that could name a track, a
# file or a person.
PRIVATE_WORDS = {"title", "artist", "album", "path", "file", "filename", "user", "owner",
                 "email", "name", "url", "cover"}
HEADERS = {
    "X-Crate-App": "decks/0.1.0",
    "X-Device-Fingerprint": "crate-decks-fixture-export",
    "Content-Type": "application/json",
}


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

    def profile(self, uid: str) -> dict:
        return self.request("GET", f"/api/tracks/by-entity/{uid}/mix-profile?detail=full")

    def batch(self, uids: list) -> dict:
        return self.request("POST", "/api/decks/mix-profiles",
                            {"entity_uids": uids, "detail": "full"})


def is_whole(bpm: float) -> bool:
    return abs(bpm - round(bpm)) < 1e-9


def is_half(bpm: float) -> bool:
    return abs(bpm * 2.0 - round(bpm * 2.0)) < 1e-9


ROLES = {
    "whole": lambda p: p.get("quality") == "full" and is_whole(p.get("bpm") or 0.0)
    and (p.get("tempoStability") or 0.0) >= 0.99,
    "fractional": lambda p: p.get("quality") == "full" and p.get("bpm")
    and not is_half(p["bpm"]) and (p.get("tempoStability") or 0.0) >= 0.99,
    "tracked": lambda p: p.get("quality") == "full" and p.get("bpm")
    and (p.get("tempoStability") or 0.0) < 0.5,
    "partial": lambda p: p.get("quality") == "partial",
}
FILES = {
    "whole": "real-full-whole-bpm.json",
    "fractional": "real-full-fractional-bpm.json",
    "tracked": "real-full-tracked.json",
    "partial": "real-partial.json",
}


def is_private(key: str) -> bool:
    words = re.findall(r"[a-z]+|[A-Z][a-z]*", key)
    return any(word.lower() in PRIVATE_WORDS for word in words)


class Anonymiser:
    """Replaces uids and revisions and drops private fields, the same way in
    every file, so the batch answer matches the single profiles."""

    def __init__(self) -> None:
        self.uids = {}

    def add_uid(self, real: str, role: str) -> None:
        self.uids[real.lower()] = str(uuid.uuid5(NAMESPACE, "mix-profile/" + role))

    def value(self, key: str, value):
        if isinstance(value, dict):
            return self.mapping(value)
        if isinstance(value, list):
            return [self.value(key, item) for item in value]
        if isinstance(value, str):
            if value.lower() in self.uids:
                return self.uids[value.lower()]
            if key.endswith("Revision"):
                digest = hashlib.sha256(f"crate-decks-fixture:{key}:{value}".encode())
                return digest.hexdigest()[: len(value)]
        return value

    def mapping(self, object_: dict) -> dict:
        result = {}
        for key, value in object_.items():
            if is_private(key):
                print(f"Dropped the field {key}", file=sys.stderr)
                continue
            new_key = self.uids.get(key.lower(), key)
            result[new_key] = self.value(key, value)
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

    server = Server(url)
    server.sign_in(email, password)
    tracks = server.playlist_tracks(os.environ.get("CRATE_DECKS_EXPORT_PLAYLIST",
                                                   DEFAULT_PLAYLIST))
    chosen = {}
    for track in tracks:
        if len(chosen) == len(ROLES):
            break
        uid = track["entity_uid"]
        if track.get("profile_quality") not in ("full", "partial"):
            continue
        profile = server.profile(uid)
        for role, matches in ROLES.items():
            if role not in chosen and matches(profile):
                chosen[role] = (uid, profile)
                break
    missing = [role for role in ROLES if role not in chosen]
    if missing:
        fail(f"The playlist has no profile for: {', '.join(missing)}")

    anonymiser = Anonymiser()
    for role, (uid, _) in chosen.items():
        anonymiser.add_uid(uid, role)
    anonymiser.add_uid(UNKNOWN_UID, "missing")
    batch = server.batch([chosen["whole"][0], chosen["partial"][0], UNKNOWN_UID])

    files = {FILES[role]: anonymiser.mapping(profile) for role, (_, profile) in chosen.items()}
    files["real-batch-response.json"] = anonymiser.mapping(batch)
    for name, payload in files.items():
        text = json.dumps(payload, indent=1, ensure_ascii=False) + "\n"
        (out / name).write_text(text, encoding="utf-8")
        print(f"Wrote {name}", file=sys.stderr)


if __name__ == "__main__":
    main()

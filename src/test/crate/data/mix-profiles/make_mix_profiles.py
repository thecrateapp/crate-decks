#!/usr/bin/env python3
"""Writes the mix-profile fixtures of analysisimporter_test.cpp and
profilestore_test.cpp.

They are not dumps of a real library. They follow the server's own schema and
serializer field by field (`MixProfileResponse` in
app/crate/api/schemas/smart_mix.py, `TrackMixProfile._serialize` in
app/crate/smart_mix/models.py, and the S4 batch answer of
`POST /api/decks/mix-profiles`): the same names, types and units, the null
fields left out as `response_model_exclude_none` does, and values in the
ranges the analyzer writes. Every uid, revision and title is made up: the
fixtures must not carry anything from a real library, and the dev stack that
could serve anonymised ones was not running when they were written.

    python3 make_mix_profiles.py [output directory]

Only the standard library. The output is deterministic, so running it again
gives the same bytes.
"""

import hashlib
import json
import random
import sys
from pathlib import Path

UID_CONSTANT = "0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10"
UID_DRIFTING = "7d1e9b44-3f2a-4e8e-8c0d-61b5a2f4d7aa"
UID_PARTIAL = "3c2a7f10-9e4b-4d21-b6f8-0a5e1c7d9b32"
UID_MISSING = "e5b8d3a6-1f7c-4a90-8d2e-6b4c0f9a1e57"


def revision(seed: str) -> str:
    """A profile or source revision: a SHA-256 hex digest, as the server's."""
    return hashlib.sha256(seed.encode()).hexdigest()


def constant_grid(first_ms: int, bpm: float, duration_ms: int) -> list[int]:
    """Analyzer v6 and later: a constant grid at a whole BPM, rounded to ms."""
    period = 60_000.0 / bpm
    beats = []
    position = float(first_ms)
    while position < duration_ms:
        beats.append(round(position))
        position += period
    return beats


def tracked_grid(first_ms: int, bpm: float, duration_ms: int, seed: int) -> list[int]:
    """Before v6: tracked beats at a fitted BPM, with the detector's jitter."""
    rng = random.Random(seed)
    period = 60_000.0 / bpm
    beats = []
    index = 0
    while True:
        position = first_ms + index * period + rng.uniform(-40.0, 40.0)
        if position >= duration_ms:
            break
        value = max(0, round(position))
        if not beats or value > beats[-1]:
            beats.append(value)
        index += 1
    return beats


def constant_profile() -> dict:
    """A `full` profile of a current analyzer: whole BPM, constant grid, the
    optional stems layer of detail=full."""
    duration_ms = 200_000
    return {
        "trackEntityUid": UID_CONSTANT,
        "profileVersion": 1,
        "profileRevision": revision("fixture-constant-1"),
        "analyzer": "crate-python",
        "analyzerVersion": "smart-mix-audio-v10",
        "sourceRevision": revision("fixture-constant-source"),
        "durationMs": duration_ms,
        "bpm": 124.0,
        "bpmConfidence": 0.93,
        "tempoStability": 0.99,
        "beatAnchorMs": 512,
        "downbeatAnchorMs": 996,
        "timeSignature": 4,
        "beatGridFormat": "delta-ms-v1",
        "key": "A",
        "scale": "minor",
        "camelot": "8A",
        "keyConfidence": 0.81,
        "introCueMs": 31_964,
        "outroCueMs": 165_802,
        "introBpm": 124.0,
        "introBeatMs": 512,
        "outroBpm": 124.0,
        "outroBeatMs": 165_802,
        "introTempoStability": 0.99,
        "outroTempoStability": 0.98,
        "introDownbeatMs": 996,
        "outroDownbeatMs": 165_802,
        "introLufs": -10.4,
        "outroLufs": -11.2,
        "truePeakDbfs": -0.7,
        "introEnergy": 0.58,
        "outroEnergy": 0.52,
        "introSpectralDensity": 0.47,
        "outroSpectralDensity": 0.44,
        "globalEnergy": 0.71,
        "danceability": 0.83,
        "valence": 0.42,
        "blissVectorRevision": revision("fixture-constant-bliss")[:16],
        "activeStartMs": 475,
        "activeEndMs": 196_350,
        "integratedLufs": -8.6,
        "measurementVersion": "bs1770-v1",
        "durationSource": "decoder",
        "quality": "full",
        "analyzedAt": "2026-11-08T21:14:03Z",
        "beatGridMs": constant_grid(512, 124.0, duration_ms),
        "stems": {
            "producer": "crate-server/2026.11.9",
            "separatorModel": "htdemucs",
            "separatorVersion": "4.0.1",
            "vocalDetector": "stem-vocals-1",
            "phrases": [
                {
                    "startMs": 512,
                    "endMs": 31_964,
                    "drums": 182,
                    "bass": 141,
                    "other": 96,
                    "vocals": 12,
                    "vocalPresent": False,
                    "bassPresent": True,
                    "drumsPresent": True,
                },
                {
                    "startMs": 31_964,
                    "endMs": 63_416,
                    "drums": 190,
                    "bass": 160,
                    "other": 120,
                    "vocals": 88,
                    "vocalPresent": True,
                    "bassPresent": True,
                    "drumsPresent": True,
                },
            ],
        },
    }


def drifting_profile() -> dict:
    """A `full` profile of an older analyzer, as F0 Q3 measured them: a BPM
    with decimals the track does not have and tracked beats that wander."""
    duration_ms = 180_000
    return {
        "trackEntityUid": UID_DRIFTING,
        "profileVersion": 1,
        "profileRevision": revision("fixture-drifting-1"),
        "analyzer": "crate-python",
        "analyzerVersion": "smart-mix-audio-v5",
        "sourceRevision": revision("fixture-drifting-source"),
        "durationMs": duration_ms,
        "bpm": 128.346,
        "bpmConfidence": 0.71,
        "tempoStability": 0.96,
        "beatAnchorMs": 253,
        "downbeatAnchorMs": 720,
        "timeSignature": 4,
        "beatGridFormat": "delta-ms-v1",
        "key": "F",
        "scale": "major",
        "camelot": "7B",
        "keyConfidence": 0.44,
        "introCueMs": 15_200,
        "outroCueMs": 160_100,
        "truePeakDbfs": -0.2,
        "globalEnergy": 0.77,
        "activeStartMs": 120,
        "activeEndMs": 176_900,
        "integratedLufs": -7.4,
        "measurementVersion": "bs1770-v1",
        "durationSource": "decoder",
        "quality": "full",
        "analyzedAt": "2026-08-02T09:41:57Z",
        "beatGridMs": tracked_grid(253, 128.346, duration_ms, seed=3),
    }


def partial_profile() -> dict:
    """A `partial` profile: the analysis could not place beats or a key."""
    return {
        "trackEntityUid": UID_PARTIAL,
        "profileVersion": 1,
        "profileRevision": revision("fixture-partial-1"),
        "analyzer": "crate-python",
        "analyzerVersion": "smart-mix-audio-v10",
        "sourceRevision": revision("fixture-partial-source"),
        "durationMs": 241_000,
        "bpm": 92.0,
        "bpmConfidence": 0.22,
        "truePeakDbfs": -3.1,
        "activeStartMs": 0,
        "activeEndMs": 238_400,
        "integratedLufs": -14.9,
        "measurementVersion": "bs1770-v1",
        "durationSource": "library",
        "quality": "partial",
        "analyzedAt": "2026-11-07T17:02:11Z",
        "beatGridMs": [],
    }


def batch_response() -> dict:
    """The S4 answer for [constant, partial, missing]: profiles keyed by
    lowercase uid in request order, and the uids without one."""
    return {
        "profiles": {
            UID_CONSTANT: constant_profile(),
            UID_PARTIAL: partial_profile(),
        },
        "missing": [UID_MISSING],
    }


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent
    files = {
        "full-constant-124.json": constant_profile(),
        "full-drifting-128.json": drifting_profile(),
        "partial.json": partial_profile(),
        "batch-response.json": batch_response(),
    }
    for name, payload in files.items():
        text = json.dumps(payload, indent=1, ensure_ascii=False) + "\n"
        (out / name).write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()

"""The shared stem-analysis/1 fixture of Crate Decks and the server.

Plan ST5.PR2. Both producers must encode the same level bytes for the
same stems (STEM-D1). This script makes four stereo stems at 44.1 kHz
from an integer generator that the C++ test
(src/test/crate/stemanalyzer_test.cpp) repeats exactly, gives them to
the server's own producer (crate.smart_mix.separation.analyze_stems)
and prints its stem-analysis/1 document. The test checks that Decks'
analysis of the same stems has the same bytes.

Run it where the server code and numpy are, for example the analysis
worker of the monorepo's dev stack, and keep its output as the fixture:

    docker exec -i crate-dev-analysis-worker python - \\
        < tools/crate-stems/stem_analysis_fixture.py \\
        > src/test/crate/data/stems/stem-analysis-fixture.json

The samples are int16 values times a power of two divided by 32768, so
they are the same float32 values on both sides.
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

import numpy as np

from crate.smart_mix.separation import SOURCES, SeparatedStems, analyze_stems

SAMPLE_RATE = 44100
# Four whole windows of 250 ms and a short one of 100 ms
FRAMES = 48510
WINDOW = 11025
SOURCE_REVISION = "5" * 64
# Per stem and window, the gain as a power of two (None is silence and
# "full" a full scale square wave), so that the bytes go from 0 to 255.
GAINS = {
    "drums": [0, 1, 2, 4, 8],
    "bass": [None, 0, 6, 12, "full"],
    "other": [3, None, 10, 1, 5],
    "vocals": [16, 9, None, 0, 7],
}


def lcg_values(seed: int, count: int) -> list[int]:
    """int16 values from a 32-bit linear congruential generator."""
    values = []
    state = seed
    for _ in range(count):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        values.append(((state >> 16) & 0xFFFF) - 32768)
    return values


def stem_samples(index: int, name: str) -> np.ndarray:
    """(2, FRAMES) float32 samples of one stem."""
    out = np.zeros((2, FRAMES), dtype=np.float32)
    for channel in range(2):
        values = lcg_values(1000 * (index + 1) + channel, FRAMES)
        for frame in range(FRAMES):
            gain = GAINS[name][frame // WINDOW]
            if gain is None:
                sample = 0.0
            elif gain == "full":
                sample = 32767.0 if frame % 2 == 0 else -32767.0
            else:
                sample = float(values[frame]) * 2.0**-gain
            out[channel, frame] = np.float32(sample) / np.float32(32768.0)
    return out


def main() -> None:
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)
        for index, name in enumerate(SOURCES):
            samples = stem_samples(index, name)
            # Interleaved stereo float32, as SeparatedStems keeps them
            samples.T.astype(np.float32).tofile(directory / f"{name}.f32")
        stems = SeparatedStems(
            directory=directory, frames=FRAMES, source_revision=SOURCE_REVISION
        )
        analysis = analyze_stems(stems)
    document = json.loads(
        analysis.model_dump_json(by_alias=True, exclude_none=True)
    )
    json.dump(document, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()

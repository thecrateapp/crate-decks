"""Write the reference fixtures for the htdemucs DSP and graph (ST4.PR2).

Two sets, both raw little-endian float32 in C order, with an index.json that
gives each file's shape, layout and SHA-256. Only synthetic signals are used
(tones, noise and clicks), never music.

--dsp DIR
    Small DSP fixtures for the C++ and server unit tests (< 1 MB, in the
    repository at src/test/crate/data/htdemucs-dsp). Numpy only:

    - STFT of a short stereo signal, in the layout of the graph input `spec`;
    - iSTFT of that STFT (the round trip) and of a random spectrum, in the
      layout of the graph output `spec_out`;
    - overlap-add at a reduced segment size: weights, summed weights and the
      output of a position-dependent mock model, including a short last
      segment padded with its neighbours;
    - the normalisation scalars and spot values of the real overlap-add
      weight.

    With torch and demucs installed, the STFT and iSTFT are first checked
    against HTDemucs._spec and _ispec (1e-6).

--golden DIR
    The full golden set, published as a release asset next to the model:

    - segment_*: one 7.8 s segment (the first of the track below): the graph
      inputs, the graph outputs from PyTorch on CPU in fp32, and its four
      stems;
    - track_*: a 10 s synthetic track and its four stems through the whole
      pipeline of stems_dsp.separate around the PyTorch graph.

    It checks that pipeline against demucs.apply.apply_model first.

Usage:
    python tools/crate-stems/make_fixtures.py \
        --dsp src/test/crate/data/htdemucs-dsp
    python tools/crate-stems/make_fixtures.py \
        --golden ~/Code/Ninja/crate-agents/stems-spike/release/golden
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

import stems_dsp as dsp

DSP_LIMIT_BYTES = 1 << 20


class FixtureWriter:
    def __init__(self, directory: Path, description: str):
        self.dir = directory
        self.dir.mkdir(parents=True, exist_ok=True)
        self.index: dict = {
            "description": description,
            "format": "raw little-endian float32, C order; complex spectra as "
            "channels [L.re, L.im, R.re, R.im]",
            "generator": "tools/crate-stems/make_fixtures.py",
            "dsp": {
                "sample_rate": dsp.SAMPLE_RATE,
                "n_fft": dsp.NFFT,
                "hop": dsp.HOP,
                "bins": dsp.BINS,
                "pre_pad": dsp.PRE_PAD,
                "edge_frames": dsp.EDGE_FRAMES,
                "window": "hann, periodic",
                "normalized": True,
                "center": True,
                "pad_mode": "reflect",
                "segment": dsp.SEGMENT,
                "stride": dsp.STRIDE,
                "sources": list(dsp.SOURCES),
            },
            "files": {},
        }

    def add(self, key: str, array: np.ndarray, layout: str) -> None:
        data = np.ascontiguousarray(array, dtype="<f4")
        name = f"{key}.bin"
        raw = data.tobytes()
        (self.dir / name).write_bytes(raw)
        self.index["files"][key] = {
            "file": name,
            "shape": list(data.shape),
            "layout": layout,
            "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
        }

    def finish(self, **extra) -> int:
        self.index.update(extra)
        total = sum(f["bytes"] for f in self.index["files"].values())
        self.index["total_bytes"] = total
        (self.dir / "index.json").write_text(
            json.dumps(self.index, indent=2) + "\n"
        )
        return total


def check_dsp_against_torch(x: np.ndarray) -> dict | None:
    """numpy STFT/iSTFT vs HTDemucs._spec/_ispec on `x` (C, L).

    Returns None without torch.
    """
    try:
        import torch
        from demucs.htdemucs import HTDemucs
    except ImportError:
        return None
    model = HTDemucs(
        sources=list(dsp.SOURCES), nfft=dsp.NFFT, cac=True
    )  # DSP only, no weights
    xt = torch.from_numpy(np.asarray(x, np.float64))[None]
    with torch.no_grad():
        tz = model._spec(xt)[0].numpy()
        z = np.stack([tz, tz * 0.5])
        ti = model._ispec(torch.from_numpy(z)[None], x.shape[-1])[0].numpy()
    nz = dsp.stft(x)
    ni = dsp.istft(np.stack([nz, nz * 0.5]), x.shape[-1])
    return {
        "stft_max_abs": float(np.abs(tz - nz).max()),
        "istft_max_abs": float(np.abs(ti - ni).max()),
    }


def write_dsp(directory: Path) -> int:
    w = FixtureWriter(
        directory,
        "Small DSP fixtures for the htdemucs pipeline (stems_dsp.py)",
    )
    rng = np.random.default_rng(7)

    length = (
        10000  # not a multiple of the hop, so the right pre-pad is exercised
    )
    x = dsp.short_test_signal(length)
    check = check_dsp_against_torch(x.astype(np.float64))
    if check is not None:
        assert (
            check["stft_max_abs"] < 1e-6 and check["istft_max_abs"] < 1e-6
        ), check
    z = dsp.stft(x)
    w.add("stft_input", x, "[channel, sample]")
    w.add(
        "stft_cac",
        dsp.to_cac(z),
        "[1, 2*channel (re, im), bin, frame]: the graph input `spec`",
    )
    w.add(
        "istft_roundtrip",
        dsp.istft(z, length),
        "[channel, sample]: iSTFT of stft_cac, length 10000 (differs "
        "from the input near the edges by design)",
    )

    frames = 4
    rand = rng.standard_normal((1, 4, dsp.BINS, frames)).astype(np.float32)
    w.add(
        "istft_random_cac",
        rand[None],
        "[1, stem, 2*channel (re, im), bin, frame]: a random spectrum in "
        "the layout of `spec_out`",
    )
    w.add(
        "istft_random_output",
        dsp.istft(dsp.from_cac(rand), 4000),
        "[stem, channel, sample]: iSTFT of istft_random_cac, length 4000",
    )

    seg, stride, ola_len = 2000, 1500, 5300
    ola_x = (0.5 * rng.standard_normal((2, ola_len))).astype(np.float32)
    ramp = 0.5 + np.arange(seg) / seg

    def mock(chunk: np.ndarray) -> np.ndarray:
        return np.stack([chunk * ramp, chunk * (1.5 - ramp)])

    ola_out, sum_weight = dsp.overlap_add(
        ola_x.astype(np.float64), mock, seg, stride
    )
    w.add("ola_input", ola_x, "[channel, sample]")
    w.add(
        "ola_weight",
        dsp.ola_weight(seg),
        "[sample]: triangular weight for segment 2000",
    )
    w.add(
        "ola_sum_weight",
        sum_weight,
        "[sample]: summed weights for length 5300, stride 1500",
    )
    w.add(
        "ola_output",
        ola_out,
        "[stem, channel, sample]: overlap-add of the mock model; stem 0 "
        "= segment * ramp, "
        "stem 1 = segment * (1.5 - ramp), ramp[n] = 0.5 + n / 2000",
    )

    full = dsp.ola_weight()
    spots = [
        0,
        1,
        dsp.SEGMENT // 2 - 1,
        dsp.SEGMENT // 2,
        dsp.SEGMENT - 2,
        dsp.SEGMENT - 1,
    ]
    mean, scale = dsp.normalisation(ola_x)
    total = w.finish(
        overlap_add={
            "segment": seg,
            "stride": stride,
            "length": ola_len,
            "offsets": dsp.segment_offsets(ola_len, stride),
            "padding": "each segment is centred in `segment` samples with "
            "real neighbouring samples or zeros, then center-trimmed",
            "real_weight": {
                "segment": dsp.SEGMENT,
                "stride": dsp.STRIDE,
                "values": {str(i): float(full[i]) for i in spots},
            },
        },
        normalisation={
            "input": "ola_input",
            "mean": mean,
            "scale": scale,
            "rule": "mono = mean over channels; mean(mono); std(mono, "
            "ddof=1) + 1e-8",
        },
        torch_check=check,
    )
    assert total <= DSP_LIMIT_BYTES, total
    return total


def write_golden(directory: Path) -> int:
    import torch

    import export_htdemucs as ex

    torch.set_num_threads(8)
    model = ex.load_model()
    core = ex.Core(model).eval()

    def graph(mix: np.ndarray, spec: np.ndarray):
        with torch.no_grad():
            x, xt = core(torch.from_numpy(mix), torch.from_numpy(spec))
        return x.numpy(), xt.numpy()

    w = FixtureWriter(
        directory,
        "Golden set for htdemucs-onnx-1: graph I/O and stems from "
        "PyTorch on CPU (fp32) with the numpy DSP",
    )
    track = dsp.synthetic_mix(10.0, seed=0)
    mean, scale = dsp.normalisation(track)

    # Segment 0 of the track is exactly its first 343980 normalised samples.
    mix = ((track[:, : dsp.SEGMENT].astype(np.float64) - mean) / scale)[
        None
    ].astype(np.float32)
    spec = dsp.to_cac(dsp.stft(mix[0]))
    spec_out, wave_out = graph(mix, spec)
    w.add(
        "segment_mix",
        mix,
        "[1, channel, sample]: graph input `mix` (normalised)",
    )
    w.add(
        "segment_spec", spec, "[1, 2*channel, bin, frame]: graph input `spec`"
    )
    w.add(
        "segment_spec_out",
        spec_out,
        "[1, stem, 2*channel, bin, frame]: graph output `spec_out`",
    )
    w.add(
        "segment_wave_out",
        wave_out,
        "[1, stem, channel, sample]: graph output `wave_out`",
    )
    w.add(
        "segment_stems",
        dsp.segment_stems(spec_out, wave_out),
        "[stem, channel, sample]: iSTFT(spec_out) + wave_out, normalised "
        "domain",
    )

    stems = dsp.separate(track, graph)
    w.add(
        "track_input",
        track,
        "[channel, sample]: 10 s synthetic track at 44.1 kHz",
    )
    w.add(
        "track_stems", stems, "[stem, channel, sample]: stems of track_input"
    )

    # The numpy pipeline against Demucs' own apply_model on the same normalised
    # track.
    from demucs.apply import apply_model

    xt = torch.from_numpy(
        ((track.astype(np.float64) - mean) / scale).astype(np.float32)
    )[None]
    with torch.no_grad():
        ref = apply_model(
            model, xt, shifts=0, split=True, overlap=0.25, progress=False
        )[0]
    ref = ref.numpy().astype(np.float64) * scale + mean
    sdr = {s: _sdr(ref[i], stems[i]) for i, s in enumerate(dsp.SOURCES)}
    print(
        "numpy pipeline vs demucs apply_model, SDR dB:",
        {k: round(v, 1) for k, v in sdr.items()},
    )
    assert min(sdr.values()) > 90, sdr

    return w.finish(
        normalisation={"mean": mean, "scale": scale},
        segment={
            "track_offset": 0,
            "note": "segment 0 of track_input; no padding needed",
        },
        reference={
            "graph": "PyTorch CPU fp32, export_htdemucs.Core with the "
            "upstream attention",
            "weights_sha256": ex.WEIGHTS_SHA256,
            "torch": torch.__version__,
            "pipeline_vs_apply_model_sdr_db": sdr,
        },
    )


def _sdr(ref: np.ndarray, est: np.ndarray) -> float:
    err = np.sum((ref - est) ** 2)
    return float(10 * np.log10(np.sum(ref**2) / max(err, 1e-30)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--dsp", type=Path, help="directory for the small DSP fixtures"
    )
    parser.add_argument(
        "--golden",
        type=Path,
        help="directory for the golden set (outside the repo)",
    )
    args = parser.parse_args()
    if not args.dsp and not args.golden:
        parser.error("give --dsp and/or --golden")
    if args.dsp:
        total = write_dsp(args.dsp)
        print(f"wrote DSP fixtures to {args.dsp}: {total / 1024:.0f} KiB")
    if args.golden:
        total = write_golden(args.golden)
        print(f"wrote golden set to {args.golden}: {total / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())

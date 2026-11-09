"""Numpy reference of the DSP around the exported htdemucs graph.

The graph exported by export_htdemucs.py contains only the network. Everything
else in Demucs' `apply_model` and `HTDemucs.forward` happens outside it, and
this module is the reference for that code in Crate Decks (C++) and on the
server:

- normalisation of the whole track by the mean and standard deviation of its
  mono mix (`demucs.api.Separator` semantics: unbiased standard deviation);
- segments of 343,980 samples with a stride of 257,985 samples, each one padded
  around its centre with real neighbouring samples or zeros
  (`TensorChunk.padded`);
- STFT of each segment (`HTDemucs._spec`, `_magnitude` with
  complex-as-channels);
- iSTFT of the masked spectrogram per stem (`HTDemucs._mask`, `_ispec`), added
  to the time-branch output of the graph;
- triangular overlap-add of the segments, then de-normalisation.

Everything runs in float64 and was checked against PyTorch to 1e-6 (STFT and
iSTFT) and against `demucs.apply.apply_model` (whole pipeline).
"""

from __future__ import annotations

import math
from collections.abc import Callable

import numpy as np

SAMPLE_RATE = 44100
CHANNELS = 2
SOURCES = ("drums", "bass", "other", "vocals")
NFFT = 4096
HOP = NFFT // 4  # 1024
BINS = NFFT // 2  # 2048: the Nyquist bin is dropped
PRE_PAD = HOP // 2 * 3  # 1536
EDGE_FRAMES = 2  # frames dropped on each side after the STFT
SEGMENT = int(SAMPLE_RATE * 7.8)  # 343980, int(model.segment * samplerate)
OVERLAP = 0.25
STRIDE = int((1 - OVERLAP) * SEGMENT)  # 257985
SEGMENT_FRAMES = math.ceil(SEGMENT / HOP)  # 336
NORM_EPS = 1e-8

# torch.hann_window(4096) is periodic: w[n] = 0.5 - 0.5 cos(2 pi n / N), N =
# 4096.
WINDOW = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)


def stft(x: np.ndarray) -> np.ndarray:
    """(C, L) real -> (C, 2048, ceil(L / 1024)) complex: HTDemucs._spec.

    Reflect-pads by 1536 samples on the left and up to a multiple of the hop
    plus 1536 on the right, runs a centred (reflect) STFT with a periodic Hann
    window and `normalized=True` (divide by 64), then drops the Nyquist bin and
    two frames on each side. Needs L > 2048 (the reflect pads must fit).
    """
    length = x.shape[-1]
    frames_out = math.ceil(length / HOP)
    x = np.pad(
        np.asarray(x, np.float64),
        ((0, 0), (PRE_PAD, PRE_PAD + frames_out * HOP - length)),
        mode="reflect",
    )
    x = np.pad(
        x, ((0, 0), (NFFT // 2, NFFT // 2)), mode="reflect"
    )  # torch.stft center=True
    n_frames = 1 + (x.shape[-1] - NFFT) // HOP
    assert n_frames == frames_out + 2 * EDGE_FRAMES
    frames = np.lib.stride_tricks.sliding_window_view(x, NFFT, axis=-1)[
        :, ::HOP
    ][:, :n_frames]
    z = np.fft.rfft(frames * WINDOW, axis=-1) / math.sqrt(NFFT)
    z = z.transpose(0, 2, 1)  # (C, 2049, frames)
    return z[:, :BINS, EDGE_FRAMES : EDGE_FRAMES + frames_out]


def istft(z: np.ndarray, length: int) -> np.ndarray:
    """(..., C, 2048, T) complex -> (..., C, length) real: HTDemucs._ispec.

    Restores the Nyquist bin (zero) and two zero frames on each side, runs a
    centred, normalised inverse STFT (multiply by 64, window, overlap-add,
    divide by the summed squared window), and trims the 1536-sample pre-pad.
    """
    z = np.asarray(z)
    lead = z.shape[:-2]
    pad = [(0, 0)] * (z.ndim - 2) + [(0, 1), (EDGE_FRAMES, EDGE_FRAMES)]
    z = np.pad(z, pad)
    padded_length = HOP * math.ceil(length / HOP) + 2 * PRE_PAD
    n_frames = z.shape[-1]
    frames = np.fft.irfft(
        z * math.sqrt(NFFT), n=NFFT, axis=-2
    )  # (..., NFFT, T)
    frames = np.swapaxes(frames, -1, -2) * WINDOW  # (..., T, NFFT)
    total = NFFT + HOP * (n_frames - 1)
    y = np.zeros(lead + (total,))
    envelope = np.zeros(total)
    for t in range(n_frames):
        y[..., t * HOP : t * HOP + NFFT] += frames[..., t, :]
        envelope[t * HOP : t * HOP + NFFT] += WINDOW**2
    start = NFFT // 2  # center=True
    y = (
        y[..., start : start + padded_length]
        / envelope[start : start + padded_length]
    )
    return y[..., PRE_PAD : PRE_PAD + length]


def to_cac(z: np.ndarray) -> np.ndarray:
    """(C, F, T) complex -> (1, 2C, F, T) float32, complex as channels.

    Channels are [L.re, L.im, R.re, R.im].

    This is the layout of the graph input `spec` (HTDemucs._magnitude with
    cac).
    """
    c, f, t = z.shape
    return (
        np.stack([z.real, z.imag], axis=1)
        .reshape(1, 2 * c, f, t)
        .astype(np.float32)
    )


def from_cac(m: np.ndarray) -> np.ndarray:
    """(S, 2C, F, T) real -> (S, C, F, T) complex.

    The inverse of to_cac per stem (HTDemucs._mask).
    """
    s, c2, f, t = m.shape
    m = np.asarray(m, np.float64).reshape(s, c2 // 2, 2, f, t)
    return m[:, :, 0] + 1j * m[:, :, 1]


def segment_stems(spec_out: np.ndarray, wave_out: np.ndarray) -> np.ndarray:
    """Graph outputs of one segment -> (S, C, L) stems.

    Each stem is iSTFT(spec_out) + wave_out.
    """
    length = wave_out.shape[-1]
    return istft(from_cac(spec_out[0]), length) + np.asarray(
        wave_out[0], np.float64
    )


def normalisation(wav: np.ndarray) -> tuple[float, float]:
    """Mean and scale of the whole track, from its mono mix.

    Same as demucs.api.Separator.

    The scale is the unbiased standard deviation (torch `std()`, ddof=1) plus
    1e-8.
    """
    ref = np.asarray(wav, np.float64).mean(axis=0)
    return float(ref.mean()), float(ref.std(ddof=1) + NORM_EPS)


def ola_weight(
    segment: int = SEGMENT, transition_power: float = 1.0
) -> np.ndarray:
    """Triangular overlap-add weight of one segment (demucs.apply.apply_model).

    [1, 2, ..., segment // 2] followed by [segment - segment // 2, ..., 1],
    divided by its maximum. A shorter last segment uses the first samples of
    this weight.
    """
    w = np.concatenate(
        [
            np.arange(1, segment // 2 + 1),
            np.arange(segment - segment // 2, 0, -1),
        ]
    ).astype(np.float64)
    return (w / w.max()) ** transition_power


def segment_offsets(length: int, stride: int = STRIDE) -> list[int]:
    return list(range(0, length, stride))


def padded_chunk(
    x: np.ndarray, offset: int, chunk_length: int, target: int
) -> np.ndarray:
    """demucs TensorChunk(x, offset, chunk_length).padded(target).

    Centres the chunk in a window of `target` samples. The window takes real
    samples of `x` where they exist and zeros beyond the track's ends.
    """
    delta = target - chunk_length
    assert delta >= 0
    start = offset - delta // 2
    end = start + target
    lo, hi = max(0, start), min(x.shape[-1], end)
    return np.pad(
        x[..., lo:hi], [(0, 0)] * (x.ndim - 1) + [(lo - start, end - hi)]
    )


def center_trim(y: np.ndarray, length: int) -> np.ndarray:
    """demucs.utils.center_trim.

    Drops delta // 2 samples on the left and the rest on the right.
    """
    delta = y.shape[-1] - length
    if delta == 0:
        return y
    return y[..., delta // 2 : y.shape[-1] - (delta - delta // 2)]


def overlap_add(
    x: np.ndarray,
    model: Callable[[np.ndarray], np.ndarray],
    segment: int = SEGMENT,
    stride: int = STRIDE,
) -> tuple[np.ndarray, np.ndarray]:
    """Segment `x` (C, N), run `model` on each padded segment, overlap-add.

    `model` maps a (C, segment) array to (S, C, segment). Returns the (S, C, N)
    result and the summed weight (N,) it was divided by.
    """
    length = x.shape[-1]
    weight = ola_weight(segment)
    out = None
    sum_weight = np.zeros(length)
    for offset in segment_offsets(length, stride):
        chunk_length = min(segment, length - offset)
        y = center_trim(
            model(padded_chunk(x, offset, chunk_length, segment)), chunk_length
        )
        if out is None:
            out = np.zeros(y.shape[:-1] + (length,))
        out[..., offset : offset + chunk_length] += weight[:chunk_length] * y
        sum_weight[offset : offset + chunk_length] += weight[:chunk_length]
    assert out is not None and sum_weight.min() > 0
    return out / sum_weight, sum_weight


GraphFn = Callable[[np.ndarray, np.ndarray], tuple[np.ndarray, np.ndarray]]


def separate(wav: np.ndarray, graph: GraphFn) -> np.ndarray:
    """Whole-track separation around the graph: (2, N) -> (4, 2, N).

    `graph(mix, spec)` runs the network on one segment: `mix` (1, 2, 343980)
    and `spec` (1, 4, 2048, 336), both float32, and returns (spec_out,
    wave_out). Equivalent to apply_model(model, x, shifts=0, split=True,
    overlap=0.25).
    """
    mean, scale = normalisation(wav)
    x = (np.asarray(wav, np.float64) - mean) / scale

    def run(seg: np.ndarray) -> np.ndarray:
        mix = seg[None].astype(np.float32)
        spec_out, wave_out = graph(mix, to_cac(stft(mix[0])))
        return segment_stems(spec_out, wave_out)

    stems, _ = overlap_add(x, run)
    return stems * scale + mean


# --- Synthetic signals (never real music)
# ---------------------------------------------------


def synthetic_mix(seconds: float, seed: int = 0) -> np.ndarray:
    """A stereo test signal with energy in all four stems: (2, N) float32.

    Built only from tones, noise and clicks: decaying sine kicks, noise hats
    and snares at 124 BPM, a harmonic bass line, a sustained chord, and a
    vibrato harmonic tone with vowel-like formants and a syllable envelope.
    """
    rng = np.random.default_rng(seed)
    n = int(round(seconds * SAMPLE_RATE))
    t = np.arange(n) / SAMPLE_RATE
    beat = 60 / 124
    out = np.zeros((2, n))

    def place(sig: np.ndarray, start_s: float, pan: float) -> None:
        i = int(start_s * SAMPLE_RATE)
        if i >= n:
            return
        seg = sig[: n - i]
        out[0, i : i + len(seg)] += seg * math.cos((pan + 1) * math.pi / 4)
        out[1, i : i + len(seg)] += seg * math.sin((pan + 1) * math.pi / 4)

    tk = np.arange(int(0.35 * SAMPLE_RATE)) / SAMPLE_RATE
    kick = np.sin(
        2 * np.pi * (45 * tk + 60 * (1 - np.exp(-tk * 30)) / 30)
    ) * np.exp(-tk * 9)
    th = np.arange(int(0.05 * SAMPLE_RATE)) / SAMPLE_RATE
    snare_t = np.arange(int(0.18 * SAMPLE_RATE)) / SAMPLE_RATE
    for k in range(int(seconds / beat) + 1):
        place(0.9 * kick, k * beat, 0.0)
        hat = np.diff(rng.standard_normal(len(th) + 1)) * np.exp(-th * 80)
        place(0.12 * hat, k * beat + beat / 2, 0.3)
        if k % 2 == 1:
            snare = (
                rng.standard_normal(len(snare_t)) * 0.5
                + np.sin(2 * np.pi * 190 * snare_t)
            ) * np.exp(-snare_t * 22)
            place(0.35 * snare, k * beat, -0.1)
        place(np.array([0.6, -0.6]), k * beat + beat / 4, 0.5)  # a click

    bass_notes = [41.2, 41.2, 55.0, 49.0]
    for k in range(int(seconds / (beat / 2)) + 1):
        f0 = bass_notes[(k // 4) % len(bass_notes)]
        tb = np.arange(int(beat / 2 * SAMPLE_RATE)) / SAMPLE_RATE
        tone = sum(np.sin(2 * np.pi * f0 * h * tb) / h for h in range(1, 6))
        place(
            0.25 * tone * np.minimum(1, tb * 200) * np.exp(-tb * 4),
            k * beat / 2,
            0.0,
        )

    chord = sum(
        np.sin(2 * np.pi * f * t + p)
        for f, p in ((261.6, 0.0), (329.6, 1.0), (392.0, 2.0))
    )
    swell = 0.5 - 0.5 * np.cos(2 * np.pi * t / (8 * beat))
    out[0] += 0.05 * chord * swell
    out[1] += 0.05 * np.roll(chord, 220) * swell

    f0 = (
        220
        * 2 ** (np.floor(t / (2 * beat)) % 3 * 2 / 12)
        * (1 + 0.012 * np.sin(2 * np.pi * 5.5 * t))
    )
    phase = 2 * np.pi * np.cumsum(f0) / SAMPLE_RATE
    voice = np.zeros(n)
    for h in range(1, 25):
        fh = h * f0
        formant = (
            np.exp(-(((fh - 750) / 180) ** 2))
            + 0.6 * np.exp(-(((fh - 1200) / 220) ** 2))
            + 0.25 * np.exp(-(((fh - 2600) / 400) ** 2))
        )
        voice += formant * np.sin(h * phase) / h**0.5
    syllables = np.clip(np.sin(np.pi * (t / (beat / 2))) ** 2, 0, 1) * (
        np.floor(t / (4 * beat)) % 2 == 0
    )
    voice *= 0.18 * syllables
    out += voice

    out += 10 ** (-50 / 20) * rng.standard_normal((2, n))
    return (out / np.abs(out).max() * 0.6).astype(np.float32)


def short_test_signal(length: int, seed: int = 1) -> np.ndarray:
    """A short stereo signal for the DSP fixtures, (2, length) float32.

    Tones, noise and clicks only.
    """
    rng = np.random.default_rng(seed)
    t = np.arange(length) / SAMPLE_RATE
    left = 0.4 * np.sin(2 * np.pi * 440 * t) + 0.2 * np.sin(
        2 * np.pi * 3001.5 * t + 0.3
    )
    right = 0.3 * np.sin(2 * np.pi * 97.3 * t) + 0.25 * np.sin(
        2 * np.pi * 12000 * t
    )
    x = np.stack([left, right]) + 0.05 * rng.standard_normal((2, length))
    x[0, length // 3] += 0.8  # clicks
    x[1, 2 * length // 3] -= 0.8
    x[:, :64] *= np.linspace(0, 1, 64)  # avoid a step at the start
    return x.astype(np.float32)

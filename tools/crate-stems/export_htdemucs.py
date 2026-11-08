"""Export Demucs `htdemucs` to ONNX for Crate Decks (STEM-D3, ST4.PR2).

The graph holds only the network, cut as in the ST0 spike (and
sevagh/demucs.onnx):

    inputs   mix       [1, 2, 343980]        normalised waveform segment
             spec      [1, 4, 2048, 336]     its STFT, complex as channels
    outputs  spec_out  [1, 4, 4, 2048, 336]  masked spectrogram per stem
             wave_out  [1, 4, 2, 343980]     time-branch waveform per stem

All four are float32.

Each stem is iSTFT(spec_out) + wave_out. Normalisation, segmentation, STFT,
iSTFT and overlap-add stay outside the graph; stems_dsp.py is their reference.

Memory. The ST0 export peaked at 3.07 GB of VRAM on the CUDA provider (5.4 GB
with ONNX Runtime's defaults), against 0.9 GB for PyTorch. Two things add up:

- Each nn.MultiheadAttention exports as a full [1, 8, 2688, 2688] score matrix
  (220 MiB per self-attention). By default this export replaces every attention
  with a numerically equivalent version that splits the queries into chunks of
  --chunk rows: standard MatMul and Softmax ops only, so the same graph runs on
  every execution provider, and the score blocks shrink to [1, 8, 256, 2688].
- ONNX Runtime's sequential executor lets a later tensor reuse the buffer of an
  earlier dead tensor of the same shape. In this U-Net the decoder reuses
  encoder buffers, so they stay allocated through the whole transformer (1.5 GB
  live at the peak with an external non-caching allocator, against a 190 MiB
  live set). The parallel executor does not plan that reuse, so runtimes set
  ExecutionMode ORT_PARALLEL with at least two inter-op threads (with one
  thread ONNX Runtime falls back to the sequential executor). The manifest
  records it.

check_export.py measures both setups. `--attention` exports the alternatives
that were measured for comparison (not for release):

    chunked      default: query chunks of --chunk rows (standard ops, opset 17)
    plain        what torch exports for nn.MultiheadAttention
    mha          com.microsoft MultiHeadAttention (ONNX Runtime contrib op)
    attention23  ONNX Attention, opset 23 (dynamo exporter)

Writes <out>/<version>.onnx and a manifest <out>/<version>.json (name, version,
SHA-256, size, opset, shapes, DSP parameters, attention, source weights and
attribution). Variants other than the default get the variant in the file name.

Usage:
    python tools/crate-stems/export_htdemucs.py --out DIR
    python tools/crate-stems/export_htdemucs.py --out DIR --attention plain
    python tools/crate-stems/export_htdemucs.py --out DIR --chunk 256

Then check it (parity, VRAM, speed):
    python tools/crate-stems/check_export.py DIR/htdemucs-onnx-1.onnx

The pretrained weights come from the Hugging Face repository adefossez/HTDemucs
at a pinned revision, through demucs 4.1.0; the tool refuses other weights.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import sys
import time
from pathlib import Path

import torch
import torch.nn.functional as F
from einops import rearrange
from torch import nn

import stems_dsp as dsp

VERSION = "htdemucs-onnx-1"
MODEL_NAME = "htdemucs"
WEIGHTS_REPO = "adefossez/HTDemucs"
WEIGHTS_REVISION = "cbc8a9b1a87023b7fd74e7b3412e6321c0eab003"
WEIGHTS_FILE = "955717e8.safetensors"
WEIGHTS_SHA256 = (
    "d9fa14133cfcc034a6758923bb3a8ca9f8dfd0b582134643bbf83f72c17576dd"
)
DEFAULT_CHUNK = 256
INPUT_NAMES = ["mix", "spec"]
OUTPUT_NAMES = ["spec_out", "wave_out"]

ATTRIBUTION = {
    "model": "Hybrid Transformer Demucs (htdemucs), by Simon Rouard, "
    "Francisco Massa and Alexandre Défossez, Meta AI",
    "paper": "Hybrid Transformers for Music Source Separation, ICASSP 2023 "
    "(arXiv:2211.08553)",
    "code": "https://github.com/facebookresearch/demucs (MIT licence); "
    "demucs 4.1.0 from https://github.com/adefossez/demucs (MIT licence)",
    "weights": f"https://huggingface.co/{WEIGHTS_REPO} "
    f"at revision {WEIGHTS_REVISION}",
    "training_data": "Trained on MUSDB18-HQ, whose tracks are licensed for "
    "research use only, plus Meta's internal songs (800 extra tracks "
    "according to the Demucs README). The weights carry no licence of their "
    "own beyond the repository's MIT licence.",
    "use_in_crate": "Accepted at STEM-G0 for Crate, a free and non-commercial "
    "project, and shipped with this attribution.",
    "conversion": "Exported to ONNX by tools/crate-stems/export_htdemucs.py "
    "in Crate Decks: network only, attention split by query chunks, no "
    "retraining.",
}


RUNTIME_NOTES = {
    "session": {
        "execution_mode": "ORT_PARALLEL",
        "inter_op_num_threads": 2,
        "graph_optimization_level": "ORT_ENABLE_ALL",
    },
    "why": "The sequential executor's buffer reuse keeps encoder buffers "
    "allocated until the decoder reuses them: 2.0-4.2 GB peak on the CUDA "
    "provider instead of about 0.65 GB. The parallel executor plans no such "
    "reuse; with one inter-op thread ONNX Runtime falls back to the "
    "sequential executor. See check_export.py for the measurements.",
}


# --- Weights
# ---------------------------------------------------------------------------------


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_model():
    """The pinned htdemucs weights, in eval mode, on CPU."""
    from huggingface_hub import hf_hub_download

    from demucs.hf import load_safetensors_model

    path = Path(
        hf_hub_download(WEIGHTS_REPO, WEIGHTS_FILE, revision=WEIGHTS_REVISION)
    )
    digest = sha256_file(path)
    if digest != WEIGHTS_SHA256:
        raise SystemExit(
            f"unexpected weights {path}: sha256 {digest}, want "
            f"{WEIGHTS_SHA256}"
        )
    model = load_safetensors_model(path)
    model.eval()
    assert tuple(model.sources) == dsp.SOURCES, model.sources
    assert int(model.segment * model.samplerate) == dsp.SEGMENT
    assert model.nfft == dsp.NFFT and model.hop_length == dsp.HOP and model.cac
    return model


# --- Attention
# -------------------------------------------------------------------------------


class _ContribMHA(torch.autograd.Function):
    """SDPA in torch, exported as com.microsoft MultiHeadAttention.

    Works on [B, T, E] tensors.
    """

    @staticmethod
    def forward(ctx, q, k, v, num_heads: int):  # noqa: ARG004
        b, tq, e = q.shape
        d = e // num_heads
        qh = q.view(b, tq, num_heads, d).transpose(1, 2)
        kh = k.view(b, -1, num_heads, d).transpose(1, 2)
        vh = v.view(b, -1, num_heads, d).transpose(1, 2)
        o = F.scaled_dot_product_attention(qh, kh, vh)
        return o.transpose(1, 2).reshape(b, tq, e)

    @staticmethod
    def symbolic(g, q, k, v, num_heads: int):
        out = g.op(
            "com.microsoft::MultiHeadAttention", q, k, v, num_heads_i=num_heads
        )
        out.setType(q.type())
        return out


class ExportAttention(nn.Module):
    """Drop-in replacement for the nn.MultiheadAttention of htdemucs.

    Same weights and the same maths, without masks or dropout (inference only).
    `impl` chooses how the attention core is written in the graph.
    """

    def __init__(self, mha: nn.MultiheadAttention, impl: str, chunk: int):
        super().__init__()
        assert (
            mha.batch_first and mha._qkv_same_embed_dim and mha.bias_k is None
        )
        assert not mha.add_zero_attn and mha.in_proj_bias is not None
        self.impl, self.chunk = impl, chunk
        self.batch_first = True
        self.embed_dim, self.num_heads = mha.embed_dim, mha.num_heads
        self.head_dim = self.embed_dim // self.num_heads
        self.in_proj_weight = mha.in_proj_weight
        self.in_proj_bias = mha.in_proj_bias
        self.out_proj = mha.out_proj

    def forward(
        self,
        query,
        key,
        value,
        attn_mask=None,
        key_padding_mask=None,
        need_weights=False,
        is_causal=False,
        average_attn_weights=True,
    ):  # noqa: ARG002
        assert attn_mask is None and key_padding_mask is None and not is_causal
        w_q, w_k, w_v = self.in_proj_weight.chunk(3)
        b_q, b_k, b_v = self.in_proj_bias.chunk(3)
        q = F.linear(query, w_q, b_q)
        k = F.linear(key, w_k, b_k)
        v = F.linear(value, w_v, b_v)
        b, tq, e = q.shape
        tk = k.shape[1]
        h, d = self.num_heads, self.head_dim
        if self.impl == "mha":
            o = _ContribMHA.apply(q, k, v, h)
            return self.out_proj(o), None
        qh = q.view(b, tq, h, d).transpose(1, 2)  # [B, H, Tq, D]
        vh = v.view(b, tk, h, d).transpose(1, 2)  # [B, H, Tk, D]
        if self.impl == "attention23":
            kh = k.view(b, tk, h, d).transpose(1, 2)
            o = F.scaled_dot_product_attention(qh, kh, vh)
        else:
            assert self.impl == "chunked"
            kt = k.view(b, tk, h, d).permute(0, 2, 3, 1)  # [B, H, D, Tk]
            qh = qh * (1.0 / math.sqrt(d))
            parts = []
            for start in range(0, tq, self.chunk):
                scores = torch.matmul(qh[:, :, start : start + self.chunk], kt)
                parts.append(torch.matmul(torch.softmax(scores, dim=-1), vh))
            o = torch.cat(parts, dim=2) if len(parts) > 1 else parts[0]
        o = o.transpose(1, 2).reshape(b, tq, e)
        return self.out_proj(o), None


def replace_attention(model, impl: str, chunk: int) -> int:
    """Swap every attention of the cross-transformer; return the count."""
    count = 0
    for layer in list(model.crosstransformer.layers) + list(
        model.crosstransformer.layers_t
    ):
        for attr in ("self_attn", "cross_attn"):
            mha = getattr(layer, attr, None)
            if isinstance(mha, nn.MultiheadAttention):
                setattr(layer, attr, ExportAttention(mha, impl, chunk))
                count += 1
    return count


# --- Graph
# -----------------------------------------------------------------------------------


class Core(nn.Module):
    """HTDemucs.forward between the STFT and the iSTFT (the ST0 cut).

    Copied from demucs 4.1.0 htdemucs.py, from the normalisation of `mag` to
    the de-normalised outputs, without the STFT, masking, iSTFT and final sum.
    """

    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, mix: torch.Tensor, spec: torch.Tensor):
        m = self.m
        x = spec
        b, _, fq, t = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)

        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                f = x.shape[2]
                x = rearrange(x, "b c f t-> b c (f t)")
                x = m.channel_upsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                x = rearrange(x, "b c f t-> b c (f t)")
                x = m.channel_downsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f)
                xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)

        s = len(m.sources)
        x = x.view(b, s, -1, fq, t)
        x = x * std[:, None] + mean[:, None]
        xt = xt.view(b, s, -1, mix.shape[-1])
        xt = xt * stdt[:, None] + meant[:, None]
        return x, xt


def example_inputs(model, seed: int = 0) -> tuple[torch.Tensor, torch.Tensor]:
    """A deterministic segment and its spectrogram, from the model itself."""
    gen = torch.Generator().manual_seed(seed)
    mix = torch.randn(1, dsp.CHANNELS, dsp.SEGMENT, generator=gen) * 0.3
    with torch.no_grad():
        spec = model._magnitude(model._spec(mix))
    return mix, spec


def io_shapes() -> dict:
    s, c, f, t, length = (
        len(dsp.SOURCES),
        dsp.CHANNELS,
        dsp.BINS,
        dsp.SEGMENT_FRAMES,
        dsp.SEGMENT,
    )
    return {
        "inputs": [
            {"name": "mix", "dtype": "float32", "shape": [1, c, length]},
            {"name": "spec", "dtype": "float32", "shape": [1, 2 * c, f, t]},
        ],
        "outputs": [
            {
                "name": "spec_out",
                "dtype": "float32",
                "shape": [1, s, 2 * c, f, t],
            },
            {
                "name": "wave_out",
                "dtype": "float32",
                "shape": [1, s, c, length],
            },
        ],
    }


def dsp_parameters() -> dict:
    return {
        "sample_rate": dsp.SAMPLE_RATE,
        "channels": dsp.CHANNELS,
        "sources": list(dsp.SOURCES),
        "segment_samples": dsp.SEGMENT,
        "stride_samples": dsp.STRIDE,
        "overlap": dsp.OVERLAP,
        "overlap_add": "triangular weight [1..L/2, L-L/2..1] / max, power 1; "
        "a short last segment uses the first samples of the weight",
        "segment_padding": "each segment is centred in 343980 samples, with "
        "real neighbouring samples where they exist and zeros past the ends",
        "normalisation": "x = (wav - mean(mono)) / (std(mono, ddof=1) + "
        "1e-8) over the whole "
        "track; stems are de-normalised with the same values",
        "stft": {
            "n_fft": dsp.NFFT,
            "hop": dsp.HOP,
            "window": "hann, periodic",
            "normalized": True,
            "scale": "forward / 64, inverse * 64",
            "center": True,
            "pad_mode": "reflect",
            "pre_pad": f"reflect {dsp.PRE_PAD} left, {dsp.PRE_PAD} + "
            "(ceil(L/1024)*1024 - L) right, before the centred STFT",
            "frames": "ceil(L / 1024); 2 edge frames dropped on each side",
            "bins": f"{dsp.BINS}; the Nyquist bin is dropped and restored as "
            "zero",
        },
        "spec_layout": "complex as channels: [L.re, L.im, R.re, R.im]; "
        "spec_out per stem the same",
        "stem": "iSTFT(spec_out[stem]) + wave_out[stem]",
    }


def export(model, path: Path, impl: str, chunk: int, opset: int) -> None:
    core = Core(model).eval()
    mix, spec = example_inputs(model)
    kwargs = dict(
        input_names=INPUT_NAMES,
        output_names=OUTPUT_NAMES,
        opset_version=opset,
        do_constant_folding=True,
        external_data=False,
    )
    # Under no_grad, nn.MultiheadAttention takes a fused fast path that does
    # not export.
    torch.backends.mha.set_fastpath_enabled(False)
    with torch.no_grad():
        if impl == "attention23":
            torch.onnx.export(
                core,
                (mix, spec),
                str(path),
                dynamo=True,
                optimize=True,
                **kwargs,
            )
        else:
            if impl == "mha":
                kwargs["custom_opsets"] = {"com.microsoft": 1}
            torch.onnx.export(
                core, (mix, spec), str(path), dynamo=False, **kwargs
            )


def check_core(model, impl: str, chunk: int) -> dict:
    """The rewritten core against the upstream forward (sanity check)."""
    mix, spec = example_inputs(model)
    with torch.no_grad():
        upstream = model(mix)
        replaced = model if impl == "plain" else None
        if replaced is None:
            import copy

            replaced = copy.deepcopy(model)
            replace_attention(replaced, impl, chunk)
        x, xt = Core(replaced).eval()(mix, spec)
        full = model._ispec(model._mask(None, x), dsp.SEGMENT) + xt
    err = (full - upstream).abs().max().item()
    return {"max_abs_vs_upstream": err, "peak": upstream.abs().max().item()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="output directory (outside the repo)",
    )
    parser.add_argument(
        "--attention",
        default="chunked",
        choices=["chunked", "plain", "mha", "attention23"],
    )
    parser.add_argument(
        "--chunk",
        type=int,
        default=DEFAULT_CHUNK,
        help=f"query rows per attention chunk (default {DEFAULT_CHUNK})",
    )
    parser.add_argument(
        "--opset",
        type=int,
        default=None,
        help="ONNX opset (default 17; 23 for attention23)",
    )
    args = parser.parse_args()
    opset = args.opset or (23 if args.attention == "attention23" else 17)
    release = (
        args.attention == "chunked"
        and args.chunk == DEFAULT_CHUNK
        and opset == 17
    )
    stem = (
        VERSION
        if release
        else f"{VERSION}.{args.attention}"
        + (f"{args.chunk}" if args.attention == "chunked" else "")
        + f".op{opset}"
    )
    args.out.mkdir(parents=True, exist_ok=True)
    onnx_path = args.out / f"{stem}.onnx"

    torch.manual_seed(0)
    model = load_model()
    sanity = check_core(model, args.attention, args.chunk)
    print(
        "core vs upstream forward: max|diff| "
        f"{sanity['max_abs_vs_upstream']:.3g} (peak {sanity['peak']:.3g})",
        flush=True,
    )
    if args.attention != "plain":
        swapped = replace_attention(model, args.attention, args.chunk)
        assert swapped == 10, swapped
    start = time.perf_counter()
    export(model, onnx_path, args.attention, args.chunk, opset)
    seconds = time.perf_counter() - start

    import onnx

    graph = onnx.load(str(onnx_path))
    ops = {}
    for node in graph.graph.node:
        key = f"{node.domain}::{node.op_type}" if node.domain else node.op_type
        ops[key] = ops.get(key, 0) + 1
    manifest = {
        "name": MODEL_NAME,
        "version": VERSION,
        "file": onnx_path.name,
        "release": release,
        "sha256": sha256_file(onnx_path),
        "size": onnx_path.stat().st_size,
        "format": "onnx",
        "opset": opset,
        "opsets": {
            imp.domain or "ai.onnx": imp.version for imp in graph.opset_import
        },
        "precision": "fp32",
        **io_shapes(),
        "dsp": dsp_parameters(),
        "attention": {
            "impl": args.attention,
            "query_chunk": args.chunk if args.attention == "chunked" else None,
        },
        "graph": {
            "nodes": len(graph.graph.node),
            "op_types": dict(sorted(ops.items())),
        },
        "source_weights": {
            "repo": WEIGHTS_REPO,
            "revision": WEIGHTS_REVISION,
            "file": WEIGHTS_FILE,
            "sha256": WEIGHTS_SHA256,
        },
        "attribution": ATTRIBUTION,
        "onnxruntime": RUNTIME_NOTES,
        "export": {
            "tool": "tools/crate-stems/export_htdemucs.py",
            "python": platform.python_version(),
            "torch": torch.__version__,
            "onnx": onnx.__version__,
            "demucs": _version("demucs"),
            "seconds": round(seconds, 1),
            "core_check": sanity,
        },
    }
    manifest_path = onnx_path.with_suffix(".json")
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    )
    print(
        f"wrote {onnx_path} ({manifest['size'] / 1e6:.1f} MB, sha256 "
        f"{manifest['sha256'][:16]}…, "
        f"{len(graph.graph.node)} nodes) and {manifest_path.name} in "
        f"{seconds:.0f}s"
    )
    return 0


def _version(dist: str) -> str:
    from importlib.metadata import version

    return version(dist)


if __name__ == "__main__":
    sys.exit(main())

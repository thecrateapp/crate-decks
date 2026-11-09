"""Check an htdemucs ONNX export: parity, peak VRAM and speed (ST4.PR2).

Runs one 7.8 s synthetic segment (stems_dsp.synthetic_mix, normalised as Demucs
does) through the graph on ONNX Runtime, with the numpy STFT and iSTFT of
stems_dsp around it, and compares the four stems with the unmodified PyTorch
model (`HTDemucs.forward`, CPU, fp32) on the same segment:

- SDR per stem, CPU and CUDA providers: at least 60 dB is required;
- peak VRAM of the process on the CUDA provider (nvidia-smi), per session
  setup;
- median time per segment on CUDA and on CPU (8 threads), after one warm-up
  run.

Each measurement runs in its own process, so the VRAM peaks do not mix.

Session setups on the CUDA provider:

recommended
    ExecutionMode.ORT_PARALLEL with two inter-op threads. The parallel
    executor turns off the allocation planner's buffer reuse, which otherwise
    keeps encoder buffers allocated until the decoder reuses them. With one
    inter-op thread ONNX Runtime falls back to the sequential executor, and
    to its peak. This is the setup Decks uses on the CUDA provider.
sequential
    ONNX Runtime's defaults (sequential executor, memory pattern on).
no_pattern
    Sequential with enable_mem_pattern=False (the ST0 measurement).

Usage:
    python tools/crate-stems/check_export.py MODEL.onnx [--report OUT.json]
    python tools/crate-stems/check_export.py MODEL.onnx \
        --setups recommended --skip-cpu

Exit status 1 when a stem is below 60 dB or the recommended setup peaks above
1.5 GB (1536 MiB).
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import numpy as np

import stems_dsp as dsp

SDR_MIN_DB = 60.0
VRAM_MAX_MIB = 1536
SETUPS = {
    "recommended": {
        "execution_mode": "parallel",
        "inter_op_threads": 2,
        "mem_pattern": True,
    },
    "sequential": {"execution_mode": "sequential", "mem_pattern": True},
    "no_pattern": {"execution_mode": "sequential", "mem_pattern": False},
}


def segment_inputs() -> tuple[np.ndarray, np.ndarray]:
    """The first segment of a synthetic track and its STFT.

    The segment is normalised over the whole track, as Demucs does.
    """
    track = dsp.synthetic_mix(dsp.SEGMENT / dsp.SAMPLE_RATE, seed=3)
    mean, scale = dsp.normalisation(track)
    mix = ((track.astype(np.float64) - mean) / scale)[None].astype(np.float32)
    return mix, dsp.to_cac(dsp.stft(mix[0]))


class GpuPeak:
    """Polls nvidia-smi for this process's GPU memory (MiB)."""

    def __init__(self):
        self.peak, self._stop = 0, threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        pid = str(os.getpid())
        while not self._stop.is_set():
            out = subprocess.run(
                [
                    "nvidia-smi",
                    "--query-compute-apps=pid,used_memory",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True,
                text=True,
            ).stdout
            for line in out.splitlines():
                p, mem = (s.strip() for s in line.split(","))
                if p == pid:
                    self.peak = max(self.peak, int(mem))
            self._stop.wait(0.05)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join()


def worker(
    model_path: str,
    ep: str,
    setup: dict,
    runs: int,
    threads: int,
    out_dir: str,
) -> dict:
    """Run the segment on one provider and setup; save the stems, time it."""
    import onnxruntime as ort

    if ep == "cuda":
        ort.preload_dlls(cuda=True, cudnn=True)
    ort.set_default_logger_severity(3)
    so = ort.SessionOptions()
    so.log_severity_level = 3
    so.enable_mem_pattern = setup.get("mem_pattern", True)
    if setup.get("execution_mode") == "parallel":
        so.execution_mode = ort.ExecutionMode.ORT_PARALLEL
        so.inter_op_num_threads = setup.get("inter_op_threads", 1)
    if ep == "cpu":
        so.intra_op_num_threads = threads
        so.inter_op_num_threads = 1
        providers = ["CPUExecutionProvider"]
    else:
        providers = [
            ("CUDAExecutionProvider", setup.get("cuda_options", {})),
            "CPUExecutionProvider",
        ]
    mix, spec = segment_inputs()
    feeds = {"mix": mix, "spec": spec}
    res: dict = {"ep": ep, "setup": setup}
    gpu = GpuPeak() if ep == "cuda" else None
    if gpu:
        gpu.__enter__()
    try:
        start = time.perf_counter()
        sess = ort.InferenceSession(model_path, so, providers=providers)
        res["session_s"] = round(time.perf_counter() - start, 3)
        wanted = providers[0] if ep == "cpu" else providers[0][0]
        assert sess.get_providers()[0] == wanted, sess.get_providers()
        times = []
        for _ in range(runs + 1):
            start = time.perf_counter()
            spec_out, wave_out = sess.run(None, feeds)
            times.append(time.perf_counter() - start)
        res["first_run_s"] = round(times[0], 3)
        res["median_s"] = round(float(np.median(times[1:])), 4)
        res["runs"] = runs
    finally:
        if gpu:
            gpu.__exit__()
            res["vram_peak_mib"] = gpu.peak
    np.save(Path(out_dir) / "stems.npy", dsp.segment_stems(spec_out, wave_out))
    return res


def reference_stems() -> np.ndarray:
    """The unmodified PyTorch htdemucs on CPU in fp32.

    Returns the (S, C, L) stems in the normalised domain.
    """
    import torch

    import export_htdemucs as ex

    torch.set_num_threads(8)
    model = ex.load_model()
    mix, _ = segment_inputs()
    with torch.no_grad():
        return model(torch.from_numpy(mix))[0].numpy().astype(np.float64)


def sdr_db(ref: np.ndarray, est: np.ndarray) -> float:
    err = np.sum((ref - est) ** 2)
    return float(10 * np.log10(np.sum(ref**2) / max(err, 1e-30)))


def run_worker(
    model: Path, ep: str, setup_name: str, runs: int, threads: int
) -> tuple[dict, np.ndarray | None]:
    setup = SETUPS.get(setup_name, {})
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [
            sys.executable,
            str(Path(__file__).resolve()),
            str(model),
            "--worker",
            ep,
            "--setup-json",
            json.dumps(setup),
            "--runs",
            str(runs),
            "--threads",
            str(threads),
            "--out-dir",
            tmp,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        lines = [ln for ln in proc.stdout.splitlines() if ln.startswith("{")]
        if proc.returncode != 0 or not lines:
            return {
                "ep": ep,
                "setup_name": setup_name,
                "error": proc.stderr[-2000:],
            }, None
        res = json.loads(lines[-1])
        res["setup_name"] = setup_name
        return res, np.load(Path(tmp) / "stems.npy")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("model", type=Path)
    parser.add_argument(
        "--report", type=Path, help="JSON report (default: MODEL.check.json)"
    )
    parser.add_argument(
        "--setups",
        default="recommended,sequential,no_pattern",
        help="CUDA session setups to measure, comma-separated",
    )
    parser.add_argument("--cuda-runs", type=int, default=10)
    parser.add_argument("--cpu-runs", type=int, default=3)
    parser.add_argument(
        "--threads", type=int, default=8, help="CPU intra-op threads"
    )
    parser.add_argument("--skip-cpu", action="store_true")
    parser.add_argument(
        "--worker", choices=["cpu", "cuda"], help=argparse.SUPPRESS
    )
    parser.add_argument("--setup-json", default="{}", help=argparse.SUPPRESS)
    parser.add_argument("--runs", type=int, default=3, help=argparse.SUPPRESS)
    parser.add_argument("--out-dir", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.worker:
        print(
            json.dumps(
                worker(
                    str(args.model),
                    args.worker,
                    json.loads(args.setup_json),
                    args.runs,
                    args.threads,
                    args.out_dir,
                )
            )
        )
        return 0

    import onnxruntime as ort

    ref = reference_stems()
    results = []
    jobs = [
        ("cuda", name, args.cuda_runs)
        for name in args.setups.split(",")
        if name
    ]
    if not args.skip_cpu:
        jobs.append(("cpu", "cpu", args.cpu_runs))
    for ep, name, runs in jobs:
        res, stems = run_worker(args.model, ep, name, runs, args.threads)
        if stems is not None:
            res["sdr_db"] = {
                s: round(sdr_db(ref[i], stems[i]), 2)
                for i, s in enumerate(dsp.SOURCES)
            }
            res["max_abs"] = round(float(np.abs(ref - stems).max()), 7)
        results.append(res)
        print(json.dumps(res), flush=True)

    failures = []
    for res in results:
        if "error" in res:
            failures.append(
                f"{res['ep']}/{res['setup_name']}: {res['error'][-300:]}"
            )
            continue
        low = {s: v for s, v in res["sdr_db"].items() if v < SDR_MIN_DB}
        if low:
            failures.append(
                f"{res['ep']}/{res['setup_name']}: SDR below {SDR_MIN_DB} "
                f"dB: {low}"
            )
        if (
            res["ep"] == "cuda"
            and res["setup_name"] == "recommended"
            and res["vram_peak_mib"] > VRAM_MAX_MIB
        ):
            failures.append(
                f"recommended setup peaks at {res['vram_peak_mib']} MiB"
            )

    manifest_path = args.model.with_suffix(".json")
    manifest = (
        json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    )
    report = {
        "model": args.model.name,
        "version": manifest.get("version"),
        "sha256": manifest.get("sha256"),
        "size": args.model.stat().st_size,
        "attention": manifest.get("attention"),
        "input": "first 7.8 s segment of stems_dsp.synthetic_mix(seed=3), "
        "normalised over the track",
        "reference": "unmodified PyTorch htdemucs forward on CPU, fp32",
        "requirements": {
            "sdr_min_db": SDR_MIN_DB,
            "vram_max_mib_recommended": VRAM_MAX_MIB,
        },
        "setups": SETUPS,
        "results": results,
        "pass": not failures,
        "failures": failures,
        "environment": {
            "onnxruntime": ort.__version__,
            "python": platform.python_version(),
            "machine": platform.machine(),
            "load_average": os.getloadavg(),
            "gpu": _gpu_name(),
            "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        },
    }
    report_path = args.report or args.model.with_suffix(".check.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {report_path}: {'PASS' if not failures else 'FAIL'}")
    for f in failures:
        print("  " + f)
    return 0 if not failures else 1


def _gpu_name() -> str | None:
    try:
        return (
            subprocess.run(
                ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
                capture_output=True,
                text=True,
            ).stdout.strip()
            or None
        )
    except FileNotFoundError:
        return None


if __name__ == "__main__":
    sys.exit(main())

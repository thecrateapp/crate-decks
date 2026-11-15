# Crate Decks — Hybrid laptops: discrete GPU and a system check

> **For agents:** approved by Diego on 2026-11-15 (§5). Phase HL of the implementation plan, before F6.
> AGENTS.md applies (no push or PR unless asked, conventional commits, each task starts with its red test).
>
> Design: [2026-11-02-crate-decks-design.md](2026-11-02-crate-decks-design.md) (§9 budgets).

**Date:** 2026-11-14. **Base:** `integ/decks-st5` @ `bb2c426845`.

## 1. What happened

On Diego's Dell XPS 16 9640 (Meteor Lake with Intel Arc integrated graphics, RTX 4060, Hyprland on three
monitors: 3840 × 2400 at 90 Hz plus two 2560 × 1440 at 60 Hz), the waveforms dropped frames while the audio
played on. Measured on the running app (thread states from `/proc`, per-process GPU time from each
process's `drm-engine-*` counters in `/proc/<pid>/fdinfo`):

- Decks' main thread spent up to 40 % of its time in `drm_syncobj_array_wait_timeout`, single waits of
  250–440 ms: it waited for the GPU to finish a frame. Decks itself used about 2 % of the GPU.
- Hyprland kept the integrated GPU 87–94 % busy compositing the three monitors. Blur, shadows and the
  hyprglass plugin made no difference.
- The firmware held the package power limit (MMIO RAPL PL1) at 20 W: the integrated GPU ran at
  400–650 MHz of its 2350 (`throttle_reason_pl1`). Raising PL1 to 45 W on AC gave it its full clock,
  Hyprland's load fell to 35–65 % and the waveforms stopped dropping frames.
- With 45 W, a burst of background CPU load (an indexer, a code graph rebuild) took the package to 101 °C;
  the firmware then throttled the integrated GPU on temperature (`throttle_reason_ratl`) and the drops came
  back until the load passed.
- Rendering Decks on the RTX 4060 (PRIME offload) removed Decks' GPU waits entirely. Through EGL on Wayland
  Qt gets an OpenGL ES context, whose waveform shaders did not compile until
  `fix/decks-gles-waveforms`. Through GLX on XWayland Decks crashed: that turned out to be the HID race
  fixed in `fix/decks-hid-open-race`, not the driver.

The lesson: a DJ app on a hybrid laptop can stutter for reasons that are not in the app (the compositor, the
firmware's power and thermal limits), and a user cannot tell which. Decks should measure and say so.

## 2. Render on the discrete GPU (preference)

- **Preferences > Interface > "Draw with the discrete GPU"** (off by default; offered, and recommended by
  §3, when a discrete GPU is present).
- It must take effect before Qt creates its first OpenGL context, so it is read from `mixxx.cfg` in `main()`
  before `QApplication` (a hook in `src/main.cpp`, already in the closed patch list), and applies on the
  next start:
  - NVIDIA proprietary: `__NV_PRIME_RENDER_OFFLOAD=1`, `__GLX_VENDOR_LIBRARY_NAME=nvidia`, and
    `__EGL_VENDOR_LIBRARY_FILENAMES` pointing at the NVIDIA vendor file on Wayland.
  - Mesa (AMD, Intel Arc dGPU, nouveau): `DRI_PRIME=1`.
- Never overrides variables the user set.
- Cost: on a laptop whose discrete GPU sleeps, keeping it awake costs battery; the text says so.
- Tests: the environment it produces for each driver/session combination (pure function); the preference
  round-trip; a skin-load test with an OpenGL ES context (the shader key test already exists).

## 3. System check

A model with no widgets (`crate::system::SystemCheck`), run by the first start and on demand from
Preferences, and its page in the first start. Each check reports *ok*, *warning* (with what to do) or
*blocking* (Decks will not play smoothly on this machine):

| Check | Source (Linux) | Warning when |
| --- | --- | --- |
| GPUs and which one draws Decks | `/sys/class/drm/*/device`, the GL renderer string | hybrid and Decks on the integrated one |
| Integrated GPU clock and throttle reasons | `/sys/class/drm/card*/gt/gt0/rps_*`, `throttle_reason_*` | below 60 % of its maximum while busy, PL1 or thermal throttling |
| Package power limit | `/sys/class/powercap/intel-rapl-mmio:0` (and `intel-rapl:0`) | sustained limit below 35 W on AC |
| Compositor GPU load | `drm-engine-*` in `/proc/<pid>/fdinfo` of the compositor | above 75 % for 5 s |
| Refresh rate and resolution of the monitor with Decks | Qt screen | high refresh at high resolution on an integrated GPU |
| Audio buffer and xruns | `SoundManager` | latency too low for the measured load |

- **Blocking** only for what Decks cannot work around (for example no OpenGL 2.1 / ES 2.0, or a sustained
  power limit that keeps the GPU under its minimum); everything else is advice.
- The check measures for a few seconds with the waveforms running, not just reads static values.
- Windows and macOS: the GPU and refresh checks only; the rest reports "not available".
- Tests: each check against a fake `/sys` and `/proc` tree, as the Linux power script's test does.

## 4. Power limit helper (Linux, optional)

The fix that worked on the XPS was a small system service, now at `~/.local/share/crate-power-limit/` on
Diego's machine: it sets PL1 to 45 W on AC and back to 20 W on battery, at boot, on charger changes (udev)
and after resume. Decks could offer it from the system check on laptops whose firmware holds PL1 low:

- Shipped in the Linux packages (AUR, `.deb`), installed and enabled only when the user accepts, with
  `pkexec`; never silently.
- The values are the user's: AC and battery watts in `/etc/default/crate-power-limit`.
- It raises temperature: the check warns when the package stays above 95 °C with it on.

## 5. Decisions (Diego, 2026-11-15)

1. **Decided:** §2 goes ahead. The discrete GPU preference is **off by default**, also on hybrid laptops;
   the system check recommends turning it on when the integrated GPU falls short.
2. **Decided:** §3 goes ahead. The system check runs **in the first start** (its own page) **and from
   Preferences** (a button that runs it again and shows the results).
3. **Decided:** Decks **ships** the power limit helper of §4 in its Linux packages and **offers** it from
   the system check. It is installed with `pkexec` only when the user accepts; never silently.
4. **Decided:** these land **now, before F6** (phase HL of the implementation plan).

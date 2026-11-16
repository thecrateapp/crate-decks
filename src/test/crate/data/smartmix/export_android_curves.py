#!/usr/bin/env python3
"""Writes android-curves.csv, the reference table of curves_test.cpp: the
Smart Mix curves as the Android player computes them.

It does not copy the formulas: it takes the source of Android's own methods
from the Crate monorepo (app/listen/android, package app.cratemusic.crate),
compiles them with javac into a small class and runs them over a grid of
progress values. Android's arithmetic is `float`, so the table has the
precision the phone has.

- NativeMixAudioProcessor.java: djBlendProgress, smoothstep,
  equalPowerOutgoing, equalPowerIncoming, staging, easeIn, easeOut and
  clampUnit verbatim; the staging gain expression of startEnvelope; and the
  two statements of envelopeFactorForNextFrame that combine them, with the
  envelope's fields as parameters.
- BassHandoffFilter.java: cutDb verbatim, with CUT_DB and SWAP_FRACTION.

    python3 export_android_curves.py <crate monorepo> [git ref] [output directory]

The ref defaults to origin/main; the files are read with `git show`, so the
checkout's branch does not matter. JAVA_HOME (or javac and java on the PATH)
must point to a JDK. Only the standard library otherwise; the output is
deterministic.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PACKAGE_DIR = "app/listen/android/app/src/main/java/app/cratemusic/crate"
PROCESSOR = "NativeMixAudioProcessor.java"
BASS = "BassHandoffFilter.java"

# The plan gains of real-beatmatch.json, so the gain columns are realistic.
OUTGOING_GAIN_DB = -4.2867
INCOMING_GAIN_DB = -3.7616
# Handoff points: the usual middle, and two that cutDb clamps.
HANDOFFS = (("050", 0.5), ("002", 0.02), ("097", 0.97))


def git_show(repo, ref, path):
    return subprocess.run(
        ["git", "-C", repo, "show", f"{ref}:{path}"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def method(source, name):
    """The whole declaration of method `name`, from its modifiers to its
    closing brace."""
    match = re.search(r"\n(\s*(?:private |public |protected )?static [\w.<>\[\]]+ " + name + r"\()", source)
    if not match:
        sys.exit(f"method {name} not found")
    start = match.start(1)
    depth = 0
    for index in range(source.index("{", start), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1].strip()
    sys.exit(f"method {name} not closed")


def constant(source, name):
    match = re.search(r"static final \w+ " + name + r" = [^;]+;", source)
    if not match:
        sys.exit(f"constant {name} not found")
    return "static final " + match.group(0).split("static final ", 1)[1]


def staging_gain_expression(source):
    match = re.search(r"float stagingGain = Float\.isFinite\(stagingGainDb\)\s*\?\s*(.+?)\s*:\s*1\.0f;", source, re.S)
    if not match:
        sys.exit("startEnvelope's staging gain not found")
    return match.group(1)


def envelope_statements(source):
    body = method_body(source, "envelopeFactorForNextFrame")
    match = re.search(r"(float crossfade = .+?;)\s*(envelopeFactor = .+?;)", body, re.S)
    if not match:
        sys.exit("envelopeFactorForNextFrame's statements not found")
    statements = match.group(1) + "\n        float " + match.group(2)
    for field in ("djBlend", "role", "stagingGain"):
        statements = statements.replace("activeEnvelope." + field, field)
    return statements


def method_body(source, name):
    start = source.index(" " + name + "() {")
    depth = 0
    for index in range(source.index("{", start), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index]
    sys.exit(f"method {name} not closed")


def java_program(processor, bass):
    methods = "\n\n    ".join(
        method(processor, name)
        for name in (
            "djBlendProgress",
            "smoothstep",
            "equalPowerOutgoing",
            "equalPowerIncoming",
            "staging",
            "easeIn",
            "easeOut",
            "clampUnit",
        )
    )
    cut_db = method(bass, "cutDb").replace("NativeMixAudioProcessor.Role", "Role")
    handoff_columns = "".join(f',bass_out_{label},bass_in_{label}' for label, _ in HANDOFFS)
    handoff_values = "".join(
        f' + "," + f(cutDb(Role.OUTGOING, p, {value}f)) + "," + f(cutDb(Role.INCOMING, p, {value}f))'
        for _, value in HANDOFFS
    )
    return f"""
import java.util.Locale;

public final class AndroidCurves {{
    enum Role {{ OUTGOING, INCOMING }}

    {constant(processor, "DJ_BLEND_ENTRY")}
    {constant(processor, "DJ_BLEND_EXIT")}
    {constant(bass, "CUT_DB")}
    {constant(bass, "SWAP_FRACTION")}

    {methods}

    {cut_db}

    static float stagingGainOf(float stagingGainDb) {{
        return {staging_gain_expression(processor)};
    }}

    static float envelope(Role role, float progress, float stagingGain, boolean djBlend) {{
        {envelope_statements(processor)}
        return envelopeFactor;
    }}

    static String f(float value) {{
        return String.format(Locale.ROOT, "%.9g", value);
    }}

    public static void main(String[] args) {{
        float outGain = stagingGainOf({OUTGOING_GAIN_DB}f);
        float inGain = stagingGainOf({INCOMING_GAIN_DB}f);
        System.out.println("progress,equal_power_out,equal_power_in,dj_blend,staging_out,staging_in,"
            + "gain_out,gain_in,gain_out_dj_blend,gain_in_dj_blend{handoff_columns}");
        float[] extra = {{-0.1f, 0.3f, 0.7f, 0.4375f, 0.5625f, 0.06f, 0.94f, 1.1f}};
        float[] grid = new float[65 + extra.length];
        for (int i = 0; i <= 64; i++) grid[i] = i / 64.0f;
        System.arraycopy(extra, 0, grid, 65, extra.length);
        for (float p : grid) {{
            System.out.println(f(p)
                + "," + f(equalPowerOutgoing(p)) + "," + f(equalPowerIncoming(p))
                + "," + f(djBlendProgress(p))
                + "," + f(staging(outGain, easeIn(p))) + "," + f(staging(inGain, easeOut(p)))
                + "," + f(envelope(Role.OUTGOING, p, outGain, false))
                + "," + f(envelope(Role.INCOMING, p, inGain, false))
                + "," + f(envelope(Role.OUTGOING, p, outGain, true))
                + "," + f(envelope(Role.INCOMING, p, inGain, true))
                {handoff_values});
        }}
    }}
}}
"""


def tool(name):
    home = os.environ.get("JAVA_HOME")
    if home:
        return str(Path(home) / "bin" / name)
    found = shutil.which(name)
    if not found:
        sys.exit(f"{name} not found: set JAVA_HOME to a JDK")
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    repo = sys.argv[1]
    ref = sys.argv[2] if len(sys.argv) > 2 else "origin/main"
    output = Path(sys.argv[3] if len(sys.argv) > 3 else Path(__file__).parent)
    processor = git_show(repo, ref, f"{PACKAGE_DIR}/{PROCESSOR}")
    bass = git_show(repo, ref, f"{PACKAGE_DIR}/{BASS}")
    commit = subprocess.run(
        ["git", "-C", repo, "rev-parse", "--short=10", ref],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "AndroidCurves.java"
        source.write_text(java_program(processor, bass))
        subprocess.run([tool("javac"), "-d", directory, str(source)], check=True)
        table = subprocess.run(
            [tool("java"), "-cp", directory, "AndroidCurves"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    header = (
        f"# Generated by export_android_curves.py from the Crate monorepo at {commit}\n"
        f"# ({PROCESSOR}, {BASS}). Gains: outgoing {OUTGOING_GAIN_DB} dB, "
        f"incoming {INCOMING_GAIN_DB} dB. bass_*_NNN: handoff at 0.NNN.\n"
    )
    (output / "android-curves.csv").write_text(header + table)


if __name__ == "__main__":
    main()

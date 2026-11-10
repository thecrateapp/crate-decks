#!/usr/bin/env python3
"""Extract the Solar icon subset used by Crate Decks.

Reads the icon modules of an unpacked ``@solar-icons/react`` npm package
(``package/dist/icons/<style>/<name>.mjs``) and writes one plain SVG per
icon to ``tools/crate-design/svg/solar/``. The modules are parsed as text,
never executed. Only the subset listed in ``svg/solar/SUBSET`` is
extracted; each line is ``<style>/<name>``.

Usage:
    npm pack @solar-icons/react@2.3.3 && tar xzf solar-icons-react-2.3.3.tgz
    python3 tools/crate-design/extract_solar.py package
"""

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOLAR_DIR = HERE / "svg" / "solar"
SUBSET_FILE = SOLAR_DIR / "SUBSET"

# jsx(`path`, {d: `...`, fill: `currentColor`}) calls without children
ELEMENT_RE = re.compile(r"\w\(`(\w+)`,\{([^{}]*)\}\)")
ALIAS_RE = re.compile(
    r'import\{\w+ as \w+\}from"\./([a-z0-9-]+\.mjs)";'
    r"const \w+=\w+;export\{\w+ as \w+\};"
)
ATTRIBUTE_RE = re.compile(r"(\w+):(`[^`]*`|-?[\d.]+)")
ALLOWED_ELEMENTS = {"path", "circle", "rect", "ellipse", "line", "polyline"}
ALLOWED_ATTRIBUTES = {
    "d",
    "fill",
    "fillRule",
    "clipRule",
    "cx",
    "cy",
    "r",
    "rx",
    "ry",
    "x",
    "y",
    "x1",
    "y1",
    "x2",
    "y2",
    "width",
    "height",
    "points",
    "stroke",
    "strokeWidth",
    "strokeLinecap",
    "strokeLinejoin",
    "opacity",
}


def kebab(name):
    return re.sub(r"(?<!^)([A-Z])", r"-\1", name).lower()


def read_subset(path):
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            style, _, name = line.partition("/")
            if style not in ("outline", "bold") or not name:
                raise ValueError(f"{path}: bad entry {line!r}")
            entries.append((style, name))
    return entries


def module_to_svg(source, origin):
    elements = ELEMENT_RE.findall(source)
    if not elements:
        raise ValueError(f"{origin}: no SVG elements found")
    body = []
    for tag, attributes in elements:
        if tag not in ALLOWED_ELEMENTS:
            raise ValueError(f"{origin}: unsupported element <{tag}>")
        pairs = ATTRIBUTE_RE.findall(attributes)
        names = {name for name, _ in pairs}
        unknown = names - ALLOWED_ATTRIBUTES
        if unknown:
            raise ValueError(f"{origin}: unsupported attributes {unknown}")
        rendered = " ".join(
            f'{kebab(name)}="{value.strip("`")}"' for name, value in pairs
        )
        body.append(f"  <{tag} {rendered}/>")
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24"'
        ' viewBox="0 0 24 24" fill="none">\n' + "\n".join(body) + "\n</svg>\n"
    )


def output_name(style, name):
    return f"{name}-bold.svg" if style == "bold" else f"{name}.svg"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("package", type=Path, help="unpacked npm package")
    args = parser.parse_args(argv)
    icons_dir = args.package / "dist" / "icons"
    wanted = read_subset(SUBSET_FILE)
    expected = set()
    for style, name in wanted:
        module = icons_dir / style / f"{name}.mjs"
        source = module.read_text(encoding="utf-8")
        # Some names are aliases that re-export another icon module
        alias = ALIAS_RE.fullmatch(source.strip())
        if alias:
            module = module.with_name(alias.group(1))
            source = module.read_text(encoding="utf-8")
        svg = module_to_svg(source, module)
        target = SOLAR_DIR / output_name(style, name)
        target.write_text(svg, encoding="utf-8")
        expected.add(target.name)
    stale = {p.name for p in SOLAR_DIR.glob("*.svg")} - expected
    for name in sorted(stale):
        (SOLAR_DIR / name).unlink()
    print(f"{len(expected)} icons written, {len(stale)} removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

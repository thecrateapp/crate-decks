#!/usr/bin/env python3
"""Checks that an installed Crate Decks names no Mixxx in its paths.

Crate Decks installs under its own names (plan BR2, BR2.PR3): its data in
share/crate-decks, its documentation in share/doc/crate-decks, only its own
skin, no Mixxx manuals. Run it on a tree made by `cmake --install build
--prefix <dir>`; it fails on any installed file or folder whose path names
Mixxx, apart from upstream identifiers that nobody reads as a product name.

Standard library only.
"""

import argparse
import os
import re
import sys

MIXXX = re.compile(r"mixxx", re.IGNORECASE)
# Mixxx's skins carry its logo: only Crate Decks' own skin is installed
SKIN = re.compile(r"(^|/)skins/([^/]+)$")
OWN_SKIN = "CrateDecks"

# Upstream identifiers that stay (plan BR2, section 3): the base name of the
# translations, files of the controller scripting API, the QML module, and
# the hardware named after Mixxx.
ALLOWED = (
    re.compile(r"(^|/)translations/mixxx_[A-Za-z0-9_]+\.qm$"),
    re.compile(r"(^|/)controllers/lodash\.mixxx\.js$"),
    re.compile(r"(^|/)controllers/mixxx-controls\.d\.ts$"),
    re.compile(r"(^|/)qml/Mixxx(/|$)"),
    re.compile(r"MiniMixxx"),
)


def offending(prefix):
    """The installed paths under `prefix` that name Mixxx or are one of its
    skins, sorted."""
    found = []
    for root, dirs, files in os.walk(prefix):
        for name in dirs + files:
            rel = os.path.relpath(os.path.join(root, name), prefix)
            rel = rel.replace(os.sep, "/")
            skin = SKIN.search(rel)
            if skin and name in dirs and name != OWN_SKIN:
                found.append(rel)
                continue
            if not MIXXX.search(name):
                continue
            if any(pattern.search(rel) for pattern in ALLOWED):
                continue
            found.append(rel)
    return sorted(found)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("prefix", help="the installation prefix")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.prefix):
        print(f"{args.prefix} is not a directory")
        return 1
    found = offending(args.prefix)
    for rel in found:
        print(f"installed path names Mixxx or is a Mixxx skin: {rel}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())

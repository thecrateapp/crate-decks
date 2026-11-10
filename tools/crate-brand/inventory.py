#!/usr/bin/env python3
"""Inventory of the source strings that name Mixxx, for Crate Decks' brand.

Crate Decks shows nothing of Mixxx (design D2). The brand layer
(src/crate/brand/) rewrites at run time every translatable string that names
Mixxx, so Mixxx's sources stay as they are. res/crate-brand/mixxx-strings.json
lists those strings and how each one is branded:

- "rule": the generic rule of crate::brandText() (Mixxx -> Crate Decks);
- "text": a curated English text ("es": its Spanish), where the rule would
  say something false, such as "Crate Decks versions before 1.11";
- "keep": left as it is, with the reason ("why"): the credits in About (D2).

It also lists the hard-coded literals that name Mixxx and are allowed to stay
("why": an identifier, a log line...). Anything visible must be translatable
or patched instead; a literal that Crate Decks shows through crate::brandText()
can carry the text to show instead ("shown", and its Spanish "es").

This tool rescans the sources and rewrites the inventory, keeping the
decisions already made. With --check it changes nothing and fails when the
inventory is out of date, as after a merge from Mixxx that adds a string:
review the new entries, then run it without --check.

Standard library only.
"""

import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INVENTORY = os.path.join("res", "crate-brand", "mixxx-strings.json")

# "Mixxx" as a word the user reads: not in a file name, a path, a URL, an
# e-mail or a longer word (mixxxdb.sqlite, ~/.mixxx, mixxx.org, MixxxControl,
# MiniMixxx, mixxx-devel@). A capital "Mixxx-" starts a compound word, as in
# Finnish "Mixxx-yhteisö". For the literals of the sources, which are English,
# it is stricter than kNamesMixxx in brandtext.cpp, which also brands the name
# glued to other scripts in translations ("启动Mixxx时").
NAMES_MIXXX = re.compile(r"(?<![\w./\\@:])(?:Mixxx|MIXXX|mixxx(?!-))(?![\w@]|[./\\:]+\w)")
# Any form of the name, for translatable strings: "MixxxControl" in an error
# dialog is a Mixxx term the user reads, even if the rule leaves it alone.
MENTIONS_MIXXX = re.compile(r"mixxx", re.IGNORECASE)

TR_CALL = re.compile(r"(?<![\w])(?:tr|qsTr|QT_TR_NOOP|QT_TR_N_NOOP)\(\s*$")
TRANSLATE_CALL = re.compile(
    r"(?<![\w])(?:translate|QT_TRANSLATE_NOOP|QT_TRANSLATE_NOOP3|QT_TRANSLATE_N_NOOP)\(\s*$"
)
SOURCE_SUFFIXES = (".cpp", ".h", ".mm")
QML_SUFFIXES = (".qml", ".js")

ESCAPES = {
    "n": "\n",
    "t": "\t",
    "r": "\r",
    '"': '"',
    "'": "'",
    "\\": "\\",
    "?": "?",
    "a": "\a",
    "b": "\b",
    "f": "\f",
    "v": "\v",
}


def unescape(body):
    """The text of a C++ string literal's body."""
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\" or i + 1 >= len(body):
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n in ESCAPES:
            out.append(ESCAPES[n])
            i += 2
        elif n == "u" or n == "U":
            width = 4 if n == "u" else 8
            out.append(chr(int(body[i + 2 : i + 2 + width], 16)))
            i += 2 + width
        elif n == "x":
            m = re.match(r"[0-9a-fA-F]+", body[i + 2 :])
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        elif n in "01234567":
            m = re.match(r"[0-7]{1,3}", body[i + 1 :])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        else:
            out.append(n)
            i += 2
    return "".join(out)


def tokens(source):
    """Splits C++ (or QML/JS) into ("code", text) and ("string", text) tokens.

    Comments become a space; character literals stay code."""
    result = []
    code = []
    i = 0
    length = len(source)

    def flush():
        if code:
            result.append(("code", "".join(code)))
            code.clear()

    while i < length:
        c = source[i]
        if source.startswith("//", i):
            end = source.find("\n", i)
            i = length if end < 0 else end
            code.append(" ")
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = length if end < 0 else end + 2
            code.append(" ")
        elif c == "R" and source.startswith('R"', i) and (i == 0 or not source[i - 1].isalnum()):
            open_paren = source.find("(", i + 2)
            delimiter = source[i + 2 : open_paren]
            close = source.find(")" + delimiter + '"', open_paren)
            flush()
            result.append(("string", source[open_paren + 1 : close]))
            i = close + len(delimiter) + 2
        elif c == '"':
            j = i + 1
            while j < length and source[j] != '"':
                j += 2 if source[j] == "\\" else 1
            flush()
            result.append(("string", unescape(source[i + 1 : j])))
            i = j + 1
        elif c == "'" and not (i > 0 and source[i - 1].isdigit()):
            j = i + 1
            while j < length and source[j] != "'" and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            code.append(source[i : j + 1])
            i = j + 1
        else:
            code.append(c)
            i += 1
    flush()
    return result


def literal_groups(source):
    """Adjacent string literals joined, each with the code before it."""
    groups = []
    current = None
    before = ""
    for kind, text in tokens(source):
        if kind == "string":
            if current is not None and not before.strip():
                current["text"] += text
            else:
                current = {"text": text, "before": before}
                groups.append(current)
            before = ""
        else:
            if current is not None and text.strip():
                current = None
            before = text if current is None else before + text
    return groups


def scan_code(source):
    """The translatable sources and the other literals of one C++/QML file."""
    translatable = []
    others = []
    groups = literal_groups(source)
    for index, group in enumerate(groups):
        before = group["before"]
        if TR_CALL.search(before):
            translatable.append(group["text"])
            continue
        if TRANSLATE_CALL.search(before):
            continue  # the context
        if (
            index > 0
            and re.fullmatch(r"\s*,\s*", before)
            and TRANSLATE_CALL.search(groups[index - 1]["before"])
        ):
            translatable.append(group["text"])
            continue
        others.append(group["text"])
    return translatable, others


def scan_ui(path):
    """The translatable strings of a Qt Designer form."""
    strings = []
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError:
        return strings
    for element in root.iter("string"):
        if element.get("notr") != "true" and element.text:
            strings.append(element.text)
    return strings


def walk(repo, top, suffixes, skip=()):
    for root, dirs, files in os.walk(os.path.join(repo, top)):
        rel_root = os.path.relpath(root, repo).replace(os.sep, "/")
        dirs[:] = sorted(d for d in dirs if f"{rel_root}/{d}" not in skip)
        for name in sorted(files):
            if name.endswith(suffixes):
                yield os.path.join(root, name), f"{rel_root}/{name}"


def scan(repo):
    """What names Mixxx in `repo`: {source: files} and [(file, literal)]."""
    strings = {}
    literals = set()

    def add(text, rel):
        strings.setdefault(text, set()).add(rel)

    # The tests and the brand layer itself name Mixxx on purpose
    skip = ("src/test", "src/crate/brand")
    for path, rel in walk(repo, "src", SOURCE_SUFFIXES + (".ui",), skip=skip):
        if rel.endswith(".ui"):
            for text in scan_ui(path):
                if MENTIONS_MIXXX.search(text):
                    add(text, rel)
            continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            translatable, others = scan_code(handle.read())
        for text in translatable:
            if MENTIONS_MIXXX.search(text):
                add(text, rel)
        for text in others:
            if NAMES_MIXXX.search(text):
                literals.add((rel, text))
    for path, rel in walk(repo, os.path.join("res", "qml"), QML_SUFFIXES):
        with open(path, encoding="utf-8", errors="replace") as handle:
            translatable, _ = scan_code(handle.read())
        for text in translatable:
            if MENTIONS_MIXXX.search(text):
                add(text, rel)
    return strings, sorted(literals)


def load(path):
    if not os.path.exists(path):
        return {"strings": [], "literals": []}
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def updated(inventory, strings, literals):
    """The inventory for the scanned sources, keeping the decisions made."""
    known = {entry["source"]: entry for entry in inventory.get("strings", [])}
    new_strings = []
    for source in sorted(strings, key=lambda s: (sorted(strings[s])[0], s)):
        entry = dict(known.get(source, {"treatment": "rule"}))
        entry["source"] = source
        entry["files"] = sorted(strings[source])
        new_strings.append(
            {k: entry[k] for k in ("source", "files", "treatment", "text", "es", "why") if k in entry}
        )
    known_literals = {(e["file"], e["text"]): e for e in inventory.get("literals", [])}
    new_literals = []
    for rel, text in literals:
        entry = known_literals.get((rel, text), {"why": ""})
        literal = {"file": rel, "text": text, "why": entry.get("why", "")}
        for key in ("shown", "es"):
            if key in entry:
                literal[key] = entry[key]
        new_literals.append(literal)
    result = {k: v for k, v in inventory.items() if k not in ("strings", "literals")}
    result["strings"] = new_strings
    result["literals"] = new_literals
    return result


def problems(inventory):
    """Entries that need a decision before the inventory is complete."""
    found = []
    for entry in inventory["strings"]:
        treatment = entry.get("treatment")
        if treatment not in ("rule", "text", "keep"):
            found.append(f"unknown treatment {treatment!r}: {entry['source']!r}")
        if treatment == "text" and "text" not in entry:
            found.append(f"'text' without a text: {entry['source']!r}")
        if treatment == "keep" and not entry.get("why"):
            found.append(f"'keep' without a reason: {entry['source']!r}")
    for entry in inventory["literals"]:
        if not entry.get("why"):
            found.append(f"literal without a reason, {entry['file']}: {entry['text']!r}")
    return found


def dump(inventory):
    return json.dumps(inventory, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", default=REPO)
    parser.add_argument("--check", action="store_true", help="fail if out of date")
    args = parser.parse_args(argv)

    path = os.path.join(args.repo, INVENTORY)
    current = load(path)
    strings, literals = scan(args.repo)
    fresh = updated(current, strings, literals)
    if args.check:
        stale = dump(fresh) != dump(current)
        issues = problems(fresh)
        if stale:
            old = {e["source"] for e in current.get("strings", [])}
            new = {e["source"] for e in fresh["strings"]}
            old_literals = {(e["file"], e["text"]) for e in current.get("literals", [])}
            new_literals = {(e["file"], e["text"]) for e in fresh["literals"]}
            for source in sorted(new - old):
                print(f"new string naming Mixxx: {source!r}")
            for source in sorted(old - new):
                print(f"string no longer in the sources: {source!r}")
            for rel, text in sorted(new_literals - old_literals):
                print(f"new literal naming Mixxx, {rel}: {text!r}")
            for rel, text in sorted(old_literals - new_literals):
                print(f"literal no longer in the sources, {rel}: {text!r}")
            print(f"{INVENTORY} is out of date: run tools/crate-brand/inventory.py and review it")
        for issue in issues:
            print(issue)
        return 1 if stale or issues else 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(dump(fresh))
    for issue in problems(fresh):
        print(issue)
    print(f"{len(fresh['strings'])} strings, {len(fresh['literals'])} literals")
    return 0


if __name__ == "__main__":
    sys.exit(main())

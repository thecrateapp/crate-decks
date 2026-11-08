"""Tests for tools/crate-design/generate.py (pytest)."""

import json
import re
import shutil

import pytest

import generate

TOKENS = {
    "meta": {"schema": 1},
    "tokens": {
        "color": {
            "bgApp": {"value": "#0a0a0f"},
            "border": {"value": "#f1f5f9", "alpha": 0.1},
            "borderHover": {"value": "#f1f5f9", "alpha": 0.2},
            "textPrimary": {"value": "#f1f5f9"},
            "accentHi": {"value": "#67e8f9"},
            "textMuted": {"value": "#999ca0"},
            "textFaint": {"value": "#4a4c52"},
            "onAccent": {"value": "#0a0a0f"},
        },
        "iconState": {
            "iconIdle": {"ref": "textMuted"},
            "iconHover": {"ref": "accentHi"},
            "iconActive": {"ref": "accentHi"},
            "iconDisabled": {"ref": "textFaint"},
            "iconOnAccent": {"ref": "onAccent"},
        },
        "geometry": {
            "radiusMd": {"value": 4, "unit": "px"},
            "radiusSm": {"value": 2, "unit": "px"},
            "strokeHairline": {"value": 1, "unit": "px"},
            "tickLong": {"value": 7, "unit": "px"},
            "tickShort": {"value": 3, "unit": "px"},
            "dashQueue": {"value": "3 4"},
            "fontMono": {"value": "JetBrains Mono"},
        },
        "controls": {
            "knobSize": {"value": 40, "unit": "px"},
            "knobSweep": {"value": 270, "unit": "deg"},
            "knobTickCount": {"value": 24},
            "knobTickEvery": {"value": 2},
            "knobTickLong": {"value": 4.5, "unit": "px"},
            "knobTickShort": {"value": 2, "unit": "px"},
            "faderRail": {"value": 1, "unit": "px"},
            "faderTickStep": {"value": 10, "unit": "%"},
            "faderHandleWidth": {"value": 18, "unit": "px"},
            "faderHandleHeight": {"value": 10, "unit": "px"},
        },
    },
}

# Uses every fixture token that the generated scales and handles don't use
QSS_TEMPLATE = """\
QWidget {
  background: {{bgApp}};
  border: {{strokeHairline}} solid {{border}};
  border-radius: {{radiusMd}};
  font-family: "{{fontMono}}";
}
/* {{dashQueue.list}} */
"""

ICON = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">'
    '<path d="M0 0h24v24H0z" fill="currentColor"/></svg>\n'
)


@pytest.fixture
def design(tmp_path):
    """A minimal design directory: tokens, one template and one icon."""
    tokens = tmp_path / "tokens.json"
    tokens.write_text(json.dumps(TOKENS), encoding="utf-8")
    templates = tmp_path / "templates"
    templates.mkdir()
    (templates / "style.qss.in").write_text(QSS_TEMPLATE, encoding="utf-8")
    svg = tmp_path / "svg"
    (svg / "solar").mkdir(parents=True)
    (svg / "controls").mkdir()
    (svg / "solar" / "play.svg").write_text(ICON, encoding="utf-8")
    return {
        "tokens": tokens,
        "templates": templates,
        "svg": svg,
        "out": tmp_path / "generated",
    }


def run(design, check=False):
    return generate.generate(
        design["tokens"],
        design["templates"],
        design["svg"],
        design["out"],
        check,
    )


def test_qss_placeholders_resolved(design):
    run(design)
    qss = (design["out"] / "style.qss").read_text(encoding="utf-8")
    assert "{{" not in qss and "}}" not in qss
    assert "background: #0a0a0f;" in qss
    # QSS alpha is an integer from 0 to 255: 0.1 * 255 rounds to 26
    assert "border: 1px solid rgba(241, 245, 249, 26);" in qss
    assert "border-radius: 4px;" in qss
    assert 'font-family: "JetBrains Mono";' in qss
    assert "/* 3, 4 */" in qss


def test_unknown_token_fails(design):
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE + "QLabel { color: {{textPrimray}}; }\n",
        encoding="utf-8",
    )
    with pytest.raises(generate.GeneratorError) as error:
        run(design)
    assert any(
        "style.qss.in:8: unknown token textPrimray" in problem
        for problem in error.value.problems
    )
    assert not design["out"].exists()


def test_unused_token_fails(design):
    data = json.loads(design["tokens"].read_text(encoding="utf-8"))
    data["tokens"]["color"]["orphan"] = {"value": "#123456"}
    design["tokens"].write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(generate.GeneratorError) as error:
        run(design)
    assert error.value.problems == [
        "orphan: token is not used by any template or SVG"
    ]


def test_svg_recolor_per_state(design):
    run(design)
    expected = {
        "idle": "#999ca0",
        "hover": "#67e8f9",
        "active": "#67e8f9",
        "disabled": "#4a4c52",
        "onaccent": "#0a0a0f",
    }
    assert set(generate.STATES) == set(expected)
    for state, colour in expected.items():
        svg = (design["out"] / "icons" / f"play_{state}.svg").read_text()
        assert "currentColor" not in svg
        assert f'fill="{colour}"' in svg
    handle = design["out"] / "controls" / "fader-handle-v_active.svg"
    assert 'fill="#67e8f9"' in handle.read_text()
    idle = design["out"] / "controls" / "fader-handle-v_idle.svg"
    assert 'stroke="#999ca0"' in idle.read_text()


def test_svg_without_current_color_fails(design):
    icon = design["svg"] / "solar" / "fixed.svg"
    icon.write_text(ICON.replace("currentColor", "#ff0000"))
    with pytest.raises(generate.GeneratorError, match="no currentColor"):
        run(design)


def test_theme_qml_has_all_tokens(tmp_path):
    out = tmp_path / "generated"
    generate.generate(
        generate.TOKENS, generate.TEMPLATES, generate.SVG, out, False
    )
    qml = (out / "Theme.qml").read_text(encoding="utf-8")
    properties = set(
        re.findall(r"^\s*readonly property \w+ (\w+):", qml, re.MULTILINE)
    )
    assert properties == set(generate.load_tokens(generate.TOKENS))
    assert "{{" not in qml


def test_check_mode_detects_drift(design):
    run(design)
    assert run(design, check=True) == []

    qss = design["out"] / "style.qss"
    qss.write_text(qss.read_text() + "/* edited by hand */\n")
    assert run(design, check=True) == ["changed: style.qss"]

    run(design)
    (design["out"] / "icons" / "extra.svg").write_text(ICON)
    assert run(design, check=True) == ["stale: icons/extra.svg"]

    shutil.rmtree(design["out"] / "icons")
    assert all(p.startswith("missing: icons/") for p in run(design, True))


def test_main_check_fails_on_drift(design, capsys):
    run(design)
    (design["out"] / "vars.xml").write_text("<Template/>\n")
    args = [
        "--check",
        "--tokens",
        str(design["tokens"]),
        "--templates",
        str(design["templates"]),
        "--svg",
        str(design["svg"]),
        "--out",
        str(design["out"]),
    ]
    assert generate.main(args) == 1
    assert "stale: vars.xml" in capsys.readouterr().err


def test_refuses_to_replace_a_hand_made_directory(design):
    design["out"].mkdir()
    (design["out"] / "notes.txt").write_text("mine")
    with pytest.raises(generate.GeneratorError, match="not a generated"):
        run(design)
    assert (design["out"] / "notes.txt").exists()


def test_committed_output_is_up_to_date():
    assert (
        generate.generate(
            generate.TOKENS,
            generate.TEMPLATES,
            generate.SVG,
            generate.OUT,
            check=True,
        )
        == []
    )


def test_only_solar_icons():
    allowed = {generate.SVG / "solar", generate.SVG / "controls"}
    for path in generate.HERE.rglob("*"):
        if path.name.endswith((".svg", ".svg.in")):
            assert path.parent in allowed, f"{path}: icons are Solar only"
    subset = set()
    for line in (generate.SVG / "solar" / "SUBSET").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            style, _, name = line.partition("/")
            subset.add(
                f"{name}-bold.svg" if style == "bold" else f"{name}.svg"
            )
    icons = {p.name for p in (generate.SVG / "solar").glob("*.svg")}
    assert icons == subset
    for licence in ("LICENSE", "LICENSE-THIRD-PARTY", "ATTRIBUTION"):
        assert (generate.SVG / "solar" / licence).is_file()
    assert "CC BY 4.0" in (generate.SVG / "solar" / "ATTRIBUTION").read_text()


def test_deck_colour_tokens():
    tokens = generate.load_tokens(generate.TOKENS)
    references = generate.refs(generate.TOKENS)
    for deck in ("deckA", "deckC"):
        assert references[deck] == "accentHi"
        assert tokens[deck].value == tokens["accentHi"].value == "#67e8f9"
    for deck in ("deckB", "deckD"):
        assert references[deck] == "textPrimary"
        assert tokens[deck].value == tokens["textPrimary"].value == "#f1f5f9"
    # Decided on 2026-11-02 (design 14.3): not pending GU1
    assert all(
        tokens[d].pending is None for d in ("deckA", "deckB", "deckC", "deckD")
    )


def test_pending_tokens_are_gu1_only():
    data = json.loads(generate.TOKENS.read_text(encoding="utf-8"))
    assert data["meta"]["source"]["commit"]
    for entries in data["tokens"].values():
        for name, entry in entries.items():
            assert entry.get("pending") in (None, "GU1"), name
            if "options" in entry:
                assert entry.get("pending") == "GU1", name

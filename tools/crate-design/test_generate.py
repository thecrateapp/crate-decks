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
            "borderSubtle": {"value": "#f1f5f9", "alpha": 0.06},
            "textSubtle": {"value": "#7e8084"},
            "dangerText": {"value": "#ef6465"},
        },
        "iconState": {
            "iconIdle": {"ref": "textMuted"},
            "iconHover": {"ref": "accentHi"},
            "iconActive": {"ref": "accentHi"},
            "iconDisabled": {"ref": "textFaint"},
            "iconOnAccent": {"ref": "onAccent"},
            "iconPrimary": {"ref": "textPrimary"},
            "iconAccent": {"ref": "accentHi"},
            "iconDanger": {"ref": "dangerText"},
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
            "faderHandleWidth": {"value": 18, "unit": "px"},
            "faderHandleHeight": {"value": 10, "unit": "px"},
            "faderHandleLine": {"value": 8, "unit": "px"},
            "faderWidth": {"value": 30, "unit": "px"},
            "pitchFaderWidth": {"value": 34, "unit": "px"},
            "knobCanvas": {"value": 40, "unit": "px"},
            "knobTickGap": {"value": 1.5, "unit": "px"},
            "knobDot": {"value": 3, "unit": "px"},
            "knobDotInset": {"value": 5, "unit": "px"},
            "knobSizeCompact": {"value": 30, "unit": "px"},
            "knobSizeFourShort": {"value": 26, "unit": "px"},
            "vuWarningDb": {"value": -6, "unit": "dB"},
            "phraseHeight": {"value": 18, "unit": "px"},
            "tickPhrase": {"value": 10, "unit": "px"},
            "stemRailHeight": {"value": 18, "unit": "px"},
            "stemRailInset": {"value": 3, "unit": "px"},
            "stemTickOffset": {"value": 4, "unit": "px"},
            "stemTickShort": {"value": 2, "unit": "px"},
            "stemTickLong": {"value": 3, "unit": "px"},
            "stemHandleWidth": {"value": 5, "unit": "px"},
            "stemHandleHeight": {"value": 9, "unit": "px"},
            "switchWidth": {"value": 32, "unit": "px"},
            "switchHeight": {"value": 18, "unit": "px"},
            "switchKnob": {"value": 14, "unit": "px"},
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
    # The icons that also get a button's colour (ICON_STATES_FOR)
    for _, icons in generate.ICON_STATES_FOR.values():
        for icon in icons:
            (svg / "solar" / f"{icon}.svg").write_text(ICON, encoding="utf-8")
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
        "primary": "#f1f5f9",
    }
    assert set(generate.STATES) == set(expected)
    for state, colour in expected.items():
        svg = (design["out"] / "icons" / f"play_{state}.svg").read_text()
        assert "currentColor" not in svg
        assert f'fill="{colour}"' in svg
    handle = (design["out"] / "controls" / "fader-handle-v30.svg").read_text()
    assert 'fill="#0a0a0f" stroke="#999ca0"' in handle


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
            # Templates draw the logo and plain shapes from tokens; they are
            # not icons and never come from an icon set.
            if generate.TEMPLATES in path.parents:
                assert path.name.endswith(".svg.in"), path
                assert "currentColor" not in path.read_text(), path
                continue
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


def test_gu1_closed_nothing_pending():
    # GU1 closed on 2026-11-09: every DJ adaptation of design 14.3 is final
    data = json.loads(generate.TOKENS.read_text(encoding="utf-8"))
    assert data["meta"]["source"]["commit"]
    assert "pending" not in data["meta"]
    for entries in data["tokens"].values():
        for name, entry in entries.items():
            assert "pending" not in entry, name
            assert "options" not in entry, name


def write_palette_tokens(design, palette):
    data = json.loads(design["tokens"].read_text(encoding="utf-8"))
    data["tokens"]["waveform"] = {
        "waveLowAccent": {"value": "#0e7490"},
        "waveLowNeutral": {"value": "#4a4c52"},
        "wavePaletteDeckAC": {"value": palette},
        "waveLowDeckAC": {"ref": "waveLow{wavePaletteDeckAC}"},
    }
    design["tokens"].write_text(json.dumps(data), encoding="utf-8")
    template = design["templates"] / "vars.xml.in"
    template.write_text(
        "<Template>{{waveLowDeckAC}}</Template>\n<!-- {{waveLowNeutral}} -->\n"
    )


def test_wave_palette_switch_picks_the_bands(design):
    write_palette_tokens(design, "Accent")
    tokens = generate.load_tokens(design["tokens"])
    assert tokens["waveLowDeckAC"].value == "#0e7490"
    # The switch and the palette it picks count as used through the ref
    run(design)
    vars_xml = (design["out"] / "vars.xml").read_text(encoding="utf-8")
    assert "<Template>#0e7490</Template>" in vars_xml


def test_wave_palette_reserve_is_greys(design):
    write_palette_tokens(design, "Neutral")
    tokens = generate.load_tokens(design["tokens"])
    assert tokens["waveLowDeckAC"].value == "#4a4c52"


def test_wave_palette_switch_must_be_text(design):
    data = json.loads(design["tokens"].read_text(encoding="utf-8"))
    data["tokens"]["waveform"] = {
        "wavePaletteDeckAC": {"ref": "bgApp"},
        "waveLowDeckAC": {"ref": "waveLow{wavePaletteDeckAC}"},
    }
    design["tokens"].write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(generate.GeneratorError, match="text value"):
        generate.load_tokens(design["tokens"])


def test_wave_palette_of_the_decks():
    tokens = generate.load_tokens(generate.TOKENS)
    # Approved in GU1: logo bands on A and C, greys on B and D
    assert tokens["wavePaletteDeckAC"].value == "Accent"
    for band in ("Low", "Mid", "High"):
        ac = tokens[f"wave{band}DeckAC"].value
        bd = tokens[f"wave{band}DeckBD"].value
        assert ac == tokens[f"wave{band}Accent"].value
        assert bd == tokens[f"wave{band}Neutral"].value


def test_qss_sum_of_tokens(design):
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE
        + "QLabel { padding-left: {{ tickLong + tickShort - strokeHairline }};"
        + " qproperty-layoutSpacing: {{tickLong+tickShort.number}}; }\n",
        encoding="utf-8",
    )
    run(design)
    qss = (design["out"] / "style.qss").read_text(encoding="utf-8")
    assert "padding-left: 9px;" in qss
    assert "qproperty-layoutSpacing: 10;" in qss


def test_sum_needs_one_unit(design):
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE + "QLabel { width: {{tickLong + knobSweep}}; }\n",
        encoding="utf-8",
    )
    with pytest.raises(generate.GeneratorError, match="mixed units"):
        run(design)


def test_sum_of_unknown_token_fails(design):
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE + "QLabel { width: {{tickLong + tickLonger}}; }\n",
        encoding="utf-8",
    )
    with pytest.raises(
        generate.GeneratorError, match="unknown token tickLonger"
    ):
        run(design)


def test_half_of_a_length(design):
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE + "QLabel { border-radius: {{tickLong.half}}; }\n",
        encoding="utf-8",
    )
    run(design)
    qss = (design["out"] / "style.qss").read_text(encoding="utf-8")
    assert "border-radius: 3.5px;" in qss


def test_tracking_times_size_is_a_length(design):
    data = json.loads(design["tokens"].read_text(encoding="utf-8"))
    data["tokens"]["geometry"]["trackingCaps"] = {"value": 0.14, "unit": "em"}
    data["tokens"]["geometry"]["fontSizeLabel"] = {"value": 11, "unit": "px"}
    design["tokens"].write_text(json.dumps(data), encoding="utf-8")
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE
        + "QLabel { letter-spacing: {{trackingCaps * fontSizeLabel}}; }\n",
        encoding="utf-8",
    )
    run(design)
    qss = (design["out"] / "style.qss").read_text(encoding="utf-8")
    assert "letter-spacing: 1.54px;" in qss


def test_fractional_font_size_in_points(design):
    data = json.loads(design["tokens"].read_text(encoding="utf-8"))
    data["tokens"]["geometry"]["fontSizeMeta"] = {"value": 10.5, "unit": "px"}
    design["tokens"].write_text(json.dumps(data), encoding="utf-8")
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE
        + "QLabel { font-size: {{fontSizeMeta.fontsize}}; }\n"
        + "QSpinBox { font-size: {{tickLong.fontsize}}; }\n",
        encoding="utf-8",
    )
    run(design)
    qss = (design["out"] / "style.qss").read_text(encoding="utf-8")
    assert "font-size: 7.875pt;" in qss
    assert "font-size: 7px;" in qss


def test_scales_keep_hairlines_when_stretched(design):
    run(design)
    controls = design["out"] / "controls"
    for name in ("phrase-ruler.svg", "stem-rail.svg", "knob-scale.svg"):
        svg = (controls / name).read_text(encoding="utf-8")
        assert 'vector-effect="non-scaling-stroke"' in svg, name
    ruler = (controls / "phrase-ruler.svg").read_text(encoding="utf-8")
    # 64 beats: a baseline and 65 ticks
    assert ruler.count("<line") == 66


def test_knob_scale_and_arc_share_the_ring(design):
    run(design)
    controls = design["out"] / "controls"
    scale = (controls / "knob-scale.svg").read_text(encoding="utf-8")
    # The GU1 mockups' 40 x 40 knob: ring at 20 - 0.5 - 4.5 - 1.5
    assert 'viewBox="0 0 40 40"' in scale
    assert 'r="13.5"' in scale
    # 19 ticks over 270 degrees, one every 15, long every 30
    assert scale.count("<line") == 19
    dot = (controls / "knob-dot.svg").read_text(encoding="utf-8")
    assert '<circle cx="20" cy="11.5" r="1.5" fill="#f1f5f9"/>' in dot

    tokens = generate.load_tokens(design["tokens"])
    derived = generate.derived_tokens(tokens, set())
    # The arc of WKnobComposed runs on the ring at each knob size
    assert derived["knobArcRadius"].value == 13.5
    assert derived["knobArcRadiusCompact"].value == 10.125
    assert derived["knobArcRadiusFourShort"].value == 8.775
    assert derived["faderHandleLength"].value == 11


def test_derived_values_reach_templates(design):
    template = design["templates"] / "style.qss.in"
    template.write_text(
        QSS_TEMPLATE + "/* {{knobArcRadiusCompact}} {{vuWarningLevel}} */\n",
        encoding="utf-8",
    )
    run(design)
    qss = (design["out"] / "style.qss").read_text(encoding="utf-8")
    assert "/* 10.125 0.7412 */" in qss


def test_vu_level_follows_mixxx_meter_scale():
    # EngineVuMeter: full scale is 1, and -6 dB below it is about 0.74
    assert generate.vu_level(0) == pytest.approx(1)
    assert generate.vu_level(-6) == pytest.approx(0.7412, abs=1e-4)
    assert generate.vu_level(-20) < generate.vu_level(-6)


def test_fader_handles_sit_on_the_rail(design):
    run(design)
    controls = design["out"] / "controls"
    v30 = (controls / "fader-handle-v30.svg").read_text(encoding="utf-8")
    # 30 px across, the rail at 15.5 as in the mockups: an 18 x 10 outline
    # on half pixels and an 8 px line through the middle
    assert 'width="30" height="11"' in v30
    assert '<rect x="6.5" y="0.5" width="18" height="10"' in v30
    assert 'x1="11.50" y1="5.50" x2="19.50" y2="5.50"' in v30
    v34 = (controls / "fader-handle-v34.svg").read_text(encoding="utf-8")
    assert '<rect x="8.5" y="0.5" width="18" height="10"' in v34
    h30 = (controls / "fader-handle-h30.svg").read_text(encoding="utf-8")
    assert 'width="11" height="30"' in h30
    assert '<rect x="0.5" y="6.5" width="10" height="18"' in h30


def test_switch_is_the_admin_switch(design):
    run(design)
    controls = design["out"] / "controls"
    off = (controls / "switch-off.svg").read_text(encoding="utf-8")
    on = (controls / "switch-on.svg").read_text(encoding="utf-8")
    # decks.css .switch: a 32 x 18 pill, the 14 px knob 1 px inside the
    # border, on the left in textSubtle and on the right over accentHi
    assert 'width="32" height="18"' in off
    assert 'rx="8.5" fill="none" stroke="#f1f5f9" stroke-opacity="0.1"' in off
    assert '<circle cx="9" cy="9" r="7" fill="#7e8084"/>' in off
    assert 'fill="#67e8f9" stroke="#67e8f9"' in on
    assert '<circle cx="23" cy="9" r="7" fill="#0a0a0f"/>' in on
    for name in ("switch-off-disabled.svg", "switch-on-disabled.svg"):
        assert (controls / name).exists()


def test_some_icons_get_a_button_colour(design):
    run(design)
    icons = design["out"] / "icons"
    danger = (icons / "logout_danger.svg").read_text(encoding="utf-8")
    assert 'fill="#ef6465"' in danger
    assert (icons / "add-circle_accent.svg").exists()
    # Only the icons that need the state get it
    assert not (icons / "play_danger.svg").exists()


def test_missing_icon_for_a_button_colour_fails(design):
    (design["svg"] / "solar" / "logout.svg").unlink()
    with pytest.raises(generate.GeneratorError, match="logout"):
        run(design)


def test_application_style_is_generated():
    # The application's default style and the splash screen: generated, and
    # with every placeholder resolved
    for name in ("default.qss", "launch.qss"):
        text = (generate.OUT / name).read_text(encoding="utf-8")
        assert "{{" not in text and "}}" not in text, name
        assert "#" in text and ":/images/" not in text, name

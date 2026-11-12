"""Tests for tools/crate-brand/check_install.py (pytest)."""

import check_install


def touch(root, rel):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("", encoding="utf-8")


def test_accepts_crate_decks_tree(tmp_path):
    for rel in (
        "bin/crate-decks",
        "share/crate-decks/skins/CrateDecks/skin.xml",
        "share/crate-decks/skins/default.qss",
        "share/crate-decks/translations/mixxx_es.qm",
        "share/crate-decks/translations/mixxx_es_419.qm",
        "share/crate-decks/controllers/lodash.mixxx.js",
        "share/crate-decks/controllers/mixxx-controls.d.ts",
        "share/crate-decks/controllers/Yaeltex MiniMixxx.midi.xml",
        "share/crate-decks/qml/Mixxx/Controls/Knob.qml",
        "share/doc/crate-decks/README.md",
        "share/applications/app.cratemusic.decks.desktop",
    ):
        touch(tmp_path, rel)
    assert check_install.offending(str(tmp_path)) == []
    assert check_install.main([str(tmp_path)]) == 0


def test_finds_paths_that_name_mixxx(tmp_path, capsys):
    for rel in (
        "share/mixxx/skins/LateNight/skin.xml",
        "share/doc/mixxx/Mixxx-Manual.pdf",
        "share/crate-decks/udev/rules.d/mixxx-usb-uaccess.rules",
        "share/applications/mixxx-appimage.desktop",
        "share/crate-decks/skins/Tango (64 Samplers)/skin.xml",
    ):
        touch(tmp_path, rel)
    assert check_install.offending(str(tmp_path)) == [
        "share/applications/mixxx-appimage.desktop",
        "share/crate-decks/skins/Tango (64 Samplers)",
        "share/crate-decks/udev/rules.d/mixxx-usb-uaccess.rules",
        "share/doc/mixxx",
        "share/doc/mixxx/Mixxx-Manual.pdf",
        "share/mixxx",
        "share/mixxx/skins/LateNight",
    ]
    assert check_install.main([str(tmp_path)]) == 1
    assert (
        "installed path names Mixxx or is a Mixxx skin: share/mixxx"
        in capsys.readouterr().out
    )

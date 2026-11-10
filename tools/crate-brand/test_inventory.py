"""Tests for tools/crate-brand/inventory.py (pytest)."""

import json
import os
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

import pytest

import inventory

SOURCE = r'''
#include "mixxxmainwindow.h" // a file name, not a string to brand
// tr("Commented out Mixxx")
/* QObject::tr("Also commented out, Mixxx") */
void f() {
    tr("Exit Mixxx?");
    QObject::tr("Two literals that "
                "name Mixxx");
    QCoreApplication::translate("MixxxMainWindow", "Quits Mixxx");
    QT_TRANSLATE_NOOP("Ctx", "Mixxx Hotcue Colors");
    QT_TR_NOOP("Mixxx mode");
    tr("Plural Mixxx %n", "a disambiguation that names Mixxx");
    const char c = '"';
    qWarning() << "Running Mixxx";
    setName(QStringLiteral("Mixxx"));
    load("mixxxdb.sqlite", "MixxxControl", "org.mixxx.Mixxx", "mixxx::audio::SampleRate");
    tr("A \"quoted\" Mixxx\n• bullet");
    int n = 1'000;
    show(R"(Raw Mixxx)");
}
'''

UI = """<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <widget class="QLabel">
  <property name="text"><string>Mixxx mode</string></property>
  <property name="toolTip"><string notr="true">Mixxx not translated</string></property>
  <property name="whatsThis"><string>No name here</string></property>
 </widget>
</ui>
"""


def make_repo(tmp_path, source=SOURCE):
    (tmp_path / "src" / "test").mkdir(parents=True)
    (tmp_path / "src" / "main.cpp").write_text(source, encoding="utf-8")
    (tmp_path / "src" / "dlg.ui").write_text(UI, encoding="utf-8")
    (tmp_path / "src" / "test" / "skipped.cpp").write_text('tr("Test Mixxx");', encoding="utf-8")
    (tmp_path / "res" / "qml").mkdir(parents=True)
    (tmp_path / "res" / "qml" / "Main.qml").write_text('Text { text: qsTr("QML Mixxx") }', encoding="utf-8")
    return tmp_path


def test_names_mixxx_skips_file_names_urls_and_longer_words():
    for text in ("Exit Mixxx?", "Mixxx's settings", "MIXXX", "mixxx 2.4", "Mixxx-yhteisö", "(Mixxx)"):
        assert inventory.NAMES_MIXXX.search(text), text
    for text in (
        "mixxxdb.sqlite",
        "~/.mixxx",
        "https://mixxx.org",
        "www.Mixxx.org",
        "org.mixxx.Mixxx",
        "MixxxControl",
        "Yaeltex MiniMixxx",
        "mixxx::audio::SampleRate",
        "mixxx-devel@lists",
        "skins:Mixxx",
    ):
        assert not inventory.NAMES_MIXXX.search(text), text


def test_scan_finds_translatable_strings(tmp_path):
    strings, _ = inventory.scan(make_repo(tmp_path))
    assert set(strings) == {
        "Exit Mixxx?",
        "Two literals that name Mixxx",
        "Quits Mixxx",
        "Mixxx Hotcue Colors",
        "Mixxx mode",
        "Plural Mixxx %n",
        'A "quoted" Mixxx\n• bullet',
        "QML Mixxx",
    }
    assert strings["Mixxx mode"] == {"src/dlg.ui", "src/main.cpp"}


def test_scan_lists_other_literals_that_name_mixxx(tmp_path):
    _, literals = inventory.scan(make_repo(tmp_path))
    assert literals == [
        ("src/main.cpp", "Mixxx"),
        ("src/main.cpp", "Raw Mixxx"),
        ("src/main.cpp", "Running Mixxx"),
        ("src/main.cpp", "a disambiguation that names Mixxx"),
    ]


def test_update_keeps_decisions_and_adds_new_strings(tmp_path):
    repo = make_repo(tmp_path)
    strings, literals = inventory.scan(repo)
    old = {
        "strings": [
            {"source": "Mixxx mode", "files": ["x"], "treatment": "text", "text": "Default mode"},
            {"source": "Gone Mixxx", "files": ["y"], "treatment": "rule"},
        ],
        "literals": [{"file": "src/main.cpp", "text": "Running Mixxx", "why": "log"}],
    }
    fresh = inventory.updated(old, strings, literals)
    by_source = {e["source"]: e for e in fresh["strings"]}
    assert by_source["Mixxx mode"]["text"] == "Default mode"
    assert by_source["Mixxx mode"]["files"] == ["src/dlg.ui", "src/main.cpp"]
    assert by_source["Exit Mixxx?"]["treatment"] == "rule"
    assert "Gone Mixxx" not in by_source
    whys = {(e["file"], e["text"]): e["why"] for e in fresh["literals"]}
    assert whys[("src/main.cpp", "Running Mixxx")] == "log"
    assert whys[("src/main.cpp", "Mixxx")] == ""
    assert any("literal without a reason" in p for p in inventory.problems(fresh))


def test_check_fails_when_upstream_adds_a_string(tmp_path, capsys):
    repo = make_repo(tmp_path)
    assert inventory.main(["--repo", str(repo)]) == 0
    path = repo / inventory.INVENTORY
    data = json.loads(path.read_text(encoding="utf-8"))
    for literal in data["literals"]:
        literal["why"] = "test"
    path.write_text(inventory.dump(data), encoding="utf-8")
    assert inventory.main(["--repo", str(repo), "--check"]) == 0

    (repo / "src" / "new.cpp").write_text('tr("New in Mixxx 2.8");', encoding="utf-8")
    assert inventory.main(["--repo", str(repo), "--check"]) == 1
    assert "new string naming Mixxx: 'New in Mixxx 2.8'" in capsys.readouterr().out


def test_inventory_of_this_repository_is_current():
    assert inventory.main(["--check"]) == 0


@pytest.mark.skipif(
    not (shutil.which("lupdate") or os.path.exists("/usr/lib/qt6/bin/lupdate")),
    reason="needs Qt's lupdate",
)
def test_scan_agrees_with_lupdate(tmp_path):
    lupdate = shutil.which("lupdate") or "/usr/lib/qt6/bin/lupdate"
    ts = tmp_path / "all.ts"
    subprocess.run(
        [lupdate, "-silent", "-recursive", "src", "res/qml", "-extensions", "cpp,h,ui,qml,js",
         "-locations", "none", "-no-obsolete", "-ts", str(ts)],
        cwd=inventory.REPO, check=True, capture_output=True,
    )
    expected = set()
    for message in ET.parse(ts).getroot().iter("message"):
        locations = message.findall("location")
        source = message.findtext("source") or ""
        if inventory.MENTIONS_MIXXX.search(source):
            expected.add(source)
    # lupdate also reads src/test; the inventory leaves tests out
    strings, _ = inventory.scan(inventory.REPO)
    tests_only = {s for s in expected - set(strings) if re.search(r"Test", s)}
    assert set(strings) == expected - tests_only

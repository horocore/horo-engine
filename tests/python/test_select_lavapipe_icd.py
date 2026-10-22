"""Native driver selection must follow the installed package, never silently skip."""

import json
from types import SimpleNamespace

import dev as SELECTOR
import pytest


@pytest.mark.parametrize("name", ["lvp_icd.json", "lvp_icd.x86_64.json"])
def test_selects_actual_installed_filename(tmp_path, name):
    manifest = tmp_path / name
    manifest.write_text(json.dumps({"ICD": {"library_path": "libvulkan_lvp.so"}}))
    assert SELECTOR.select_manifest([str(tmp_path / "missing.json"), str(manifest), str(manifest)]) == manifest.resolve()


def test_absence_is_explicit():
    with pytest.raises(ValueError, match="found 0"):
        SELECTOR.select_manifest([])


def test_multiple_installed_candidates_are_rejected(tmp_path):
    paths = [tmp_path / "lvp_icd.json", tmp_path / "lvp_icd.x86_64.json"]
    for path in paths:
        path.write_text(json.dumps({"ICD": {"library_path": "libvulkan_lvp.so"}}))
    with pytest.raises(ValueError, match="found 2"):
        SELECTOR.select_manifest([str(path) for path in paths])


def test_malformed_manifest_is_rejected(tmp_path):
    path = tmp_path / "lvp_icd.json"
    path.write_text(json.dumps({"ICD": {}}))
    with pytest.raises(ValueError, match="no library path"):
        SELECTOR.select_manifest([str(path)])


def test_dev_command_queries_installed_package_and_prints_only_manifest(monkeypatch, tmp_path, capsys):
    manifest = tmp_path / "lvp_icd.json"
    manifest.write_text(json.dumps({"ICD": {"library_path": "libvulkan_lvp.so"}}))
    calls = []

    def inventory(argv, **kwargs):
        calls.append((argv, kwargs))
        return SimpleNamespace(stdout=f"{manifest}\n")

    monkeypatch.setattr(SELECTOR.subprocess, "run", inventory)
    assert SELECTOR.main(["select-lavapipe-icd"]) == 0
    assert calls == [(["dpkg-query", "-L", "mesa-vulkan-drivers"],
                      {"check": True, "capture_output": True, "text": True})]
    assert capsys.readouterr().out.strip() == str(manifest.resolve())


def test_dev_command_reports_absent_driver_without_fallback(monkeypatch, capsys):
    monkeypatch.setattr(SELECTOR.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(stdout=""))
    assert SELECTOR.main(["select-lavapipe-icd"]) == 1
    assert "found 0" in capsys.readouterr().err

"""Native driver selection must follow the installed package, never silently skip."""

import importlib.util
import json
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location(
    "select_lavapipe_icd", Path(__file__).resolve().parents[2] / "scripts/select_lavapipe_icd.py"
)
SELECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SELECTOR)


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

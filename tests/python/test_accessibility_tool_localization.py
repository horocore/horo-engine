"""Accessibility tools follow the paired editor catalog contract."""
from __future__ import annotations

import json
import re

from test_ci_workflow_grouping import ROOT


def test_contrast_tool_copy_is_paired_and_covers_rendered_states() -> None:
    prefix = "workspace.accessibility_contrast."
    catalogs = [json.loads((ROOT / f"assets/localization/editor/{locale}.json").read_text(encoding="utf-8"))["messages"]
                for locale in ("en-US", "tr-TR")]
    keys = {key.removeprefix(prefix) for key in catalogs[0] if key.startswith(prefix)}
    assert keys == {key.removeprefix(prefix) for key in catalogs[1] if key.startswith(prefix)}
    source = (ROOT / "src/editor/screens/workspace/panels/accessibility/AccessibilityContrastPanel.cpp").read_text(encoding="utf-8")
    direct_keys = set(re.findall(r'LocalizedText\(context\.localization, "([a-z_]+)"\)', source))
    assert direct_keys <= keys
    assert {"title", "invalid", "needs_composition", "pass", "fail", "custom", "theme", "large_hint"} <= keys
    assert all(catalog[prefix + key]["text"] for catalog in catalogs for key in keys)


def test_binding_accessibility_copy_covers_duration_editing_and_rejection() -> None:
    prefix = "workspace.input_mapping.accessibility."
    catalogs = [json.loads((ROOT / f"assets/localization/editor/{locale}.json").read_text(encoding="utf-8"))["messages"]
                for locale in ("en-US", "tr-TR")]
    keys = {key.removeprefix(prefix) for key in catalogs[0] if key.startswith(prefix)}
    assert keys == {key.removeprefix(prefix) for key in catalogs[1] if key.startswith(prefix)}
    folder = ROOT / "src/editor/screens/workspace/panels/input_mapping"
    direct_keys: set[str] = set()
    for name in ("InputMappingPanel.cpp", "InputMappingAccessibility.cpp"):
        direct_keys |= set(re.findall(r'"accessibility\.([a-z_]+)"', (folder / name).read_text(encoding="utf-8")))
    assert direct_keys <= keys
    assert {"toggle", "sticky", "repeat", "hold_ms", "delay_ms", "interval_ms", "invalid", "stale", "apply", "cancel"} <= keys
    assert all(catalog[prefix + key]["text"] for catalog in catalogs for key in keys)

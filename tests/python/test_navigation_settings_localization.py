"""Project authoring copy is paired; local overlays stay outside packaged policy."""
from __future__ import annotations

import json
import re

from test_ci_workflow_grouping import ROOT


def test_navigation_settings_visible_copy_is_localized_in_both_catalogs() -> None:
    english = json.loads((ROOT / "assets/localization/editor/en-US.json").read_text(encoding="utf-8"))["messages"]
    turkish = json.loads((ROOT / "assets/localization/editor/tr-TR.json").read_text(encoding="utf-8"))["messages"]
    keys = {key for key in english if key.startswith("settings.navigation.")}
    assert keys == {key for key in turkish if key.startswith("settings.navigation.")}
    assert all(isinstance(english[key]["text"], str) and english[key]["text"] and turkish[key]["text"] for key in keys)
    for group, count in (("capacity", 7), ("geometry", 7), ("capability", 6), ("reference", 6)):
        assert {f"settings.navigation.{group}.{index}" for index in range(count)} <= keys
    for path in (ROOT / "src/editor/modals/settings").glob("SettingsModalNavigation*.cpp"):
        literals = set(re.findall(r'"(settings\.navigation\.[a-z_]+)"', path.read_text(encoding="utf-8")))
        assert literals <= keys


def test_navigation_policy_does_not_depend_on_editor_local_preferences() -> None:
    for path in (
        ROOT / "include/Horo/Application/NavigationProjectSettings.h",
        ROOT / "src/application/project/NavigationProjectSettingsCodec.cpp",
        ROOT / "src/application/project/NavigationProjectSettingsStore.cpp",
    ):
        source = path.read_text(encoding="utf-8")
        assert "EditorSettings" not in source
        assert "navigationOverlay" not in source
    policy_ui = (ROOT / "src/editor/modals/settings/SettingsModalNavigationPolicy.cpp").read_text(encoding="utf-8")
    assert "RequestOpenDefinition" in policy_ui
    assert "PutNavigationProfile" not in policy_ui
    assert "RemoveNavigationProfile" not in policy_ui


def test_navigation_bake_copy_is_paired_and_covers_operation_decisions() -> None:
    english = json.loads((ROOT / "assets/localization/editor/en-US.json").read_text(encoding="utf-8"))["messages"]
    turkish = json.loads((ROOT / "assets/localization/editor/tr-TR.json").read_text(encoding="utf-8"))["messages"]
    prefix = "workspace.navigation_bake."
    keys = {key for key in english if key.startswith(prefix)}
    assert keys == {key for key in turkish if key.startswith(prefix)}
    required = {"keep_running", "confirm_cancel", "return", "preparing", "unavailable", "selected_only", "retry",
                "failed", "cancelling", "succeeded", "cancelled", "resources", "elapsed", "stage", "profiles_hint", "saved_source", "reload"}
    assert {prefix + suffix for suffix in required} <= keys
    assert all(english[key]["text"] and turkish[key]["text"] for key in keys)

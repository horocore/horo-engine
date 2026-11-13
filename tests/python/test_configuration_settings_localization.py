"""Shared configuration editing keeps policy and conflict copy paired across locales."""
from __future__ import annotations

import json
import re
from pathlib import Path


def test_configuration_settings_copy_covers_typed_policies_and_conflicts() -> None:
    root = Path(__file__).resolve().parents[2]
    prefix = "settings.configuration."
    catalogs = [
        json.loads((root / f"assets/localization/editor/{locale}.json").read_text(encoding="utf-8"))["messages"]
        for locale in ("en-US", "tr-TR")
    ]
    keys = {key for key in catalogs[0] if key.startswith(prefix)}
    assert keys == {key for key in catalogs[1] if key.startswith(prefix)}
    required = {
        "title", "public", "readonly", "sensitive", "effective", "inactive", "user_value",
        "invalid", "preview", "unsaved", "persist_hint", "loaded", "applied", "defaults", "stale",
        "theme_package", "theme_override", "allowed_range", "text_bytes",
        "project_title", "project_nav", "project_value", "project_public", "project_persist_hint", "project_unavailable",
    }
    required.update(f"source.{source}" for source in ("invocation", "environment", "session", "project", "user", "profile", "default"))
    required.update(f"reload.{policy}" for policy in ("immediate", "frame", "operation", "project", "process"))
    assert {prefix + suffix for suffix in required} <= keys
    assert all(isinstance(catalog[key]["text"], str) and catalog[key]["text"].strip() for catalog in catalogs for key in keys)


def test_platform_setting_descriptor_labels_have_aligned_localized_copy() -> None:
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/platform_services/PlatformProjectSettings.cpp").read_text(encoding="utf-8")
    labels = set(re.findall(r'"(settings\.platform_services\.[a-z_]+)"', source))
    assert labels
    for locale in ("en-US", "tr-TR"):
        catalog = json.loads((root / f"assets/localization/editor/{locale}.json").read_text(encoding="utf-8"))["messages"]
        assert labels == {key for key in catalog if key.startswith("settings.platform_services.")}
        assert all(catalog[key]["text"].strip() for key in labels)

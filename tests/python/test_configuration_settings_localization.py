"""Shared configuration editing keeps policy and conflict copy paired across locales."""
from __future__ import annotations

import json
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
    }
    required.update(f"source.{source}" for source in ("invocation", "environment", "session", "project", "user", "profile", "default"))
    required.update(f"reload.{policy}" for policy in ("immediate", "frame", "operation", "project", "process"))
    assert {prefix + suffix for suffix in required} <= keys
    assert all(isinstance(catalog[key]["text"], str) and catalog[key]["text"].strip() for catalog in catalogs for key in keys)

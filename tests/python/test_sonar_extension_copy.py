"""Transactional Sonar extension copies, including native Windows long paths."""
from __future__ import annotations

import analyze

import os
import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import analyze as sessions


def profile_inputs(tmp_path):
    source = tmp_path / "source Code ü"
    (source / "User").mkdir(parents=True)
    extensions = tmp_path / "installed extensions"
    extension = extensions / "sonarsource.sonarlint-vscode-6.1.1-win32-x64"
    extension.mkdir(parents=True)
    (extension / "package.json").write_text('{}', encoding="utf-8")
    (extension / "LICENSE.txt").write_bytes(b"license must not be skipped\n")
    profile = tmp_path / "managed" / "profile"
    target = profile.parent / "extensions" / extension.name
    return source, extensions, extension, profile, target


def test_launch_uses_exact_newly_seeded_extension_not_older_cached_copy(tmp_path, monkeypatch):
    source, extensions, older, profile, old_target = profile_inputs(tmp_path)
    (older / "package.json").write_text('{"version":"6.1.1"}', encoding="utf-8")
    sessions.seed_profile(profile, source, extensions)
    newer = extensions / "sonarsource.sonarlint-vscode-6.2.0-win32-x64"
    newer.mkdir()
    package = newer / "package.json"
    package.write_text('{"version":"6.2.0"}', encoding="utf-8")
    os.utime(older / "package.json", (1, 1))
    os.utime(package, (2, 2))
    expected = old_target.parent / newer.name
    commands = []
    monkeypatch.setattr(sessions, 'quality_run', lambda *args: commands.append(args[0]))
    _, actual = sessions.launch_profile(tmp_path, profile, tmp_path / "workspace", source, extensions,
                                        1, tmp_path / "x.cpp")
    assert actual == expected
    assert commands[0][commands[0].index("--extensionDevelopmentPath") + 1] == str(expected)
    assert sessions.seed_profile(profile, source, extensions) == expected
    assert (old_target / "package.json").is_file(), "old caches need not be deleted to select correctly"


def test_failed_copy_is_never_published_and_retry_copies_all_files(tmp_path, monkeypatch):
    source, extensions, extension, profile, target = profile_inputs(tmp_path)
    original = shutil.copytree

    def interrupted_copy(src, dst, *args, **kwargs):
        # Use real filesystem work before injecting an I/O failure.
        Path(dst).mkdir()
        shutil.copy2(Path(src) / "package.json", Path(dst) / "package.json")
        raise OSError("injected disk failure")

    with monkeypatch.context() as patch:
        patch.setattr(sessions.shutil, 'copytree', interrupted_copy)
        with pytest.raises(OSError, match="injected disk failure"):
            sessions.seed_profile(profile, source, extensions)
    assert not target.exists(), "a failed copy must not look like an installed extension"
    assert list(target.parent.iterdir()) == [], "ordinary failures clean their staging directory"
    sessions.seed_profile(profile, source, extensions)
    assert (target / "LICENSE.txt").read_bytes() == (extension / "LICENSE.txt").read_bytes()
    assert (target / "package.json").read_bytes() == (extension / "package.json").read_bytes()
    assert sessions.shutil.copytree is original


def test_existing_partial_directory_is_replaced_not_reused(tmp_path):
    source, extensions, extension, profile, target = profile_inputs(tmp_path)
    target.mkdir(parents=True)
    (target / "package.json").write_text("partial")
    sessions.seed_profile(profile, source, extensions)
    assert (target / "LICENSE.txt").read_bytes() == (extension / "LICENSE.txt").read_bytes()
    assert (target / "package.json").read_bytes() == (extension / "package.json").read_bytes()


def test_readonly_partial_extension_is_replaced_without_changing_source(tmp_path):
    import stat
    source, extensions, extension, profile, target = profile_inputs(tmp_path)
    target.mkdir(parents=True)
    partial = target / "readonly.jsa"
    partial.write_bytes(b"incomplete")
    partial.chmod(stat.S_IREAD)
    sessions.seed_profile(profile, source, extensions)
    assert not partial.exists()
    assert (target / "LICENSE.txt").read_bytes() == (extension / "LICENSE.txt").read_bytes()


def test_native_windows_deep_extension_copy_keeps_every_file(tmp_path):
    if sys.platform != "win32":
        pytest.skip("native Windows extended-length path regression")
    source, extensions, extension, profile, target = profile_inputs(tmp_path)
    relative = Path("node_modules") / ("deep_dependency_" * 7) / ("license_" * 12 + ".txt")
    source_file = Path("\\\\?\\" + str((extension / relative).resolve()))
    source_file.parent.mkdir(parents=True)
    source_file.write_bytes(b"complete license contents")
    sessions.seed_profile(profile, source, extensions)
    copied = Path("\\\\?\\" + str((target / relative).resolve()))
    assert copied.read_bytes() == source_file.read_bytes()
    sessions.seed_profile(profile, source, extensions)
    assert copied.read_bytes() == source_file.read_bytes()

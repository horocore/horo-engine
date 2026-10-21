"""Read-only profile seeding, secret rejection and encryption import contracts."""
from __future__ import annotations

import json
import secrets
from types import SimpleNamespace

import pytest

import analyze as sessions
from analyze import AnalysisError
from sonar_session_fixtures import profile_inputs


def test_new_source_profile_without_settings_keeps_workspace_trust(tmp_path):
    source, extensions, profile = profile_inputs(tmp_path)
    sessions.seed_profile(profile, source, extensions)
    settings = json.loads((profile / "User/settings.json").read_text())
    assert settings["security.workspace.trust.enabled"] is True
    assert settings["sonarlint.connectedMode.connections.sonarcloud"] == []
    assert not (source / "User/settings.json").exists()
    assert not (profile / "User/globalStorage/state.vscdb").exists()
    assert not (profile / "User/workspaceStorage").exists()
    assert not (profile / "Local State").exists()
    assert (profile.parent / "extensions" / "sonarsource.sonarlint-vscode-6.1.1-win32-x64/package.json").exists()



def test_source_trust_and_inline_tokens_are_not_copied(tmp_path):
    source, extensions, profile = profile_inputs(tmp_path)
    disposable_token = secrets.token_hex(16)
    original = json.dumps({
        "security.workspace.trust.enabled": False,
        "security.workspace.trust.untrustedFiles": "open",
        "sonarlint.connectedMode.connections.sonarcloud": [
            {"connectionId": "cloud", "organizationKey": "org", "token": disposable_token}],
    })
    (source / "User/settings.json").write_text(original, encoding="utf-8")
    sessions.seed_profile(profile, source, extensions)
    settings_text = (profile / "User/settings.json").read_text()
    assert json.loads(settings_text)["security.workspace.trust.enabled"] is True
    assert disposable_token not in settings_text
    assert "untrustedFiles" not in settings_text
    assert (source / "User/settings.json").read_text() == original



def test_windows_secret_import_requires_profile_encryption_key(monkeypatch, tmp_path):
    import sqlite3
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"))
    source, extensions, profile = profile_inputs(tmp_path)
    storage = source / "User/globalStorage"
    storage.mkdir()
    key = 'secret://{"extensionId":"sonarsource.sonarlint-vscode","key":"cloud"}'
    value = json.dumps({"type": "Buffer", "data": list(b"v10" + bytes(range(40)))})
    with sqlite3.connect(storage / "state.vscdb") as db:
        db.execute("CREATE TABLE ItemTable(key TEXT, value BLOB)")
        db.execute("INSERT INTO ItemTable VALUES (?, ?)", (key, value))
    before = (storage / "state.vscdb").read_bytes()
    with pytest.raises(AnalysisError, match="authenticate.*managed profile"):
        sessions.seed_profile(profile, source, extensions)
    assert (storage / "state.vscdb").read_bytes() == before
    assert not (profile / "User/globalStorage/state.vscdb").exists()
    assert not (profile / "Local State").exists()
    assert (profile.parent / "extensions" / "sonarsource.sonarlint-vscode-6.1.1-win32-x64/package.json").exists()



@pytest.mark.parametrize("platform_name,prefix,payload", [
    ("win32", b"v10", bytes(range(40))), ("linux", b"v11", bytes(range(32)))])
def test_encryption_import_is_minimal_read_only(monkeypatch, tmp_path, platform_name, prefix, payload):
    import base64
    import sqlite3
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform=platform_name))
    source, extensions, profile = profile_inputs(tmp_path)
    storage = source / "User/globalStorage"
    storage.mkdir()
    key = 'secret://{"extensionId":"sonarsource.sonarlint-vscode","key":"cloud"}'
    unrelated = 'secret://{"extensionId":"other.extension","key":"other"}'
    value = json.dumps({"type": "Buffer", "data": list(prefix + payload)})
    wrapped = base64.b64encode(b"DPAPI" + bytes(range(100))).decode("ascii")
    original_state = json.dumps({"os_crypt": {"encrypted_key": wrapped, "other": "unrelated"},
                                 "trusted_folders": ["never-copy"]})
    (source / "Local State").write_text(original_state, encoding="utf-8")
    with sqlite3.connect(storage / "state.vscdb") as db:
        db.execute("CREATE TABLE ItemTable(key TEXT, value BLOB)")
        db.executemany("INSERT INTO ItemTable VALUES (?, ?)", [
            (key, value), (unrelated, "do-not-copy"), ("secret://null", "do-not-copy"),
            ("content.trust.model.key", "do-not-copy")])
    before = (storage / "state.vscdb").read_bytes()
    sessions.seed_profile(profile, source, extensions)
    if platform_name == "win32":
        assert json.loads((profile / "Local State").read_text()) == {"os_crypt": {"encrypted_key": wrapped}}
    else:
        assert not (profile / "Local State").exists()
    with sqlite3.connect(profile / "User/globalStorage/state.vscdb") as db:
        assert db.execute("SELECT key,value FROM ItemTable").fetchall() == [(key, value)]
    assert (storage / "state.vscdb").read_bytes() == before
    assert (source / "Local State").read_text() == original_state



def test_windows_plaintext_extra_fields_are_not_imported(monkeypatch, tmp_path):
    import base64
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"))
    source, extensions, profile = profile_inputs(tmp_path)
    profile.mkdir(parents=True)
    wrapped = base64.b64encode(b"DPAPI" + bytes(range(100))).decode("ascii")
    (source / "Local State").write_text(json.dumps({"os_crypt": {"encrypted_key": wrapped}}))
    value = json.dumps({"type": "Buffer", "data": list(b"v10" + bytes(range(40))), "token": secrets.token_hex(16)})
    with pytest.raises(AnalysisError, match="authenticate.*managed profile"):
        sessions.prepare_windows_encryption(profile, source, [("sonar", value)])
    assert not (profile / "Local State").exists()



@pytest.mark.parametrize("existing_key", [None, {"encrypted_key": "managed-key"}])
def test_windows_import_never_overwrites_managed_encryption_state(monkeypatch, tmp_path, existing_key):
    import base64
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"))
    source, extensions, profile = profile_inputs(tmp_path)
    profile.mkdir(parents=True)
    wrapped = base64.b64encode(b"DPAPI" + bytes(range(100))).decode("ascii")
    (source / "Local State").write_text(json.dumps({"os_crypt": {"encrypted_key": wrapped}}))
    original = json.dumps({"os_crypt": existing_key, "managed": "preserve"})
    (profile / "Local State").write_text(original)
    value = json.dumps({"type": "Buffer", "data": list(b"v10" + bytes(range(40)))})
    with pytest.raises(AnalysisError, match="authenticate.*managed profile"):
        sessions.prepare_windows_encryption(profile, source, [("sonar", value)])
    assert (profile / "Local State").read_text() == original



def test_linux_plaintext_secretstorage_is_never_copied(monkeypatch, tmp_path):
    import sqlite3
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="linux"))
    source, extensions, profile = profile_inputs(tmp_path)
    storage = source / "User/globalStorage"
    storage.mkdir()
    key = 'secret://{"extensionId":"sonarsource.sonarlint-vscode","key":"cloud"}'
    with sqlite3.connect(storage / "state.vscdb") as db:
        db.execute("CREATE TABLE ItemTable(key TEXT, value BLOB)")
        db.execute("INSERT INTO ItemTable VALUES (?, ?)", (key, "not-encrypted"))
    with pytest.raises(AnalysisError, match="authenticate.*managed profile"):
        sessions.seed_profile(profile, source, extensions)
    assert not (profile / "User/globalStorage/state.vscdb").exists()



def test_managed_auth_connection_metadata_survives_reseed(tmp_path):
    source, extensions, profile = profile_inputs(tmp_path)
    sessions.seed_profile(profile, source, extensions)
    settings_path = profile / "User/settings.json"
    managed = json.loads(settings_path.read_text())
    key = "sonarlint.connectedMode.connections.sonarcloud"
    managed[key] = [{"connectionId": "managed-login", "organizationKey": "org", "token": secrets.token_hex(16)}]
    settings_path.write_text(json.dumps(managed), encoding="utf-8")
    sessions.seed_profile(profile, source, extensions)
    assert json.loads(settings_path.read_text())[key] == [
        {"connectionId": "managed-login", "organizationKey": "org"}]
    assert not (source / "User/settings.json").exists()

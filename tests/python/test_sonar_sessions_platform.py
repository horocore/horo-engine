"""Ownership and private-profile regressions for Linux/Windows sessions."""
from __future__ import annotations

import analyze

import json
import sys
from types import SimpleNamespace
from pathlib import Path, PurePosixPath

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import analyze as sessions
from analyze import AnalysisError as AnalysisError


def profile_inputs(tmp_path):
    source = tmp_path / "source Code"
    (source / "User").mkdir(parents=True)
    extensions = tmp_path / "extensions"
    extension = extensions / "sonarsource.sonarlint-vscode-6.1.1-win32-x64"
    extension.mkdir(parents=True)
    (extension / "package.json").write_text('{}', encoding="utf-8")
    profile = tmp_path / "managed" / "profile"
    return source, extensions, profile


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
    original = json.dumps({
        "security.workspace.trust.enabled": False,
        "security.workspace.trust.untrustedFiles": "open",
        "sonarlint.connectedMode.connections.sonarcloud": [
            {"connectionId": "cloud", "organizationKey": "org", "token": "never-copy"}],
    })
    (source / "User/settings.json").write_text(original, encoding="utf-8")
    sessions.seed_profile(profile, source, extensions)
    settings_text = (profile / "User/settings.json").read_text()
    assert json.loads(settings_text)["security.workspace.trust.enabled"] is True
    assert "never-copy" not in settings_text
    assert "untrustedFiles" not in settings_text
    assert (source / "User/settings.json").read_text() == original


def test_process_and_listener_wrappers_use_native_facade(monkeypatch):
    import analyze as quality_platform
    table = {42: {"started": "native-creation-id"}}
    monkeypatch.setattr(quality_platform, 'native_processes', lambda: table, raising=False)
    monkeypatch.setattr(quality_platform, 'native_listener_port', lambda pids, ports:
                        (42, min(ports)) if pids == {42} else None, raising=False)
    assert sessions.session_processes() is table
    assert sessions.session_listener_port({42}) == (42, min(sessions.BRIDGE_PORTS))


def test_two_slot_leases_use_native_locks(tmp_path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root", 0.01)
    with pool.lease():
        assert pool.stop() == [{"slot": 0, "status": "busy"}]
        with pool.lease():
            with pytest.raises(AnalysisError, match="two Sonar sessions"):
                pool.acquire()
    with pool.lease() as (slot, record):
        assert slot == 0
        assert record == {}


@pytest.mark.parametrize("argv, expected", [
    (["Code.exe", "--user-data-dir", "c:/CACHE/Managed Profile"], True),
    (["Code.exe", "--user-data-dir=c:/CACHE/Managed Profile"], True),
    (["Code.exe", "--user-data-dir=c:/CACHE/Managed Profile-other"], False),
    (["Code.exe", "--unrelated=--user-data-dir=c:/CACHE/Managed Profile"], False),
    (["Code.exe", "--user-data-dir", "x c:/CACHE/Managed Profile"], False),
    (["Code.exe", "--user-data-dir=c:/cache/managed profile", "--user-data-dir=c:/other"], False),
])
def test_windows_profile_matches_only_complete_normalized_path(monkeypatch, argv, expected):
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"), raising=False)
    assert sessions.has_profile({"argv": argv}, Path("C:/cache/managed profile")) is expected


def code_tree(profile):
    record = {"pid": 10, "started": "100", "profile": str(profile), "nonce": "nonce",
              "executable": "C:/Code/Code.exe"}
    table = {
        10: {"parent": 1, "started": "100", "executable": record["executable"],
             "argv": ["Code.exe", "--user-data-dir", str(profile)], "markers": {}},
        11: {"parent": 10, "started": "110", "executable": "C:/Code/Code.exe",
             "argv": ["Code.exe", "--type=extensionHost"], "markers": {
                 "HORO_QUALITY_PROFILE": str(profile), "HORO_QUALITY_SESSION": "nonce"}},
    }
    return record, table


def test_windows_code_main_discovered_only_through_nonce_witness(monkeypatch, tmp_path):
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"), raising=False)
    profile = tmp_path / "Managed Profile"
    record, table = code_tree(profile)
    result = sessions.discover_main(table, profile, tmp_path / "extension", tmp_path / "workspace", "nonce")
    assert result == [(10, table[10], "nonce")]
    table[11]["markers"]["HORO_QUALITY_SESSION"] = "foreign"
    assert sessions.discover_main(table, profile, tmp_path, tmp_path, "nonce") == []


@pytest.mark.parametrize("independent", [False, True])
def test_real_discovery_excludes_cli_launcher_but_keeps_independent_gui_mains(monkeypatch, tmp_path, independent):
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"))
    profile = tmp_path / "profile"
    _, table = code_tree(profile)
    markers = dict(table[11]["markers"])
    table[9] = dict(table[10], parent=1, started="90", markers=markers,
                    argv=["Code.exe", "C:/Code/resources/app/out/cli.js", "--user-data-dir", str(profile)])
    table[10].update(parent=9, markers=markers)
    if independent:
        table[20] = dict(table[10], parent=1, started="200")
        table[21] = dict(table[11], parent=20, started="210")
    result = sessions.discover_main(table, profile, tmp_path / "extension", tmp_path / "workspace", "nonce")
    assert {item[0] for item in result} == ({10, 20} if independent else {10})


@pytest.mark.parametrize("operation", ["descendants", "discover_main"])
def test_windows_reused_parent_pid_cannot_adopt_older_witness(monkeypatch, tmp_path, operation):
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"))
    profile = tmp_path / "profile"
    record, table = code_tree(profile)
    # A child retains a dead parent's PID; that PID is now a newer Code process.
    table[10]["started"] = "120"
    if operation == "descendants":
        assert sessions.descendants(10, table) == {10}
    else:
        assert sessions.discover_main(table, profile, tmp_path, tmp_path, "nonce") == []


@pytest.mark.parametrize("failure", [AnalysisError("second launch rejected"), KeyboardInterrupt()])
def test_activation_failure_retains_launch_identity_for_safe_retirement(monkeypatch, tmp_path, failure):
    source, extensions, unused = profile_inputs(tmp_path)
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    profile = pool.directory / sessions.identity(pool.root) / "profile"
    record, table = code_tree(profile)
    launched = []
    def run(*args):
        launched.append(args)
        if len(launched) == 2:
            raise failure
    monkeypatch.setattr(sessions, 'quality_run', run)
    monkeypatch.setattr(sessions.uuid, 'uuid4', lambda: SimpleNamespace(hex="nonce"))
    monkeypatch.setattr(sessions, 'session_processes', lambda: table)
    # A safe termination failure must reserve the exact pending launch, not orphan it.
    monkeypatch.setattr(sessions, 'terminate', lambda old: False)
    with pytest.raises(type(failure)):
        pool.open(0, {}, tmp_path / "workspace", source, extensions, 0.01, tmp_path / "x.cpp")
    saved = json.loads((pool.directory / "0.json").read_text())
    assert saved["nonce"] == "nonce"
    assert saved["profile"] == str(profile)
    assert saved["pid"] == 10
    assert saved["started"] == "100"


@pytest.mark.parametrize("prior_record", [False, True])
@pytest.mark.parametrize("failure", ["resolution", "batch", "popen", "contained"])
def test_proven_no_escape_launch_failure_releases_slot_for_retry_and_stop(monkeypatch, tmp_path, failure, prior_record):
    source, extensions, _ = profile_inputs(tmp_path)
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    pool.root.mkdir()
    monkeypatch.setattr(sessions, 'session_processes', lambda: {})
    if failure == "resolution":
        monkeypatch.setattr(sessions.shutil, 'which', lambda *args, **kwargs: None)
    elif failure == "batch":
        monkeypatch.setattr(sessions.shutil, 'which', lambda *args, **kwargs: 'C:/Code/code.cmd')
    elif failure == "popen":
        monkeypatch.setattr(sessions, '_command', lambda *args: ['missing.exe'])
        def denied(*args, **kwargs):
            raise OSError("launch denied")
        monkeypatch.setattr(sessions.subprocess, 'Popen', denied)
    else:
        monkeypatch.setattr(sessions, '_command', lambda *args: [sys.executable, '-c', 'import time; time.sleep(60)'])
    workspace = tmp_path / ("unsafe&workspace" if failure == "batch" else "workspace")
    for _ in range(2):
        with pytest.raises(AnalysisError):
            with pool.lease() as (slot, previous):
                pool.open(slot, previous or ({"pid": 999, "started": "old"} if prior_record else {}),
                          workspace, source, extensions, 0.01, tmp_path / "x.cpp")
        assert not (pool.directory / "0.json").exists(), "proven no escape must not exhaust capacity"
    assert pool.stop() == []


def test_invisible_ambiguous_launch_keeps_slot_reserved(monkeypatch, tmp_path):
    source, extensions, _ = profile_inputs(tmp_path)
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    monkeypatch.setattr(sessions, 'session_processes', lambda: {})
    def unknown(*args, **kwargs):
        raise AnalysisError("launcher outcome unknown")
    monkeypatch.setattr(sessions, 'quality_run', unknown)
    with pytest.raises(AnalysisError):
        with pool.lease() as (slot, previous):
            pool.open(slot, previous, tmp_path / "workspace", source, extensions, 0.01, tmp_path / "x.cpp")
    saved = sessions.read_json(pool.directory / "0.json")
    assert saved["pending"] is True
    assert saved["launchOutcome"] == "ambiguous"
    assert pool.stop()[0]["status"] == "unverified"
    assert (pool.directory / "0.json").exists()


def test_first_launch_escape_is_not_erased_by_second_no_launch_failure(monkeypatch, tmp_path):
    source, extensions, _ = profile_inputs(tmp_path)
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    monkeypatch.setattr(sessions, 'session_processes', lambda: {})
    calls = []
    def run(*args):
        calls.append(args)
        if len(calls) == 2:
            args[-1]["outcome"] = "no_escape"
            raise AnalysisError("activation never launched")
    monkeypatch.setattr(sessions, 'quality_run', run)
    with pytest.raises(AnalysisError):
        with pool.lease() as (slot, previous):
            pool.open(slot, previous, tmp_path / "workspace", source, extensions, 0.01, tmp_path / "x.cpp")
    assert sessions.read_json(pool.directory / "0.json")["pending"] is True
    assert pool.stop()[0]["status"] == "unverified"


def test_stop_releases_persisted_positive_no_escape_evidence_without_discovery(monkeypatch, tmp_path):
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    sessions.write_json(pool.directory / "0.json", {"pending": True, "launchOutcome": "no_escape"})
    monkeypatch.setattr(sessions, 'session_processes', lambda: pytest.fail("positive lifecycle evidence is sufficient"))
    assert pool.stop() == [{"slot": 0, "status": "already_exited"}]
    assert not (pool.directory / "0.json").exists()


def test_owned_requires_exact_profile_even_with_valid_nonce_witness(tmp_path):
    record, table = code_tree(tmp_path / "profile")
    assert sessions.owned(record, table)
    table[10]["argv"][-1] += "-other"
    assert not sessions.owned(record, table)


@pytest.mark.parametrize("mismatch", [None, "creation", "executable", "nonce", "listener"])
def test_retirement_signals_only_owned_exact_tree(monkeypatch, tmp_path, mismatch):
    record, table = code_tree(tmp_path / "profile")
    record.update(listenerPid=11, listenerStarted="110", port=min(sessions.BRIDGE_PORTS))
    if mismatch == "creation":
        table[10]["started"] = "101"
    elif mismatch == "executable":
        table[10]["executable"] = "C:/Other/Code.exe"
    elif mismatch == "nonce":
        table[11]["markers"]["HORO_QUALITY_SESSION"] = "foreign"
    elif mismatch == "listener":
        table[11]["started"] = "111"
    monkeypatch.setattr(sessions, 'session_processes', lambda: table)
    monkeypatch.setattr(sessions, 'session_listener_port', lambda pids: (11, record["port"]))
    calls = []
    monkeypatch.setattr(sessions, 'terminate_processes',
                        lambda pids, observed: calls.append((pids, observed)) or True, raising=False)
    assert sessions.terminate(record) is (mismatch is None)
    assert calls == ([({10, 11}, table)] if mismatch is None else [])


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


def test_linux_code_process_title_remains_owned(monkeypatch):
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="linux"))
    profile = PurePosixPath("/cache/Managed Profile/profile")
    extension = PurePosixPath("/cache/Managed Profile/extensions/sonar")
    workspace = PurePosixPath("/work/space/workspace.code-workspace")
    executable = "/usr/share/code/code"
    title = (f"{executable} --user-data-dir --extensions-dir --extensionDevelopmentPath --new-window "
             f"{profile} {profile.parent / 'extensions'} {extension} {workspace}")
    record, table = code_tree(profile)
    record.update(executable=executable, title=title)
    table[10].update(executable=executable, argv=[title])
    assert sessions.discover_main(table, profile, extension, workspace, "nonce") == [(10, table[10], "nonce")]
    assert sessions.owned(record, table)
    assert not sessions.same_path("/Profile", "/profile")


def test_windows_plaintext_extra_fields_are_not_imported(monkeypatch, tmp_path):
    import base64
    monkeypatch.setattr(sessions, 'sys', SimpleNamespace(platform="win32"))
    source, extensions, profile = profile_inputs(tmp_path)
    profile.mkdir(parents=True)
    wrapped = base64.b64encode(b"DPAPI" + bytes(range(100))).decode("ascii")
    (source / "Local State").write_text(json.dumps({"os_crypt": {"encrypted_key": wrapped}}))
    value = json.dumps({"type": "Buffer", "data": list(b"v10" + bytes(range(40))), "token": "never-copy"})
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
    managed[key] = [{"connectionId": "managed-login", "organizationKey": "org", "token": "not-a-real-token"}]
    settings_path.write_text(json.dumps(managed), encoding="utf-8")
    sessions.seed_profile(profile, source, extensions)
    assert json.loads(settings_path.read_text())[key] == [
        {"connectionId": "managed-login", "organizationKey": "org"}]
    assert not (source / "User/settings.json").exists()


def test_stop_explicitly_releases_every_native_slot_lock(monkeypatch, tmp_path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root")
    original_unlock = sessions.unlock
    released = []

    def unlock(stream):
        released.append(Path(stream.name).name)
        original_unlock(stream)

    monkeypatch.setattr(sessions, 'unlock', unlock)
    assert pool.stop() == []
    assert set(released) == {"registry.lock", "0.lock", "1.lock"}


@pytest.mark.parametrize("mismatch", [None, "missing", "path", "version", "new_source"])
def test_session_reuse_requires_exact_selected_extension_identity(monkeypatch, tmp_path, mismatch):
    source, extensions, _ = profile_inputs(tmp_path)
    package = next(extensions.glob('*/package.json'))
    package.write_text('{"version":"6.1.1"}', encoding="utf-8")
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    profile = pool.directory / sessions.identity(pool.root) / "profile"
    sessions.seed_profile(profile, source, extensions)
    extension = profile.parent / "extensions" / package.parent.name
    record, table = code_tree(profile)
    record.update(isolationVersion=4, listenerPid=11, listenerStarted="110", port=min(sessions.BRIDGE_PORTS),
                  extension=str(extension), extensionVersion="6.1.1")
    if mismatch == "missing":
        record.pop("extension")
        record.pop("extensionVersion")
    elif mismatch == "path":
        record["extension"] += "-old"
    elif mismatch == "version":
        record["extensionVersion"] = "6.0.0"
    elif mismatch == "new_source":
        import os
        newer = extensions / "sonarsource.sonarlint-vscode-6.2.0-win32-x64"
        newer.mkdir()
        (newer / "package.json").write_text('{"version":"6.2.0"}', encoding="utf-8")
        os.utime(package, (1, 1))
    monkeypatch.setattr(sessions, 'session_processes', lambda: table)
    monkeypatch.setattr(sessions, 'session_listener_port', lambda pids: (11, record["port"]))
    monkeypatch.setattr(sessions, 'request_bridge', lambda *args: {})
    def retired(old):
        assert old is record
        raise AnalysisError("retired stale extension")
    monkeypatch.setattr(sessions, 'retire', retired)
    if mismatch:
        with pytest.raises(AnalysisError, match="retired stale extension"):
            pool.open(0, record, tmp_path, source, extensions, 0, tmp_path / "x.cpp")
    else:
        assert pool.open(0, record, tmp_path, source, extensions, 0, tmp_path / "x.cpp") is record


def test_sonar_report_names_only_the_active_extension_not_stale_cached_versions(monkeypatch, tmp_path):
    source, extensions, profile = profile_inputs(tmp_path)
    next(extensions.glob('*/package.json')).write_text('{"version":"6.1.1"}', encoding="utf-8")
    sessions.seed_profile(profile, source, extensions)
    active = profile.parent / "extensions/sonarsource.sonarlint-vscode-6.2.0-win32-x64"
    active.mkdir()
    (active / "package.json").write_text('{"version":"6.2.0"}', encoding="utf-8")
    database = tmp_path / "compile_commands.json"
    database.write_text('[]', encoding="utf-8")
    record = dict(profile=str(profile), extension=str(active), extensionVersion="6.2.0", bindingReady=True,
                  pid=10, started="100", port=min(sessions.BRIDGE_PORTS))
    monkeypatch.setattr(sessions, 'prepare', lambda *args: (database, {}))
    monkeypatch.setattr(sessions, 'write_workspace', lambda *args: tmp_path / "workspace")
    monkeypatch.setattr(sessions.SessionPool, 'open', lambda *args: record)
    monkeypatch.setattr(sessions, 'analyze_files', lambda *args: ([], []))
    args = SimpleNamespace(compile_commands=None, timeout=1, connection_id="cloud", project_key="project",
                           code_user_data=source, extensions_directory=extensions, startup_timeout=1)
    result = sessions.sonar_check(tmp_path, tmp_path / "cache", tmp_path / "repo-cache",
                                  [tmp_path / "x.cpp"], args)
    assert result["versions"] == ["6.2.0"]
    assert result["extension"] == {"path": str(active), "version": "6.2.0"}


def test_legacy_trust_disabled_session_is_not_reused(monkeypatch, tmp_path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root")
    record, table = code_tree(pool.directory / sessions.identity(pool.root) / "profile")
    record.update(isolationVersion=3, listenerPid=11, listenerStarted="110", port=min(sessions.BRIDGE_PORTS))
    monkeypatch.setattr(sessions, 'session_processes', lambda: table)
    monkeypatch.setattr(sessions, 'session_listener_port', lambda pids: (11, record["port"]))
    monkeypatch.setattr(sessions, 'request_bridge', lambda *args: {})
    retired = []
    monkeypatch.setattr(sessions, 'retire', lambda old: retired.append(old) or True)

    def launch(*args):
        raise AnalysisError("fresh secured profile")

    monkeypatch.setattr(sessions, 'launch_profile', launch)
    with pytest.raises(AnalysisError, match="fresh secured profile"):
        pool.open(0, record, tmp_path, tmp_path, tmp_path, 0, tmp_path / "source.cpp")
    assert retired == [record]


@pytest.mark.parametrize("leased", [False, True])
def test_unique_startup_becoming_ambiguous_keeps_all_launches_reserved(monkeypatch, tmp_path, leased):
    source, extensions, _ = profile_inputs(tmp_path)
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    profile = pool.directory / sessions.identity(pool.root) / "profile"
    _, first = code_tree(profile)
    multiple = dict(first)
    multiple[20] = dict(first[10], parent=1, started="200")
    multiple[21] = dict(first[11], parent=20, started="210")
    snapshots = []

    def processes():
        snapshots.append(None)
        return first if len(snapshots) == 1 else multiple

    clock = iter(range(1000))
    monkeypatch.setattr(sessions.time, "monotonic", lambda: next(clock))
    monkeypatch.setattr(sessions.time, "sleep", lambda *_: None)
    monkeypatch.setattr(sessions, "session_processes", processes)
    monkeypatch.setattr(sessions.uuid, "uuid4", lambda: SimpleNamespace(hex="nonce"))
    monkeypatch.setattr(sessions, "quality_run", lambda *args: None)
    monkeypatch.setattr(sessions, "session_listener_port", lambda pids: None)
    terminated = []
    monkeypatch.setattr(sessions, "terminate_processes", lambda pids, table: terminated.append(pids) or True)
    with pytest.raises(AnalysisError):
        if leased:
            with pool.lease() as (slot, record):
                pool.open(slot, record, tmp_path, source, extensions, 4, tmp_path / "x.cpp")
        else:
            pool.open(0, {}, tmp_path, source, extensions, 4, tmp_path / "x.cpp")
    reservation = sessions.read_json(pool.directory / "0.json")
    assert reservation["pending"] is True
    assert reservation["nonce"] == "nonce"
    assert "pid" not in reservation
    assert terminated == []
    assert pool.stop() == [{"slot": 0, "status": "unverified",
                            "error": "Managed launch ownership is not yet verifiable; slot remains reserved"}]
    assert (pool.directory / "0.json").exists()


def test_transient_activation_gui_must_exit_before_a_unique_main_is_accepted(monkeypatch, tmp_path):
    source, extensions, _ = profile_inputs(tmp_path)
    pool = sessions.SessionPool(tmp_path / "cache", tmp_path / "root")
    profile = pool.directory / sessions.identity(pool.root) / "profile"
    _, table = code_tree(profile)
    table[10]["markers"] = dict(table[11]["markers"])
    table[20] = dict(table[10], parent=1, started="200",
                     argv=["Code.exe", "--user-data-dir", str(profile), "--reuse-window", "--goto", str(tmp_path / "x.cpp")])
    snapshots = []
    def processes():
        snapshots.append(None)
        return table if len(snapshots) == 1 else {pid: data for pid, data in table.items() if pid != 20}
    monkeypatch.setattr(sessions, 'session_processes', processes)
    monkeypatch.setattr(sessions.uuid, 'uuid4', lambda: SimpleNamespace(hex="nonce"))
    monkeypatch.setattr(sessions, 'quality_run', lambda *args: None)
    bridges = []
    monkeypatch.setattr(sessions, 'session_listener_port', lambda pids: (11, min(sessions.BRIDGE_PORTS)))
    monkeypatch.setattr(sessions, 'request_bridge', lambda *args: bridges.append(len(snapshots)))
    monkeypatch.setattr(sessions, 'terminate', lambda *args: False)
    record = pool.open(0, {}, tmp_path / "workspace", source, extensions, 1, tmp_path / "x.cpp")
    assert record["pid"] == 10
    assert bridges == [2], "do not select a main or contact a bridge while ambiguity survives"


def test_ambiguous_code_mains_are_not_guessed(monkeypatch, tmp_path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root")
    profile = pool.directory / sessions.identity(pool.root) / "profile"
    record, table = code_tree(profile)
    table[20] = dict(table[10], started="200")
    monkeypatch.setattr(sessions, 'launch_profile', lambda *args: ("nonce", tmp_path / "extension"))
    monkeypatch.setattr(sessions, 'session_processes', lambda: table)
    monkeypatch.setattr(sessions, 'discover_main', lambda *args:
                        [(10, table[10], "nonce"), (20, table[20], "nonce")])
    monkeypatch.setattr(sessions, 'session_listener_port', lambda pids: (11, min(sessions.BRIDGE_PORTS)))
    monkeypatch.setattr(sessions, 'request_bridge', lambda *args: {})
    monkeypatch.setattr(sessions, 'terminate', lambda old: False)
    with pytest.raises(AnalysisError, match="ambiguous.*Code main"):
        pool.open(0, {}, tmp_path, tmp_path, tmp_path, 1, tmp_path / "source.cpp")
    assert not (pool.directory / "0.json").exists()


def test_startup_prerequisite_names_workspace_trust_and_managed_auth(monkeypatch, tmp_path):
    pool = sessions.SessionPool(tmp_path, tmp_path / "root")
    monkeypatch.setattr(sessions, 'launch_profile', lambda *args: ("nonce", tmp_path / "extension"))
    monkeypatch.setattr(sessions, 'terminate', lambda record: False)
    with pytest.raises(AnalysisError, match="workspace trust.*authenticate.*managed"):
        pool.open(0, {}, tmp_path, tmp_path, tmp_path, 0, tmp_path / "source.cpp")

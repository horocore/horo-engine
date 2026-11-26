"""Exercise approval, provenance and redaction through the offline exporter."""

from datetime import UTC, datetime, timedelta
import hashlib
import json
import os
from pathlib import Path
import sys

import pytest
import qualify_ai_evaluation as evaluation


PRIVATE = "private-caption-player-value"


def frame(tick, outcome):
    return {
        "format": "horo-ai-replay-evidence", "version": 1, "tick": str(tick), "agentId": "438438438",
        "sceneDefinitionId": "448448448", "sceneInput": {"schemaVersion": 2, "digest": "1" * 64},
        "canonicalAi": {"recordVersion": 2, "digest": "2" * 64},
        "decision": {"traceVersion": 4, "planDigest": "3" * 64, "outcome": outcome,
                     "blackboard": {"schemaVersion": 1, "schemaId": "123", "value": PRIVATE},
                     "records": [{"note": PRIVATE}]},
        "perceptionInput": {"records": [{"description": PRIVATE}]},
    }


def approval_for(data):
    return {
        "schemaVersion": 1, "purpose": "offline-evaluation", "replaySha256": hashlib.sha256(data).hexdigest(),
        "expiresOnUtc": (datetime.now(UTC).date() + timedelta(days=1)).isoformat(),
        "qualification": {
            "provider": {"version": "1.2.3", "sha256": "4" * 64},
            "model": {"version": "2", "sha256": "5" * 64},
            "platform": {"os": "windows", "architecture": "x64", "version": "10.0.26100"},
            "workload": {"version": "1", "sha256": "6" * 64},
        },
    }


@pytest.fixture
def export(tmp_path, monkeypatch, capsys):
    def run(data, approval, *options):
        replay = tmp_path / "evidence.json"
        grant = tmp_path / "approval.json"
        replay.write_bytes(data)
        grant.write_text(json.dumps(approval), encoding="utf-8")
        monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(replay), "--approval", str(grant), *options])
        status = evaluation.main()
        return status, capsys.readouterr()

    return run


def test_publishes_the_same_redacted_manifest_to_a_new_file(tmp_path, export):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    status, streamed = export(data, approval)
    assert status == 0
    destination = tmp_path / "evaluation ç report.json"
    status, written = export(data, approval, "--output", str(destination))
    assert status == 0
    assert not written.out and not written.err
    assert destination.read_bytes() == streamed.out.encode("utf-8")
    assert not list(tmp_path.glob(".horo-ai-evaluation-*"))


@pytest.fixture
def retained_pair(tmp_path, export, monkeypatch, capsys):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    manifest = tmp_path / "retained ç report.json"
    status, result = export(data, approval, "--output", str(manifest))
    assert status == 0, result.err
    replay = tmp_path / "evidence.json"
    grant = tmp_path / "approval.json"
    record = json.loads(manifest.read_bytes())
    today = datetime.now(UTC).date()

    def expire():
        approval["expiresOnUtc"] = record["expiresOnUtc"] = today.isoformat()
        manifest.write_text(json.dumps(record), encoding="utf-8")
        grant.write_text(json.dumps(approval), encoding="utf-8")

    def purge(source=replay):
        monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(manifest), "--approval", str(grant),
                                          "--purge-expired", "--source-replay", str(source)])
        status = evaluation.main()
        return status, capsys.readouterr()

    return manifest, replay, grant, expire, purge


def test_purge_removes_expired_pair_and_preserves_audit_approval(retained_pair):
    manifest, replay, grant, expire, purge = retained_pair
    expire()
    approved = grant.read_bytes()
    status, result = purge()
    assert status == 0, result.err
    assert json.loads(result.out) == {"schemaVersion": 1, "status": "purged",
                                      "expiresOnUtc": json.loads(approved)["expiresOnUtc"], "sourceRemoved": True}
    assert PRIVATE not in result.out and str(replay) not in result.out
    assert not manifest.exists() and not replay.exists()
    assert grant.read_bytes() == approved


@pytest.mark.parametrize("scenario", ["current", "source-mismatch", "approval-mismatch", "hardlink", "symlink", "other-directory"])
def test_purge_refuses_unapproved_sources_and_leaves_both_files(tmp_path, retained_pair, scenario):
    manifest, replay, grant, expire, purge = retained_pair
    source = replay
    if scenario != "current":
        expire()
    if scenario == "source-mismatch":
        replay.write_bytes(json.dumps(frame(3, 4)).encode())
    if scenario == "approval-mismatch":
        grant.write_text(json.dumps(approval_for(b"other source")), encoding="utf-8")
    if scenario == "hardlink":
        os.link(replay, tmp_path / "alias.json")
    if scenario == "symlink":
        source = tmp_path / "alias.json"
        try:
            source.symlink_to(replay)
        except OSError:
            pytest.skip("File symlinks are unavailable")
    if scenario == "other-directory":
        alternate = tmp_path / "alternate"
        alternate.mkdir()
        source = alternate / replay.name
        source.write_bytes(replay.read_bytes())
    old_manifest, old_replay = manifest.read_bytes(), replay.read_bytes()
    status, result = purge(source)
    assert status == 1 and not result.out
    assert PRIVATE not in result.err and str(tmp_path) not in result.err
    assert manifest.read_bytes() == old_manifest and replay.read_bytes() == old_replay


def test_purge_retries_after_replay_was_removed(retained_pair, monkeypatch):
    manifest, replay, grant, expire, purge = retained_pair
    expire()
    existing_unlink = Path.unlink

    def fail_manifest(path):
        if path == manifest:
            raise PermissionError("simulated access loss")
        existing_unlink(path)

    with monkeypatch.context() as patch:
        patch.setattr(Path, "unlink", fail_manifest)
        status, result = purge()
        assert status == 1 and not result.out and "retry" in result.err
    assert not replay.exists() and manifest.exists()

    status, result = purge()
    assert status == 0, result.err
    assert json.loads(result.out)["sourceRemoved"] is False
    assert not manifest.exists() and grant.exists()


def test_purge_preserves_replay_when_the_manifest_changes_after_validation(retained_pair, monkeypatch):
    manifest, replay, _grant, expire, purge = retained_pair
    expire()
    verify = evaluation.verify_replay_bytes

    def replace_manifest(*args):
        verify(*args)
        manifest.write_bytes(b"separate retention operation")

    monkeypatch.setattr(evaluation, "verify_replay_bytes", replace_manifest)
    previous_replay = replay.read_bytes()
    status, result = purge()
    assert status == 1 and not result.out and "changed after validation" in result.err
    assert replay.read_bytes() == previous_replay
    assert manifest.read_bytes() == b"separate retention operation"


@pytest.mark.parametrize("scenario", ["match", "no-model", "wrong-model", "missing-workload", "over-budget"])
@pytest.mark.parametrize("retained", [False, True])
def test_evaluation_can_check_the_approved_artifact_bytes(tmp_path, export, monkeypatch, capsys, scenario, retained):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    artifacts = tmp_path / "qualification artifacts"
    artifacts.mkdir()
    for component in ("provider", "model", "workload"):
        if scenario == "no-model" and component == "model":
            approval["qualification"][component] = None
            continue
        contents = f"approved {component}".encode()
        digest = hashlib.sha256(contents).hexdigest()
        approval["qualification"][component]["sha256"] = digest
        if scenario != "missing-workload" or component != "workload":
            (artifacts / digest).write_bytes(PRIVATE.encode() if scenario == "wrong-model" and component == "model" else contents)
    if scenario == "over-budget":
        monkeypatch.setattr(evaluation, "MAX_QUALIFICATION_ARTIFACT_BYTES", 4, raising=False)

    artifact_options = ["--qualification-artifacts", str(artifacts)]
    if retained:
        manifest = tmp_path / "manifest.json"
        status, result = export(data, approval, "--output", str(manifest))
        assert status == 0, result.err
        monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(manifest), "--approval", str(tmp_path / "approval.json"),
                                          "--read-retained", "--source-replay", str(tmp_path / "evidence.json"), *artifact_options])
        status = evaluation.main()
        result = capsys.readouterr()
    else:
        status, result = export(data, approval, *artifact_options)

    if scenario in ("match", "no-model"):
        assert status == 0, result.err
        assert json.loads(result.out)["qualification"] == approval["qualification"]
    else:
        component = {"wrong-model": "model", "missing-workload": "workload", "over-budget": "provider"}[scenario]
        assert status == 1 and not result.out
        assert f"Qualification {component} artifact" in result.err
        assert PRIVATE not in result.err and str(artifacts) not in result.err


@pytest.mark.parametrize("name", ["evidence.json", "approval.json", "existing.json"])
def test_export_preserves_existing_files_at_the_destination(tmp_path, export, name):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    destination = tmp_path / name
    if name == "existing.json":
        destination.write_bytes(b"existing manifest")
    expected = {"evidence.json": data, "approval.json": json.dumps(approval).encode(), "existing.json": b"existing manifest"}[name]
    status, result = export(data, approval, "--output", str(destination))
    assert status == 1
    assert not result.out and "output already exists" in result.err
    assert destination.read_bytes() == expected
    assert not list(tmp_path.glob(".horo-ai-evaluation-*"))


def test_publishing_never_replaces_a_concurrent_destination(tmp_path, export, monkeypatch):
    data = json.dumps(frame(3, 2)).encode()
    destination = tmp_path / "evaluation.json"
    link = evaluation.os.link

    def race(staged, path):
        path.write_bytes(b"concurrent publisher")
        link(staged, path)

    monkeypatch.setattr(evaluation.os, "link", race)
    status, result = export(data, approval_for(data), "--output", str(destination))
    assert status == 1
    assert not result.out and "output already exists" in result.err
    assert destination.read_bytes() == b"concurrent publisher"
    assert not list(tmp_path.glob(".horo-ai-evaluation-*"))


def test_expiry_during_staging_leaves_no_published_manifest(tmp_path, export, monkeypatch):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    expiry = datetime.fromisoformat(approval["expiresOnUtc"]).replace(tzinfo=UTC)
    destination = tmp_path / "evaluation.json"

    class Clock:
        now = staticmethod(lambda _zone: expiry - timedelta(seconds=1))

    def cross_expiry(_descriptor):
        Clock.now = staticmethod(lambda _zone: expiry)

    monkeypatch.setattr(evaluation, "datetime", Clock)
    monkeypatch.setattr(evaluation.os, "fsync", cross_expiry)
    status, result = export(data, approval, "--output", str(destination))
    assert status == 1
    assert not result.out and "approval has expired" in result.err
    assert not destination.exists()
    assert not list(tmp_path.glob(".horo-ai-evaluation-*"))


@pytest.mark.parametrize("window", [False, True])
def test_exports_versions_and_counts_without_replay_values(tmp_path, export, window):
    source = frame(3, 2)
    if window:
        source = {"format": source["format"], "version": 2, "frames": [source, frame(4, 3)]}
    data = json.dumps(source).encode()
    approval = approval_for(data)
    if not window:
        approval["qualification"]["model"] = None
    status, result = export(data, approval)
    assert status == 0, result.err
    assert not result.err
    payload = json.loads(result.out)
    assert payload["sourceReplaySha256"] == approval["replaySha256"]
    assert payload["qualification"] == approval["qualification"]
    assert payload["expiresOnUtc"] == approval["expiresOnUtc"]
    evidence = payload["evidence"]
    assert evidence["frameCount"] == 1 + window
    assert evidence["decisionOutcomes"] == {"idle": 0, "running": 0, "succeeded": 1, "failed": int(window), "cancelled": 0}
    assert evidence["frames"][0] == {
        "sceneInput": {"schemaVersion": 2, "sha256": "1" * 64},
        "canonicalAi": {"recordVersion": 2, "sha256": "2" * 64},
        "decision": {"traceVersion": 4, "planSha256": "3" * 64},
        "blackboard": {"schemaVersion": 1},
    }
    for value in (PRIVATE, "438438438", "448448448", str(tmp_path), "perceptionInput", "records", "schemaId"):
        assert value not in result.out


@pytest.mark.parametrize("failure", ["expired", "digest", "unclassified", "wrong-purpose", "invalid-version"])
def test_rejects_unapproved_export_before_writing_output(export, failure):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    if failure == "expired":
        approval["expiresOnUtc"] = datetime.now(UTC).date().isoformat()
    elif failure == "digest":
        approval["replaySha256"] = "7" * 64
    elif failure == "unclassified":
        approval["qualification"]["provider"]["token"] = PRIVATE
    elif failure == "wrong-purpose":
        approval["purpose"] = "production-capture"
    else:
        approval["qualification"]["model"]["version"] = PRIVATE
    status, result = export(data, approval)
    assert status == 1
    assert not result.out and PRIVATE not in result.err


@pytest.mark.parametrize("data", [b'{"format":1,"format":2}', b"{" + b" " * (128 * 1024 * 32),
                                  json.dumps({"format": "horo-ai-replay-evidence", "version": 2,
                                              "frames": [frame(3, 2), frame(5, 3)]}).encode(),
                                  json.dumps(frame(3, True)).encode()],
                         ids=["duplicate-field", "oversized", "discontinuous", "boolean-outcome"])
def test_rejects_malformed_replay_even_if_approved(export, data):
    status, result = export(data, approval_for(data))
    assert status == 1
    assert not result.out and PRIVATE not in result.err


@pytest.mark.parametrize(("frame_bytes", "status"), [
    (evaluation.MAX_FRAME_BYTES - 1, 0),
    (evaluation.MAX_FRAME_BYTES, 1),
])
def test_approved_replay_window_enforces_the_native_per_frame_byte_limit(export, frame_bytes, status):
    first = frame(3, 2)
    note = {"note": ""}
    first["decision"]["records"] = [note]
    empty_size = len(json.dumps(first, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    pairs, remainder = divmod(frame_bytes - empty_size, 2)
    note["note"] = "\u00e9" * pairs + "x" * remainder
    data = json.dumps({"format": evaluation.REPLAY_FORMAT, "version": 2, "frames": [first, frame(4, 3)]},
                      ensure_ascii=False).encode("utf-8")
    assert len(data) < evaluation.MAX_FRAME_BYTES * 2

    actual, result = export(data, approval_for(data))

    assert actual == status
    if status:
        assert not result.out
        assert result.err == "Replay window contains an oversized frame\n"
    else:
        assert not result.err
        assert json.loads(result.out)["evidence"]["frameCount"] == 2


def test_approved_replay_window_rejects_agent_identity_reused_in_another_scene(export):
    other_scene = dict(frame(4, 3), sceneDefinitionId="448448449")
    data = json.dumps({"format": evaluation.REPLAY_FORMAT, "version": 2,
                       "frames": [frame(3, 2), other_scene]}).encode()

    status, result = export(data, approval_for(data))

    assert status == 1
    assert not result.out
    assert result.err == "Replay window is discontinuous or changed subject\n"


def test_requires_explicit_approval_argument(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", "evidence.json"])
    with pytest.raises(SystemExit) as exit_status:
        evaluation.main()
    result = capsys.readouterr()
    assert exit_status.value.code == 2
    assert not result.out and "--approval" in result.err


def test_source_replay_is_only_available_in_verification_mode(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", "input.json", "--approval", "approval.json",
                                      "--source-replay", "evidence.json"])
    with pytest.raises(SystemExit) as exit_status:
        evaluation.main()
    result = capsys.readouterr()
    assert exit_status.value.code == 2
    assert not result.out and "--source-replay requires --verify-retained" in result.err


def test_read_retained_requires_the_original_replay_argument(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", "manifest.json", "--approval", "approval.json", "--read-retained"])
    with pytest.raises(SystemExit) as exit_status:
        evaluation.main()
    result = capsys.readouterr()
    assert exit_status.value.code == 2
    assert not result.out and "--read-retained requires --source-replay" in result.err


@pytest.mark.parametrize("mode", ["--verify-retained", "--audit-retention"])
@pytest.mark.parametrize("scenario", ["valid", "expired", "extended", "different-replay", "different-provider", "raw-record", "bad-count"])
def test_retention_check_uses_the_original_approval(export, tmp_path, monkeypatch, capsys, mode, scenario):
    data = json.dumps(frame(3, 2)).encode()
    approval = approval_for(data)
    status, result = export(data, approval)
    assert status == 0, result.err
    manifest = json.loads(result.out)
    if scenario == "expired":
        approval["expiresOnUtc"] = manifest["expiresOnUtc"] = datetime.now(UTC).date().isoformat()
    elif scenario == "extended":
        manifest["expiresOnUtc"] = "2099-01-01"
    elif scenario == "different-replay":
        manifest["sourceReplaySha256"] = "7" * 64
    elif scenario == "different-provider":
        manifest["qualification"]["provider"]["version"] = "3"
    elif scenario == "raw-record":
        manifest["evidence"]["frames"][0]["decision"]["rawText"] = PRIVATE
    elif scenario == "bad-count":
        manifest["evidence"]["decisionOutcomes"]["succeeded"] = False

    retained = tmp_path / "retained.json"
    grant = tmp_path / "approval.json"
    retained.write_text(json.dumps(manifest), encoding="utf-8")
    grant.write_text(json.dumps(approval), encoding="utf-8")
    inputs = (retained.read_bytes(), grant.read_bytes())
    monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(retained), "--approval", str(grant), mode])
    audit = mode == "--audit-retention"
    success = scenario == "valid" or (audit and scenario == "expired")
    assert evaluation.main() == (0 if success else 1)
    result = capsys.readouterr()
    if audit and success:
        assert json.loads(result.out) == {
            "schemaVersion": 1,
            "status": "expired" if scenario == "expired" else "current",
            "expiresOnUtc": approval["expiresOnUtc"],
        }
    else:
        assert not result.out
    assert PRIVATE not in result.err
    assert bool(result.err) == (not success)
    assert (retained.read_bytes(), grant.read_bytes()) == inputs


@pytest.mark.parametrize("mode", ["--verify-retained", "--read-retained"])
@pytest.mark.parametrize("scenario", ["valid", "changed-count", "changed-provenance", "wrong-source", "expired", "expired-while-reading", "raw-record"])
def test_source_verification_fences_retained_evidence(export, tmp_path, monkeypatch, capsys, mode, scenario):
    replay = tmp_path / "evidence.json"
    grant = tmp_path / "approval.json"
    original = json.dumps(frame(3, 2)).encode()
    status, result = export(original, approval_for(original))
    assert status == 0, result.err
    manifest = json.loads(result.out)
    if scenario == "changed-count":
        manifest["evidence"]["decisionOutcomes"].update(succeeded=0, failed=1)
    elif scenario == "changed-provenance":
        manifest["evidence"]["frames"][0]["decision"]["planSha256"] = "7" * 64
    elif scenario == "wrong-source":
        replay.write_bytes(json.dumps(frame(3, 3)).encode())
    elif scenario == "expired":
        expired = approval_for(original) | {"expiresOnUtc": datetime.now(UTC).date().isoformat()}
        grant.write_text(json.dumps(expired), encoding="utf-8")
        replay = tmp_path / "missing-replay.json"
    elif scenario == "expired-while-reading":
        expiry = datetime.fromisoformat(manifest["expiresOnUtc"]).replace(tzinfo=UTC)
        times = iter((expiry - timedelta(seconds=1), expiry))

        class Clock:
            @staticmethod
            def now(tz):
                assert tz == UTC
                return next(times)

        monkeypatch.setattr(evaluation, "datetime", Clock)
    elif scenario == "raw-record":
        manifest["evidence"]["frames"][0]["decision"]["rawText"] = PRIVATE
    retained = tmp_path / "manifest.json"
    retained.write_text(json.dumps(manifest), encoding="utf-8")
    retained_bytes = retained.read_bytes()
    monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(retained), "--approval", str(grant),
                                      mode, "--source-replay", str(replay)])
    assert evaluation.main() == (0 if scenario == "valid" else 1)
    result = capsys.readouterr()
    if mode == "--read-retained" and scenario == "valid":
        assert json.loads(result.out) == manifest
        assert PRIVATE not in result.out
    else:
        assert not result.out
    assert PRIVATE not in result.err
    assert retained.read_bytes() == retained_bytes
    if scenario.startswith("changed-"):
        assert "evidence does not match" in result.err
    elif scenario == "wrong-source":
        assert "approved digest" in result.err
    elif scenario in ("expired", "expired-while-reading"):
        assert "approval has expired" in result.err
    elif scenario == "raw-record":
        assert "Unexpected fields" in result.err

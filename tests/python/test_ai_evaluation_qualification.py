"""Exercise approval, provenance and redaction through the offline exporter."""

from datetime import UTC, datetime, timedelta
import hashlib
import json
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
    def run(data, approval):
        replay = tmp_path / "evidence.json"
        grant = tmp_path / "approval.json"
        replay.write_bytes(data)
        grant.write_text(json.dumps(approval), encoding="utf-8")
        monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(replay), "--approval", str(grant)])
        status = evaluation.main()
        return status, capsys.readouterr()

    return run


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


@pytest.mark.parametrize("scenario", ["valid", "changed-count", "changed-provenance", "wrong-source", "expired"])
def test_source_verification_rejects_well_formed_but_unreproducible_evidence(export, tmp_path, monkeypatch, capsys, scenario):
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
    retained = tmp_path / "manifest.json"
    retained.write_text(json.dumps(manifest), encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["qualify_ai_evaluation.py", str(retained), "--approval", str(grant),
                                      "--verify-retained", "--source-replay", str(replay)])
    assert evaluation.main() == (0 if scenario == "valid" else 1)
    result = capsys.readouterr()
    assert not result.out and PRIVATE not in result.err
    if scenario.startswith("changed-"):
        assert "evidence does not match" in result.err
    elif scenario == "wrong-source":
        assert "approved digest" in result.err
    elif scenario == "expired":
        assert "approval has expired" in result.err

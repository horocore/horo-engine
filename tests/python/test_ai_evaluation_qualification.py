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

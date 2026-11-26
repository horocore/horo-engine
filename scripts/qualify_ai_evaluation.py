"""Export or verify a redacted evaluation manifest under an explicit offline approval."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import UTC, date, datetime
import hashlib
import json
from pathlib import Path
import re
import sys


REPLAY_FORMAT = "horo-ai-replay-evidence"
MAX_FRAME_BYTES = 128 * 1024
MAX_FRAMES = 32
MAX_APPROVAL_BYTES = 4096
MAX_MANIFEST_BYTES = 32 * 1024
MANIFEST_FORMAT = "horo-ai-evaluation-manifest"
APPROVAL_PURPOSE = "offline-evaluation"
QUALIFICATION_SOURCE = "operator-declared"
OUTCOMES = ("idle", "running", "succeeded", "failed", "cancelled")
DIGEST = re.compile(r"[0-9a-f]{64}")
VERSION = re.compile(r"[0-9]{1,9}(?:\.[0-9]{1,9}){0,3}")
DECIMAL_ID = re.compile(r"[1-9][0-9]{0,19}")


# Validation errors contain only diagnostics approved for display.
class _EvidenceError(ValueError):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise _EvidenceError(reason)


def unique_members(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON field")
        result[key] = value
    return result


def read_bounded(path: Path, limit: int) -> bytes:
    require(path.is_file(), "Input must be a regular file")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(0 < len(data) <= limit, "Input exceeds the size limit or is empty")
    return data


def decode(data: bytes) -> object:
    def reject_constant(_value: str) -> None:
        raise _EvidenceError("Nonfinite JSON number")

    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_members, parse_constant=reject_constant)


def exact_fields(value: object, names: set[str], label: str) -> dict:
    require(isinstance(value, dict) and value.keys() == names, f"Unexpected fields in {label}")
    return value


def digest(value: object) -> str:
    require(isinstance(value, str) and DIGEST.fullmatch(value) is not None and value != "0" * 64, "Invalid content digest")
    return value


def version(value: object) -> str:
    require(isinstance(value, str) and VERSION.fullmatch(value) is not None, "Invalid qualification version")
    return value


def schema(value: object) -> int:
    require(isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= 0xFFFFFFFF, "Invalid schema version")
    return value


def identity(value: object) -> int:
    require(isinstance(value, str) and DECIMAL_ID.fullmatch(value) is not None, "Invalid replay identity")
    number = int(value)
    require(number <= 0xFFFFFFFFFFFFFFFF, "Replay identity exceeds 64 bits")
    return number


def qualified_component(value: object) -> dict:
    record = exact_fields(value, {"version", "sha256"}, "qualification component")
    return {"version": version(record["version"]), "sha256": digest(record["sha256"])}


def qualification_tuple(value: object) -> dict:
    record = exact_fields(value, {"provider", "model", "platform", "workload"}, "qualification tuple")
    platform = exact_fields(record["platform"], {"os", "architecture", "version"}, "qualification platform")
    require(platform["os"] in ("windows", "linux", "macos", "android"), "Invalid qualification OS")
    require(platform["architecture"] in ("x64", "arm64"), "Invalid qualification architecture")
    return {
        "provider": qualified_component(record["provider"]),
        "model": None if record["model"] is None else qualified_component(record["model"]),
        "platform": {"os": platform["os"], "architecture": platform["architecture"], "version": version(platform["version"])},
        "workload": qualified_component(record["workload"]),
    }


def validate_approval(value: object, replay_digest: str, today: date) -> dict:
    record = exact_fields(value, {"schemaVersion", "purpose", "replaySha256", "expiresOnUtc", "qualification"}, "export approval")
    require(schema(record["schemaVersion"]) == 1 and record["purpose"] == APPROVAL_PURPOSE, "Unsupported export approval")
    require(digest(record["replaySha256"]) == replay_digest, "Approval does not match this replay")
    expiry = record["expiresOnUtc"]
    require(isinstance(expiry, str) and len(expiry) == 10, "Invalid approval expiry")
    expires_on = date.fromisoformat(expiry)
    require(expires_on.isoformat() == expiry and today < expires_on, "Export approval has expired or is invalid")
    return {"expiresOnUtc": expiry, "qualification": qualification_tuple(record["qualification"])}


def object_field(value: object, name: str) -> dict:
    require(isinstance(value, dict), "Invalid replay object")
    child = value.get(name)
    require(isinstance(child, dict), f"Missing or invalid replay {name}")
    return child


def replay_frames(value: object, bytes_read: int) -> list[dict]:
    require(isinstance(value, dict) and value.get("format") == REPLAY_FORMAT, "Unsupported replay format")
    wire_version = schema(value.get("version"))
    if wire_version == 1:
        require(bytes_read <= MAX_FRAME_BYTES, "Single replay frame exceeds the size limit")
        return [value]
    require(wire_version == 2 and value.keys() == {"format", "version", "frames"}, "Unsupported replay window")
    frames = value["frames"]
    require(isinstance(frames, list) and 2 <= len(frames) <= MAX_FRAMES, "Invalid replay window length")
    return frames


def frame_provenance(frame: dict) -> dict:
    scene = object_field(frame, "sceneInput")
    canonical = object_field(frame, "canonicalAi")
    decision = object_field(frame, "decision")
    blackboard = object_field(decision, "blackboard")
    return {
        "sceneInput": {"schemaVersion": schema(scene.get("schemaVersion")), "sha256": digest(scene.get("digest"))},
        "canonicalAi": {"recordVersion": schema(canonical.get("recordVersion")), "sha256": digest(canonical.get("digest"))},
        "decision": {"traceVersion": schema(decision.get("traceVersion")), "planSha256": digest(decision.get("planDigest"))},
        "blackboard": {"schemaVersion": schema(blackboard.get("schemaVersion"))},
    }


def summarize_replay(frames: list[dict]) -> dict:
    outcomes = Counter()
    provenance = []
    last_tick = None
    subject = None
    for frame in frames:
        require(isinstance(frame, dict) and frame.get("format") == REPLAY_FORMAT, "Invalid replay frame")
        require(schema(frame.get("version")) == 1, "Unsupported replay frame version")
        tick = identity(frame.get("tick"))
        agent = identity(frame.get("agentId"))
        if last_tick is not None:
            require(tick == last_tick + 1 and agent == subject, "Replay window is discontinuous or changed subject")
        last_tick, subject = tick, agent
        outcome = object_field(frame, "decision").get("outcome")
        require(isinstance(outcome, int) and not isinstance(outcome, bool) and 0 <= outcome < len(OUTCOMES), "Invalid decision outcome")
        outcomes[OUTCOMES[outcome]] += 1
        provenance.append(frame_provenance(frame))
    return {"frameCount": len(frames), "decisionOutcomes": {name: outcomes[name] for name in OUTCOMES}, "frames": provenance}


def validate_retained_frame(value: object) -> None:
    fields = {
        "sceneInput": ("schemaVersion", "sha256"),
        "canonicalAi": ("recordVersion", "sha256"),
        "decision": ("traceVersion", "planSha256"),
        "blackboard": ("schemaVersion",),
    }
    frame = exact_fields(value, set(fields), "manifest frame")
    for name, (version_field, *hash_fields) in fields.items():
        record = exact_fields(frame[name], {version_field, *hash_fields}, "manifest provenance")
        schema(record[version_field])
        for hash_field in hash_fields:
            digest(record[hash_field])


def validate_retained_evidence(value: object) -> None:
    evidence = exact_fields(value, {"frameCount", "decisionOutcomes", "frames"}, "manifest evidence")
    count = schema(evidence["frameCount"])
    frames = evidence["frames"]
    require(isinstance(frames, list) and len(frames) == count <= MAX_FRAMES, "Invalid manifest frame count")
    counts = exact_fields(evidence["decisionOutcomes"], set(OUTCOMES), "manifest decision counts")
    require(all(type(total) is int and 0 <= total <= count for total in counts.values()), "Invalid manifest decision count")
    require(sum(counts.values()) == count, "Manifest decisions do not match frame count")
    for frame in frames:
        validate_retained_frame(frame)


def validate_retained_manifest(value: object, approval: object, today: date) -> None:
    record = exact_fields(value, {"format", "schemaVersion", "purpose", "qualificationSource", "sourceReplaySha256",
                                  "expiresOnUtc", "qualification", "evidence"}, "evaluation manifest")
    require(record["format"] == MANIFEST_FORMAT and schema(record["schemaVersion"]) == 1, "Unsupported evaluation manifest")
    require(record["purpose"] == APPROVAL_PURPOSE and record["qualificationSource"] == QUALIFICATION_SOURCE,
            "Unsupported manifest purpose or qualification source")
    allowed = validate_approval(approval, digest(record["sourceReplaySha256"]), today)
    require(record["expiresOnUtc"] == allowed["expiresOnUtc"] and qualification_tuple(record["qualification"]) == allowed["qualification"],
            "Manifest does not match its approval")
    validate_retained_evidence(record["evidence"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Existing headless evidence.json or retained evaluation manifest")
    parser.add_argument("--approval", required=True, type=Path, help="Explicit, digest-bound offline export approval")
    parser.add_argument("--verify-retained", action="store_true", help="Check a stored manifest against its original approval and UTC expiry")
    args = parser.parse_args()
    try:
        approval = decode(read_bounded(args.approval, MAX_APPROVAL_BYTES))
        today = datetime.now(UTC).date()
        if args.verify_retained:
            validate_retained_manifest(decode(read_bounded(args.input, MAX_MANIFEST_BYTES)), approval, today)
            return 0
        data = read_bounded(args.input, MAX_FRAME_BYTES * MAX_FRAMES)
        replay_digest = hashlib.sha256(data).hexdigest()
        allowed = validate_approval(approval, replay_digest, today)
        evidence = summarize_replay(replay_frames(decode(data), len(data)))
    except _EvidenceError as error:
        print(error, file=sys.stderr)
        return 1
    except (OSError, ValueError, RecursionError):
        print("AI evaluation input could not be read as bounded JSON", file=sys.stderr)
        return 1
    manifest = {"format": MANIFEST_FORMAT, "schemaVersion": 1, "purpose": APPROVAL_PURPOSE,
                "qualificationSource": QUALIFICATION_SOURCE, "sourceReplaySha256": replay_digest, **allowed, "evidence": evidence}
    print(json.dumps(manifest, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

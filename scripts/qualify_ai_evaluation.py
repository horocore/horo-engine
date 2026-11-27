"""Export, verify or retire evaluation evidence under an explicit offline approval."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import UTC, date, datetime
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile


REPLAY_FORMAT = "horo-ai-replay-evidence"
MAX_FRAME_BYTES = 128 * 1024
MAX_FRAMES = 32
MAX_APPROVAL_BYTES = 4096
MAX_MANIFEST_BYTES = 32 * 1024
MAX_QUALIFICATION_ARTIFACT_BYTES = 512 * 1024 * 1024
MANIFEST_FORMAT = "horo-ai-evaluation-manifest"
REPLAY_COMPARISON_FORMAT = "horo-ai-evaluation-replay-comparison"
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
    before = path.stat()
    require(stat.S_ISREG(before.st_mode), "Input must be a regular file")
    require(0 < before.st_size <= limit, "Input exceeds the size limit or is empty")
    expected = file_identity(before)
    with open(path, "rb", opener=open_nonblocking) as stream:
        require(same_regular_file(os.fstat(stream.fileno()), expected), "Input changed before reading")
        data = stream.read(limit + 1)
        require(same_regular_file(os.fstat(stream.fileno()), expected), "Input changed while reading")
    require(len(data) == before.st_size and same_regular_file(path.stat(), expected), "Input changed while reading")
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


def validate_approval(value: object, replay_digest: str, today: date | None) -> dict:
    record = exact_fields(value, {"schemaVersion", "purpose", "replaySha256", "expiresOnUtc", "qualification"}, "export approval")
    require(schema(record["schemaVersion"]) == 1 and record["purpose"] == APPROVAL_PURPOSE, "Unsupported export approval")
    require(digest(record["replaySha256"]) == replay_digest, "Approval does not match this replay")
    expiry = record["expiresOnUtc"]
    require(isinstance(expiry, str) and len(expiry) == 10, "Invalid approval expiry")
    expires_on = date.fromisoformat(expiry)
    require(expires_on.isoformat() == expiry, "Invalid approval expiry")
    require(today is None or today < expires_on, "Export approval has expired")
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
    for frame in frames:
        compact = json.dumps(frame, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
        # Native evidence reserves one byte for the trailing newline.
        require(len(compact.encode("utf-8")) < MAX_FRAME_BYTES, "Replay window contains an oversized frame")
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
        frame_subject = (identity(frame.get("sceneDefinitionId")), identity(frame.get("agentId")))
        if last_tick is not None:
            require(tick == last_tick + 1 and frame_subject == subject, "Replay window is discontinuous or changed subject")
        last_tick, subject = tick, frame_subject
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


def validate_retained_manifest(value: object, approval: object, today: date | None) -> dict:
    record = exact_fields(value, {"format", "schemaVersion", "purpose", "qualificationSource", "sourceReplaySha256",
                                  "expiresOnUtc", "qualification", "evidence"}, "evaluation manifest")
    require(record["format"] == MANIFEST_FORMAT and schema(record["schemaVersion"]) == 1, "Unsupported evaluation manifest")
    require(record["purpose"] == APPROVAL_PURPOSE and record["qualificationSource"] == QUALIFICATION_SOURCE,
            "Unsupported manifest purpose or qualification source")
    allowed = validate_approval(approval, digest(record["sourceReplaySha256"]), today)
    require(record["expiresOnUtc"] == allowed["expiresOnUtc"] and qualification_tuple(record["qualification"]) == allowed["qualification"],
            "Manifest does not match its approval")
    validate_retained_evidence(record["evidence"])
    return record


def audit_retention(path: Path, approval: object, approval_path: Path) -> dict:
    data = read_bounded(path, MAX_MANIFEST_BYTES)
    manifest = validate_retained_manifest(decode(data), approval, None)
    require_manifest_approval(approval_path, manifest, operation="Retention audit")
    require(read_bounded(path, MAX_MANIFEST_BYTES) == data, "Retention audit manifest changed after validation")
    expiry = manifest["expiresOnUtc"]
    status = "expired" if datetime.now(UTC).date() >= date.fromisoformat(expiry) else "current"
    return {"schemaVersion": 1, "status": status, "expiresOnUtc": expiry}


def read_replay(path: Path) -> tuple[bytes, str]:
    data = read_bounded(path, MAX_FRAME_BYTES * MAX_FRAMES)
    return data, hashlib.sha256(data).hexdigest()


def summarize_source(data: bytes) -> dict:
    return summarize_replay(replay_frames(decode(data), len(data)))


def verify_source_evidence(record: dict, path: Path) -> None:
    data, source_digest = read_replay(path)
    verify_replay_bytes(record, data, source_digest)


def verify_replay_bytes(record: dict, data: bytes, source_digest: str) -> None:
    require(source_digest == record["sourceReplaySha256"], "Source replay does not match the approved digest")
    require(summarize_source(data) == record["evidence"], "Manifest evidence does not match the approved source replay")


def is_plain_file(info: os.stat_result) -> bool:
    reparse = getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    return stat.S_ISREG(info.st_mode) and not reparse


def file_identity(info: os.stat_result) -> tuple[int, ...]:
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def same_regular_file(info: os.stat_result, expected: tuple[int, ...]) -> bool:
    return stat.S_ISREG(info.st_mode) and file_identity(info) == expected


def open_nonblocking(name: str, flags: int) -> int:
    return os.open(name, flags | getattr(os, "O_NONBLOCK", 0))


def retention_identity(info: os.stat_result) -> tuple[int, ...]:
    require(is_plain_file(info) and info.st_nlink == 1, "Retention requires independent regular files")
    return file_identity(info)


def read_retention_file(path: Path, limit: int) -> tuple[bytes, tuple[int, ...]]:
    identity = retention_identity(path.lstat())
    with open(path, "rb", opener=open_nonblocking) as stream:
        require(retention_identity(os.fstat(stream.fileno())) == identity, "Retention file changed before reading")
        data = stream.read(limit + 1)
        require(retention_identity(os.fstat(stream.fileno())) == identity, "Retention file changed while reading")
    require(0 < len(data) <= limit, "Retention input exceeds the size limit or is empty")
    return data, identity


def require_retention_identity(path: Path, expected: tuple[int, ...]) -> None:
    require(retention_identity(path.lstat()) == expected, "Retention file changed after validation")


def remove_retention_file(path: Path, expected: tuple[int, ...]) -> None:
    require_retention_identity(path, expected)
    try:
        path.unlink()
    except OSError:
        raise _EvidenceError("A verified evaluation file could not be removed; retry the purge") from None


def purge_expired_manifest(path: Path, replay: Path, approval: object, today: date, approval_path: Path) -> dict:
    """Remove an expired pair from the storage owner's exclusive directory."""
    directory = path.parent.resolve(strict=True)
    require(replay.parent.resolve(strict=True) == directory, "Retention inputs must share one storage directory")
    path, replay = directory / path.name, directory / replay.name
    require(path != replay, "Retention requires distinct manifest and replay paths")
    raw, manifest_identity = read_retention_file(path, MAX_MANIFEST_BYTES)
    record = validate_retained_manifest(decode(raw), approval, None)
    expiry = record["expiresOnUtc"]
    require(date.fromisoformat(expiry) <= today, "Evaluation retention is still current")
    replay_identity = None
    try:
        replay.lstat()
    except FileNotFoundError:
        # The source may already have been removed by an earlier, interrupted purge.
        pass
    else:
        data, replay_identity = read_retention_file(replay, MAX_FRAME_BYTES * MAX_FRAMES)
        verify_replay_bytes(record, data, hashlib.sha256(data).hexdigest())
    require_manifest_approval(approval_path, record, operation="Retention purge")
    require_retention_identity(path, manifest_identity)
    if replay_identity is not None:
        remove_retention_file(replay, replay_identity)
        require_manifest_approval(approval_path, record, operation="Retention purge")
    remove_retention_file(path, manifest_identity)
    return {"schemaVersion": 1, "status": "purged", "expiresOnUtc": expiry, "sourceRemoved": replay_identity is not None}


def artifact_identity(info: os.stat_result, label: str) -> tuple[int, ...]:
    require(is_plain_file(info), f"{label} is missing or is not a regular file")
    return file_identity(info)


def hash_qualification_artifact(path: Path, label: str) -> str:
    try:
        before = path.lstat()
        identity = artifact_identity(before, label)
        require(0 < before.st_size <= MAX_QUALIFICATION_ARTIFACT_BYTES, f"{label} exceeds the size limit or is empty")
        inspection_flags = getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
        observed = hashlib.sha256()
        size = 0
        with open(path, "rb", opener=lambda name, flags: os.open(name, flags | inspection_flags)) as stream:
            require(artifact_identity(os.fstat(stream.fileno()), label) == identity, f"{label} changed before reading")
            while chunk := stream.read(1024 * 1024):
                size += len(chunk)
                require(size <= MAX_QUALIFICATION_ARTIFACT_BYTES, f"{label} exceeds the size limit")
                observed.update(chunk)
            require(artifact_identity(os.fstat(stream.fileno()), label) == identity, f"{label} changed while reading")
        require(size == before.st_size and artifact_identity(path.lstat(), label) == identity, f"{label} changed while reading")
        return observed.hexdigest()
    except OSError:
        raise _EvidenceError(f"{label} could not be read as a regular file") from None


def verify_qualification_artifacts(qualification: dict, root: Path) -> None:
    """Check the approved provider, optional model and workload bytes without exporting them."""
    for name in ("provider", "model", "workload"):
        component = qualification[name]
        if component is None:
            continue
        expected = component["sha256"]
        label = f"Qualification {name} artifact"
        require(hash_qualification_artifact(root / expected, label) == expected, f"{label} does not match its approved digest")


def json_line(value: dict) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"


def emit_json(value: dict) -> None:
    sys.stdout.write(json_line(value))


def require_current_expiry(expiry: str) -> None:
    require(datetime.now(UTC).date() < date.fromisoformat(expiry), "Export approval has expired")


def approved_replay_digest(path: Path, approval: object, today: date) -> tuple[str, dict]:
    data, source = read_replay(path)
    allowed = validate_approval(approval, source, today)
    summarize_source(data)
    return source, allowed


def require_same_replay_approval(path: Path, replay_digest: str, approved: dict, *, operation: str = "Evaluation") -> None:
    """Recheck one approval without exposing private input or storage paths."""
    reason = f"{operation} approval is no longer current"
    try:
        current = validate_approval(decode(read_bounded(path, MAX_APPROVAL_BYTES)), replay_digest, None)
    except (OSError, ValueError, RecursionError):
        raise _EvidenceError(reason) from None
    require(current == approved, reason)


def require_manifest_approval(path: Path, manifest: dict, *, operation: str = "Evaluation") -> None:
    """Recheck that the original grant still covers the manifest, including during retention."""
    allowed = {key: manifest[key] for key in ("expiresOnUtc", "qualification")}
    require_same_replay_approval(path, manifest["sourceReplaySha256"], allowed, operation=operation)


def require_current_manifest_approval(path: Path, manifest: dict) -> None:
    require_manifest_approval(path, manifest)
    require_current_expiry(manifest["expiresOnUtc"])


def require_current_comparison(approvals: tuple[tuple[Path, str, dict], ...], expiry: str) -> None:
    for path, replay_digest, approved in approvals:
        require_same_replay_approval(path, replay_digest, approved, operation="Replay comparison")
    require_current_expiry(expiry)


def native_replay_status(engine: Path, inputs: tuple[Path, Path], digests: tuple[str, str]) -> int:
    try:
        result = subprocess.run(
            [str(engine.resolve(strict=True)), "--compare-ai-replay", *(str(path.absolute()) for path in inputs),
             "--approved-digests", *(f"sha256:{value}" for value in digests)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30, check=False, text=True,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise _EvidenceError("Approved replay comparison could not be completed") from None
    expected = {0: "approved-replay-v1 matched\n", 1: "approved-replay-v1 diverged\n"}
    require(result.returncode in expected and result.stdout == expected[result.returncode],
            "Approved replay comparison could not be completed")
    return result.returncode


def compare_approved_replays(args: argparse.Namespace, approval: object, today: date) -> int:
    """Publish a redacted comparison of two approved recordings under the same qualification tuple."""
    inputs = (args.input, args.compare_replay)
    require(not inputs[0].samefile(inputs[1]), "Replay comparison requires two distinct files")
    comparison_approval = decode(read_bounded(args.comparison_approval, MAX_APPROVAL_BYTES))
    left, recorded = approved_replay_digest(inputs[0], approval, today)
    right, replayed = approved_replay_digest(inputs[1], comparison_approval, today)
    qualification = recorded["qualification"]
    require(qualification == replayed["qualification"], "Replay approvals use different qualification tuples")
    if args.qualification_artifacts is not None:
        verify_qualification_artifacts(qualification, args.qualification_artifacts)
    expiry = min(recorded["expiresOnUtc"], replayed["expiresOnUtc"])
    approvals = ((args.approval, left, recorded), (args.comparison_approval, right, replayed))
    require_current_comparison(approvals, expiry)
    result = native_replay_status(args.engine, inputs, (left, right))
    require_current_comparison(approvals, expiry)
    emit_json({"format": REPLAY_COMPARISON_FORMAT, "schemaVersion": 1,
               "status": "matched" if result == 0 else "diverged", "expiresOnUtc": expiry,
               "qualificationSource": QUALIFICATION_SOURCE, "qualification": qualification,
               "recordedReplaySha256": left, "replayedReplaySha256": right})
    return result


def publish_manifest(path: Path, manifest: dict, approval: Path) -> None:
    """Publish a complete new manifest while its approval is current."""
    data = json_line(manifest).encode("utf-8")
    try:
        with tempfile.TemporaryDirectory(prefix=".horo-ai-evaluation-", dir=path.parent) as scratch:
            staged = Path(scratch) / "manifest.json"
            with staged.open("xb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            require_current_manifest_approval(approval, manifest)
            os.link(staged, path)
    except FileExistsError:
        raise _EvidenceError("Evaluation output already exists") from None
    except OSError:
        raise _EvidenceError("Evaluation output could not be published") from None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Existing headless evidence.json or retained evaluation manifest")
    parser.add_argument("--approval", required=True, type=Path, help="Explicit, digest-bound offline export approval")
    retained = parser.add_mutually_exclusive_group()
    retained.add_argument("--verify-retained", action="store_true", help="Check a stored manifest against its original approval and UTC expiry")
    retained.add_argument("--read-retained", action="store_true", help="Emit a current stored manifest after reproducing its approved evidence")
    retained.add_argument("--audit-retention", action="store_true", help="Report the UTC retention status of a stored manifest and its approval")
    retained.add_argument("--purge-expired", action="store_true", help="Remove an expired manifest and source from owner-exclusive storage")
    retained.add_argument("--compare-replay", type=Path, help="Compare another approved recording under the same qualification tuple")
    parser.add_argument("--comparison-approval", type=Path, help="Current approval for the second recording")
    parser.add_argument("--engine", type=Path, help="Trusted horo-engine executable for an approved replay comparison")
    parser.add_argument("--source-replay", type=Path, help="Reproduce the evidence from the original approved replay")
    parser.add_argument("--qualification-artifacts", type=Path, help="Check approved provider, model and workload files named by SHA-256")
    parser.add_argument("--output", type=Path, help="Publish the export to a new manifest file")
    args = parser.parse_args()
    comparison_options = (args.comparison_approval, args.engine)
    if args.compare_replay is not None:
        if None in comparison_options:
            parser.error("--compare-replay requires --comparison-approval and --engine")
    elif any(option is not None for option in comparison_options):
        parser.error("--comparison-approval and --engine require --compare-replay")
    if args.read_retained and args.source_replay is None:
        parser.error("--read-retained requires --source-replay")
    if args.purge_expired and args.source_replay is None:
        parser.error("--purge-expired requires --source-replay")
    if args.source_replay is not None and not (args.verify_retained or args.read_retained or args.purge_expired):
        parser.error("--source-replay requires --verify-retained, --read-retained or --purge-expired")
    if args.output is not None and (args.verify_retained or args.read_retained or args.audit_retention or args.purge_expired
                                    or args.compare_replay is not None):
        parser.error("--output is only available when exporting a replay")
    if args.qualification_artifacts is not None:
        if args.audit_retention or args.purge_expired:
            parser.error("--qualification-artifacts cannot be used for retention operations")
        if args.verify_retained and args.source_replay is None:
            parser.error("--qualification-artifacts requires --source-replay when verifying a retained manifest")
    try:
        approval = decode(read_bounded(args.approval, MAX_APPROVAL_BYTES))
        if args.audit_retention:
            emit_json(audit_retention(args.input, approval, args.approval))
            return 0
        today = datetime.now(UTC).date()
        if args.compare_replay is not None:
            return compare_approved_replays(args, approval, today)
        if args.purge_expired:
            emit_json(purge_expired_manifest(args.input, args.source_replay, approval, today, args.approval))
            return 0
        if args.verify_retained or args.read_retained:
            record = validate_retained_manifest(decode(read_bounded(args.input, MAX_MANIFEST_BYTES)), approval, today)
            if args.source_replay is not None:
                verify_source_evidence(record, args.source_replay)
                if args.qualification_artifacts is not None:
                    verify_qualification_artifacts(record["qualification"], args.qualification_artifacts)
            require_current_manifest_approval(args.approval, record)
            if args.read_retained:
                emit_json(record)
            return 0
        data, replay_digest = read_replay(args.input)
        allowed = validate_approval(approval, replay_digest, today)
        evidence = summarize_source(data)
        if args.qualification_artifacts is not None:
            verify_qualification_artifacts(allowed["qualification"], args.qualification_artifacts)
        manifest = {"format": MANIFEST_FORMAT, "schemaVersion": 1, "purpose": APPROVAL_PURPOSE,
                    "qualificationSource": QUALIFICATION_SOURCE, "sourceReplaySha256": replay_digest, **allowed, "evidence": evidence}
        if args.output is None:
            require_current_manifest_approval(args.approval, manifest)
            emit_json(manifest)
        else:
            publish_manifest(args.output, manifest, args.approval)
    except _EvidenceError as error:
        print(error, file=sys.stderr)
        return 1
    except (OSError, ValueError, RecursionError):
        print("AI evaluation input could not be read as bounded JSON", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Verify an XR 1.0 matrix against the declared release evidence."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from qualify_xr_android import check_android_preflight_file, validate_android_preflight
from qualify_xr_device import (
    CATEGORIES, CORE_CHECKS, DESKTOP_HOSTS, DESKTOP_RENDERER, LEVELS, MAX_DESKTOP_PREFLIGHT_BYTES, MAX_RECORD_BYTES, OPTIONAL_CHECKS, STATUSES,
    check_desktop_preflight, check_display_capture, check_physical_gate, decode_record, desktop_receipt_digest, fields, in_set, is_digest, is_token,
    is_source_revision, load_record, require, validate_declaration, validate_record, verify_artifact,
)


REVIEWS = {"foundation", "backend", "input", "renderer", "interaction"}
MAX_TUPLES = 64
MAX_TIMING_WINDOW_NS = 86_400_000_000_000
MAX_TIMING_FRAMES = 10_000_000
PRIMARY_STEREO_VIEW_COUNT = 2
SPECIALIZED_PHYSICAL_CHECKS = {"displayEnumeration", "renderTiming", "optionalMR"}
TIMING_LIMITS = {
    "minimumDurationNs": (1, MAX_TIMING_WINDOW_NS),
    "minimumFrameCount": (1, MAX_TIMING_FRAMES),
    "maximumPeakFrameIntervalNs": (1, MAX_TIMING_WINDOW_NS),
    "maximumMissedDeadlines": (0, MAX_TIMING_FRAMES),
    "maximumSwapchainFailures": (0, MAX_TIMING_FRAMES),
}


class Evidence:
    """Verifies the local content-addressed artifacts required by the matrix."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.verified: set[str] = set()

    def path(self, digest: str) -> Path:
        require(is_digest(digest), "Missing or invalid artifact digest")
        path = self.root / digest
        require(path.is_file() and not path.is_symlink(), f"Missing regular artifact {digest}")
        return path

    def verify(self, digest: str) -> None:
        require(is_digest(digest), "Missing or invalid artifact digest")
        if digest in self.verified:
            return
        verify_artifact(self.path(digest), digest)
        self.verified.add(digest)

    def desktop_preflight(self, digest: str, identity: dict, component_digest: str | None) -> None:
        with self.path(digest).open("rb") as source:
            view_count = check_desktop_preflight(source.read(MAX_DESKTOP_PREFLIGHT_BYTES + 1), identity, digest, component_digest)
        if view_count is not None:
            require(view_count == PRIMARY_STEREO_VIEW_COUNT, "Desktop preflight must report primary stereo for XR 1.0")

    def android_preflight(self, expected: dict, record: dict) -> None:
        self.verify(expected["apkSha256"])
        check_android_preflight_file(self.path(expected["receiptSha256"]), expected, record)

    def smoke_record(self, digest: str) -> dict:
        with self.path(digest).open("rb") as source:
            data = source.read(MAX_RECORD_BYTES + 1)
        require(hashlib.sha256(data).hexdigest() == digest, f"Smoke record content does not match {digest}")
        return validate_record(decode_record(data))

    def display_capture(self, record: dict) -> None:
        with self.path(record["display"]["evidenceSha256"]).open("rb") as source:
            check_display_capture(source.read(MAX_RECORD_BYTES + 1), record)

    def render_timing(self, record: dict, limits: dict) -> None:
        digest = record["checks"]["renderTiming"]["evidenceSha256"]
        with self.path(digest).open("rb") as source:
            check_render_timing(source.read(MAX_RECORD_BYTES + 1), digest, record, limits)

    def mixed_reality_capture(self, record: dict) -> bytes:
        with self.path(record["checks"]["optionalMR"]["evidenceSha256"]).open("rb") as source:
            return source.read(MAX_RECORD_BYTES + 1)

    def physical_check_capture(self, record: dict, check: str) -> str:
        """Verify the run-bound receipt and its redacted underlying artifact."""
        digest = record["checks"][check]["evidenceSha256"]
        with self.path(digest).open("rb") as source:
            data = source.read(MAX_RECORD_BYTES + 1)
        require(len(data) <= MAX_RECORD_BYTES, f"{check} capture exceeds the size limit")
        require(hashlib.sha256(data).hexdigest() == digest, f"{check} capture digest does not match")
        capture = fields(decode_record(data), {"schemaVersion", "evidenceLevel", "tuple", "provenance", "check", "artifactSha256"},
                         f"{check} capture")
        require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1, f"Unsupported {check} capture schema")
        require(capture["evidenceLevel"] == record["evidenceLevel"], f"{check} capture must identify physical evidence")
        require(capture["check"] == check, f"{check} capture is for a different check")
        require(capture["tuple"] == record["tuple"], f"{check} capture is from a different product tuple")
        require(capture["provenance"] == record["provenance"], f"{check} capture is from a different smoke run")
        artifact = capture["artifactSha256"]
        self.verify(artifact)
        return artifact


def validate_preflight_declarations(row: dict) -> None:
    preflight = row.get("desktopPreflightSha256")
    require(preflight is None or is_digest(preflight), "Invalid Desktop preflight digest")
    component = row.get("desktopComponentSha256")
    require(component is None or is_digest(component), "Invalid Desktop component digest")
    if "androidPreflight" in row:
        validate_android_preflight(row["androidPreflight"])


def validate_timing_limits(value: object) -> None:
    limits = fields(value, set(TIMING_LIMITS), "render timing limits")
    require(all(type(limits[name]) is int and lower <= limits[name] <= upper
                for name, (lower, upper) in TIMING_LIMITS.items()), "Invalid render timing limits")


def check_render_timing(data: bytes, digest: str, record: dict, limits: dict) -> None:
    """Verify a bounded timing observation from the exact physical run against its release limits."""
    require(len(data) <= MAX_RECORD_BYTES, "Render timing capture exceeds the size limit")
    require(hashlib.sha256(data).hexdigest() == digest, "Render timing capture digest does not match")
    capture = fields(decode_record(data), {"schemaVersion", "tuple", "provenance", "targetRefreshMilliHertz",
                                           "durationNs", "frameCount", "peakFrameIntervalNs", "missedDeadlines",
                                           "swapchainFailures"}, "render timing capture")
    require(type(capture["schemaVersion"]) is int and capture["schemaVersion"] == 1, "Unsupported render timing capture schema")
    require(capture["tuple"] == record["tuple"], "Render timing capture is from a different product tuple")
    require(capture["provenance"] == record["provenance"], "Render timing capture is from a different smoke run")
    refresh = capture["targetRefreshMilliHertz"]
    require(type(refresh) is int and refresh in record["display"]["refreshMilliHertz"], "Render timing target was not enumerated")
    duration, frames, peak, missed, failures = (capture[name] for name in (
        "durationNs", "frameCount", "peakFrameIntervalNs", "missedDeadlines", "swapchainFailures"))
    require(all(type(value) is int for value in (duration, frames, peak, missed, failures)) and
            0 < peak <= duration <= MAX_TIMING_WINDOW_NS and
            0 < frames <= MAX_TIMING_FRAMES and 0 <= missed <= frames and 0 <= failures <= frames,
            "Invalid render timing measurements")
    require(peak + frames - 1 <= duration <= frames * peak, "Render timing interval totals are inconsistent")
    require(duration >= limits["minimumDurationNs"] and frames >= limits["minimumFrameCount"],
            "Render timing observation did not meet the required duration and frame count")
    require(peak <= limits["maximumPeakFrameIntervalNs"] and missed <= limits["maximumMissedDeadlines"] and
            failures <= limits["maximumSwapchainFailures"], "Render timing observation exceeded the declared limits")


def validate_row(value: object) -> dict:
    required = {"id", "category", "owner", "releaseClaim", "declaration", "requiredOptionalChecks", "recordSha256"}
    if isinstance(value, dict):
        required |= value.keys() & {"desktopPreflightSha256", "desktopComponentSha256", "androidPreflight", "renderTimingLimits"}
    row = fields(value, required, "matrix row")
    require(is_token(row["id"]) and is_token(row["owner"]), "Each matrix row requires an ID and owner")
    require(in_set(row["category"], CATEGORIES), "Invalid matrix planning category")
    require(type(row["releaseClaim"]) is bool, "Release claim must be explicit")
    if row["releaseClaim"]:
        require(row["category"] not in {"Conditional", "Future"}, "Conditional and Future rows are planning only")
        require(row["declaration"] is not None, "Claimed tuple requires a release declaration")
        require("renderTimingLimits" in row, "Claimed tuple requires render timing limits")
    if "renderTimingLimits" in row:
        validate_timing_limits(row["renderTimingLimits"])
    optional = row["requiredOptionalChecks"]
    require(isinstance(optional, list) and all(in_set(name, OPTIONAL_CHECKS) for name in optional),
            "Invalid optional physical checks")
    require(len(optional) == len(set(optional)), "Duplicate optional physical checks")
    if row["declaration"] is not None:
        validate_declaration(row["declaration"], set(optional))
    require(row["recordSha256"] is None or is_digest(row["recordSha256"]), "Invalid smoke record digest")
    validate_preflight_declarations(row)
    return row


def verify_review(value: object, evidence: Evidence, revision: str) -> None:
    review = fields(value, {"owner", "status", "sourceRevision", "evidenceLevel", "artifactSha256"}, "subsystem review")
    require(is_token(review["owner"]), "Subsystem review needs an owner")
    require(in_set(review["status"], STATUSES), "Invalid subsystem review status")
    require(review["status"] == "Passed", f"Subsystem review {review['status']}")
    require(review["sourceRevision"] == revision, "Subsystem review is from another source revision")
    require(in_set(review["evidenceLevel"], LEVELS), "Invalid subsystem evidence level")
    evidence.verify(review["artifactSha256"])


def verify_desktop_route(row: dict, evidence: Evidence, identity: dict) -> None:
    component = row.get("desktopComponentSha256")
    if identity["connectionMode"] == "Standalone":
        require(row.get("desktopPreflightSha256") is None and component is None,
                "Desktop preflight is unavailable for this tuple")
        return
    require(identity["hostPlatform"].lower() in DESKTOP_HOSTS and identity["renderer"].lower() == DESKTOP_RENDERER,
            "Unsupported PCVR host or renderer for the packaged OpenXR route")
    digest = row.get("desktopPreflightSha256", identity["packageReceiptSha256"])
    require(digest is not None, "Required Desktop preflight was not run")
    expected = desktop_receipt_digest(identity["packageReceiptSha256"], digest, component)
    evidence.desktop_preflight(expected, identity, component)


def verify_claim(row: dict, evidence: Evidence, revision: str) -> tuple[str, set[str]]:
    record = evidence.smoke_record(row["recordSha256"])
    require(record["category"] == row["category"], "Evidence category does not match the matrix row")
    optional = set(row["requiredOptionalChecks"])
    claimed_mr_passed = "optionalMR" in optional and record["checks"]["optionalMR"]["status"] == "Passed"
    mr_capture = evidence.mixed_reality_capture(record) if claimed_mr_passed else None
    check_physical_gate(record, row["declaration"], optional, mr_capture=mr_capture)
    require(len(record["display"]["viewExtents"]) == PRIMARY_STEREO_VIEW_COUNT, "XR 1.0 requires a primary stereo display capture")
    observed_revision = record["provenance"].get("sourceRevision")
    require(observed_revision is not None, "Device evidence does not record a source revision")
    require(observed_revision == revision, "Device evidence is from another source revision")
    identity = record["tuple"]
    evidence.verify(identity["buildSha256"])
    evidence.verify(identity["packageReceiptSha256"])
    verify_desktop_route(row, evidence, identity)
    if "androidPreflight" in row:
        evidence.android_preflight(row["androidPreflight"], record)
    evidence.display_capture(record)
    evidence.render_timing(record, row["renderTimingLimits"])
    check_digests = {check["evidenceSha256"] for check in record["checks"].values()}
    check_digests.difference_update({None, record["display"]["evidenceSha256"], record["checks"]["renderTiming"]["evidenceSha256"]})
    if mr_capture is not None:
        check_digests.discard(record["checks"]["optionalMR"]["evidenceSha256"])
    observations = set()
    for check in sorted((CORE_CHECKS | optional) - SPECIALIZED_PHYSICAL_CHECKS):
        observations.add(evidence.physical_check_capture(record, check))
        check_digests.discard(record["checks"][check]["evidenceSha256"])
    for digest in check_digests:
        evidence.verify(digest)
    return record["status"], observations


def assess(value: object, evidence: Evidence) -> tuple[list[str], list[str]]:
    matrix = fields(value, {"schemaVersion", "sourceRevision", "reviews", "tuples"}, "release matrix")
    require(type(matrix["schemaVersion"]) is int and matrix["schemaVersion"] == 1, "Unsupported release matrix schema")
    revision = matrix["sourceRevision"]
    require(is_source_revision(revision), "Missing exact release source revision")
    reviews = fields(matrix["reviews"], REVIEWS, "subsystem reviews")
    raw_rows = matrix["tuples"]
    require(isinstance(raw_rows, list) and len(raw_rows) <= MAX_TUPLES, "Invalid matrix tuple inventory")
    rows = [validate_row(row) for row in raw_rows]
    require(len({row["id"] for row in rows}) == len(rows), "Duplicate matrix row IDs")
    claims = [row for row in rows if row["releaseClaim"]]
    identities = [row["declaration"]["tuple"] for row in claims]
    require(all(identity not in identities[:position] for position, identity in enumerate(identities)),
            "Duplicate claimed product tuple")
    messages = []
    failures = []
    reference_modes = {row["declaration"]["tuple"]["connectionMode"] for row in claims if row["category"] == "Reference"}
    if "Standalone" not in reference_modes:
        failures.append("Missing declared standalone reference tuple")
    if not reference_modes.intersection({"Tethered", "WirelessStreaming"}):
        failures.append("Missing declared PCVR reference tuple")
    for name, review in sorted(reviews.items()):
        try:
            verify_review(review, evidence, revision)
        except (OSError, ValueError) as error:
            owner = review.get("owner") if isinstance(review, dict) else None
            label = f"{name} ({owner})" if is_token(owner) else name
            failures.append(f"{label}: {error}")
    claimed_observations: set[str] = set()
    for row in rows:
        label = f"{row['id']} ({row['owner']})"
        if not row["releaseClaim"]:
            messages.append(f"{label}: {row['category']} planning")
            continue
        try:
            status, observations = verify_claim(row, evidence, revision)
            require(claimed_observations.isdisjoint(observations), "Physical observation artifact was reused across smoke runs")
            claimed_observations.update(observations)
            messages.append(f"{label}: {status}")
        except (OSError, ValueError, RecursionError) as error:
            failures.append(f"{label}: {error}")
    return messages, failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        messages, failures = assess(load_record(args.matrix), Evidence(args.evidence_dir))
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"XR matrix rejected: {error}\n")
    for message in messages:
        print(message)
    if failures:
        parser.exit(1, "\n".join(failures) + "\n")
    print("XR 1.0 matrix evidence passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

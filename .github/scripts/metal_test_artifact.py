"""Verify immutable Actions artifact provenance and bounded runtime downloads."""
from __future__ import annotations

from pathlib import Path
import json
import os
import re
import stat
import struct
import urllib.error
import urllib.parse
import urllib.request
import zipfile

import dependency_package as package

REPOSITORY = "horocore/horo-engine"
PAYLOAD = "metal-tests.tar.gz"
MAX_ARCHIVE_BYTES = 1024 ** 3
MAX_METADATA_BYTES = 8 * 1024 ** 2
DIGEST = re.compile(r"sha256:[0-9a-f]{64}")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Keep the GitHub bearer token on the GitHub API origin only."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def api_request(path: str):
    if not path.startswith(f"repos/{REPOSITORY}/actions/"):
        raise ValueError("Unexpected Actions API path")
    token = os.environ["GITHUB_TOKEN"]
    request = urllib.request.Request("https://api.github.com/" + path,
                                     headers={"Authorization": "Bearer " + token,
                                              "Accept": "application/vnd.github+json",
                                              "X-GitHub-Api-Version": "2022-11-28"})
    return urllib.request.build_opener(NoRedirect()).open(request, timeout=30)


def api_document(path: str) -> dict:
    with api_request(path) as response:
        data = response.read(MAX_METADATA_BYTES + 1)
    if len(data) > MAX_METADATA_BYTES:
        raise ValueError("Actions metadata exceeds its bound")
    return json.loads(data, object_pairs_hook=package.unique_object)


def producer_identity(consumer: dict, attempt: str) -> dict:
    """Retain source guards while admitting only an earlier attempt of this run."""
    if not re.fullmatch(r"[1-9]\d{0,19}", attempt, flags=re.ASCII) or int(attempt) > consumer["attempt"]:
        raise ValueError("Invalid producer attempt")
    return dict(consumer, attempt=int(attempt))


def artifact_provenance(identity: dict, artifact_id: str, digest: str, producer_name: str) -> dict:
    """Check server metadata and the specific completed producer before download."""
    if re.fullmatch(r"[0-9a-f]{64}", digest):
        digest = "sha256:" + digest
    if not re.fullmatch(r"[1-9]\d{0,19}", artifact_id, flags=re.ASCII) or not DIGEST.fullmatch(digest):
        raise ValueError("Missing immutable producer artifact outputs")
    artifact = api_document(f"repos/{REPOSITORY}/actions/artifacts/{artifact_id}")
    validate_artifact_record(artifact, identity, artifact_id, digest)
    document = api_document(f"repos/{REPOSITORY}/actions/runs/{identity['run']}/attempts/{identity['attempt']}/jobs?per_page=100")
    jobs = [job for job in document["jobs"] if job["name"] == producer_name]
    if len(jobs) != 1:
        raise ValueError("The producer job is missing or ambiguous")
    producer = jobs[0]
    validate_producer_record(producer, identity, artifact["created_at"])
    return {"artifact": artifact, "producer": producer["id"]}


def validate_artifact_record(artifact: dict, identity: dict, artifact_id: str, digest: str) -> None:
    """Bind the server's immutable artifact to the exact source run and attempt."""
    run = artifact.get("workflow_run", {})
    expected_name = f"metal-tests-{identity['run']}-{identity['attempt']}"
    if (artifact.get("id") != int(artifact_id) or artifact.get("expired") is not False or artifact.get("name") != expected_name
            or artifact.get("digest") != digest or run.get("id") != identity["run"]
            or run.get("head_sha") != identity["head"]):
        raise ValueError("The server artifact does not belong to this source run")
    validate_artifact_size(artifact.get("size_in_bytes"))


def validate_artifact_size(size) -> None:
    """Reject unbounded or nonnumeric server archive sizes before download."""
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_ARCHIVE_BYTES:
        raise ValueError("Invalid server artifact size")


def validate_producer_record(producer: dict, identity: dict, created_at: str) -> None:
    """Require a successful job from the selected attempt's execution window."""
    job_id = producer.get("id")
    if (not isinstance(job_id, int) or isinstance(job_id, bool) or job_id <= 0
            or producer.get("run_id") != identity["run"]
            or producer.get("conclusion") != "success" or producer.get("run_attempt") != identity["attempt"]
            or producer.get("head_sha") != identity["head"]
            or not producer["started_at"] <= created_at <= producer["completed_at"]):
        raise ValueError("The artifact is outside the admitted producer execution")


def signed_storage_response(location: str):
    """Open only an admitted HTTPS storage origin without API authorization."""
    parsed = urllib.parse.urlsplit(location)
    hostname = parsed.hostname or ""
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or not hostname.endswith((".blob.core.windows.net", ".actions.githubusercontent.com"))):
        raise ValueError("Unexpected artifact storage origin")
    return urllib.request.build_opener(NoRedirect()).open(location, timeout=120)


def download_artifact(artifact_id: str, destination: Path, expected_size: int, expected_digest: str) -> None:
    """Follow one signed storage redirect without forwarding API credentials."""
    try:
        response = api_request(f"repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip")
    except urllib.error.HTTPError as error:
        if error.code != 302:
            raise
        response = signed_storage_response(error.headers["Location"])
    with response, destination.open("xb") as output:
        total = 0
        while chunk := response.read(1024 * 1024):
            total += len(chunk)
            if total > expected_size or total > MAX_ARCHIVE_BYTES:
                raise ValueError("The downloaded artifact exceeds its server size")
            output.write(chunk)
    if total != expected_size or package.digest_file(destination) != expected_digest.removeprefix("sha256:"):
        raise ValueError("The artifact digest differs from GitHub's immutable record")


def unwrap_artifact(archive: Path, output: Path, payload_digest: str) -> None:
    """Admit the single expected payload from the upload action's ZIP envelope."""
    if not re.fullmatch(r"[0-9a-f]{64}", payload_digest):
        raise ValueError("Missing producer payload digest")
    with zipfile.ZipFile(archive) as zipped:
        members = zipped.infolist()
        if len(members) != 1 or members[0].filename != PAYLOAD:
            raise ValueError("The artifact must contain exactly one declared test bundle")
        member = members[0]
        mode = member.external_attr >> 16
        if stat.S_ISLNK(mode) or not 0 < member.file_size <= MAX_ARCHIVE_BYTES:
            raise ValueError("The artifact payload is a link or oversized")
        with zipped.open(member) as source, output.open("xb") as destination:
            remaining = member.file_size
            while remaining:
                chunk = source.read(min(remaining, 1024 * 1024))
                if not chunk:
                    raise ValueError("Truncated artifact payload")
                destination.write(chunk)
                remaining -= len(chunk)
            if source.read(1):
                raise ValueError("The artifact payload exceeds its declared size")
    if package.digest_file(output) != payload_digest:
        raise ValueError("The test bundle differs from the producer output")


def deployment_identity(entry: bytes) -> dict:
    """Decode a single macOS build-version load command."""
    if len(entry) < 24:
        raise ValueError("Invalid deployment command")
    target_platform, minimum, sdk, _ = struct.unpack_from("<4I", entry, 8)
    if target_platform != 1:
        raise ValueError("Invalid macOS deployment platform")
    return {"minimum": minimum, "sdk": sdk}


def system_library(entry: bytes) -> str:
    """Admit only a terminated system-library install name."""
    if len(entry) < 24:
        raise ValueError("Invalid library command")
    offset = struct.unpack_from("<I", entry, 8)[0]
    if not 24 <= offset < len(entry) or b"\0" not in entry[offset:]:
        raise ValueError("Invalid library path")
    name = entry[offset:].split(b"\0", 1)[0].decode("utf-8")
    if not name.startswith(("/System/Library/", "/usr/lib/")):
        raise ValueError("The executable requires an unstaged non-system library")
    return name


def macho_commands(path: Path) -> tuple[int, bytes]:
    """Bound the native arm64 executable's load-command table before parsing."""
    with path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32:
            raise ValueError("Truncated Mach-O header")
        magic, cpu, _, kind, count, size, _, _ = struct.unpack("<8I", header)
        if magic != 0xFEEDFACF or cpu != 0x0100000C or kind != 2 or count > 4096 or size > 2 * 1024 ** 2:
            raise ValueError("Only native arm64 Mach-O executables are supported")
        data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated Mach-O commands")
    return count, data


def macho_identity(path: Path) -> dict:
    """Read bounded Mach-O load commands without executing the binary."""
    count, data = macho_commands(path)
    position, deployment, libraries = 0, None, []
    for _ in range(count):
        if position + 8 > len(data):
            raise ValueError("Truncated load command")
        kind, length = struct.unpack_from("<II", data, position)
        if length < 8 or position + length > len(data):
            raise ValueError("Invalid load command length")
        entry = data[position:position + length]
        if kind == 0x32:
            if deployment is not None:
                raise ValueError("Invalid macOS deployment platform")
            deployment = deployment_identity(entry)
        elif kind in {0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20}:
            libraries.append(system_library(entry))
        position += length
    if position != len(data) or deployment is None:
        raise ValueError("Missing or inconsistent deployment identity")
    return {"architecture": "arm64", **deployment, "libraries": libraries}

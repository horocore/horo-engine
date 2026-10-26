"""Immutable producer identity, artifact envelope and token confinement tests."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import urllib.error
import zipfile

import pytest
from test_dependency_package import sdk

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("metal_test_artifact", ROOT / ".github/scripts/metal_test_artifact.py")
artifact_module = importlib.util.module_from_spec(SPEC)
sys.modules["dependency_package"] = sdk
sys.modules["metal_test_artifact"] = artifact_module
SPEC.loader.exec_module(artifact_module)
IDENTITY = {"repository": "horocore/horo-engine", "run": 7, "attempt": 1,
            "commit": "a" * 40, "head": "b" * 40, "tree": "c" * 40}


@pytest.fixture
def server_metadata(monkeypatch):
    artifact = {"id": 8, "name": "metal-tests-7-1", "expired": False, "digest": "sha256:" + "1" * 64,
                "size_in_bytes": 100, "created_at": "2026-10-25T12:01:00Z",
                "workflow_run": {"id": 7, "head_sha": IDENTITY["head"]}}
    job = {"id": 9, "run_id": 7, "name": "Build · macOS / Clang", "conclusion": "success", "run_attempt": 1,
           "head_sha": IDENTITY["head"], "started_at": "2026-10-25T12:00:00Z", "completed_at": "2026-10-25T12:02:00Z"}
    monkeypatch.setattr(artifact_module, "api_document", lambda path: {"jobs": [job]} if "/jobs?" in path else artifact)
    return artifact, job



def test_server_provenance_requires_the_specific_successful_producer(server_metadata) -> None:
    artifact, _ = server_metadata
    assert artifact_module.artifact_provenance(IDENTITY, "8", artifact["digest"], "Build · macOS / Clang")["producer"] == 9



@pytest.mark.parametrize("attack", ["run", "head", "digest", "expired", "producer", "attempt", "window", "job-run", "job-id"])
def test_manifest_claims_cannot_replace_external_provenance(server_metadata, attack: str) -> None:
    artifact, job = server_metadata
    expected_digest = artifact["digest"]
    if attack == "run":
        artifact["workflow_run"]["id"] = 6
    elif attack == "head":
        artifact["workflow_run"]["head_sha"] = "d" * 40
    elif attack == "digest":
        artifact["digest"] = "sha256:" + "2" * 64
    elif attack == "expired":
        artifact["expired"] = True
    elif attack == "producer":
        job["conclusion"] = "failure"
    elif attack == "attempt":
        job["run_attempt"] = 2
    elif attack == "job-run":
        job["run_id"] = 6
    elif attack == "job-id":
        job["id"] = 0
    else:
        artifact["created_at"] = "2026-10-25T11:59:00Z"
    with pytest.raises(ValueError):
        artifact_module.artifact_provenance(IDENTITY, "8", expected_digest, "Build · macOS / Clang")



@pytest.mark.parametrize("consumer_attempt", [1, 2])
def test_hardware_retry_uses_explicit_producer_attempt_without_changing_consumer(server_metadata, monkeypatch, consumer_attempt: int) -> None:
    artifact, job = server_metadata
    consumer = dict(IDENTITY, attempt=consumer_attempt)
    before = copy.deepcopy(consumer)
    produced = artifact_module.producer_identity(consumer, "1")
    requests = []

    def server(path):
        requests.append(path)
        return {"jobs": [job]} if "/jobs?" in path else artifact

    monkeypatch.setattr(artifact_module, "api_document", server)
    assert artifact_module.artifact_provenance(produced, "8", artifact["digest"], job["name"])["producer"] == 9
    assert requests[-1] == "repos/horocore/horo-engine/actions/runs/7/attempts/1/jobs?per_page=100"
    assert produced == IDENTITY
    assert consumer == before



@pytest.mark.parametrize("attempt", ["", "0", "-1", "2", "1.0", "01", "1/../2", "1" * 21, "١", "1٢"])
def test_missing_malformed_or_future_producer_attempt_fails_closed(attempt: str) -> None:
    with pytest.raises(ValueError, match="producer attempt"):
        artifact_module.producer_identity(IDENTITY, attempt)


@pytest.mark.parametrize("artifact_id", ["٨", "1٨"])
def test_non_ascii_artifact_ids_fail_before_metadata_lookup(monkeypatch, artifact_id: str) -> None:
    def unexpected_request(_path):
        pytest.fail("Malformed artifact IDs must fail before an API request")

    monkeypatch.setattr(artifact_module, "api_document", unexpected_request)
    with pytest.raises(ValueError, match="immutable producer artifact outputs"):
        artifact_module.artifact_provenance(IDENTITY, artifact_id, "sha256:" + "a" * 64, "Build · macOS / Clang")



@pytest.mark.parametrize("attack", [None, "extra", "path", "symlink", "oversized", "digest"])
def test_zip_envelope_only_admits_the_bounded_expected_payload(tmp_path: Path, monkeypatch, attack) -> None:
    data = b"verified compressed payload"
    archive, output = tmp_path / "artifact_module.zip", tmp_path / artifact_module.PAYLOAD
    member = zipfile.ZipInfo("../" + artifact_module.PAYLOAD if attack == "path" else artifact_module.PAYLOAD)
    if attack == "symlink":
        member.create_system = 3
        member.external_attr = 0o120777 << 16
    with zipfile.ZipFile(archive, "w") as zipped:
        zipped.writestr(member, data)
        if attack == "extra":
            zipped.writestr("extra", b"unexpected")
    if attack == "oversized":
        monkeypatch.setattr(artifact_module, "MAX_ARCHIVE_BYTES", len(data) - 1)
    digest = hashlib.sha256(data if attack != "digest" else b"other").hexdigest()
    if attack is None:
        artifact_module.unwrap_artifact(archive, output, digest)
        assert output.read_bytes() == data
    else:
        with pytest.raises(ValueError):
            artifact_module.unwrap_artifact(archive, output, digest)



@pytest.mark.parametrize("location", ["http://storage.blob.core.windows.net/payload",
                                      "https://api.github.com.attacker.invalid/payload",
                                      "https://blob.core.windows.net.attacker.invalid/payload",
                                      "https://user@storage.blob.core.windows.net/payload"])
def test_artifact_redirect_rejects_untrusted_storage_without_forwarding_token(tmp_path: Path, monkeypatch, location: str) -> None:
    def redirect(_):
        raise urllib.error.HTTPError("https://api.github.com/", 302, "redirect", {"Location": location}, None)

    monkeypatch.setattr(artifact_module, "api_request", redirect)
    monkeypatch.setattr(artifact_module.urllib.request, "build_opener", lambda *_: pytest.fail("Untrusted network access"))
    with pytest.raises(ValueError, match="storage origin"):
        artifact_module.download_artifact("8", tmp_path / "artifact_module.zip", 100, "1" * 64)



def test_signed_storage_download_receives_no_api_authorization(tmp_path: Path, monkeypatch) -> None:
    data = b"artifact bytes"
    location = "https://storage.blob.core.windows.net/payload?signature=example"

    def redirect(_):
        raise urllib.error.HTTPError("https://api.github.com/", 302, "redirect", {"Location": location}, None)

    class Storage:
        def open(self, request, *, timeout):
            assert request == location
            assert timeout == 120
            return io.BytesIO(data)

    monkeypatch.setattr(artifact_module, "api_request", redirect)
    monkeypatch.setattr(artifact_module.urllib.request, "build_opener", lambda *_: Storage())
    output = tmp_path / "artifact_module.zip"
    artifact_module.download_artifact("8", output, len(data), hashlib.sha256(data).hexdigest())
    assert output.read_bytes() == data

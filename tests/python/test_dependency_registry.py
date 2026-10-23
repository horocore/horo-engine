"""Registry corruption, credential isolation, and publication authority contracts."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
from types import SimpleNamespace
import uuid

import pytest

from test_dependency_package import sdk


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "dependency_registry", ROOT / ".github/scripts/dependency_registry.py")
registry = importlib.util.module_from_spec(SPEC)
sys.modules["dependency_package"] = sdk
SPEC.loader.exec_module(registry)


@pytest.fixture(name="identity")
def registry_identity():
    """Use a complete compatibility identity without importing another fixture."""
    return {key: "registry-fixture" for key in sdk.IDENTITY_FIELDS}


@pytest.fixture(name="publisher_environment")
def trusted_environment(monkeypatch):
    """Provide a fresh non-secret credential value for each publisher test."""
    for key, value in {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/main",
                       "GITHUB_REPOSITORY": "horocore/horo-engine", "GITHUB_ACTOR": "fixture",
                       "HORO_PACKAGE_TOKEN": uuid.uuid4().hex}.items():
        monkeypatch.setenv(key, value)


@pytest.fixture
def remote(tmp_path, identity, monkeypatch):
    stage = tmp_path / "stage"
    stage.mkdir()
    (stage / "NOTICE").write_text("fixture license", encoding="utf-8")
    archive = tmp_path / ("Unicode-" + "a" * 64 + ".tar.gz")
    sdk.pack(stage, identity, archive)
    layer = {"size": archive.stat().st_size,
             "digest": "sha256:" + sdk.digest_file(archive),
             "mediaType": registry.LAYER_TYPE}
    calls = []

    class FakeRegistry:
        def __init__(self, *args, **kwargs):
            pass

        def run(self, *args, **kwargs):
            calls.append(args)
            if args[:2] == ("manifest", "fetch"):
                return json.dumps({"schemaVersion": 2, "artifactType": registry.ARTIFACT_TYPE,
                                   "layers": [layer]})
            if args[:2] == ("blob", "fetch"):
                shutil.copyfile(archive, args[-1])
            return ""

    monkeypatch.setattr(registry, "Registry", FakeRegistry)
    return archive, layer, calls


def test_anonymous_fetch_verifies_before_install(remote, tmp_path, identity):
    archive, layer, calls = remote
    destination = tmp_path / "cache" / archive.name
    registry.fetch(destination, identity)
    assert destination.read_bytes() == archive.read_bytes()
    assert calls[1][2] == registry.REGISTRY + "@" + layer["digest"]
    assert all(call[0] != "login" for call in calls)


@pytest.mark.parametrize("mutation", ["digest", "size", "identity"])
def test_invalid_remote_never_installs(remote, tmp_path, identity, mutation):
    archive, layer, _ = remote
    if mutation == "digest":
        layer["digest"] = "sha256:" + "0" * 64
    elif mutation == "size":
        layer["size"] += 1
    else:
        identity = dict(identity, compiler="incompatible")
    destination = tmp_path / "cache" / archive.name
    with pytest.raises(ValueError):
        registry.fetch(destination, identity)
    assert not destination.exists()
    assert list(destination.parent.iterdir()) == []


@pytest.mark.parametrize("event,ref,repository", [
    ("pull_request", "refs/heads/main", "horocore/horo-engine"),
    ("pull_request_target", "refs/heads/main", "horocore/horo-engine"),
    ("push", "refs/heads/topic", "horocore/horo-engine"),
    ("push", "refs/heads/main", "fork/horo-engine"),
])
def test_untrusted_publish_rejected_before_io(monkeypatch, identity, event, ref, repository):
    monkeypatch.setenv("GITHUB_EVENT_NAME", event)
    monkeypatch.setenv("GITHUB_REF", ref)
    monkeypatch.setenv("GITHUB_REPOSITORY", repository)
    with pytest.raises(ValueError, match="canonical main"):
        registry.publish(Path("nonexistent.tar.gz"), identity)


@pytest.mark.usefixtures("publisher_environment")
def test_existing_key_is_never_replaced(remote, identity):
    archive, _, calls = remote
    registry.publish(archive, identity)
    assert all(call[0] != "push" for call in calls)


@pytest.mark.parametrize("size", [0, -1, True, registry.MAX_ARCHIVE_BYTES + 1])
def test_invalid_layer_size_rejected(size):
    with pytest.raises(ValueError):
        registry.descriptor(json.dumps({"schemaVersion": 2, "artifactType": registry.ARTIFACT_TYPE,
            "layers": [{"size": size, "digest": "sha256:" + "0" * 64,
                        "mediaType": registry.LAYER_TYPE}]}))


def test_credential_store_does_not_inherit_docker_login(monkeypatch, tmp_path):
    monkeypatch.setattr(shutil, "which", lambda _: "/fixture/oras")
    client = registry.Registry(tmp_path)
    assert json.loads(client.config.read_text()) == {"auths": {}}
    if os.name != "nt":
        assert client.config.stat().st_mode & 0o077 == 0


def test_registry_uses_fixed_client_and_stdin_credentials(monkeypatch, tmp_path):
    monkeypatch.setattr(shutil, "which", lambda _: "/fixture/oras")
    calls = []

    def execute(arguments, **options):
        calls.append((arguments, options))
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(registry.subprocess, "run", execute)
    client = registry.Registry(tmp_path)
    secret = uuid.uuid4().hex
    client.run("login", "ghcr.io", "--username", "fixture", "--password-stdin", secret=secret)
    arguments, options = calls[0]
    assert arguments[0] == "oras"
    assert options["executable"] == "/fixture/oras"
    assert options["shell"] is False
    assert options["input"] == secret
    assert secret not in arguments


@pytest.mark.usefixtures("publisher_environment")
def test_registry_outage_cannot_replace_a_published_key(remote, monkeypatch, identity):
    archive, _, calls = remote
    original = registry.Registry.run

    def unavailable(self, *args, **kwargs):
        if args[:2] == ("manifest", "fetch"):
            raise ValueError("Registry operation unavailable")
        return original(self, *args, **kwargs)

    monkeypatch.setattr(registry.Registry, "run", unavailable)
    with pytest.raises(ValueError, match="unavailable"):
        registry.publish(archive, identity)
    assert all(call[0] != "push" for call in calls)

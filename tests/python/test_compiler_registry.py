"""Compiler transport provenance, corruption, and non-destructive restore contracts."""
import importlib.util
import io
import json
import re
from pathlib import Path
import shutil
import sys
import tarfile
import uuid

import pytest

from test_dependency_registry import registry as transport

SPEC = importlib.util.spec_from_file_location(
    "compiler_registry", Path(__file__).resolve().parents[2]
    / ".github/scripts/compiler_registry.py")
compiler = importlib.util.module_from_spec(SPEC)
sys.modules["dependency_registry"] = transport
SPEC.loader.exec_module(compiler)
NAMESPACE = "sonar-v2-gcc-Linux-X64-main-"
KEY = NAMESPACE + "400-1"


@pytest.fixture
def snapshot(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    source = tmp_path / "source"
    (source / "a/b").mkdir(parents=True)
    (source / "a/b/entryR").write_bytes(b"compiler object")
    (source / "ccache.conf").write_text("ignored configuration", encoding="utf-8")
    (source / "a/stats").write_text("ignored counter", encoding="utf-8")
    archive = tmp_path / "checkpoint.tar.gz"
    compiler.pack(source, KEY, archive)
    return archive


def test_verified_entries_keep_local_configuration(snapshot, tmp_path):
    staging = tmp_path / "staging"
    staging.mkdir()
    assert compiler.unpack(snapshot, NAMESPACE, staging) == KEY
    destination = compiler.cache_root(NAMESPACE)
    destination.mkdir(parents=True)
    settings = destination / "ccache.conf"
    settings.write_text("local settings", encoding="utf-8")
    compiler.merge_entries(staging, destination)
    assert settings.read_text(encoding="utf-8") == "local settings"
    assert (destination / "a/b/entryR").read_bytes() == b"compiler object"
    assert not (destination / "a/stats").exists()


def rewrite_archive(archive, members):
    """Rewrite only literal fixture members, including intentionally unsafe ones."""
    with tarfile.open(archive, "w:gz") as output:
        for name, payload, kind in members:
            info = tarfile.TarInfo(name)
            info.type = kind
            info.size = len(payload)
            output.addfile(info, io.BytesIO(payload))


@pytest.mark.parametrize("attack", [
    "digest", "extra", "duplicate", "missing", "traversal", "symlink", "config", "namespace",
])
def test_invalid_archive_never_reaches_live_cache(snapshot, tmp_path, attack, monkeypatch):
    with tarfile.open(snapshot) as archive:
        members = [(item.name, archive.extractfile(item).read(), item.type)
                   for item in archive.getmembers()]
    manifest = json.loads(members[0][1])
    if attack == "namespace":
        manifest["sourceKey"] = "native-v1-Linux-X64-gcc-13.3.0-gcc-13.3.0-native-tests-v1-Debug-main-400-1"
        members[0] = (compiler.MANIFEST, json.dumps(manifest).encode(), tarfile.REGTYPE)
    elif attack == "missing":
        members.pop()
    elif attack == "digest":
        members[-1] = (members[-1][0], b"corrupt object!", tarfile.REGTYPE)
    else:
        name = {"extra": "extra", "duplicate": members[-1][0], "traversal": "../outside",
                "symlink": "link", "config": "ccache.conf"}[attack]
        kind = tarfile.SYMTYPE if attack == "symlink" else tarfile.REGTYPE
        members.append((name, b"bad", kind))
    rewrite_archive(snapshot, members)
    install_fake_registry(snapshot, monkeypatch)
    destination = compiler.cache_root(NAMESPACE)
    with pytest.raises((ValueError, tarfile.TarError)):
        compiler.fetch(NAMESPACE)
    assert not destination.exists()
    assert not (tmp_path / "outside").exists()


def install_fake_registry(archive, monkeypatch, *, existing=None, outage=False, key=KEY):
    """Use a valid typed OCI manifest and one digest-addressed local blob."""
    layer = {"mediaType": compiler.LAYER_TYPE, "size": archive.stat().st_size,
             "digest": "sha256:" + compiler.package.digest_file(archive)}
    document = {"schemaVersion": 2, "artifactType": compiler.ARTIFACT_TYPE,
                "layers": [layer], "annotations": {"io.horo.compiler.source-key": key}}
    calls = []

    class Client:
        def __init__(self, *_args, **_kwargs):
            pass

        def run(self, *args, **kwargs):
            calls.append(args)
            if args[:2] == ("manifest", "fetch"):
                if outage:
                    raise ValueError("fixture outage")
                if existing is False:
                    raise transport.MissingManifest("fixture absent")
                if existing:
                    document["annotations"]["io.horo.compiler.source-key"] = existing
                return json.dumps(document)
            if args[:2] == ("blob", "fetch"):
                shutil.copyfile(archive, args[-1])
            return ""

    monkeypatch.setattr(transport, "Registry", Client)
    return calls, document


def test_fetch_uses_anonymous_digest_and_preserves_existing_entry(snapshot, monkeypatch):
    calls, _ = install_fake_registry(snapshot, monkeypatch)
    destination = compiler.cache_root(NAMESPACE)
    (destination / "a/b").mkdir(parents=True)
    local = destination / "a/b/entryR"
    local.write_bytes(b"local entry")
    compiler.fetch(NAMESPACE)
    assert local.read_bytes() == b"local entry"
    assert all(call[0] != "login" for call in calls)
    assert calls[1][2].startswith(transport.REGISTRY + "@sha256:")


@pytest.mark.parametrize("field,value", [
    ("size", True), ("size", compiler.MAX_ARCHIVE_BYTES + 1),
    ("mediaType", transport.LAYER_TYPE), ("digest", "sha256:invalid"),
])
def test_invalid_oci_layer_prevents_blob_download(snapshot, monkeypatch, field, value):
    calls, document = install_fake_registry(snapshot, monkeypatch)
    document["layers"][0][field] = value
    with pytest.raises(ValueError):
        compiler.fetch(NAMESPACE)
    assert len(calls) == 1


@pytest.mark.parametrize("key", [
    "native-v1-Linux-X64-gcc-13.3.0-gcc-13.3.0-native-tests-v1-Debug-main-400-1",
    "native-v1-macOS-ARM64-appleclang-21.0.0-appleclang-21.0.0-native-tests-v1-Debug-main-400-1",
    "native-v1-Windows-X64-msvc-19.44.35229-msvc-19.44.35229-native-tests-v1-Debug-main-400-1",
    KEY,
])
def test_real_checkpoint_names(key):
    namespace, order = compiler.split_key(key)
    assert order == (400, 1)
    assert compiler.reference(namespace).startswith(transport.REGISTRY + ":compiler-v1-")


@pytest.mark.parametrize("key", [KEY + "/../x", KEY.replace("main", "pr"), KEY + "\n", "unknown-1-1"])
def test_unowned_checkpoint_names_rejected(key):
    with pytest.raises(ValueError):
        compiler.split_key(key)


@pytest.mark.parametrize("event,ref,repo,allowed", [
    ("pull_request", "refs/heads/main", "horocore/horo-engine", False),
    ("schedule", "refs/heads/main", "horocore/horo-engine", True),
    ("workflow_dispatch", "refs/heads/topic", "horocore/horo-engine", False),
    ("workflow_dispatch", "refs/heads/main", "fork/horo-engine", False),
])
def test_publication_authority(event, ref, repo, allowed):
    environment = {"GITHUB_EVENT_NAME": event, "GITHUB_REF": ref, "GITHUB_REPOSITORY": repo}
    assert compiler.trusted_publisher(environment) is allowed


@pytest.mark.parametrize("branch,event,repository,allowed", [
    ("main", "push", "horocore/horo-engine", True),
    ("topic", "push", "horocore/horo-engine", False),
    ("main", "pull_request", "horocore/horo-engine", False),
    ("main", "push", "fork/horo-engine", False),
])
def test_workflow_run_provenance(tmp_path, branch, event, repository, allowed):
    payload = tmp_path / "event.json"
    payload.write_text(json.dumps({"workflow_run": {
        "head_branch": branch, "event": event, "head_repository": {"full_name": repository}}}),
        encoding="utf-8")
    environment = {"GITHUB_EVENT_NAME": "workflow_run", "GITHUB_REF": "refs/heads/main",
                   "GITHUB_REPOSITORY": "horocore/horo-engine", "GITHUB_EVENT_PATH": str(payload)}
    assert compiler.trusted_publisher(environment) is allowed


@pytest.mark.parametrize("existing,uploads", [(KEY, 0), (NAMESPACE + "500-1", 0), (False, 1)])
def test_publish_skips_same_or_newer_checkpoint(snapshot, monkeypatch, existing, uploads):
    for name, value in {"GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF": "refs/heads/main",
                        "GITHUB_REPOSITORY": "horocore/horo-engine", "GITHUB_ACTOR": "fixture",
                        "HORO_PACKAGE_TOKEN": uuid.uuid4().hex}.items():
        monkeypatch.setenv(name, value)
    source = compiler.cache_root(NAMESPACE)
    (source / "a/b").mkdir(parents=True)
    (source / "a/b/entryR").write_bytes(b"compiler object")
    calls, _ = install_fake_registry(snapshot, monkeypatch, existing=existing)
    compiler.publish(KEY)
    assert sum(call[0] == "push" for call in calls) == uploads


def test_publisher_outage_never_overwrites(snapshot, monkeypatch):
    monkeypatch.setattr(compiler, "trusted_publisher", lambda _env: True)
    monkeypatch.setenv("GITHUB_ACTOR", "fixture")
    monkeypatch.setenv("HORO_PACKAGE_TOKEN", uuid.uuid4().hex)
    source = compiler.cache_root(NAMESPACE)
    source.mkdir(parents=True)
    calls, _ = install_fake_registry(snapshot, monkeypatch, outage=True)
    with pytest.raises(ValueError):
        compiler.publish(KEY)
    assert all(call[0] != "push" for call in calls)


def test_workspace_symlink_rejected(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (workspace / ".sonar").symlink_to(outside, target_is_directory=True)
    monkeypatch.chdir(workspace)
    with pytest.raises(ValueError):
        compiler.cache_root(NAMESPACE)
    assert list(outside.iterdir()) == []


def test_workflow_keeps_publisher_out_of_pr_capacity():
    root = Path(__file__).resolve().parents[2]
    workflow = (root / ".github/workflows/compiler-checkpoints.yml").read_text(encoding="utf-8")
    triggers = workflow.split("permissions:", 1)[0]
    assert "pull_request:" not in triggers
    assert "workflow_run:" in triggers
    assert "schedule:" in triggers
    assert "workflow_dispatch:" in triggers
    assert "max-parallel: 1" in workflow
    assert workflow.count("- runner:") == 4
    assert "packages: write" in workflow
    assert "cancel-in-progress: false" in workflow
    assert "ref: main" in workflow
    assert "persist-credentials: false" in workflow
    assert "compiler-only: 'true'" in workflow
    assert "external-cache: 'false'" in workflow
    assert "cmake --build" not in workflow


def test_archive_expanded_limit_prevents_restore(snapshot, monkeypatch, tmp_path):
    monkeypatch.setattr(compiler, "MAX_EXPANDED_BYTES", 1)
    staging = tmp_path / "staging"
    staging.mkdir()
    with pytest.raises(ValueError, match="expanded limit"):
        compiler.unpack(snapshot, NAMESPACE, staging)
    assert list(staging.iterdir()) == []


def test_destination_symlink_cannot_mutate_outside(tmp_path):
    staging = tmp_path / "staging"
    (staging / "a").mkdir(parents=True)
    (staging / "a/entryR").write_bytes(b"object")
    destination = tmp_path / "destination"
    destination.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (destination / "a").symlink_to(outside, target_is_directory=True)
    with pytest.raises(ValueError):
        compiler.merge_entries(staging, destination)
    assert list(outside.iterdir()) == []


@pytest.mark.parametrize("name", ["CCACHE.CONF", "nested/Ccache.Conf", "nested/x.LOCK", "STATS"])
def test_windows_operational_names_rejected(name):
    with pytest.raises(ValueError):
        compiler.entry_name(name)


def test_composite_step_schema_excludes_workflow_only_properties():
    root = Path(__file__).resolve().parents[2]
    action = (root / ".github/actions/restore-native-cache/action.yml").read_text(encoding="utf-8")
    # GitHub validates composite metadata before any step condition is evaluated.
    # These are step properties; nested env/with keys have deeper indentation.
    properties = set(re.findall(r"^    (?:- |  )([a-z-]+):", action, re.MULTILINE))
    allowed = {"name", "id", "if", "run", "shell", "uses", "with", "env",
               "working-directory", "continue-on-error"}
    assert properties <= allowed
    assert "timeout-minutes" not in properties


def test_primary_and_auxiliary_entries_remain_independent(snapshot, tmp_path):
    primary = compiler.cache_root(NAMESPACE)
    namespace = "native-v1-Linux-X64-gcc-13.3.0-gcc-13.3.0-native-variants-v1-Mixed-main-"
    variants = compiler.cache_root(namespace)
    assert primary != variants
    staged = tmp_path / "primary-stage"
    staged.mkdir()
    compiler.unpack(snapshot, NAMESPACE, staged)
    compiler.merge_entries(staged, primary)
    source = tmp_path / "auxiliary-source"
    (source / "a/b").mkdir(parents=True)
    (source / "a/b/entryR").write_bytes(b"release variant object")
    archive = tmp_path / "variants.tar.gz"
    compiler.pack(source, namespace + "400-1", archive)
    restored = tmp_path / "auxiliary-stage"
    restored.mkdir()
    compiler.unpack(archive, namespace, restored)
    compiler.merge_entries(restored, variants)
    assert (primary / "a/b/entryR").read_bytes() == b"compiler object"
    assert (variants / "a/b/entryR").read_bytes() == b"release variant object"
    assert compiler.reference(NAMESPACE) != compiler.reference(namespace)
    with pytest.raises(ValueError, match="Incompatible"):
        compiler.unpack(archive, NAMESPACE, tmp_path / "mismatched")


def test_variant_restore_happens_after_debug_checkpoint_before_auxiliary_builds():
    root = Path(__file__).resolve().parents[2]
    workflow = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    checkpoint = workflow.index("- name: Checkpoint main Debug compiler cache")
    restore = workflow.index("- name: Restore auxiliary compiler variants")
    metal = workflow.index("- name: Configure Metal shader and presentation tests")
    release = workflow.index("- name: Configure Release audio checks")
    save = workflow.index("- name: Checkpoint auxiliary compiler variants")
    tests = workflow.index("- name: Test Debug")
    assert checkpoint < restore < metal < release < save < tests
    restore_step = workflow[restore:metal]
    assert "profile: native-variants-v1" in restore_step
    assert "mode: Mixed" in restore_step
    assert "compiler-only: 'true'" in restore_step


@pytest.mark.parametrize("version", ["13.3.0", "15.2.0"])
def test_versioned_sonar_archive_roundtrip_and_compiler_boundary(tmp_path, monkeypatch, version):
    monkeypatch.chdir(tmp_path)
    namespace = f"sonar-v3-Linux-X64-gcc-{version}-gcc-{version}-main-"
    key = namespace + "400-1"
    source = tmp_path / "source"
    source.mkdir()
    (source / "entryR").write_bytes(b"compiler object")
    archive = tmp_path / "snapshot.tar.gz"
    compiler.pack(source, key, archive)
    calls, _ = install_fake_registry(archive, monkeypatch, key=key)
    compiler.fetch(namespace)
    assert (compiler.cache_root(namespace) / "entryR").read_bytes() == b"compiler object"
    assert calls[0][2] == compiler.reference(namespace)
    other = "15.2.0" if version == "13.3.0" else "13.3.0"
    other_namespace = f"sonar-v3-Linux-X64-gcc-{other}-gcc-{other}-main-"
    assert compiler.reference(namespace) != compiler.reference(other_namespace)
    assert compiler.reference(namespace) != compiler.reference(NAMESPACE)
    with pytest.raises(ValueError, match="Incompatible"):
        compiler.unpack(archive, other_namespace, tmp_path / "rejected")


def test_missing_versioned_sonar_bootstraps_legacy_in_the_same_client(snapshot, monkeypatch):
    namespace = "sonar-v3-Linux-X64-gcc-15.2.0-gcc-15.2.0-main-"
    calls, _ = install_fake_registry(snapshot, monkeypatch)
    original = transport.Registry.run

    def missing_versioned(client, *args, **kwargs):
        if args == ("manifest", "fetch", compiler.reference(namespace)):
            calls.append(args)
            raise transport.MissingManifest("fixture missing versioned checkpoint")
        return original(client, *args, **kwargs)

    monkeypatch.setattr(transport.Registry, "run", missing_versioned)
    compiler.fetch(namespace)
    assert [call[2] for call in calls if call[:2] == ("manifest", "fetch")] == [
        compiler.reference(namespace), compiler.reference(NAMESPACE),
    ]
    assert (compiler.cache_root(namespace) / "a/b/entryR").read_bytes() == b"compiler object"
    assert all(call[0] != "login" for call in calls)


@pytest.mark.parametrize("outage", [True, False])
def test_versioned_outage_or_invalid_manifest_never_tries_legacy(snapshot, monkeypatch, outage):
    namespace = "sonar-v3-Linux-X64-gcc-15.2.0-gcc-15.2.0-main-"
    # A v2 annotation presented as v3 is invalid, not a missing manifest.
    calls, _ = install_fake_registry(snapshot, monkeypatch, outage=outage)
    with pytest.raises(ValueError):
        compiler.fetch(namespace)
    assert calls == [("manifest", "fetch", compiler.reference(namespace))]
    assert not compiler.cache_root(namespace).exists()

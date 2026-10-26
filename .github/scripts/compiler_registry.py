"""Mirror trusted compiler checkpoints to public GHCR with anonymous fallback."""

# Compiler entries retain their own source/compiler/flag checks. This envelope
# never installs libraries or changes compiler settings.
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

import dependency_package as package
import dependency_registry as transport

ARTIFACT_TYPE = "application/vnd.horo.compiler-cache.v1"
LAYER_TYPE = ARTIFACT_TYPE + ".tar+gzip"
MANIFEST = "horo-compiler.json"
LEGACY_SONAR_NAMESPACE = "sonar-v2-gcc-Linux-X64-main-"
MAX_ARCHIVE_BYTES = 3 * 1024 ** 3
MAX_EXPANDED_BYTES = 4 * 1024 ** 3
# Multi-gigabyte checkpoints can outlast a 90-second transfer window. Keep one
# shared network deadline, with room for a slower successful download.
RESTORE_TIMEOUT_SECONDS = 180
KEY_PATTERN = re.compile(
    r"(native-v1-(?:Linux|Windows|macOS)-(?:X64|ARM64)-"
    r"[a-z0-9.-]+-[a-z0-9.-]+-(?:native-tests-v1-Debug|native-shaders-v1-Debug|native-variants-v1-Mixed)-main-|"
    r"sonar-v2-gcc-Linux-X64-main-|"
    r"sonar-v3-Linux-X64-gcc-[0-9]+(?:\.[0-9]+)+-gcc-[0-9]+(?:\.[0-9]+)+-main-)"
    r"([1-9][0-9]*)-([1-9][0-9]*)")
# Never import remote configuration, operational locks, or per-run counters.
CCACHE_CONFIGURATION = "ccache.conf"
LOCAL_FILES = {CCACHE_CONFIGURATION, "stats", "stats.log", "cachedir.tag", MANIFEST}
CI_CCACHE_DIRECTORIES = {".ci-cache/compiler", ".ci-cache/compiler-variants", ".sonar/ccache"}


def split_key(key: str) -> tuple[str, tuple[int, int]]:
    """Accept only compiler checkpoints owned by the primary main workflows."""
    match = KEY_PATTERN.fullmatch(key) if len(key) <= 256 else None
    if not match:
        raise ValueError("Unsupported compiler checkpoint key")
    return match[1], (int(match[2]), int(match[3]))


def reference(namespace: str) -> str:
    """Separate compiler manifests from SDK tags in the existing public package."""
    if split_key(namespace + "1-1")[0] != namespace:
        raise ValueError("Invalid compiler namespace")
    tag = hashlib.sha256(namespace.encode()).hexdigest()
    return f"{transport.REGISTRY}:compiler-v1-{tag}"


def cache_root(namespace: str) -> Path:
    """Use fixed workspace-owned cache directories, never an arbitrary CLI path."""
    relative = ".sonar/ccache" if namespace.startswith("sonar-") else ".ci-cache/compiler"
    if "-native-variants-v1-Mixed-main-" in namespace:
        relative = ".ci-cache/compiler-variants"
    return package.workspace_path(Path(relative))


def ccache_directory() -> Path:
    """Resolve only an explicitly selected CI store, without cache aliases."""
    workspace = Path.cwd().resolve(strict=True)
    selected = Path(os.environ.get("CCACHE_DIR", ""))
    if not selected.is_absolute() or ".." in selected.parts:
        raise ValueError("The compiler cache directory must be explicit and normalized")
    root = package.workspace_path(selected)
    if root.relative_to(workspace).as_posix() not in CI_CCACHE_DIRECTORIES:
        raise ValueError("The compiler cache directory is not a CI-owned store")
    for parent in selected.parents:
        if parent.resolve() == workspace:
            break
        if parent.is_symlink():
            raise ValueError("The compiler cache directory must not use symlink aliases")
    if (root / CCACHE_CONFIGURATION).is_symlink():
        raise ValueError("The local compiler configuration must not be a symlink")
    return root


def ccache_budget() -> str:
    """Require a positive explicit CI budget within the archive's expanded bound."""
    budget = os.environ.get("CCACHE_MAXSIZE", "")
    match = re.fullmatch(r"([1-9]\d{0,8})([KMG])(iB|B)?", budget, flags=re.ASCII)
    if not match:
        raise ValueError("The compiler cache budget must be explicit and positive")
    unit = 1024 if match[3] == "iB" else 1000
    maximum = int(match[1]) * unit ** {"K": 1, "M": 2, "G": 3}[match[2]]
    if maximum > MAX_EXPANDED_BYTES:
        raise ValueError("The compiler cache budget exceeds its transport bound")
    return budget


def reconcile() -> None:
    """Recount and bound only the selected workspace-owned ccache before builds."""
    if os.environ.get("HORO_COMPILER_LAUNCHER") != "ccache":
        raise ValueError("Local ccache maintenance requires the ccache launcher")
    root, budget = ccache_directory(), ccache_budget()
    executable = shutil.which("ccache")
    if not executable:
        raise ValueError("ccache unavailable for local maintenance")
    executable = str(Path(executable).resolve(strict=True))
    root.mkdir(parents=True, exist_ok=True)
    # Bind every write to the selected store. Reading local settings is allowed;
    # importing settings or changing a global configuration is not maintenance.
    environment = dict(os.environ, CCACHE_DIR=str(root),
                       CCACHE_CONFIGPATH=str(root / CCACHE_CONFIGURATION), CCACHE_MAXSIZE=budget)
    # Imported stores have no counters. The first pass rebuilds them; ccache
    # chooses eviction limits from the counters present at the start of a pass.
    # A second bounded pass therefore applies the budget to the recounted store.
    for _pass in range(2):
        subprocess.run(["ccache", "--dir", str(root), "--cleanup"], executable=executable,
                       shell=False, check=True, capture_output=True, text=True,
                       timeout=30, env=environment)
    print(f"Reconciled local compiler cache within configured budget {budget}")


def is_payload_name(name: str) -> bool:
    """Keep cache configuration and operational state local, including on Windows."""
    return all(part.casefold() not in LOCAL_FILES and not part.casefold().endswith(".lock")
               for part in name.split("/"))


def entry_name(name: str) -> str:
    """Reject portable path hazards and configuration at every directory level."""
    package.portable_path(name)
    if not is_payload_name(name):
        raise ValueError("Compiler snapshot contains operational state")
    return name


def pack(root: Path, key: str, output: Path) -> None:
    """Freeze the restored, idle checkpoint into a bounded regular-file archive."""
    split_key(key)
    records = package.staged_files(root)
    records = {name: dict(record, executable=False) for name, record in records.items()
               if is_payload_name(name)}
    if not records or sum(record["size"] for record in records.values()) > MAX_EXPANDED_BYTES:
        raise ValueError("Empty or oversized compiler snapshot")
    manifest = json.dumps({"schema": 1, "sourceKey": key, "files": records},
                          sort_keys=True).encode()
    if len(manifest) > package.MAX_MANIFEST_BYTES:
        raise ValueError("Compiler file table exceeds limit")
    with tarfile.open(output, "w:gz", compresslevel=1) as archive:
        header = tarfile.TarInfo(MANIFEST)
        header.size = len(manifest)
        archive.addfile(header, io.BytesIO(manifest))
        for name in records:
            archive.add(root / name, arcname=name, recursive=False)
    if output.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Compiler archive exceeds limit")


def read_manifest(archive: tarfile.TarFile, namespace: str) -> tuple[str, dict]:
    """Require independent compatibility and bounded file metadata before writes."""
    member = archive.next()
    if (member is None or member.name != MANIFEST or not member.isfile()
            or not 0 < member.size <= package.MAX_MANIFEST_BYTES):
        raise ValueError("Invalid compiler manifest")
    with archive.extractfile(member) as source:
        document = json.load(source, object_pairs_hook=package.unique_object)
    if document.get("schema") != 1 or split_key(document["sourceKey"])[0] != namespace:
        raise ValueError("Incompatible compiler snapshot")
    files = package.validate_file_records(document.get("files"))
    if sum(record["size"] for record in files.values()) > MAX_EXPANDED_BYTES:
        raise ValueError("Compiler snapshot exceeds expanded limit")
    for name in files:
        entry_name(name)
    return document["sourceKey"], files


def unpack(archive_path: Path, namespace: str, staging: Path) -> str:
    """Verify every member in private staging before exposing any cache entries."""
    seen = set()
    with tarfile.open(archive_path, "r|gz") as archive:
        key, files = read_manifest(archive, namespace)
        while (member := archive.next()) is not None:
            name = entry_name(member.name)
            if (not member.isfile() or name in seen or name not in files
                    or member.size != files[name]["size"]):
                raise ValueError("Unexpected compiler archive member")
            target = staging / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output, length=1024 * 1024)
            if package.digest_file(target) != files[name]["sha256"]:
                raise ValueError("Compiler member digest mismatch")
            target.chmod(0o644)
            seen.add(name)
        if seen != set(files):
            raise ValueError("Missing compiler archive members")
    return key


def merge_entries(staging: Path, destination: Path) -> None:
    """Publish verified missing entries while preserving local settings/files."""
    # The compiler server must be stopped. Move entries on the same filesystem
    # only after the complete archive was verified, so none is partially visible.
    destination.mkdir(parents=True, exist_ok=True)
    for source in staging.rglob("*"):
        if not source.is_file():
            continue
        target = destination / source.relative_to(staging)
        if not target.resolve().is_relative_to(destination) or target.is_symlink():
            raise ValueError("Compiler destination contains an escaping symlink")
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            source.rename(target)


def descriptor(manifest: str, namespace: str) -> tuple[dict, str]:
    """Validate the OCI layer and trusted checkpoint annotation before download."""
    document = json.loads(manifest, object_pairs_hook=package.unique_object)
    if (document.get("schemaVersion") != 2 or document.get("artifactType") != ARTIFACT_TYPE
            or len(document.get("layers", [])) != 1):
        raise ValueError("Unexpected compiler OCI manifest")
    key = document.get("annotations", {}).get("io.horo.compiler.source-key", "")
    if split_key(key)[0] != namespace:
        raise ValueError("Compiler OCI namespace mismatch")
    layer = document["layers"][0]
    size = layer.get("size")
    if (layer.get("mediaType") != LAYER_TYPE or not isinstance(size, int) or isinstance(size, bool)
            or not 0 < size <= MAX_ARCHIVE_BYTES
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", layer.get("digest", ""))):
        raise ValueError("Invalid compiler OCI layer")
    return layer, key


def fetch_descriptor(registry: transport.Registry, namespace: str) -> tuple[dict, str, str]:
    """Bootstrap a missing versioned Sonar snapshot without masking invalid ones."""
    try:
        layer, key = descriptor(registry.run("manifest", "fetch", reference(namespace)), namespace)
        return layer, key, namespace
    except transport.MissingManifest:
        if not namespace.startswith("sonar-v3-"):
            raise
    print("No versioned Sonar checkpoint; try legacy entries with compiler hash checks intact")
    layer, key = descriptor(
        registry.run("manifest", "fetch", reference(LEGACY_SONAR_NAMESPACE)),
        LEGACY_SONAR_NAMESPACE,
    )
    return layer, key, LEGACY_SONAR_NAMESPACE


def fetch(namespace: str) -> None:
    """Restore anonymously by blob digest; a failed fetch leaves compilation usable."""
    reference(namespace)
    destination = cache_root(namespace)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        root = Path(temporary)
        registry = transport.Registry(root, seconds=RESTORE_TIMEOUT_SECONDS)
        layer, expected, selected_namespace = fetch_descriptor(registry, namespace)
        payload = root / "compiler.tar.gz"
        registry.run("blob", "fetch", f"{transport.REGISTRY}@{layer['digest']}",
                     "--output", str(payload))
        if (payload.stat().st_size != layer["size"] or package.digest_file(payload)
                != layer["digest"].removeprefix("sha256:")):
            raise ValueError("Compiler OCI digest mismatch")
        staging = root / "verified"
        staging.mkdir()
        if unpack(payload, selected_namespace, staging) != expected:
            raise ValueError("Compiler OCI and payload checkpoint differ")
        merge_entries(staging, destination)
        print(f"Verified public compiler checkpoint: {expected}")


def trusted_publisher(environment: dict, namespace: str) -> bool:
    """Allow only the owning build workflow on canonical main, never a delayed exporter."""
    owner = {"native": "CI", "sonar": "SonarCloud"}.get(namespace.split("-", 1)[0])
    return (owner is not None and environment.get("GITHUB_WORKFLOW") == owner
            and environment.get("GITHUB_REPOSITORY") == "horocore/horo-engine"
            and environment.get("GITHUB_REF") == "refs/heads/main"
            and environment.get("GITHUB_EVENT_NAME") in {"push", "workflow_dispatch"})


def publish(key: str) -> None:
    """Single-writer latest checkpoint; never replace it with an older main run."""
    namespace, order = split_key(key)
    if not trusted_publisher(os.environ, namespace):
        raise ValueError("Only the owning main build may publish compiler entries")
    if not namespace.startswith("native-v1-Windows-") and "CCACHE_DISABLE" in os.environ:
        raise ValueError("Disabled ccache stores cannot publish compiler entries")
    ref = reference(namespace)
    source = cache_root(namespace)
    with tempfile.TemporaryDirectory(dir=source.parent) as temporary:
        root = Path(temporary)
        registry = transport.Registry(root, seconds=180)
        registry.run("login", "ghcr.io", "--username", os.environ["GITHUB_ACTOR"],
                     "--password-stdin", secret=os.environ["HORO_PACKAGE_TOKEN"])
        try:
            _, existing = descriptor(registry.run("manifest", "fetch", ref), namespace)
            if split_key(existing)[1] >= order:
                print("Compiler checkpoint already published or superseded")
                return
        except transport.MissingManifest:
            pass
        payload = root / "compiler.tar.gz"
        pack(source, key, payload)
        registry.run("push", ref, f"{payload.name}:{LAYER_TYPE}",
                     "--artifact-type", ARTIFACT_TYPE,
                     "--annotation", f"io.horo.compiler.source-key={key}",
                     "--annotation", "org.opencontainers.image.source=https://github.com/horocore/horo-engine",
                     cwd=root)
        print(f"Published compiler checkpoint: {key}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    for operation in ("fetch", "publish"):
        command = commands.add_parser(operation)
        command.add_argument("--key", required=True,
                             help="Compatible namespace for fetch; main source key for publish")
    commands.add_parser("reconcile")
    args = parser.parse_args()
    try:
        if args.operation == "reconcile":
            reconcile()
        else:
            {"fetch": fetch, "publish": publish}[args.operation](args.key)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, TimeoutError,
            subprocess.SubprocessError, tarfile.TarError) as error:
        # Exception messages/arguments may contain client credentials or output.
        operation = ("Local compiler cache maintenance" if args.operation == "reconcile"
                     else "Public compiler checkpoint")
        detail = type(error).__name__
        if isinstance(error, subprocess.CalledProcessError):
            detail += f", exit {error.returncode}"
        print(f"{operation} unavailable ({detail}); retain ordinary compilation")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

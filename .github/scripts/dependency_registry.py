"""Bounded GHCR transport for independently verified dependency SDK packages.

Readers use an empty ORAS credential store, including on trusted runners. Only
main pushes or manual main publications in the canonical repository may write.
Transport failures are cache misses; package verification remains mandatory.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import time

import dependency_package as package


REGISTRY = "ghcr.io/horocore/horo-engine-dependency-sdk"
ARTIFACT_TYPE = "application/vnd.horo.dependency-sdk.v1"
LAYER_TYPE = "application/vnd.horo.dependency-sdk.v1.tar+gzip"
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024


class MissingManifest(ValueError):
    """The registry explicitly reported a missing manifest, not an outage."""


class Registry:
    """Own a short-lived credential store and a shared network time budget."""

    def __init__(self, directory: Path, seconds: int = 60):
        """Create isolated credentials and one deadline for all client calls."""
        self.executable = shutil.which("oras")
        if not self.executable:
            raise ValueError("ORAS unavailable")
        self.config = directory / "credentials.json"
        self.config.write_text('{"auths":{}}', encoding="utf-8")
        self.config.chmod(0o600)
        self.deadline = time.monotonic() + seconds

    def run(self, *arguments: str, cwd: Path | None = None,
            secret: str | None = None) -> str:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Registry time budget exhausted")
        # Never include stdout/stderr in exceptions: authentication output is
        # not part of user diagnostics. Credentials are passed only on stdin.
        result = subprocess.run(
            ["oras", *arguments, "--registry-config", str(self.config)],
            executable=self.executable, shell=False,
            cwd=cwd, input=secret, text=True, capture_output=True,
            timeout=remaining, check=False)
        if result.returncode:
            if arguments[:2] == ("manifest", "fetch") and (
                    "MANIFEST_UNKNOWN" in result.stderr
                    or re.search(r": (?:not found|manifest unknown)(?:\s|$)", result.stderr)):
                raise MissingManifest("SDK manifest does not exist")
            raise ValueError("Registry operation unavailable")
        return result.stdout


def archive_tag(archive: Path) -> str:
    """Only accept the exact CMake-generated compatibility-addressed name."""
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*-[0-9a-f]{64}\.tar\.gz", archive.name):
        raise ValueError("Invalid dependency SDK archive name")
    return archive.name.removesuffix(".tar.gz").lower()


def descriptor(manifest: str) -> dict:
    document = json.loads(manifest, object_pairs_hook=package.unique_object)
    if (document.get("schemaVersion") != 2
            or document.get("artifactType") != ARTIFACT_TYPE
            or len(document.get("layers", [])) != 1):
        raise ValueError("Unexpected SDK registry manifest")
    layer = document["layers"][0]
    if (layer.get("mediaType") != LAYER_TYPE
            or not isinstance(layer.get("size"), int)
            or isinstance(layer.get("size"), bool)
            or not 0 < layer["size"] <= MAX_ARCHIVE_BYTES
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", layer.get("digest", ""))):
        raise ValueError("Invalid SDK registry layer")
    return layer


def fetch(archive: Path, identity: dict) -> None:
    tag = archive_tag(archive)
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="horo-ghcr-", dir=archive.parent) as temporary:
        root = Path(temporary)
        registry = Registry(root)
        layer = descriptor(registry.run("manifest", "fetch", f"{REGISTRY}:{tag}"))
        payload = root / "payload.tar.gz"
        # Fetch one blob by digest. Registry-supplied filenames never reach the
        # filesystem and a mutable tag cannot change the selected payload.
        registry.run("blob", "fetch", f"{REGISTRY}@{layer['digest']}",
                     "--output", str(payload))
        if (payload.stat().st_size != layer["size"]
                or package.digest_file(payload) != layer["digest"].removeprefix("sha256:")):
            raise ValueError("SDK registry digest mismatch")
        package.restore(payload, identity, root / "verified")
        payload.replace(archive)


def trusted_publisher(environment: dict) -> bool:
    return (environment.get("GITHUB_REPOSITORY") == "horocore/horo-engine"
            and environment.get("GITHUB_EVENT_NAME") in {"push", "workflow_dispatch"}
            and environment.get("GITHUB_REF") == "refs/heads/main")


def publish(archive: Path, identity: dict) -> None:
    if not trusted_publisher(os.environ):
        raise ValueError("Only canonical main publisher may write SDK packages")
    token = os.environ.get("HORO_PACKAGE_TOKEN")
    actor = os.environ.get("GITHUB_ACTOR")
    if not token or not actor:
        raise ValueError("SDK publisher credentials unavailable")
    tag = archive_tag(archive)
    if not 0 < archive.stat().st_size <= MAX_ARCHIVE_BYTES:
        raise ValueError("SDK archive exceeds transport limit")
    with tempfile.TemporaryDirectory(prefix="horo-ghcr-") as temporary:
        root = Path(temporary)
        package.restore(archive, identity, root / "verified")
        registry = Registry(root, seconds=120)
        registry.run("login", "ghcr.io", "--username", actor,
                     "--password-stdin", secret=token)
        reference = f"{REGISTRY}:{tag}"
        # Never replace an existing compatibility key, even if rebuilding the
        # same source would produce different archive metadata.
        try:
            descriptor(registry.run("manifest", "fetch", reference))
        except MissingManifest:
            registry.run("push", reference, f"{archive.name}:{LAYER_TYPE}",
                         "--artifact-type", ARTIFACT_TYPE,
                         "--annotation", "org.opencontainers.image.source=https://github.com/horocore/horo-engine",
                         cwd=archive.parent.resolve())
        anonymous = root / "anonymous"
        anonymous.mkdir()
        # New GHCR packages default to private. A successful authenticated push
        # does not prove fork PRs can consume them.
        try:
            descriptor(Registry(anonymous, seconds=15).run("manifest", "fetch", reference))
        except ValueError:
            print("SDK publication needs anonymous access: set the GHCR package visibility to public")
            raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("fetch", "publish"))
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True)
    args = parser.parse_args()
    try:
        identity = json.loads(args.identity.read_text(encoding="utf-8"),
                              object_pairs_hook=package.unique_object)
        package.canonical_identity(identity)
        {"fetch": fetch, "publish": publish}[args.operation](args.archive, identity)
    except (OSError, ValueError, TypeError, KeyError, AttributeError,
            TimeoutError, subprocess.SubprocessError, tarfile.TarError):
        print("Dependency SDK registry unavailable or incompatible; retain source build")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

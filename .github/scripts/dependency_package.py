"""Create and verify relocatable dependency SDK payloads (no network access).

The producer stages libraries, headers, host tools, notices and fixtures in one
directory. Consumers supply their independently computed build identity. Only a
fully verified package is published to the destination; callers may fall back to
source builds on any error. Storage authentication remains the transport's job.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile


MANIFEST = "horo-sdk.json"
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_EXPANDED_BYTES = 16 * 1024 * 1024 * 1024
IDENTITY_FIELDS = {
    "platform", "architecture", "compiler", "sdk", "abi", "configuration",
    "dependencies", "recipe", "instrumentation", "features",
}


def canonical_identity(identity: dict) -> str:
    """Reject incomplete identities; callers own the exact ABI/recipe inputs."""
    if not isinstance(identity, dict) or set(identity) != IDENTITY_FIELDS:
        raise ValueError("SDK identity must contain every required compatibility field")
    if any(value is None or value == "" for value in identity.values()):
        raise ValueError("SDK identity fields must be explicit")
    return json.dumps(identity, sort_keys=True, separators=(",", ":"), allow_nan=False)


def portable_path(name: str) -> str:
    """Use a common path subset so archives cannot escape on any native runner."""
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError("Invalid SDK member path")
    path = PurePosixPath(name)
    if not path.parts or path.is_absolute() or path.as_posix() != name:
        raise ValueError("SDK paths must be normalized and relative")
    for part in path.parts:
        if part in {".", ".."} or part.endswith((".", " ")):
            raise ValueError("Invalid SDK path component")
        if re.search(r'[\x00-\x1f<>"|?*]', part):
            raise ValueError("Non-portable SDK path component")
        if re.fullmatch(r"(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])", part.split(".")[0]):
            raise ValueError("Reserved SDK path component")
    return name


def digest_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def unique_object(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate SDK manifest property")
        result[key] = value
    return result


def pack(root: Path, identity: dict, output: Path) -> None:
    canonical_identity(identity)
    root = root.resolve(strict=True)
    if output.resolve().is_relative_to(root):
        raise ValueError("SDK output must be outside the staging directory")
    records = {}
    folded = set()
    total = 0
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("SDK staging must materialize symlinks before packaging")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError("SDK staging contains a special file")
        name = portable_path(path.relative_to(root).as_posix())
        if name.casefold() in folded or name.casefold() == MANIFEST.casefold():
            raise ValueError("SDK staging contains a duplicate or reserved path")
        folded.add(name.casefold())
        size = path.stat().st_size
        total += size
        if total > MAX_EXPANDED_BYTES:
            raise ValueError("SDK exceeds expanded size limit")
        records[name] = {"size": size, "sha256": digest_file(path),
                         "executable": bool(path.stat().st_mode & 0o111)}
    manifest = json.dumps({"schema": 1, "identity": identity, "files": records},
                          sort_keys=True, allow_nan=False).encode("utf-8")
    if not records or len(manifest) > MAX_MANIFEST_BYTES:
        raise ValueError("SDK must have payload files and a bounded manifest")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        manifest_path = Path(temporary) / MANIFEST
        manifest_path.write_bytes(manifest)
        archive_path = Path(temporary) / "sdk.tar.gz"
        with tarfile.open(archive_path, "w:gz", compresslevel=1) as archive:
            archive.add(manifest_path, arcname=MANIFEST, recursive=False)
            for name in records:
                archive.add(root / name, arcname=name, recursive=False)
        os.replace(archive_path, output)


def read_manifest(archive: tarfile.TarFile, identity: dict) -> dict:
    member = archive.next()
    if member is None or member.name != MANIFEST or not member.isfile():
        raise ValueError("SDK must begin with a regular manifest")
    if member.size > MAX_MANIFEST_BYTES:
        raise ValueError("SDK manifest exceeds size limit")
    with archive.extractfile(member) as stream:
        manifest = json.load(stream, object_pairs_hook=unique_object)
    return validate_manifest(manifest, identity)


def validate_manifest(manifest: dict, identity: dict) -> dict:
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        raise ValueError("Unsupported SDK schema")
    if canonical_identity(manifest.get("identity")) != canonical_identity(identity):
        raise ValueError("SDK compatibility identity mismatch")
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        raise ValueError("SDK manifest has no payload")
    folded = {MANIFEST.casefold()}
    total = 0
    for name, record in files.items():
        portable_path(name)
        if name.casefold() in folded:
            raise ValueError("SDK manifest contains colliding paths")
        folded.add(name.casefold())
        if not isinstance(record, dict) or set(record) != {"size", "sha256", "executable"}:
            raise ValueError("Invalid SDK file record")
        size = record["size"]
        if type(size) is not int or size < 0 or type(record["executable"]) is not bool:
            raise ValueError("Invalid SDK file metadata")
        if not isinstance(record["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]):
            raise ValueError("Invalid SDK file digest")
        total += size
    if total > MAX_EXPANDED_BYTES:
        raise ValueError("SDK exceeds expanded size limit")
    return files


def verify(root: Path, identity: dict) -> None:
    """Verify a restored package again before CMake reuses it on reconfigure."""
    receipt = root / MANIFEST
    if root.is_symlink() or receipt.is_symlink() or receipt.stat().st_size > MAX_MANIFEST_BYTES:
        raise ValueError("Invalid SDK receipt")
    files = validate_manifest(json.loads(receipt.read_text(encoding="utf-8"),
                                        object_pairs_hook=unique_object), identity)
    seen = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("Restored SDK contains a symlink")
        if path.is_dir():
            continue
        name = path.relative_to(root).as_posix()
        if name == MANIFEST:
            continue
        if not path.is_file() or name not in files:
            raise ValueError("Restored SDK contains an unexpected file")
        if path.stat().st_size != files[name]["size"] or digest_file(path) != files[name]["sha256"]:
            raise ValueError("Restored SDK file mismatch")
        seen.add(name)
    if seen != set(files):
        raise ValueError("Restored SDK is missing files")


def restore(archive_path: Path, identity: dict, destination: Path) -> None:
    canonical_identity(identity)
    if destination.exists() or destination.is_symlink():
        raise ValueError("SDK destination must be absent; never overwrite a live package")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        staged = Path(temporary) / "payload"
        staged.mkdir()
        with tarfile.open(archive_path, "r|gz") as archive:
            files = read_manifest(archive, identity)
            seen = set()
            # next() avoids re-reading the first manifest returned by tarfile's iterator.
            while (member := archive.next()) is not None:
                name = portable_path(member.name)
                if not member.isfile() or name not in files or name in seen:
                    raise ValueError("Unexpected, duplicate or non-regular SDK member")
                record = files[name]
                if member.size != record["size"]:
                    raise ValueError("SDK member size mismatch")
                target = staged / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output, length=1024 * 1024)
                if digest_file(target) != record["sha256"]:
                    raise ValueError("SDK member digest mismatch")
                target.chmod(0o755 if record["executable"] else 0o644)
                seen.add(name)
            if seen != set(files):
                raise ValueError("SDK archive is missing manifest members")
        (staged / MANIFEST).write_text(
            json.dumps({"schema": 1, "identity": identity, "files": files}, sort_keys=True),
            encoding="utf-8")
        # Rename stays on one filesystem. No partial package is exposed to CMake.
        staged.rename(destination)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    producer = commands.add_parser("pack")
    producer.add_argument("--root", type=Path, required=True)
    producer.add_argument("--output", type=Path, required=True)
    consumer = commands.add_parser("restore")
    consumer.add_argument("--archive", type=Path, required=True)
    consumer.add_argument("--destination", type=Path, required=True)
    verifier = commands.add_parser("verify")
    verifier.add_argument("--root", type=Path, required=True)
    for command in (producer, consumer, verifier):
        command.add_argument("--identity", type=Path, required=True)
    args = parser.parse_args()
    identity = json.loads(args.identity.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    if args.command == "pack":
        pack(args.root, identity, args.output)
    elif args.command == "restore":
        restore(args.archive, identity, args.destination)
    else:
        verify(args.root, identity)


if __name__ == "__main__":
    main()

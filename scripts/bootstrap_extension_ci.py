#!/usr/bin/env python3
"""Fetch one digest-pinned extension SDK and run its source-free CI tool."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform as host_platform
import re
import subprocess  # nosec B404 - the digest-verified SDK tool is the intended child.
import sys
import tempfile
import urllib.request
import zipfile


MAXIMUM_ARCHIVE_BYTES = 256 * 1024 * 1024
MAXIMUM_EXPANDED_BYTES = 512 * 1024 * 1024
MAXIMUM_ENTRIES = 4096
PLATFORMS = ("linux-x64", "macos-arm64", "macos-x64", "windows-x64")
HOSTS = {
    "linux-x64": ("Linux", ("x86_64", "AMD64")),
    "macos-arm64": ("Darwin", ("arm64", "aarch64")),
    "macos-x64": ("Darwin", ("x86_64",)),
    "windows-x64": ("Windows", ("AMD64", "x86_64")),
}


def canonical_platform(value: str) -> str:
    # Return a literal from the supported matrix, never the caller's raw argv.
    if value == "linux-x64":
        return "linux-x64"
    if value == "macos-arm64":
        return "macos-arm64"
    if value == "macos-x64":
        return "macos-x64"
    if value == "windows-x64":
        return "windows-x64"
    raise ValueError("unsupported extension CI platform")


def canonical_commit(value: str) -> str:
    if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise ValueError("invalid source commit attribution")
    # Pass a fresh, fixed-width hexadecimal value to the SDK runner, not raw CLI text.
    return f"{int(value, 16):040x}"


def canonical_repository(value: str) -> str:
    match = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9_.-]*)/([A-Za-z0-9_.-]+)", value)
    if match is None:
        raise ValueError("invalid repository attribution")
    owner, name = match.groups()
    # Reconstruct the bounded identifier rather than forwarding the raw CLI field.
    return f"{owner}/{name}"


def read_lock(project: Path, platform: str) -> tuple[str, str, str]:
    document = json.loads((project / ".horo/extension-ci.lock.json").read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 1 or not isinstance(document.get("sdkVersion"), str):
        raise ValueError("invalid extension CI lock schema or SDK version")
    platforms = document.get("platforms")
    if platform not in PLATFORMS or not isinstance(platforms, dict) or set(platforms) != set(PLATFORMS):
        raise ValueError("extension CI lock must cover exactly the four supported platform/architecture tuples")
    entry = platforms[platform]
    if not isinstance(entry, dict):
        raise ValueError("extension CI lock platform entry must be an object")
    url, digest = entry.get("url"), entry.get("sha256")
    if not isinstance(url, str) or not url.startswith("https://") or "REPLACE_" in url:
        raise ValueError("pin the HTTPS SDK archive URL in .horo/extension-ci.lock.json")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise ValueError("pin the lowercase SHA-256 SDK archive digest")
    return document["sdkVersion"], url, digest


def fetch_archive(url: str, digest: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "horo-extension-author-ci/1"})
    with urllib.request.urlopen(request, timeout=60) as response:  # nosec B310 - HTTPS and digest are required.
        if not response.geturl().startswith("https://"):
            raise ValueError("SDK download redirected outside HTTPS")
        data = response.read(MAXIMUM_ARCHIVE_BYTES + 1)
    if len(data) > MAXIMUM_ARCHIVE_BYTES:
        raise ValueError("SDK archive exceeds the bounded download size")
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("SDK archive SHA-256 does not match the lock")
    return data


def safe_member_path(member: zipfile.ZipInfo, seen: set[str]) -> PurePosixPath:
    path = PurePosixPath(member.filename)
    mode = member.external_attr >> 16
    components = member.filename.rstrip("/").split("/")
    if (not member.filename or "\\" in member.filename or path.is_absolute() or
            any(part in ("", ".", "..") for part in components) or
            ":" in components[0] or path.as_posix() != member.filename.rstrip("/") or
            (mode & 0o170000) not in (0, 0o100000, 0o040000) or member.filename.casefold() in seen):
        raise ValueError("SDK archive contains an unsafe or duplicate path")
    if member.is_dir() != ((mode & 0o170000) == 0o040000) and (mode & 0o170000) != 0:
        raise ValueError("SDK archive entry type disagrees with its path")
    seen.add(member.filename.casefold())
    return path


def extract_member(archive: zipfile.ZipFile, member: zipfile.ZipInfo, destination: Path,
                   path: PurePosixPath) -> None:
    target = destination.joinpath(*path.parts)
    if member.is_dir():
        target.mkdir(parents=True, exist_ok=True)
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, target.open("xb") as output:
        while block := source.read(1024 * 1024):
            output.write(block)
    mode = member.external_attr >> 16
    if os.name != "nt" and (mode & 0o170000) == 0o100000:
        target.chmod(mode & 0o777)


def extract_sdk(data: bytes, destination: Path) -> None:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        members = archive.infolist()
        if len(members) > MAXIMUM_ENTRIES or sum(member.file_size for member in members) > MAXIMUM_EXPANDED_BYTES:
            raise ValueError("SDK archive exceeds bounded extraction limits")
        seen: set[str] = set()
        for member in members:
            path = safe_member_path(member, seen)
            extract_member(archive, member, destination, path)


def author_paths(args: argparse.Namespace) -> tuple[Path, Path]:
    project = args.project.resolve(strict=True)
    if not project.is_dir():
        raise ValueError("author project must be a directory")
    lock = project / ".horo/extension-ci.lock.json"
    output = project / "ci-artifacts"
    if args.lock.absolute() != lock or lock.resolve(strict=True) != lock:
        raise ValueError("extension CI lock must be the author project's .horo lock")
    if args.output.absolute() != output or output.is_symlink():
        raise ValueError("extension CI output must be the author project's ci-artifacts")
    if re.fullmatch(r"[0-9a-f]{40}", args.commit) is None or \
            re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) is None:
        raise ValueError("invalid source commit or repository attribution")
    return project, output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--platform", choices=PLATFORMS, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    try:
        platform = canonical_platform(args.platform)
        operating_system, machines = HOSTS[platform]
        if host_platform.system() != operating_system or host_platform.machine() not in machines:
            raise ValueError("CI runner architecture does not match the locked platform")
        project, output = author_paths(args)
        commit = canonical_commit(args.commit)
        repository = canonical_repository(args.repository)
        version, url, digest = read_lock(project, platform)
        with tempfile.TemporaryDirectory(prefix="horo-extension-sdk-") as temporary:
            root = Path(temporary)
            extract_sdk(fetch_archive(url, digest), root)
            metadata = json.loads((root / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8"))
            if metadata["sdk"]["version"] != version:
                raise ValueError("downloaded SDK version differs from the lock")
            runner = root / "bin/horo-extension-author-ci.py"
            if not runner.is_file() or runner.is_symlink():
                raise ValueError("downloaded SDK does not contain the author CI tool")
            command = [sys.executable, str(runner), "--sdk", str(root), "--project", str(project),
                       "--output", str(output), "--platform", platform,
                       "--repository", repository, "--commit", commit,
                       "--sdk-sha256", digest]
            # The runner came from the SHA-256-pinned archive, arguments are separate
            # argv values, and no user-provided text is interpreted by a shell.
            # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args,python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
            return subprocess.run(command, check=False, shell=False).returncode  # nosec B603
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"extension CI bootstrap: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

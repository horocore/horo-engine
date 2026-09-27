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


def read_lock(path: Path, platform: str) -> tuple[str, str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
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


def extract_sdk(data: bytes, destination: Path) -> None:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        members = archive.infolist()
        if len(members) > MAXIMUM_ENTRIES or sum(member.file_size for member in members) > MAXIMUM_EXPANDED_BYTES:
            raise ValueError("SDK archive exceeds bounded extraction limits")
        seen: set[str] = set()
        for member in members:
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
            target = destination.joinpath(*path.parts)
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as source, target.open("xb") as output:
                    while block := source.read(1024 * 1024):
                        output.write(block)
                if os.name != "nt" and (mode & 0o170000) == 0o100000:
                    target.chmod(mode & 0o777)


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
        operating_system, machines = HOSTS[args.platform]
        if host_platform.system() != operating_system or host_platform.machine() not in machines:
            raise ValueError("CI runner architecture does not match the locked platform")
        version, url, digest = read_lock(args.lock, args.platform)
        with tempfile.TemporaryDirectory(prefix="horo-extension-sdk-") as temporary:
            root = Path(temporary)
            extract_sdk(fetch_archive(url, digest), root)
            metadata = json.loads((root / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8"))
            if metadata["sdk"]["version"] != version:
                raise ValueError("downloaded SDK version differs from the lock")
            runner = root / "bin/horo-extension-author-ci.py"
            if not runner.is_file():
                raise ValueError("downloaded SDK does not contain the author CI tool")
            command = [sys.executable, str(runner), "--sdk", str(root), "--project", str(args.project),
                       "--output", str(args.output), "--platform", args.platform,
                       "--repository", args.repository, "--commit", args.commit,
                       "--sdk-sha256", digest]
            # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
            return subprocess.run(command, check=False).returncode  # nosec B603
    except (OSError, ValueError, KeyError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        print(f"extension CI bootstrap: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

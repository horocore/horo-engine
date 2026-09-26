#!/usr/bin/env python3
"""Fail a published binary build when tag, product, body, and compiled notes diverge."""

import argparse
import json
import os
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen
from pathlib import Path

from parse_changelog import NotesError, MAX_SNAPSHOT_BYTES, version_parts


def verify(snapshot_bytes: bytes, tag: str, cmake_text: str, release_body: str) -> None:
    if not tag.startswith("v"):
        raise NotesError("release tag must use v<exact-semver>")
    version = tag[1:]
    version_parts(version)
    match = re.search(r'^set\(HORO_ENGINE_VERSION "([^"]+)"\)$', cmake_text, re.MULTILINE)
    if not match or match.group(1) != version:
        raise NotesError(f"release tag {tag} does not match HORO_ENGINE_VERSION")
    if not snapshot_bytes or len(snapshot_bytes) > MAX_SNAPSHOT_BYTES:
        raise NotesError("reviewed notes snapshot is missing or oversized")
    try:
        snapshot = json.loads(snapshot_bytes)
    except (UnicodeError, ValueError) as error:
        raise NotesError("reviewed notes snapshot is malformed") from error
    if not isinstance(snapshot, dict):
        raise NotesError("reviewed notes snapshot must be an object")
    if (snapshot.get("schemaVersion") != 1 or snapshot.get("product") != "horo-editor"
        or snapshot.get("version") != version or snapshot.get("locale") != "en-US"):
        raise NotesError("reviewed notes snapshot product or exact version differs from release tag")
    if release_body.replace("\r\n", "\n").strip() != snapshot.get("markdown", "").strip():
        raise NotesError("GitHub Release body must equal the reviewed snapshot Markdown")


def release_body(tag: str) -> str:
    if not tag.startswith("v"):
        raise NotesError("release tag must use v<exact-semver>")
    version_parts(tag[1:])
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    token = os.environ.get("GH_TOKEN", "")
    parts = repository.split("/")
    if (len(parts) != 2 or not all(part and all(char.isascii() and
        (char.isalnum() or char in "._-") for char in part) for part in parts) or not token):
        raise NotesError("GITHUB_REPOSITORY and GH_TOKEN are required for release verification")
    address = f"https://api.github.com/repos/{repository}/releases/tags/{quote(tag, safe='')}"
    request = Request(address, headers={"Accept": "application/vnd.github+json",
                                        "Authorization": f"Bearer {token}",
                                        "User-Agent": "horo-release-notes-verifier"})
    try:
        with urlopen(request, timeout=30) as response:
            data = json.load(response)
    except (HTTPError, URLError, ValueError) as error:
        raise NotesError(f"cannot read GitHub Release for {tag}: {error}") from error
    if not isinstance(data, dict) or not isinstance(data.get("body"), str):
        raise NotesError(f"GitHub Release for {tag} has no reviewed body")
    return data["body"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, default=Path("CMakeLists.txt"))
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    try:
        verify(args.snapshot.read_bytes(), args.tag, args.cmake.read_text(encoding="utf-8"), release_body(args.tag))
    except (OSError, NotesError) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

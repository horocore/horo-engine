#!/usr/bin/env python3
"""Fail a published binary build when tag, product, body, and compiled notes diverge."""

import argparse
import json
import re
import subprocess
import sys
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, default=Path("CMakeLists.txt"))
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    try:
        body = subprocess.run(
            ["gh", "release", "view", args.tag, "--json", "body", "--jq", ".body"],
            check=True, capture_output=True, text=True, timeout=30
        ).stdout
        verify(args.snapshot.read_bytes(), args.tag, args.cmake.read_text(encoding="utf-8"), body)
    except (OSError, subprocess.SubprocessError, NotesError) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Freeze reviewed Keep a Changelog content for both Welcome and distribution."""

import argparse
import calendar
import datetime
import json
import re
import sys
from pathlib import Path

MAX_SOURCE_BYTES = 262144
MAX_SNAPSHOT_BYTES = 32768
MAX_ITEM_BYTES = 2048
MAX_ITEMS = 64
CATEGORIES = ("Added", "Changed", "Deprecated", "Removed", "Fixed", "Security")
HEADER = re.compile(r"^## \[([^\]]+)\](?: [—-] (\d{4}-\d{2}-\d{2}))?$", re.MULTILINE)
SEMVER = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\Z"
)


class NotesError(ValueError):
    """Actionable release-notes validation failure."""


def version_parts(text: str) -> tuple:
    match = SEMVER.fullmatch(text)
    if not match or len(text) > 128:
        raise NotesError(f"invalid semantic version '{text}'")
    if any(int(part) > 0xffffffff for part in match.group(1, 2, 3)):
        raise NotesError(f"semantic version core overflows 32 bits: '{text}'")
    if match.group(4) and any(
        len(part) > 1 and part[0] == "0"
        for part in match.group(4).split(".") if part.isdigit()
    ):
        raise NotesError(f"noncanonical prerelease version '{text}'")
    return tuple(map(int, match.group(1, 2, 3))) + (match.group(4) or "", match.group(5) or "")


def parse_changelog(text: str) -> list[dict]:
    """Parse bounded released entries; Unreleased is never candidate content."""
    if len(text.encode("utf-8")) > MAX_SOURCE_BYTES:
        raise NotesError(f"CHANGELOG.md exceeds {MAX_SOURCE_BYTES} bytes")
    headings = list(HEADER.finditer(text))
    if not headings:
        raise NotesError("CHANGELOG.md has no version headings")
    entries = []
    seen = set()
    for index, heading in enumerate(headings):
        version, date = heading.groups()
        if version == "Unreleased":
            if date:
                raise NotesError("[Unreleased] must not have a date")
            continue
        version_parts(version)
        if version in seen:
            raise NotesError(f"duplicate release notes for {version}")
        seen.add(version)
        try:
            if not date:
                raise ValueError()
            datetime.date.fromisoformat(date)
        except ValueError as error:
            raise NotesError(f"{version}: missing or invalid ISO release date") from error
        end = headings[index + 1].start() if index + 1 < len(headings) else len(text)
        sections = []
        current = None
        count = 0
        for raw in text[heading.end():end].splitlines():
            line = raw.strip()
            if not line:
                continue
            if line.startswith("### "):
                category = line[4:]
                if category not in CATEGORIES or any(part["category"] == category for part in sections):
                    raise NotesError(f"{version}: unknown or repeated category '{category}'")
                if len(sections) == len(CATEGORIES):
                    raise NotesError(f"{version}: too many categories")
                current = {"category": category, "items": []}
                sections.append(current)
            elif line.startswith("- ") and current is not None:
                item = line[2:].strip()
                count += 1
                if count > MAX_ITEMS or not item or len(item.encode("utf-8")) > MAX_ITEM_BYTES:
                    raise NotesError(f"{version}: missing or oversized note item")
                if (any(ord(char) < 32 or ord(char) == 127 for char in item)
                    or "<" in item or ">" in item
                    or re.search(r"!\[|\]\((?!https://)", item)
                    or item.count("`") % 2 or item.count("[") != item.count("]")
                    or item.count("(") != item.count(")")):
                    raise NotesError(f"{version}: unsafe or malformed Markdown item")
                current["items"].append(item)
            else:
                raise NotesError(f"{version}: expected category heading or bullet near '{line[:64]}'")
        if not sections or any(not part["items"] for part in sections):
            raise NotesError(f"{version}: every category must have bullets")
        entries.append({"version": version, "date": date, "sections": sections})
    return entries


def select_version(entries: list[dict], version: str) -> dict:
    version_parts(version)
    for entry in entries:
        if entry["version"] == version:
            return entry
    raise NotesError(f"missing release notes for exact candidate version {version}")


def select_range(entries: list[dict], lower: str, upper: str) -> list[dict]:
    """Select released notes in (lower, upper], excluding Unreleased."""
    def precedence(version: str) -> tuple:
        major, minor, patch, prerelease, _build = version_parts(version)
        identifiers = tuple((0, int(part)) if part.isdigit() else (1, part)
                            for part in prerelease.split(".")) if prerelease else ()
        return major, minor, patch, bool(not prerelease), identifiers

    low, high = precedence(lower), precedence(upper)
    if low >= high:
        raise NotesError("release notes range must advance")
    return [entry for entry in entries if low < precedence(entry["version"]) <= high]


def render_markdown(entry: dict) -> str:
    lines = [f'## [{entry["version"]}] — {entry["date"]}']
    for section in entry["sections"]:
        lines += ["", f'### {section["category"]}']
        lines += [f"- {item}" for item in section["items"]]
    return "\n".join(lines) + "\n"


def snapshot_bytes(snapshot: dict) -> bytes:
    return (json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def make_snapshot(entry: dict, product: str, locale: str = "en-US") -> dict:
    if not re.fullmatch(r"[a-z][a-z0-9-]{1,63}", product):
        raise NotesError("product identity must be a bounded lowercase slug")
    if not re.fullmatch(r"[a-z]{2}-[A-Z]{2}", locale):
        raise NotesError("locale must be a language-region tag such as en-US")
    snapshot = {"schemaVersion": 1, "product": product, "version": entry["version"],
                "locale": locale, "date": entry["date"], "sections": entry["sections"],
                "markdown": render_markdown(entry)}
    if len(snapshot_bytes(snapshot)) > MAX_SNAPSHOT_BYTES:
        raise NotesError(f'{entry["version"]}: snapshot exceeds {MAX_SNAPSHOT_BYTES} bytes')
    return snapshot


def render_header(snapshot: dict) -> str:
    """Produce Welcome cards from exactly the frozen candidate snapshot."""
    items = [item for section in snapshot["sections"] for item in section["items"]][:2]
    items += [""] * (2 - len(items))
    title = f'v{snapshot["version"]} — {calendar.month_name[int(snapshot["date"][5:7])]} {snapshot["date"][:4]}'
    cards = [f'    {{"Release Notes", {json.dumps(title, ensure_ascii=False)}, '
             f'{json.dumps(item[:120], ensure_ascii=False)}}}' for item in items]
    return "\n".join((
        "// Generated from the reviewed release-notes snapshot. Do not edit.",
        "#pragma once", "", "namespace Horo::Generated {",
        "struct WhatsNewEntry { const char* tag; const char* title; const char* body; };",
        "inline constexpr int kWhatsNewCount = 2;",
        "inline constexpr WhatsNewEntry kWhatsNewEntries[2] = {",
        ",\n".join(cards), "};", "} // namespace Horo::Generated", ""
    ))


def safe_output(raw: Path) -> Path:
    """Reject traversal and symlink redirection before creating generated files."""
    if ".." in raw.parts:
        raise NotesError(f"unsafe output path: {raw}")
    path = raw if raw.is_absolute() else Path.cwd() / raw
    if any(part.is_symlink() for part in (path, *path.parents)):
        raise NotesError(f"output path traverses a symlink: {raw}")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--product", required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()
    try:
        if not args.source.is_file() or args.source.stat().st_size > MAX_SOURCE_BYTES:
            raise NotesError("CHANGELOG.md is missing or oversized")
        entry = select_version(parse_changelog(args.source.read_text(encoding="utf-8")), args.version)
        snapshot = make_snapshot(entry, args.product)
        args.snapshot = safe_output(args.snapshot)
        args.header = safe_output(args.header)
        for output in (args.snapshot, args.header):
            output.parent.mkdir(parents=True, exist_ok=True)
        args.snapshot.write_bytes(snapshot_bytes(snapshot))
        args.header.write_text(render_header(snapshot), encoding="utf-8")
    except (OSError, UnicodeError, NotesError) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

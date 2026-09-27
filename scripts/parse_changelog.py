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


class NotesError(ValueError):
    """Actionable release-notes validation failure."""


def parse_version_core(core: str, text: str) -> tuple[int, int, int]:
    components = core.split(".")
    if len(components) != 3:
        raise NotesError(f"invalid semantic version '{text}'")
    for part in components:
        if not part.isascii() or not part.isdecimal() or (len(part) > 1 and part[0] == "0"):
            raise NotesError(f"invalid semantic version '{text}'")
    values = tuple(map(int, components))
    if any(value > 0xffffffff for value in values):
        raise NotesError(f"semantic version core overflows 32 bits: '{text}'")
    return values


def validate_identifiers(identifiers: str, numeric_leading_zero: bool, text: str) -> None:
    for part in identifiers.split("."):
        if not part or not part.isascii() or not all(char.isalnum() or char == "-" for char in part):
            raise NotesError(f"invalid semantic version '{text}'")
        if numeric_leading_zero and part.isdecimal() and len(part) > 1 and part[0] == "0":
            raise NotesError(f"noncanonical prerelease version '{text}'")


def version_parts(text: str) -> tuple:
    if not text or len(text) > 128:
        raise NotesError(f"invalid semantic version '{text[:128]}'")
    core_and_prerelease, separator, build = text.partition("+")
    core, prerelease_separator, prerelease = core_and_prerelease.partition("-")
    if (separator and not build) or (prerelease_separator and not prerelease):
        raise NotesError(f"invalid semantic version '{text}'")
    values = parse_version_core(core, text)
    if prerelease_separator:
        validate_identifiers(prerelease, True, text)
    if separator:
        validate_identifiers(build, False, text)
    return values + (prerelease, build)


def unsafe_markdown(item: str) -> bool:
    return (any(ord(char) < 32 or ord(char) == 127 for char in item)
            or "<" in item or ">" in item or re.search(r"!\[|\]\((?!https://)", item) is not None
            or item.count("`") % 2 != 0 or item.count("[") != item.count("]")
            or item.count("(") != item.count(")"))


def validate_item(item: str, version: str) -> str:
    if not item or len(item.encode("utf-8")) > MAX_ITEM_BYTES:
        raise NotesError(f"{version}: missing or oversized note item")
    if unsafe_markdown(item):
        raise NotesError(f"{version}: unsafe or malformed Markdown item")
    return item


def validate_date(version: str, date: str | None) -> None:
    try:
        if not date:
            raise ValueError()
        datetime.date.fromisoformat(date)
    except ValueError as error:
        raise NotesError(f"{version}: missing or invalid ISO release date") from error


def start_category(line: str, version: str, sections: list[dict]) -> dict:
    category = line[4:]
    if category not in CATEGORIES or any(part["category"] == category for part in sections):
        raise NotesError(f"{version}: unknown or repeated category '{category}'")
    section = {"category": category, "items": []}
    sections.append(section)
    return section


def parse_sections(version: str, lines: str) -> list[dict]:
    sections = []
    current = None
    count = 0
    for raw in lines.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("### "):
            current = start_category(line, version, sections)
        elif line.startswith("- ") and current is not None:
            count += 1
            if count > MAX_ITEMS:
                raise NotesError(f"{version}: too many note items")
            current["items"].append(validate_item(line[2:].strip(), version))
        else:
            raise NotesError(f"{version}: expected category heading or bullet near '{line[:64]}'")
    if not sections or any(not part["items"] for part in sections):
        raise NotesError(f"{version}: every category must have bullets")
    return sections


def parse_entry(version: str, date: str | None, lines: str) -> dict:
    version_parts(version)
    validate_date(version, date)
    return {"version": version, "date": date, "sections": parse_sections(version, lines)}


def release_headings(text: str) -> list[re.Match]:
    for line in text.splitlines():
        if line.startswith("## ") and HEADER.fullmatch(line) is None:
            raise NotesError(f"malformed release heading: {line[:80]}")
    headings = list(HEADER.finditer(text))
    if not headings:
        raise NotesError("CHANGELOG.md has no version headings")
    return headings


def parse_changelog(text: str) -> list[dict]:
    """Parse bounded released entries; Unreleased is never candidate content."""
    if len(text.encode("utf-8")) > MAX_SOURCE_BYTES:
        raise NotesError(f"CHANGELOG.md exceeds {MAX_SOURCE_BYTES} bytes")
    headings = release_headings(text)
    entries = []
    seen = set()
    for index, heading in enumerate(headings):
        version, date = heading.groups()
        if version == "Unreleased":
            if date:
                raise NotesError("[Unreleased] must not have a date")
            continue
        if version in seen:
            raise NotesError(f"duplicate release notes for {version}")
        seen.add(version)
        end = headings[index + 1].start() if index + 1 < len(headings) else len(text)
        entries.append(parse_entry(version, date, text[heading.end():end]))
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


def generated_outputs() -> tuple[Path, Path]:
    """Keep generated output names fixed beneath the invocation's build directory."""
    directory = Path.cwd() / "generated"
    if directory.is_symlink():
        raise NotesError("generated output directory must not be a symlink")
    directory.mkdir(exist_ok=True)
    if not directory.is_dir():
        raise NotesError("generated output directory is not a directory")
    snapshot = directory / "ReleaseNotesSnapshot.json"
    header = directory / "GeneratedBuildInfo.h"
    if snapshot.is_symlink() or header.is_symlink():
        raise NotesError("generated output must not be a symlink")
    return snapshot, header


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--product", required=True)
    args = parser.parse_args()
    try:
        if not args.source.is_file() or args.source.stat().st_size > MAX_SOURCE_BYTES:
            raise NotesError("CHANGELOG.md is missing or oversized")
        entry = select_version(parse_changelog(args.source.read_text(encoding="utf-8")), args.version)
        snapshot = make_snapshot(entry, args.product)
        snapshot_path, header_path = generated_outputs()
        snapshot_path.write_bytes(snapshot_bytes(snapshot))
        header_path.write_text(render_header(snapshot), encoding="utf-8")
    except (OSError, UnicodeError, NotesError) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

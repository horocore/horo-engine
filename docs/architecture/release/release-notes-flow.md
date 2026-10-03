# Release Notes Flow

## Authority and selection

`CHANGELOG.md` is the reviewed Keep a Changelog source. A release entry uses
`## [exact-semver] — YYYY-MM-DD` and one or more of the standard
`Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, or `Security`
categories, each with nonempty bullets. `[Unreleased]` is not candidate
content. Selection uses full SemVer identity, including prerelease and build
metadata; a range query selects released entries in `(lower, upper]`.

`scripts/parse_changelog.py` validates this bounded source before it emits a
UTF-8 `ReleaseNotesSnapshot.json`. Missing, duplicate, malformed, unsafe
Markdown, invalid dates, and oversized content fail with source-context
diagnostics. The snapshot schema is:

```json
{
  "schemaVersion": 1,
  "product": "horo-editor",
  "version": "0.1.0",
  "locale": "en-US",
  "date": "2026-06-01",
  "sections": [{"category": "Added", "items": ["Reviewed note."]}],
  "markdown": "## [0.1.0] — 2026-06-01\n\n### Added\n- Reviewed note.\n"
}
```

The product and locale fields are explicit presentation metadata. Categories
remain stable translation keys; the reviewed item text is not translated or
rewritten by a consuming surface. The snapshot is limited to 32 KiB, 64 items,
and 2 KiB per item. The source file is limited to 256 KiB.

## Candidate freeze and consumers

`ReleasePreflight` receives the host-read snapshot bytes, validates its
structure, exact candidate version and product, and owns a copy in
`ReleaseExecutionPlan`. Candidate metadata serializes that copy and its
SHA-256 digest. The digest covers the exact UTF-8 snapshot bytes. A fresh
observation with a different digest fails input-freeze validation; editing the
source cannot silently alter an existing candidate. A host must not reconstruct
notes from `CHANGELOG.md` after candidate creation.

CMake generates `build/generated/ReleaseNotesSnapshot.json` and
`GeneratedBuildInfo.h` together from one source selection for the configured
`HORO_ENGINE_VERSION`. Welcome cards are a two-item projection of the
snapshot; HoroEditor reads only compiled literals at runtime. Distribution
packages carry the same snapshot as `release-notes.json`, which update and
installation surfaces may read from the installed artifact, not from the
repository. Release Binaries validates tag, CMake version, snapshot identity,
and the published GitHub Release body before building or packaging, then
verifies that each produced archive contains the exact snapshot bytes before
upload. The published body must equal the snapshot's reviewed Markdown.

There is no silent fallback when Python or candidate notes are unavailable:
configuration fails with an actionable diagnostic. Maintainers update the
version and changelog together before tagging, then paste the selected
snapshot Markdown into the GitHub Release body. The release decision remains
manual; notes validation does not create a tag, release, or publication.

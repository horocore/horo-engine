# Changelog

All notable changes to Horo Engine are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.4.0] — 2026-11-13
### Added
- Development snapshot with project-owned Platform Services identity, definition documents and portable provider policy.
- Recoverable adoption of legacy projects into the Platform Services authoring format without replacing existing project identities.
- Headless Platform Services validation and deterministic paired manifest cooking with source-revision checks before publication.

### Changed
- Project creation uses an explicitly supplied durable filesystem and preserves published projects when durability requires reconciliation.
- Native consumers must rebuild for the project creation, migration, filesystem and Platform Services source snapshot contracts.

## [0.3.0] — 2026-11-01
### Added
- Development snapshot with a project-owned semantic surface catalog, separate from physical materials and presentation assets.
- Journaled adoption of missing catalogs without inferring surfaces or changing existing project identities.
- Bounded D3D12 graph-copy command submission, explicit legacy/enhanced barrier admission, queue fences and GPU-safe resource retirement.
- Opt-in development shader hot reload with bounded asynchronous compilation, complete program-family adoption and last-known-good preservation.
- Bounded camera-relative cascaded directional shadows, guarded atlas profiles, native depth/material batches and GPU-safe atlas retirement.

### Changed
- Gameplay SDK 25 requires rebuilding native gameplay modules and descriptor bundles for the new surface and Character value contracts.

## [0.2.0] — 2026-07-10
### Added
- New Project wizard with template selection, dynamic inputs, and project creation pipeline.
- Welcome screen "What's New" section now driven by CHANGELOG.md at build time.
- `ScopedCard` auto-resize-Y mode to eliminate overflow in variable-height cards.
- Recent projects persisted to `~/.horo/recent_projects.json` and reloaded on every return to Welcome.

### Fixed
- Project creation "Create Project" button now reliably triggers and updates recent projects list.
- JSON regex in recent projects parser changed to non-greedy to prevent field bleed.

## [0.1.0] — 2026-06-01
### Added
- Initial HoroEditor welcome screen with recent projects list and action buttons.
- New Project modal with Settings and Review tabs.
- Settings modal with editor preferences and theme controls.
- Project creation service with background job system and progress reporting.
- `~/.horo/editor_settings.json` persistence for editor configuration.

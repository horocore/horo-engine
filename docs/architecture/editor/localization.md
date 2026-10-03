# Localization Architecture

## Purpose

This document defines localization resources, message identity, formatting,
locale resolution, runtime switching, fallback, fonts, layout behavior, and
testing for HoroEditor, CLI presentation, and engine-provided game UI strings.

Project content and game-specific localization remain project-owned but may use
the same resource contracts. Engine-provided runtime UI, including default input
prompts, debug overlays, engine error dialogs, and built-in widgets, uses
engine-owned namespaces. Narrative text, quests, item names, game menus, and
project-authored UI use project-owned namespaces.

## Core Decisions

- User-facing engine text uses stable message keys outside low-level diagnostic
  fallback paths.
- Translations are structured resources loaded by locale and namespace.
- Formatting uses typed named arguments and locale-aware rules.
- Live GUI surfaces support locale changes without restarting unless explicitly
  documented as reload-required.
- Missing translations follow deterministic fallback and are observable.
- Internal logs, error codes, protocol fields, and serialized identifiers are
  not localized.
- Layout and components must tolerate expansion, right-to-left text, and font
  fallback.

## Runtime UI Boundary

[ADR-081](../../adr/081-runtime-ui-and-localization-ownership-boundary.md) makes
Localization the authority for catalog schemas, namespaces, locale resolution,
typed formatting, translation fallback and immutable localization snapshots.
Application/game policy selects the requested/default/source locale; Runtime UI
stores typed localized references but never owns a second active locale or opens
catalog files.

Localization returns owned Unicode text, typed spans and locale evidence. Runtime
UI Text owns segmentation, bidi, shaping, line breaking, ADR-075 font fallback and
ADR-074 measurement/layout. Translation fallback, font fallback and localized
visual/audio asset fallback are distinct decisions with separate identities and
failure policy.

Locale/catalog publication is immutable and revisioned. Runtime UI pins one exact
snapshot per text/font/layout generation and may retain its last-good presentation
while a replacement prepares. Editor GUI localization and isolated game preview
use separate locale contexts; neither silently mutates the other.

## Editor Adoption And Dynamic Text Extraction

The editor migration replaces user-facing hardcoded sentences with semantic
message keys. Branding and proper names such as `Horo`, plugin IDs, asset names,
file paths, command names, and other technical identifiers remain explicit data;
they are not translated merely because they are string literals.

Localization is configuration-driven at runtime, but source extraction is a
build/editor-tool concern. The extractor must not perform blind text replacement
or infer translation keys from English prose at render time. It produces a
deterministic report containing:

- source location and owning UI component;
- candidate literal and its classification;
- proposed namespace/key based on the component contract;
- whether the literal is user-facing, technical, branding, or intentionally
  excluded;
- catalog coverage and argument metadata status.

The first extractor implementation may use a conservative source scanner for
known UI call sites, but the production path should use compiler-backed source
locations (for example, a Clang AST tool) once the call-site inventory is stable.
Every automatic candidate requires an explicit classification or allowlist entry.
The extractor is therefore a migration aid and coverage gate, not a runtime
dependency and not an excuse to translate arbitrary identifiers.

The editor runtime owns one `LocalizationService` and one immutable active
snapshot. GUI components request `LocalizedText` by key and resolve it during
presentation. The selected editor locale is an editor setting stored as a
normalized BCP 47 tag (for example, `en-US` or `tr-TR`). A locale change is a
settings transaction: the service validates and prepares the candidate snapshot,
the settings authority commits the user preference, and the GUI activates the
new snapshot at a frame boundary. A failed catalog load must leave both the
previous active locale and the committed settings unchanged.

The initial editor language control is intentionally a finite list of packaged
locale tags. Arbitrary locale entry, remote catalog download, project-owned
locale policy, and translation editing are follow-up capabilities. The language
setting belongs to the General settings surface, while catalog diagnostics and
missing-key reports belong to Diagnostics. The selected language label itself is
resolved through the source fallback catalog so the control remains usable when
switching languages.

### Initial migration boundary

ADR-110 navigation surfaces use editor-owned stable message keys for visible
document/panel/modal labels, actions, tooltips, empty/loading/stale/error states,
validation messages and accessibility names. Asset names, profile IDs, provider
fingerprints, paths and error codes remain typed technical data. Navigation UI
implementation changes update `en-US` and `tr-TR` catalogs together and test long
localized text in narrow dock/modal layouts.

The first migration covers editor-owned visible labels, buttons, tabs, tooltips,
placeholders, modal status text, and user-facing validation/error text. It does
not rewrite logs, stable error codes, JSON keys, protocol fields, asset paths,
plugin IDs, shader names, or persisted identifiers. Each migrated component must
use one catalog namespace and must not concatenate translated sentence
fragments.

## Message Identity

Message identity is the pair `(namespace, localKey)`. Its canonical serialized
form is `namespace:local.key`:

```text
editor:menu.file.save
editor:settings.title
editor:recovery.restore_action
cli:project.validate.summary
error:asset.unsupported_format
```

Catalog entries use local keys because the catalog already declares its
namespace. For example, `project.item_count` in the `editor` catalog resolves to
`editor:project.item_count`. The namespace is part of identity, not optional
packaging metadata.

Core namespaces are reserved. Plugin namespaces derive from the canonical plugin
ID, and project namespaces derive from stable project identity or an explicitly
declared project-local catalog ID. Catalog validation rejects duplicate fully
qualified identities and namespace ownership violations.

The local key describes semantic meaning, not the English sentence. Renaming
English copy does not require changing identity unless meaning changes.

## Key Lifecycle

Message keys are append-only within a compatibility window.
Changing English source text does not change the key, but changing semantic meaning requires a new key.

Removed keys are deprecated first and kept for at least one minor release when referenced by public plugin APIs or project-facing contracts.

## Resource Format

```json
{
  "schemaVersion": 1,
  "messageFormat": "icu-messageformat-1",
  "locale": "en-US",
  "namespace": "editor",
  "messages": {
    "project.item_count": {
      "text": "{count, plural, one {# project} other {# projects}}",
      "args": {
        "count": "integer"
      },
      "description": "Shown in project list summary."
    }
  }
}
```

Resources are:

- UTF-8
- schema validated
- namespace scoped
- schema and message-format versioned
- deterministic to merge and inspect

The implementation uses a proven message-format library that supports plural,
select, number, date, and time rules. These rules are not hand-written.
`messageFormat` identifies the exact syntax contract used by build tools and the
runtime loader. Unsupported schema or message-format versions are rejected
before catalog activation.

Argument metadata and translator descriptions must be provided to ensure
build-time argument type consistency and translator context. Authoring catalogs
are the source of truth. Translator interchange formats such as XLIFF may be
generated and re-imported through validated tooling; compiled runtime catalogs
are generated build artifacts.

## Locale Resolution

Precedence:

1. explicit invocation/session override
2. user-selected locale
3. operating system locale
4. packaged product default
5. source fallback locale

- **Product default locale**: The default language presented to the user (e.g. `en-US`).
- **Source fallback locale**: The canonical language the developers write the source catalog in. While typically identical to the product default (`en-US`), it is technically distinct for fallback purposes.

Project/game locale policy may override the product locale only within its own
content surface.

Locale identifiers use normalized BCP 47 language tags.

## Fallback

Fallback is deterministic. It combines normalized BCP 47 locale identifiers
with Unicode CLDR parent-locale data rather than blindly removing only the final
subtag. The resolver uses a standards-backed locale library for cases such as
`zh-Hant-TW`, `pt-BR`, and `sr-Latn-RS`.

```text
tr-TR -> tr -> product default -> source fallback
```

The chain is normalized and de-duplicated before lookup. If product default and
source fallback are both `en-US`, they appear only once.

Missing keys:

- use the fallback translation
- increment a bounded, low-cardinality missing-key metric using dimensions such
  as locale and namespace
- emit a rate-limited development diagnostic
- display a visible placeholder in localization test mode

Raw missing keys are not metric labels. Development diagnostics may include
sampled or rate-limited keys; shipping metrics remain bounded.

Shipping UI does not expose raw keys unless no safe fallback exists.

## Formatting API

```cpp
struct MessageKey {
    LocalizationNamespace namespaceId;
    LocalMessageKey localKey;
};

LocalizedText Localize(
    MessageKey key,
    MessageArguments arguments = {});

ResolvedLocalizedText Resolve(
    const LocalizationSnapshot& snapshot,
    const LocalizedText& text);
```

`MessageKey` preserves namespace and local-key structure in memory; callers do
not parse or concatenate canonical key strings during lookup.

`LocalizedText` is a value object containing the key and typed arguments; it is
not a borrowed pointer into a resource snapshot. It acts as a reactive/lazy
handle that is resolved to display text during UI render using the currently
active localization snapshot. `Resolve()` performs explicit fixed-snapshot
resolution for immutable presentation records and exports.

Arguments are named and typed. Translation strings cannot select arbitrary
format specifiers or access application objects.

User-facing sentences are not assembled by concatenating translated fragments.
The complete grammatical message belongs to one key.

Messages may declare typed placeholders for values such as commands, shortcuts,
paths, links, code spans, and asset names. These values are formatted by
registered safe presenters. A platform-aware shortcut placeholder, for example,
does not expose raw markup or require the translator to spell platform-specific
modifier names.

Translations do not contain arbitrary HTML, ImGui commands, or application
callbacks. Rich presentation is composed by the caller from validated typed
spans and message parts. Links and commands retain typed targets outside the
translated string.

## Snapshot And Resolution Semantics

Live labels, menus, forms, panels, tabs, and modals keep `LocalizedText` and
resolve it against the active immutable localization snapshot during
presentation.

Immutable user-facing presentation records, such as completed notification
history or a generated report, resolve against an explicit snapshot when the
record is created and retain the resolved text plus locale identity. They do not
change language retroactively after a locale switch.

Authoritative logs, audit records, errors, and job results retain stable codes
and typed data rather than persisted translated prose. A UI view may resolve
them lazily. An export operation selects one explicit localization snapshot for
the complete export so its language cannot change partway through generation.

Resource reload, locale switch, plugin registration, and namespace removal build
a candidate immutable snapshot. The candidate is schema, ownership, argument,
and message-format validated before activation.

## Errors And Diagnostics

Stable error and diagnostic codes remain language-neutral. GUI and CLI human
presenters map them to localized messages using safe metadata.

Machine-readable CLI and MCP output (`--format json`) retains stable unlocalized codes, field names, and values. It is never localized.
Human CLI output may localize when explicitly requested (`--human --locale tr-TR`), but default automation behavior remains strict and stable.
Localized human CLI output is not a scripting interface. Tests verify that
machine-readable output remains schema- and byte-stable across locale changes
for deterministic inputs.

## Runtime Switching

Locale changes build and validate a new immutable localization snapshot. The GUI
activates it at a frame boundary and publishes one locale-changed notification.

Localization snapshots are reference-counted or otherwise lifetime-safe.
Background jobs, async UI tasks, and plugin callbacks must not access mutable localization state directly.

Screens, panels, tabs, and modals query translated text during presentation or
invalidate cached text by localization revision. They do not retain references
into a replaced resource snapshot.

Live GUI surfaces are expected to respond to locale revision changes. A surface
that cannot safely update live must be explicitly marked reload-required; it
either closes and recreates, reloads its presentation state, or continues using
its captured previous snapshot until recreation. It must not mix text from two
snapshots in one rendered frame.

## Fonts And Text Shaping

The GUI design system provides:

- script- and run-appropriate font fallback
- glyph coverage validation
- text shaping and bidirectional support through a proven library
- locale-aware line breaking
- semantic text styles independent from a specific font file

Missing glyphs are detected during resource/component verification.

Dear ImGui is the immediate-mode presentation backend, not the shaping engine.
The GUI text renderer shapes Unicode text into directional glyph runs before
submitting positioned glyphs to ImGui drawing. A proven shaping and
bidirectional pipeline, such as HarfBuzz with a Unicode bidi implementation,
handles script shaping, ligatures, combining marks, and run ordering.

Font fallback selection occurs per shaped script run, with compatible metrics
and coverage, rather than independently replacing each missing glyph. Components
measure and clip the shaped output, not the unshaped source string.

## Layout

Components must support:

- text expansion without clipping or overlap
- right-to-left flow where required
- mirrored directional icons when semantically appropriate
- locale-aware alignment
- multiline labels and error text
- dynamic measurement without viewport-based font scaling

Components must not assume translated text fits into a single line unless the component explicitly defines truncation behavior.
Truncation must preserve accessible full text through tooltips or accessibility metadata.

Fixed icon-only controls retain accessible localized names and tooltips.

## Resource Ownership

Engine resources ship with the product. Core key overrides are not allowed.
Plugins and projects use their own namespaces. Branding layers may provide explicit replacement catalogs only for documented branding namespaces.

Unloading a plugin or project removes its localization resources only after all
surfaces using the namespace are destroyed.

Pending asynchronous work or callbacks that may present text from that namespace
must be cancelled, drained, or retain a lifetime-safe immutable snapshot until
completion. Namespace removal activates a new validated snapshot; existing
snapshot handles may continue resolving immutable catalog data but do not retain
plugin code, callbacks, registrations, or capabilities.

## Translation Workflow

The build validates:

- duplicate and missing keys
- argument-name/type consistency across locales
- malformed message format
- unsupported catalog schema or message-format syntax version
- unsupported locale tags
- forbidden markup or control characters
- source and translation namespace ownership

Generated catalogs and reports are build artifacts; translators do not edit
compiled resources.

## Testing

Required tests cover:

- locale precedence and normalization
- canonical namespace/local-key identity and ownership validation
- fallback chain
- BCP 47 normalization, CLDR parent fallback, and chain de-duplication
- plural/select formatting
- argument mismatch diagnostics
- unsupported schema and message-format versions reject catalog activation
- typed rich placeholders cannot inject markup, commands, or links
- runtime locale change
- explicit snapshot resolution remains stable across later locale changes
- reload-required surfaces do not mix localization snapshots
- right-to-left component scenarios
- shaped Arabic/Hebrew runs and script-run font fallback
- long pseudo-localized text without clipping
- font/glyph coverage
- stable machine-readable CLI and MCP output
- missing-key metrics remain low-cardinality and bounded
- plugin/project namespace isolation
- async namespace removal retains or drains snapshot users safely
- translation catalog compatibility across product versions

Pseudo-locales include expanded Latin text and right-to-left mirrored text for
GUI screenshot and interaction scenarios.

## Related Documents

- [Localization Editor UI Reference](../../../mock-studio/designs.md#architecture-editor-localization-editor): string table grid, locale switcher, missing translation view, and CSV import/export panel.

- [GUI Design System](./ui-design-system.md)
- [GUI Screen Host](./gui-screen-host.md)
- [Configuration System](../foundation/configuration-system.md)
- [Error And Diagnostics](../foundation/error-and-diagnostics.md)
- [Extension System](../extensions/plugin-system.md)

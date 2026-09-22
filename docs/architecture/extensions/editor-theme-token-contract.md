# Extension Theme Token Contract

## Purpose

`Horo/Extensions/EditorThemeTokens.h` is the stable, backend-neutral theme
boundary for host-rendered GUI extensions. It exposes semantic roles, not
editor theme files, ImGui values, renderer resources, native handles, or cached
color literals. A form, component, or future declarative UI adapter can use the
same contract in GUI, headless, and out-of-process projections.

## Version and frame ownership

The contract is schema version `1`. `EditorThemeFrame::schemaVersion` is
forward-compatible only within the advertised minimum/current range. Additive
roles are represented by support masks and must have a safe fallback. A host
must reject an incompatible schema with
`editor_theme_token_version_unsupported`; malformed metadata or resolved values
fail with `editor_theme_token_invalid`. Extensions must not reinterpret unknown
role values or read a theme file as a compatibility workaround.

`EditorThemeFrame` is immutable evidence for one editor frame. The host resolves
the active layers, validates the complete frame, and publishes a new frame at a
frame boundary with a monotonically increasing `revision` when a theme,
accessibility, or DPI input changes. Extension projections read the new frame on
the next render/update pass; they do not retain the frame, resolved values, or
font/icon resources across revisions, and no extension restart or reconstruction
is required.

The host layer order remains the editor design-system contract:

```text
packaged/built-in theme -> selected custom preset -> project branding hints
    -> personal overrides -> accessibility and reduced-motion constraints
```

The frame's `changeMask` identifies affected categories. `Dpi` is explicit even
when a scale change also changes geometry and typography. `uiScale` is a
validated logical scale, so consumers do not multiply raw pixels or select a
platform DPI policy themselves.

## Semantic roles

The frame publishes support masks and resolved values for:

- colors: surfaces, borders, text, focus, accent, status, overlays, and
  interaction states;
- typography: caption, label, body, card title, title, heading, and display;
- spacing and size: layout gaps, padding, controls, dialogs, settings, icons,
  and minimum interactive target;
- radius and interaction: controls, cards, dialogs, focus rings, opacity,
  pressed scale, and keyboard-focus visibility;
- motion: instant/fast/normal/slow and interaction transition timings;
- font and icon roles: host-selected logical roles and normalized sizes, while
  actual font atlases and glyph resources remain host-owned; and
- accessibility: high contrast, text contrast multiplier, color-vision mode,
  color-vision severity, reduced motion, and flash-effect suppression.

Consumers use the typed `*For` accessors or role resolvers. Each resolver first
returns a supported role and then follows a deterministic category-specific
fallback chain. If no safe role is available it returns `None`; it never exposes
an editor-internal token or invents a literal. Reduced motion resolves every
requested motion role to `Instant` when that role is supported.

`EditorUiThemeToken`, `EditorUiThemeMetrics`, and `EditorUiThemeFrame` in
`EditorUiForm.h` are compatibility aliases to this contract. The form render
snapshot carries the complete resolved frame as well as its revision and
scale-adjusted geometry, so the standard form kit and future extension UI
surfaces cannot drift into separate theme schemas.

## Boundary and verification

The Extensions target owns the public header and validator. Validation is inert:
it does not register services, select a backend, query a service locator, or
invoke extension code. The GUI host is responsible for adapting its active
design-system snapshot into this backend-neutral frame; the Extensions target
does not depend on Dear ImGui.

Contract tests cover every role family, support-mask fallback, schema/version
rejection, finite/range validation, reduced-motion behavior, accessibility
metadata, and live revision/DPI/color changes. Form tests continue to verify
that existing standard components consume the shared color-role fallback and
retain the historical `editor_ui_theme_invalid` boundary error.

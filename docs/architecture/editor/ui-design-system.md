# GUI Design System

## Purpose

The Horo graphical host uses Dear ImGui as its rendering toolkit and provides a
reusable, component-based UI system inspired by React and Vue component
composition.

The GUI is one application. Welcome, project browser, project creation, and
editor workspaces are screens within the same host. Editor Settings and
Build & Release are modal workflow surfaces above the editor workspace, not
application routes or editor tabs. A separate launcher UI or launcher component
set does not exist.

ImGui remains an implementation detail of the GUI module. Engine, application,
editor model, CLI, and MCP code do not depend on ImGui.

## UI Layers

```text
GUI screens, workspaces, feature panels, and modal workflows
              |
      composite components
              |
      primitive components
              |
     layout and interaction
              |
        Dear ImGui adapter

resolved design tokens/theme are immutable frame inputs used by components,
layout, and the adapter
```

Required module layout:

```text
src/editor/
  design_system/
    Theme.h
    ThemeRegistry.h
    ThemeLoader.h
    DesignTokens.h
    components/
      primitives/
      layout/
      composite/
      feedback/
      navigation/
  screens/
    welcome/
    projects/
    workspace/
  modals/
  panels/
  state/
  testing/
```

## ImGui Usage Boundary

Feature code does not call raw Dear ImGui widgets directly except inside
approved adapter, layout, or primitive-component implementations. Screens,
panels, tabs, and modal workflows compose design-system components so styling,
identity, accessibility, localization, and tests remain consistent.

Low-level ImGui calls are allowed only where they are the implementation detail
of a reviewed design-system primitive or host integration point. Any new direct
ImGui wrapper must define its typed props/result contract, token usage,
accessibility behavior, and component-gallery coverage before feature code
depends on it.

### External Package Boundary

External packages do not implement that low-level integration point. Embedded
extension tabs, panels, drawers, modal pages and settings pages publish bounded
typed UI schemas and state; the GUI host renders them with the same components,
tokens, localization, focus and accessibility rules as built-in surfaces. A
versioned extension C ABI may carry copied schemas, action requests and lifecycle
values, but never an ImGui context, draw list, SDL event, renderer handle, native
child window or C++ component object.

Advanced external tools use registered host-owned schemas for plots, timelines,
node graphs, image/canvas annotation and other reviewed specialized views. There
is no generic immediate-mode drawing escape hatch. Isolation-sensitive modules
may put their controller in a supervised helper process while retaining the same
host-rendered schema/action contract. See
[ADR-056](../../adr/056-external-editor-ui-boundary.md).

## Editor Platform And Renderer Boundary

The graphical editor uses SDL3 for platform windowing and multimedia
integration. Dear ImGui renderer integration is selected with the active Horo
renderer backend. SDL3 and concrete ImGui renderer backends are implementation
details hidden behind Horo-owned editor adapters.

Reasons:

- SDL3 gives Horo one stable platform surface for windowing, input, gamepads,
  clipboard, timers, and future multimedia-oriented host work.
- Dear ImGui has a maintained SDL3 backend.
- OpenGL and Metal are equal first-class renderer implementations behind the
  same Horo-owned editor rendering contract.
- Renderer selection does not change screen, modal, project-model, CLI, MCP, or
  runtime feature code.

Constraints:

- No public Horo header, screen, panel, modal, project-model, CLI, MCP, or
  runtime subsystem includes SDL3, OpenGL, or raw Dear ImGui backend headers.
- Screens and panels depend on design-system interfaces, not backend objects.
- No concrete GUI backend may become an RHI shortcut or a compatibility layer
  used by another renderer backend.
- A null GUI backend exists for deterministic non-window tests.

GLFW and web/Electron shells remain rejected for the native editor host. SDL3 is
isolated behind the editor platform adapter so renderer or host changes do not
affect editor-facing contracts. Backend implementation order is not an
architectural priority; every interactive backend must satisfy the shared
[Render Backend Parity Contract](../runtime/render-backend-parity-contract.md).

## DPI, Docking, And Viewports

Initial DPI policy:

- Use per-monitor content scale from the SDL3-backed window adapter where
  available.
- Store current scale in editor GUI state.
- Rebuild font resources when the scale bucket changes.
- Express component sizes in design tokens, not raw pixels.
- Screens consume scaled design tokens; they do not query SDL3 or native monitor
  state directly.

Docking policy:

- Welcome, project browser, and project creation are full-route screens and do
  not expose dock nodes.
- Docking is enabled only for the editor workspace after the workspace host
  exists.
- Docking state belongs to editor workspace persistence, not global startup
  state.

Multi-viewport is disabled for the initial bootstrap because it multiplies
platform-window, DPI, focus, and renderer-lifetime cases. Enabling it requires a
future architecture update.

## UI Text

### Semantic typography scale

Visible editor text uses semantic theme roles rather than feature-local pixel
sizes. The packaged scale is:

| Role | Default | Use |
| --- | ---: | --- |
| `caption` | 18 px | metadata, hints, secondary and supporting text |
| `label` | 18 px | controls, tabs, tree rows, badges and field labels |
| `body` | 18 px | paragraphs and primary content |
| `cardTitle` | 18 px | compact card and component-section titles |
| `title` | 20 px | panel and modal titles |
| `heading` | 24 px | section headings comparable to H2 |
| `display` | 30 px | top-level screen headings comparable to H1 |

Feature code obtains these sizes through `Theme::TextPx` and selects the font
family or emphasis separately. It must not introduce raw visible font sizes.
Theme overrides may customize the scale, but normalization preserves a 16 px
minimum and the ordering `body <= cardTitle <= title <= heading <= display`. Component-size
tokens may change padding and interaction geometry without making visible text
smaller than the active `caption` role.
The compact font atlas starts at 18 px. Existing callers keep their
semantic roles; custom themes specifying smaller font bases, caption, label,
or component text are raised to the minimum when loaded.

Bottom-dock tabs share the semantic `BottomDockToolbarSurface`,
`BottomDockContentSurface`, and `BottomDockControlSurface` theme roles. The
toolbar is intentionally elevated above the content surface; individual tabs
must not replace this hierarchy with feature-local toolbar colors.

Every bottom-dock pane uses the shared `GlobalDockPaneLayout` metrics and region
partitioning. A pane may omit its top toolbar or request a left rail; its content
extends to the bottom of the dock without a pane footer. Panes do not redefine
the canonical toolbar, control, table, or spacing dimensions. `GlobalDockPanel` owns registered panes through
`IGlobalDockPane`; built-in and internal module-provided panes use the same
stable identity, localization-key, attach/detach, and draw contract. Pane
registration is completed before panel attachment so service lifetimes remain
explicit and the frame-hot draw path performs no ownership mutation.

User-facing component text uses `UiText` rather than raw visible strings:

```cpp
enum class UiTextKind {
    LocalizedKey,
    ResolvedDisplayText,
    TechnicalText,
};

struct UiTextArgument;

struct UiTextArguments {
    std::span<const UiTextArgument> values;
};

struct UiText {
    UiTextKind kind = UiTextKind::ResolvedDisplayText;
    std::string_view value;
    UiTextArguments args = {};
};
```

`UiText` may represent a localization key, already resolved display text, or
non-localized technical text depending on the surface contract. Visible labels,
tooltips, placeholders, and accessibility labels follow the same localization
rules. Visible text is never used as the only ImGui identity; stable `UiId`
values remain the source of widget identity.

Localized `UiText` may carry typed formatting arguments. Components do not build
user-facing sentences by concatenating translated fragments; complete messages
come from one localization entry so pluralization, word order, and bidirectional
text remain correct.

Text views passed through props are consumed during the component call. Any text
retained beyond the current frame, including delayed tooltips, accessibility
snapshots, component-gallery captures, or test snapshots, is copied into owned
storage by the retaining system.

Text primitives route through the GUI text system before submitting text to
Dear ImGui. Font fallback, glyph coverage, shaping, bidirectional ordering,
truncation, ellipsis behavior, and tooltip fallback are handled consistently in
that path. Feature code does not call raw ImGui text rendering for user-facing
strings.

## Component Contract

Components are reusable functions or focused objects with explicit inputs,
outputs, and optional local state.

```cpp
enum class ComponentSize {
    XS,
    Small,
    Medium,
    Large,
    XL,
};

enum class ButtonVariant {
    Primary,
    Secondary,
    Ghost,
    Destructive,
};

enum class IconPlacement {
    Leading,
    Trailing,
};

struct ButtonProps {
    UiId id;
    UiText label;
    UiText accessibleLabel = {};
    UiText tooltip = {};
    UiIcon icon = UiIcon::None;
    IconPlacement iconPlacement = IconPlacement::Leading;
    ButtonVariant variant = ButtonVariant::Primary;
    ComponentSize size = ComponentSize::Medium;
    bool enabled = true;
    bool loading = false;
    StyleProperties style = {};
};

struct ButtonResult {
    bool pressed = false;
};

ButtonResult Button(const ButtonProps& props);
```

Component rules:

- Inputs are passed through typed `Props` structures.
- User interaction is returned through result values, commands, or callbacks.
- Business logic and project mutation do not live inside components.
- Persistent state is owned by the calling feature or an explicit state object.
- Primitive-local state is limited to ephemeral interaction state such as hover,
  active press, keyboard focus, transient text-edit composition, or bounded
  animation progress. Persistent, project-affecting, or cross-frame feature
  state is passed through explicit state objects owned by the feature.
- ImGui IDs are stable and are not derived only from visible labels.
- Components do not access unrelated global GUI or editor state.
- Component APIs use semantic variants instead of raw colors and dimensions.
- Shared size values map to theme tokens; feature code does not choose control
  heights or padding with numeric literals.
- Loading controls preserve their geometry, prevent duplicate activation, and
  expose progress through the shared feedback primitives.
- Icon-only actions use the dedicated icon-button primitive and require an
  accessible label and tooltip.
- A control with visible text may use that text as its default accessible label;
  icon-only controls, ambiguous controls, and destructive actions provide an
  explicit accessible label and tooltip.
- Optional icons do not change the meaning of a semantic variant. Icon
  placement is explicit and layout remains stable when an icon appears.

### Icon ownership

Editor controls use `UiIcon` identities or resolve stable tokens through
`UiIconRegistry` at catalog and serialized boundaries. The registry is a fixed,
editor-owned table: each identity has one canonical token and one drawing
definition. Aliases map legacy or catalog tokens to an existing identity.

The drawing definition may use the editor's Material Symbols font, a Horo-owned
ImGui drawing routine, or a font glyph with a custom fallback. The font atlas is
built before the editor UI runs, so ordinary panel drawing cannot register new
glyphs or replace icon providers. Atlas glyph ranges are derived from the
registry's font-backed definitions. A runtime extension that needs custom artwork
must use a separate, explicitly owned asset and lifetime contract; it cannot
mutate the built-in icon table.

Panels select icons by meaning rather than by the visual source. The selected
scene object uses `UiIcon::SceneObject` in both Hierarchy and Inspector; the
package icon has its own identity even when it currently uses the same glyph.
- Primitive components remain feature-independent.
- Composite components are built by composing primitives.
- Screens, panels, and modal workflows orchestrate components and application
  use cases.

The same common API conventions apply where meaningful to fields, selects,
badges, menu items, and other controls:

- explicit stable identity
- `Small`, `Medium`, and `Large` sizing
- semantic visual variant
- enabled, disabled, loading, error, and focus state
- optional leading or trailing icon
- accessible label and tooltip metadata

`StyleProperties` carries opt-in call-site presentation such as filling the
available horizontal space and token-backed padding overrides. Component size
remains the primary geometry contract: `XS`, `Small`, `Medium`, `Large`, and
`XL` resolve font size, padding, minimum height, and icon size together.

Tooltips use the shared `Ui::ShowTooltip` or `Ui::ScopedTooltip` primitives.
Their surface, border, radius, padding, typography, wrapping width, and display
scale resolve from the active design tokens; feature code must not call raw
ImGui tooltip APIs or define local tooltip chrome.

The base values are theme data rather than component implementation literals.
Custom themes discovered under `~/.horo/themes/` may override them with:

```json
{
  "tokens": {
    "componentSizes": {
      "xs": {
        "fontSize": 12,
        "paddingX": 8,
        "paddingY": 3,
        "minimumHeight": 24,
        "iconSize": 12
      }
    },
    "styleSpacing": {
      "xs": 4,
      "s": 8,
      "m": 12,
      "l": 16,
      "xl": 24
    }
  }
}
```

Global display scaling is temporarily disabled. The persisted setting remains
in the settings document for compatibility, but the editor renders at 100%
until font, layout, native DPI, and viewport scaling share one coherent policy.

Not every primitive exposes every option. A control only exposes properties
that have a defined behavior and token mapping. Feature code composes or wraps a
primitive instead of adding one-off drawing branches to it.

New variants and sizes become part of the shared design-system contract. They
require semantic token mappings, component-gallery coverage, and behavior tests.
Feature-specific names such as `ReleaseBlue` or `HierarchyCompact` are not valid
shared variants.

## Component Categories

Primitives provide the shared visual vocabulary:

- button
- icon button
- text and heading
- text field
- numeric field
- checkbox
- select and combo box
- separator
- badge
- tooltip
- progress indicator
- modal surface
- context menu item

Layout components provide consistent structure:

- stack
- inline row
- grid
- spacing
- section
- panel
- split view
- scroll region
- toolbar

Composite components combine primitives into reusable behaviors:

- search field
- property row
- file picker
- color editor
- asset card
- tree item
- command palette item
- notification
- confirmation dialog
- empty state

Feature-specific behavior belongs in screens, workspaces, panels, and modal
workflows, not primitive components. Welcome and project-management screens use
the same primitives, themes, and layout components as scene-editing workspaces
and editor modals.

Primitive components do not publish to `EditorDataBus` or `EngineDataBus`.
They return interaction results to their owning feature. A screen, panel, tab,
or modal may then invoke a typed command or use case; the state authority
publishes the resulting notification after commit.

## Modal Workflow Surfaces

Large editor modals may contain internal navigation, forms, logs, progress, and
footer actions, but they are composed from the same primitives and semantic
tokens as other GUI surfaces.

The modal-surface primitive provides visual structure only. Exclusive focus,
input blocking, modal stack ownership, close policy, and focus restoration belong
to `EditorModalHost`.

The current GUI implementation exposes this structure through
`Ui::ScopedModalShell`. It owns responsive centering, shared title-bar chrome,
the close affordance, body/footer geometry, and the fixed footer surface.
`Ui::ModalSplitPane` composes an optional leading navigation/sidebar with a
scrollable primary pane. Modal workflows supply content and actions; they do not
redraw this chrome. Compact status metadata uses the semantic `Ui::Badge`
primitive rather than feature-local tag or pill drawing.

Rules:

- modal content fits within the editor client area at every supported UI scale
- long content scrolls inside the modal without moving footer actions off-screen
- keyboard traversal remains inside the active modal
- the dim layer uses semantic overlay tokens and does not replace input gating
- widget popups opened by a modal remain clipped and focused within that modal
- destructive confirmations use a child modal, not an ad hoc nested window
- background panels may appear disabled but cannot receive hidden input

See [Editor Modal Host](./editor-modal-host.md).

ADR-110 navigation documents, panels, operation projections and transient modals
reuse these fields, tables, badges, buttons, split panes, progress/status and modal
shells. Navigation/provider code does not draw feature-local pills or pass raw
ImGui through a provider extension. Long profile/asset/fingerprint values, narrow
docks and every disabled/loading/stale/error state retain the shared typography,
focus, scale and accessibility rules.

## Theme Model

The editor `Theme` and design tokens in this section style HoroEditor only.
[ADR-076](../../adr/076-runtime-ui-style-asset-token-and-inheritance.md) defines the
separate cooked Runtime UI style system. A game style cannot inherit this `Theme`,
reference an editor token, import `ImGuiStyle` or capture user workspace theme
preferences. Runtime-style authoring/preview uses an isolated Runtime UI adapter;
any explicit conversion creates reviewed runtime values rather than a live link.

All visual values are referenced through typed design tokens. Components do not
hardcode colors, font sizes, spacing, radii, or standard dimensions.

Inspector property rows obtain their vertical control separation from the
theme's `propertyRowGap` spacing token. Feature panels must not collapse or
override that gap with local cursor offsets.

```cpp
struct TypographyTokens {
    float bodySize;
    float labelSize;
    float windowTitleSize;
    float sectionTitleSize;
};

struct SpacingTokens {
    float xSmall;
    float small;
    float medium;
    float large;
    float xLarge;
};

struct Theme {
    TypographyTokens typography;
    SpacingTokens spacing;
    ColorTokens colors;
    RadiusTokens radii;
    SizeTokens sizes;
};
```

Components reference semantic tokens:

```cpp
theme.typography.windowTitleSize
theme.spacing.medium
theme.colors.surfacePanel
theme.colors.textMuted
theme.sizes.toolbarHeight
```

Raw literals such as a repeated `48.0f` title size or a direct RGBA color are
not valid component styling.

Visual literals are allowed only in versioned theme resource files and test
fixtures. They are not allowed in GUI component, panel, modal, or screen source
code. The generated or parsed typed `Theme` object is the only styling input
used during rendering.

## Token Categories

The theme system defines:

- typography: font family, weight, size, and line height
- colors: surfaces, text, borders, actions, status, selection, and overlays
- spacing: a consistent spacing scale
- dimensions: control heights, panel sizes, toolbar sizes, and icon sizes
- shape: corner radii and border widths
- interaction: hover, active, disabled, focus, and selection states
- motion: durations and easing metadata where animation is supported

Tokens use semantic names. Components request `colors.actionPrimary`, not
`colors.blue500`.

Motion tokens describe timing only. Animation state is owned by the caller, by
an explicit component state object, or by a bounded GUI animation registry keyed
by stable `UiId`. Components do not create hidden unbounded static maps or
global animation state.

## Theme Resolution

Themes are data-driven and resolved in layers:

```text
packaged base theme
        |
selected packaged or custom preset
        |
optional project branding hints
        |
personal token overrides
        |
runtime accessibility overrides
```

The resolved theme is immutable for a rendered frame. Theme changes are applied
at a frame boundary and propagate to every component using the affected tokens.

When Settings previews a theme or UI-scale change, its preview session updates
`ResolvedEditorSettings` and publishes an editor-session settings notification.
Panels, tabs, and modals invalidate presentation caches and read the resolved
settings on the next frame. They do not read controls or draft fields from
`SettingsModal`.

Theme files are versioned, validated, and produce actionable diagnostics for
unknown, missing, or invalid values. Invalid overrides fall back to the nearest
valid layer without corrupting the active theme.

The selected preset is resolved in this order:

1. `HORO_THEME_FILE`, when set for development or automated testing
2. `HORO_THEME`, when set to a registered preset ID
3. the user's persisted editor setting
4. the packaged default preset

`HORO_THEME_FILE` wins when both environment variables are present. Environment
selection is process-local and is never persisted back to user settings.
Project theme data is treated as optional branding hints and is applied before
personal token overrides. Users may disable project branding for the editor
chrome. Personal accessibility requirements remain the final authority for
minimum size, contrast, and reduced-motion constraints.

## Theme Files And Discovery

Theme presets use a versioned data format:

```json
{
  "schemaVersion": 1,
  "id": "user.midnight",
  "displayName": "Midnight",
  "extends": "horo.base-dark",
  "tokens": {
    "colors": {
      "surfacePanel": "#101722",
      "textPrimary": "#F2F5F8",
      "actionPrimary": "#3B8EF3"
    },
    "spacing": {
      "medium": 8
    },
    "radii": {
      "control": 4
    }
  }
}
```

Theme sources are:

- packaged presets in the application's read-only `themes/` resources
- custom presets in `~/.horo/themes/*.json` (current implementation; target:
  `Platform::UserConfigDirectory()/horo/themes/*.theme.json`)
- an optional project override at `<project>/.horo/theme.json`
- an explicit external preset selected through `HORO_THEME_FILE`

Current theme file format (flat, for rapid iteration):

```json
{
    "name": "Monokai",
    "WindowBg": "#272822",
    "ChildBg": "#1e1f1c",
    "Text": "#f8f8f2",
    "Border": "#49483e"
}
```

Target format (versioned, with inheritance):

```json
{
    "schemaVersion": 1,
    "id": "user.midnight",
    "displayName": "Midnight",
    "extends": "horo.base-dark",
    "tokens": {
        "colors": {
            "surfacePanel": "#101722"
        }
    }
}
```

Migration from flat to versioned format is planned. Custom presets discovered
at runtime are added to the theme selector alongside built-in presets.
an explicit rescan requested by Settings. Development builds may watch the
active external or custom preset file. Reload follows this contract:

1. Parse and validate into a candidate theme without changing the active theme.
2. Resolve inheritance and all override layers.
3. Atomically install the immutable resolved theme at a frame boundary.
4. Publish one settings/theme notification through the owning settings
   authority.

An invalid reload keeps the last-known-good theme active and reports a
structured diagnostic. Color, spacing, shape, dimensions, and motion changes
apply live. Typography changes that require a font-atlas rebuild are prepared
outside rendering and swapped at a safe frame boundary; they do not require an
application restart.

## User Customization

Users can:

- choose a built-in or custom theme
- discover custom presets added to the user theme directory
- override individual semantic tokens
- change typography and UI scale
- export and import theme definitions
- restore individual values or the full theme to defaults
- preview changes before persistence

Theme configuration is stored independently from project scene data. Project
theme overrides are optional branding hints: they cannot silently replace
personal token overrides, accessibility settings, or reduced-motion
requirements.

## Accessibility

The design system records accessibility metadata even when the current Dear
ImGui backend cannot expose a complete native accessibility tree. Keyboard
access, visible focus, tooltips, accessible names, and test-visible metadata are
required baseline behavior from the first implementation. Native screen-reader
integration may be added per platform without changing component props.

[ADR-082](../../adr/082-runtime-ui-accessibility-capability-and-ownership.md)
applies the same truthful capability principle to Runtime UI: semantic metadata,
recording tests and native assistive-technology integration are distinct evidence
levels. Editor validation uses the versioned semantic schema but does not own or
patch live runtime nodes, settings, focus or platform dispatch.

The design system supports:

- global UI scaling
- minimum readable font and control sizes; the workspace global dock uses the
  compact typography token from its tab labels as its text-size floor
- contrast validation for semantic color pairs
- keyboard navigation
- modal focus trapping and deterministic focus restoration
- visible focus state
- color-independent status indicators
- reduced-motion preferences where motion exists

## Component Verification

Each shared component has:

- deterministic behavior tests for state and result contracts
- matrix coverage for supported sizes, variants, icons, loading, and disabled
  behavior
- identity and text-contract coverage proving labels, localization changes,
  tooltips, and accessible labels do not change stable widget identity
- text rendering scenarios for font fallback, shaping, bidirectional ordering,
  truncation, and tooltip fallback where applicable
- theme-token tests for required semantic values
- component-gallery matrix coverage for compact and comfortable density, long
  pseudo-localized labels, right-to-left layout where supported, high-contrast
  themes, and 125%, 150%, and 200% UI scales
- rendering scenarios for default, hover, active, disabled, focused, and error
  states where applicable
- screenshot baselines for stable representative themes
- at least one usage example or component gallery scenario

The GUI provides a component gallery that renders reusable components and theme
variants in isolation.

Theme-system verification covers schema validation, source discovery, selection
precedence, inheritance, environment overrides, live reload, last-known-good
fallback, and custom preset import/export.

GUI source review or static checks enforce the raw ImGui usage boundary:
feature screens, panels, tabs, and modal workflows do not bypass approved
design-system primitives for user-facing controls.

## Related Documents

- [GUI Screen Host](./gui-screen-host.md): top-level route and screen lifetime.
- [Editor Panel Host](./editor-panel-host.md): persistent workspace composition.
- [Editor Modal Host](./editor-modal-host.md): exclusive workflow presentation.
- [Input Architecture](../runtime/input-architecture.md): focus, capture, keyboard, and
  interaction-scope routing.
- [Configuration System](../foundation/configuration-system.md): theme selection,
  precedence, and immutable resolved settings.
- [Localization](./localization.md): translated text, font fallback,
  right-to-left layout, and pseudo-localization scenarios.

# Editor Status Bar Architecture

## Purpose

The editor status bar is the persistent bottom edge of the Horo Editor/IDE shell.
It remains present when the active `GuiScreen` changes and is not owned by the
Editor Workspace, a panel, or a document page.

This document defines the current in-process registry contract and the boundary
that future `editor.status_item` extension adapters must preserve.

Visual reference: [Editor Workspace](../../../mock-studio/designs.md#architecture-editor-editor-workspace).

## Core Decisions

- `GuiScreenHost` owns one `EditorStatusBar` and one
  `EditorStatusItemRegistry` for the editor process.
- `GuiScreenHost` reserves the fixed status-bar height before calling the active
  screen. Every `GuiScreen::Draw` receives the remaining `GuiContentRegion`.
- Built-ins, first-party modules, and future plugin adapters contribute through
  the same typed registry.
- A contributor supplies declarative, bounded state. It never receives an ImGui
  context or arbitrary draw callback.
- The status renderer does not poll module/plugin code during `Draw`. Providers
  update snapshots from their normal update/event path.
- Ordering is explicit and deterministic. Registration order is not a semantic
  tie-breaker.
- Width is a host-owned budget. Lower-priority items disappear before the bar
  grows, overlaps content, or changes height.
- A modal may not be bypassed through status-bar interaction. Snapshots may keep
  updating while status item input is disabled.
- The status bar reports only authoritative metrics. A GPU duration is not shown
  until the active renderer provides a real timestamp-query result.
- The bottom-dock toggle shown in the React mock reference is outside the first status
  contribution contract.

## Ownership

```text
HoroEditorApp
  +-- GuiScreenHost
        +-- active GuiScreen
        +-- EditorStatusItemRegistry
        +-- EditorStatusBar
        +-- EditorModalHost (borrowed interaction authority)
```

`EditorWorkspaceScreen` may publish document and selection snapshots, but it
does not draw or own the bar. On route exit, it marks workspace-scoped snapshots
unavailable. Process-scoped items such as the renderer backend remain visible.

The host computes:

```text
viewport work region
  - fixed 30 px status bar
  = GuiContentRegion passed to active screen
```

This compile-time screen contract prevents a new screen from accidentally
rendering behind the persistent bar.

## Contribution Model

A registered item has immutable placement/policy metadata and a replaceable
presentation snapshot.

```cpp
struct EditorStatusItemDescriptor {
    std::string id;
    std::string localizationNamespace;
    std::string labelKey;
    EditorStatusBarAlignment alignment;       // Left or Right
    EditorStatusItemVisibility visibility;    // Always or OnlyWhenPanelActive
    std::string ownerPanelId;
    int priority;
    std::uint16_t order;
    float maxWidth;
    EditorStatusItemPresentation presentation;
    bool interactive;
    std::string actionId;
};

struct EditorStatusItemContent {
    std::string iconResourceId;
    std::string label; // dynamic/localized override when no descriptor key exists
    std::string value;
    EditorStatusItemTone tone;
    bool available;
};
```

`Always` means independent of panel activation; it does not force an unavailable
snapshot to render. `OnlyWhenPanelActive` additionally requires the descriptor's
`ownerPanelId` to appear in the active screen's panel context. This supports the
user-facing policies “always” and “only while the owning panel is active” without
making the status bar depend on a specific panel-host implementation.

The initial registry is an in-process C++ service. The future plugin loader must
translate validated `editor.status_item` package descriptors and capability
updates into this registry; it must not expose this C++ type as a binary ABI.

## Validation And Limits

The host rejects a registration or update before mutation when it violates a
limit. Current limits are part of the defensive host policy, not extension ABI:

- maximum 64 registered items
- canonical bounded IDs
- maximum 96 bytes for label snapshots
- maximum 64 bytes for value snapshots
- maximum 128 bytes for icon resource IDs
- maximum 128 bytes for action IDs
- item width accepted only within 32–240 px
- `OnlyWhenPanelActive` requires a non-empty owner panel ID
- interactive items require a non-empty typed action ID
- duplicate contribution IDs are rejected

An invalid update preserves the previous valid snapshot. Unknown icon resource
IDs must use a host fallback glyph; they must not trigger file I/O during draw.

## Ordering And Width Budget

Visibility resolution first removes unavailable and inactive-panel items.
Surviving items are ranked for admission by:

1. higher priority
2. explicit order
3. stable contribution ID

The planner admits no more than 12 visible items and never exceeds the available
horizontal budget. When all candidates do not fit, it reserves a bounded overflow
indicator and admits a strict highest-ranked prefix: after the first candidate
cannot fit, no lower-ranked candidate may bypass it merely because it is
narrower. The planner reports the number omitted. Final visual ordering is left
group then right group, each ordered by explicit order and stable ID.

Each renderer-measured preferred width is clamped by the descriptor's `maxWidth`.
Text is clipped inside that width and the full label/value is available as a
hover tooltip. The bar remains 30 px high at every window size and uses the
13 px compact Inter role for readable shell metadata.

Priority is retention policy, not visual order. A provider must not use extreme
priority values merely to move an item left or right.

## State And Performance Contract

Status snapshots are read-mostly presentation data:

- registration/unregistration occurs at composition or module lifecycle
  boundaries
- content updates occur when authoritative state changes or at a bounded frame
  update for cheap metrics
- update validation completes before replacing the previous snapshot
- the renderer reuses scratch storage reserved to the registry limit
- no filesystem, network, shader compilation, GPU readback, job wait, or plugin
  callback is allowed in the status render path

A producer of an expensive metric owns asynchronous collection and publishes the
last completed bounded snapshot. “No completed sample” is represented by
`available = false`, not a fabricated value or a synchronous wait.

The current CPU value is host frame delta converted to milliseconds. It is a CPU
frame-duration indicator, not profiler instrumentation. The renderer backend
item is `OpenGL`, matching the current editor backend. GPU duration remains absent
until timestamp queries and delayed readback are implemented.

## Interaction

An interactive item declares an `actionId`; it does not store a callback. On an
enabled click, the shell publishes `EditorStatusItemInvokedEvent` with the stable
item and action IDs. The owning module handles that typed process event and calls
its authoritative capability.

Clicks are disabled whenever `EditorModalHost` or the leave-resolution dialog
owns exclusive input. The status window itself uses no-input mode in that state,
so it cannot steal hover/focus from a modal.

## Localization And Icons

Static labels should use `localizationNamespace` plus `labelKey`. Dynamic labels
or values must already be localized when the snapshot is published. Plugin keys
must remain namespaced according to the plugin-system localization contract.

`iconResourceId` is a declarative resource identity. The current host renders
every unresolved non-empty ID as the same host-tinted semantic dot; no resource
lookup is claimed. An icon registry may resolve known IDs while preserving the
dot fallback.
Contributions never pass `ImTextureID`, draw lists, native handles, or file
paths.

## Lifecycle

1. `GuiScreenHost` creates and registers built-in status descriptors.
2. Modules or validated plugin adapters register additional descriptors.
3. Providers publish bounded content snapshots.
4. Each frame, the active screen contributes current active panel IDs.
5. The host resolves visibility and width, then draws the bar after the screen.
6. Invocations are published only when shell interaction is enabled.
7. Module/plugin unload unregisters its items before code or resources unload.

Workspace route exit marks its document/selection snapshots unavailable before
its controller is destroyed. The registry therefore never holds views into
screen-owned strings; active panel IDs are borrowed only for the immediate draw.

## Failure Policy

- Duplicate/invalid registration: reject the contribution and emit a diagnostic.
- Invalid update: retain the previous valid snapshot.
- Unknown item update/unregister: return `UnknownItem`.
- Unknown icon: render host fallback.
- Missing localized key: use the localization service's normal missing-key
  diagnostic/fallback behavior.
- Provider failure: mark the item unavailable or publish a bounded error tone;
  never block the frame.
- Width exhaustion: omit lower-priority items and report bounded overflow.

## Testing Requirements

Pure model tests cover:

- duplicate and capacity rejection
- active-panel visibility
- transactional content-update limits
- deterministic ordering independent of registration order
- priority-based width admission and overflow count

GUI/runtime verification must additionally check:

- the bar persists across screen navigation
- screen content ends above the bar
- left/right groups do not overlap at narrow widths
- language switching updates static and dynamic labels
- modal presentation disables status interaction
- workspace exit hides workspace-only snapshots

## Related Documents

- [GUI Screen Host Architecture](./gui-screen-host.md)
- [Editor Panel and Tab Architecture](./editor-panel-host.md)
- [Editor Modal Host Architecture](./editor-modal-host.md)
- [Plugin System Architecture](../extensions/plugin-system.md)

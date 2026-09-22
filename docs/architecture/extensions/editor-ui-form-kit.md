# Extension Declarative Form Kit

## Purpose

`Horo/Extensions/EditorUiForm.h` is the public standard-component vocabulary
for host-rendered extension forms. It is deliberately separate from
`Horo/Editor/EditorUiComponents.h`: the latter is an in-process Dear ImGui
implementation detail, while this contract is usable by GUI, headless, and
future out-of-process adapters.

## Contract

An extension publishes an owned `EditorUiForm` through
`EditorUiFormBuilder`. The public primitive set contains:

- label, text, help, and validation messages;
- text, number, boolean, choice, path, color, and vector fields;
- typed action nodes; and
- group, stack, row, and grid layout nodes.

Every node has a stable identity. Value fields refer to a typed
`EditorUiBindingId`, and actions refer to a typed `EditorUiActionId`. The form
owns copied strings and values; no node stores a callback, editor pointer,
renderer resource, service locator, or persistence authority.

Labels, descriptions, help, and validation messages are localization-key text
unless they are explicitly marked as technical text. The host owns locale
resolution, shaping, truncation, accessible-name fallback, focus traversal,
keyboard navigation, disabled/read-only presentation, and action routing.

The extension or owning capability supplies current field values and receives
typed action/value requests. Domain validation, configuration commits, and
persistence remain with the owning capability or configuration service. A
validation node is presentation evidence, not a validator callback or a second
source of truth.

## Bounds and lifecycle

`ValidateEditorUiForm` applies finite bounds to identity and text sizes, node
count, nesting depth, choices, and validation messages. Parent nodes must be
declared before children, node/binding/action identities are unique, and the
closed node vocabulary rejects unknown or mismatched payloads. A form is inert
metadata: validation does not register a surface, invoke extension code, query
services, or select a backend.

The host should validate and copy the form before attaching it to an
`EditorSurfaceDescriptor`. Detach closes action/value admission and discards
the session projection; late module results are rejected by the owning surface
generation.

## Theme, DPI, and adapters

`EditorUiThemeFrame` is a compatibility alias for the complete
[`EditorThemeFrame`](./editor-theme-token-contract.md) contract. It is
immutable evidence for one host frame and carries the revision, UI scale,
resolved colors, typography, spacing, sizes, radii, interaction, motion, font,
icon, and accessibility tokens. It carries no raw theme-file access or editor
handles. Unsupported semantic roles are resolved through the shared typed
fallback policy.

`BuildEditorUiRenderSnapshot` projects the same validated form into an
adapter-neutral snapshot containing stable IDs, node kinds, focus order,
responsive geometry, scale-adjusted metrics, semantic token references, and the
complete resolved theme frame used for that projection.
The Dear ImGui/editor adapter and non-GUI inspection/test adapters consume this
same projection. A theme or UI-scale change publishes a new frame and therefore
rebuilds every node's geometry and semantic roles without extension-owned
reconstruction or restart.

The snapshot is not a renderer ABI. It contains no ImGui types, SDL events,
native handles, draw commands, arbitrary property maps, or extension callbacks.

The full role/version/fallback/accessibility contract is documented in the
[Extension Theme Token Contract](./editor-theme-token-contract.md).

## Reference fixture

`examples/extensions/gui-form-basic` builds a non-trivial settings form using
only the public form primitives and validates its headless render projection.
`tests/unit/extensions/EditorUiFormTests.cpp` covers the complete primitive
vocabulary, invalid trees and values, bounds, theme fallback, focus order, and
responsive geometry.

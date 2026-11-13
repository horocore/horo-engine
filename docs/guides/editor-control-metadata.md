# Editor shared-control metadata

The editor's `GuiScreenHost` owns a bounded observation capture bound explicitly
to its actual ImGui context. Shared controls submit role, resolved name, typed
value and current framework state immediately after their real item. The first
coverage includes buttons, activity/icon buttons, checkboxes, toggles, text
inputs, text links, theme chips, and dropdown fields/rows. Other shared primitives and raw-widget bypass
coverage remain pending.

This implements the design system's baseline metadata seam. These observations
are distinct from a Runtime UI accessibility snapshot: they have no Runtime UI
node identity, audience, focus graph, presentation receipt or native action grant.
Runtime UI retains those authorities. The platform bridge's capability is
unchanged; this capture does not establish native screen-reader support.

## Ownership and lifecycle

The capture is target-private to `HoroGui`. The non-installed
`HoroEditorControlMetadataInternal` interface exposes only its narrow header to
GUI implementation and regression tests, with an explicit Runtime UI schema
dependency. No repository-wide include path or new installed header is added.

One capture may bind each framework context. Binding neither reads nor replaces
foreign `ImGuiIO::UserData`. Actual lifecycle hooks start and finish frame
attempts, and context shutdown revokes the context borrow. Owner destruction
removes only its own hooks. Explicit rebinding retires the previous context's
observations; shutdown does not permit host drawing to recreate the binding.
All operations run on the existing GUI owner thread, including context teardown.
There are no worker callbacks or independent mutable widget/focus values.

Built-in GUI live compilation resolves the observation/lifecycle symbols to their
original host addresses. Mutable capture storage stays with the host. Changes to
its private layout, the GUI host object layout or shared headers require a host
rebuild/restart under the existing GUI live-development contract.

## Bounded observations and privacy

Cold setup reserves two stores, each with at most 2,048 controls and 128 KiB of
copied text. Each field is limited to 4,096 bytes. Context-hook lookup has a fixed
64-hook admission ceiling. Identity duplicate detection uses a reserved table.
Successful registration copies into those stores without growing containers.

The complete attempt is published at the framework's actual end-of-frame hook.
Capacity, duplicate source identity and invalid-fact failures expose empty control
and text spans with an explicit status. The next frame may recover. Source IDs
remain actual window/item observations and cannot authorize native callbacks.

Framework label identity suffixes are excluded from names. Resolved tooltip and
content text preserves literal hash characters. Missing visible names are counted;
hidden widget IDs and text-input placeholders are not substituted as names.
Resolved labels are copied before return. Editable input values are redacted, and
the capture enforces redaction even if a caller supplies a text value. Framework
focus/disablement override conflicting supplied state bits.

Frame views are synchronous borrows, valid until the next completed frame or
capture retirement. An asynchronous consumer must use the existing Runtime UI
publication/copying boundary. Persistent QA recording, its explicit opt-in,
semantic publication, native dispatch and a review panel remain separate work.

## Migration and verification

`GuiScreenHost` gains a private owned member, changing its C++ object layout.
Rebuild the editor, all C++ consumers of that host header and built-in GUI live
modules; restart the development host. Component props, the versioned Runtime UI
semantic schema and the gameplay module C ABI are unchanged. Public-header
ownership remains with the existing `HoroGui` owner.

Eight added cases in `HoroEditorUiComponentsRenderTests` exercise actual shared
widgets and framework frame hooks: copied labels/typed values/redaction, observed
focus ownership and allocation probing, foreign context isolation/rebinding,
both destruction orders, capacity recovery, duplicate IDs, keyboard link activation
and inherited link disablement. Text links now participate in actual framework
keyboard navigation and retain their existing visible focus rendering. The full family
now contains an expected 51 cases. Native execution and required final-source
quality analysis are pending; the earlier 43-case result applies to the source
before this integration.

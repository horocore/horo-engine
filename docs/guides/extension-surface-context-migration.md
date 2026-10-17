# Extension-scoped editor surface context

HORO-105 adds the restricted context that follows an
`EditorSurfaceDescriptor`. The descriptor remains inert metadata; a host now
binds it to an exact extension/module activation through
`EditorSurfaceContextProvider::Attach`.

## Migration

- Build an `EditorSurfaceContextDescriptor` with bounded, canonical allowlists
  for commands, state keys, services, localization keys, and diagnostics.
- Obtain `ExtensionActivationLease` from the activation's capability admission.
- Attach with only the `ExtensionCapabilityHandle` grants approved for that
  surface. Declared capabilities without a grant remain unavailable.
- Retain the move-only `EditorSurfaceContextRegistration` through the surface
  session and reset it during detach before revoking the activation admission.
- Treat copied contexts as disconnected after reset, provider shutdown, or
  activation revocation. Do not retain `EditorLayer`, ImGui, renderer, native
  platform, service-locator, or callback ownership.

The change is additive for descriptor-only consumers. The context header is
owned by `HoroExtensions`; its public-header consumer is generated from the
target ownership registry, so consumers must depend on `HoroExtensions` rather
than a repository-wide include root.

## Explicit editor host composition

HORO-108 groups the C++ `ExtensionManager` constructor's editor publication
inputs in `ExtensionEditorHosts`. Replace the trailing activity/page arguments
with one value:

```cpp
ExtensionEditorHosts{.activities = activityHost, .pages = pageHost}
```

`GuiScreenHost` and the activity/page native host tests use this explicit
composition. An omitted member denies its corresponding publication; creating
the carrier does not register or activate anything. Rebuild C++ consumers
against the new constructor declaration. The native C ABI 1.4/1.5 descriptors
and callback layouts are unchanged.

A const `EditorPageHost` borrow remains observational. Registering, editing,
applying, cancelling, focusing and shutdown require the mutable owner-lane
host. Foreign provider exceptions must complete the failed asynchronous
handoff. Foreign durable-store exceptions preserve `RecoveryRequired`, since
replacement may already have happened; cancellation cannot turn that outcome
into a successful publication.

Package lifecycle hosts migrate the previous `editorActivityHost` configuration
member to `editorHosts.activities`, and supply `editorHosts.pages` when admitting
settings/modal contributions. The package bridge forwards the same carrier to
`ExtensionManager`; it neither discovers hosts nor treats an activity owner as
page authority. An interactive page-only host can activate an admitted native
presentation module, while headless and omitted-host compositions refuse that
role. The verified package registration/retirement regression exercises the C11
page fixture through this production bridge.

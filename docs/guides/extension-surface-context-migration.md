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

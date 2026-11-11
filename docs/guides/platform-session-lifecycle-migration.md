# Frontend cancellation and session invalidation

PLS-006.4 enforces the existing frontend-issued-handle cancellation contract.
Different backends may use the same numeric request ID and request-store generation.
Those numbers alone cannot prove which frontend admitted a request; a retained
old-account handle must not cancel a new account's coincidentally equal request.

`PlatformRequestHandle` now carries a private weak issuance capability populated by
`PlatformServicesFrontend` when it returns an admitted handle. `RequestCancel`
checks that capability before calling the backend. The frontend owns one small
authority allocation for its lifetime. Handles retain neither the frontend nor
request/provider resources, and moving a frontend or handle transfers provenance.
There is no duplicate request store, session generation or mutable account pointer.

Conforming public consumers need no call-site change: obtain handles from the
frontend and cancel them through that same frontend. Code obtaining a handle
directly from a backend or `PlatformRequestStore` must cancel through its owning
backend/store. Passing it to a frontend now returns `platform.request.stale` before
any backend call. C++ consumers must rebuild because the private handle layout
changed. The native provider C ABI is unchanged.

`HoroEngine::PlatformServices` continues to own `PlatformRequest.h` and
`PlatformServicesFrontend.h`; no header dependency or SDK visibility is expanded.
The affected staged consumers are `HoroPlatformServicesPublicHeaderConsumer` and
`HoroPlatformServicesExtensionPublicHeaderConsumer`, with runtime regressions in
`HoroPlatformRequestTests` and `HoroPlatformProviderAdmissionTests`.

The native lifecycle host proactively terminalizes stale in-flight work and sends
cancellation once while retaining its native lease until callback retirement or
successful drain. Session ingress and terminal publication share a fence. An
already expired admission deadline retains timeout precedence, and already
terminal results remain immutable. Cancellation rejection is never permission to
release provider code or callback state early.

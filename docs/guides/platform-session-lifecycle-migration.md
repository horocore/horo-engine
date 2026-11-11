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

## Protected binding handoff

`PlatformSessionBinding.h` adds the identity-owned handoff needed by explicit
same-binding replay. A private authenticated identity broker resolves the existing
protected product/environment/provider namespace and stable account pseudonym,
then builds `ProviderAccountBindingId` and `PlatformSessionBindingEvidence` against
the selected provider capabilities and current published Active snapshot. The
namespace comes from its protected storage owner; no second product identity or
current-account authority is introduced. Inputs contain no native IDs, display
hashes, credentials or live handle bytes. Ordinary consumers receive no binding
byte accessor, stream formatter or persistence codec.

Composition keeps `PlatformSessionBindingIssuer` exclusively in that private broker.
Both builders require this capability. A consumer holding an expected binding and
the public current snapshot cannot issue a new association through a separately
created issuer: exact weak issuing provenance participates in binding equality and
capture validation. Moving the issuer preserves its authority; Close/destruction
revokes its bindings and evidence without retaining or releasing native resources.
Issuance, revocation and guarded commits share the composed engine owner lane.

The public binding object is an issuer-bound view of the durable protected value.
The protected store retains its canonical namespace/pseudonym inputs. After broker
restart, the trusted owner resolves an old durable partition into a new expected
binding under the new issuer before reauthorization. Public binding objects and
evidence are never the persisted record or a second identity database.

The durable queue/save/progression owner obtains its expected opaque binding from
an explicit trusted host association with its protected partition. Before replay
or commit it calls `ValidatePlatformSessionBindingAccess` with current provider
capabilities, the canonical current session, fresh broker evidence, expected
binding and exact service. The check preserves Null, unavailable, restricted,
stale session/access and binding-mismatch failures. A new current account cannot
retarget an old partition. Same-binding reauthentication requires new evidence;
captured live evidence is never written into a durable record.

These functions validate Horo association and generation consistency. Native
authentication and protected stable-subject resolution are preconditions on the
private broker caller, qualified separately by provider tests. A provider unable
to resolve that mapping returns `platform.identity.mapping_unavailable` and keeps
account-bound replay unavailable. The public helper performs no account lookup,
credential storage, disk operation, lifecycle callback or provider call.

The header remains solely owned by `HoroEngine::PlatformServices`; generated staged
header coverage and `HoroPlatformFriendsPublicConsumer` cover its opaque types.
`PlatformSessionBindingTests.cpp` exercises same-binding reauthorization, cross-
account/environment rejection, consent changes, capability absence and sign-out.

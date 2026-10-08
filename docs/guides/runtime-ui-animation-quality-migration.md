# Runtime UI animation signature migration

Rebuild C++ consumers after these signature changes. C ABI and published frame
record layouts are unchanged.

`UiAnimationOwner::Reload` groups its policy and structural commit point in
`const UiAnimationReloadAdmission&`: pass `{policy, point}` before the optional
cancellation token. This synchronous inert record is borrowed until return;
replacement generations, registries, styles and definitions remain transferred
values. `UiAnimationRuntimeParticipant::Reload` retains its host call syntax.

`UiAnimationRuntimeParticipant::Navigate` now borrows
`const UiRouteOperationRequest&` until return. Admission still copies retained
request evidence. Update member function pointer aliases accordingly.

The following operations now use const member function signatures while mutating
their explicitly owned internal state: `RuntimeStyleRegistry::BeginRetirement`
and `Shutdown`, `UiStyleResolver::Prepare`, `UiLayoutClipEngine::Prepare`,
`UiAnimationClockController::Seek`, and the private prepared-publication helpers
on layout, routes and pointer capture. Update member function pointer aliases
and rebuild all callers; owner-thread and lifecycle admission remain required.

`UiAnimationOwner::Navigate` also borrows its request until return; route
admission retains a copy. `UiAnimationClockController::Step`, layout invalidation,
preparation and retirement, clipping publication and shutdown, style invalidation
and shutdown, and abandoning prepared action replacements now have const member
signatures. These remain owner-thread operations with the existing admission
checks.

`UiScreenStack::DrainRetiredActions` still requires a mutable owner. During router
reclamation, it temporarily withdraws the stack's publisher handle and holds its
state locally. Reentrant calls cannot admit new work during this
quiescent drain; the handle is restored on every exit. Const views cannot reclaim
retired actions.

Animation scratch arrays are grouped by purpose and the committed-tick ledger
uses a vector allocated once at creation. The ledger never grows during frame
processing; moving it leaves the source unavailable, as before.

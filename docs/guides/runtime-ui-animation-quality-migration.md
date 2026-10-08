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

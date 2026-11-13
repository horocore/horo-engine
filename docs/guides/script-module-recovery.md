# Script module API recovery

Provider API refresh uses the canonical export descriptor fingerprint and compatibility validator. The runtime's report contains alias-sorted actual fingerprints and a registered diagnostic for breaking or unavailable candidates. Provider version text, a pathname, an arbitrary caller hash, and an uninspected native image never establish compatibility.

A compatible in-process typed provider is registered in the **same** capability/backend/invocation registries. The new binding carries the actual resolver result, exact provider generation, grants, descriptor snapshot and code lease. The existing opaque backend port re-resolves the actual imported member on every invocation. No parallel provider registry, callback dispatcher, permission, or VM scheduler is introduced.

`PrepareReplacement` derives the candidate from the original operation's actual registry, scopes, source owner, policy, runtime and resource limits. A rebind cannot change the signed operation's aliases or reset its memory, instruction or execution deadline. `PrepareRebind` prepares new namespaces and plans inside the same Lua/QuickJS VM. `Validate` runs before publication; `Commit` only swaps the prepared binding/namespace roots. It does not allocate, wait, invoke a provider, evaluate source, or run user callbacks. The host prepares and validates **all** affected operations before any commit. A rejected or abandoned candidate leaves every previous binding usable.

Lua function upvalues and QuickJS function data contain stable alias/function identities instead of indices into a descriptor array. Held functions therefore use the newly admitted exact member after an additive update, even if a new function sorts before it. VM locals, closures, coroutine/Promise continuations and existing script object identities survive. A held namespace object remains its existing value snapshot; a fresh `horo.import(alias)` exposes additive members. Removed members, altered existing signatures and weakened required bindings are rejected. Opaque handles retain their original authority; an update never relabels them for a new provider generation. A retained handle may therefore be denied by the new generation even though its VM value survives. A source edit is a separate explicit new-run lifecycle: `ReplaceCompatible` requires the original source digest and never reruns it.

## Accepted work and closing

The old context is **sealed for new admission**, not cancelled, when publication succeeds. Accepted work retains its original controller, backend/source grants, code lease and generation. Its actual terminal result is delivered to its existing pending handle in the same VM. Only after its real jobs and VM delivery drain is the predecessor context closed. One preparation and one draining predecessor are allowed per VM; another update receives typed backpressure. The product provider owner likewise retains one active and one draining provider registration. Explicit Close, module/source revocation, Scene change, Stop and reload continue to cancel normally. Close does not force an uncooperative native callback to finish: canonical backend call/retirement leases retain it under the existing job/registry bounds. No successful completion or safe unload is inferred from cancellation.

All VM methods except `Close` are owner-lane operations. The VM State stays the same and is retained by outstanding preparations. Owner-only mutable namespaces/containers are never read by concurrent Close. An atomic shared immutable publication holds the actual active and predecessor contexts. Close first sets the sticky close latch and snapshots that publication; commit publishes the new contexts then checks the latch. These four handshake operations are sequentially consistent, preventing the store-buffering result in which both sides observe the other's old value. If Close snapshots the old publication before commit, commit closes the new context before returning. A closed operation cannot be revived by a prepared update.

## Product safe points

`ScriptModuleHostServices::Rebind` is the generic same-registry typed-provider batch path. Its bounded operation list rejects duplicate or foreign operations and checks the actual active signed capture again. The builtin reference API demonstrates a real additive 1.0.0 to 1.1.0 provider replacement; the additive `apirevision` member returns `1.1.0`. It does not stand in for arbitrary native library replacement.

The signed product CLI supports an explicit request:

```text
horo-engine extension module-run --runtime lua|javascript ... --approve-native --approve-script --refresh-reference
```

All existing required package/trust/path arguments remain required. The option does not grant the reference capability: the signed module, project policy and context policy must already admit it. The CLI begins the actual operation, then requests the compatible owner-safe-point update before polling its continuation.

Editor Play/Workspace exposes `RequestModuleReferenceUpdate`; the request is consumed at the existing owner Poll safe point after the real Scene epoch refresh. Stop, Scene replacement and reload revoke that epoch and discard a queued update. Failure leaves the old operation active and produces a typed update report. A real native package update request verifies the candidate's signed bytes under current independent trust and requires the actual live source owner. It reports `RestartRequired` without staging, unloading, replacing, revoking or calling the candidate. The report is not approval of its future native ABI, policy or unload safety. A changed candidate cannot enter the old active artifact generation; the prior verified provider can continue serving its existing bindings.

## Durable authoring data

Module operations are ephemeral. This feature does not serialize a VM heap, create a second checkpoint, invent a state file, or move object behaviors to JavaScript. ADR-006's Lua object-behavior scope is unchanged and ADR-059 remains Proposed.

The existing `SceneDocument`/scene source codec owns authored `BehaviorComponent` identities, schema version, enabled state and fields. Real `.horo_script` files and sidecars remain project assets. Their persistence does not depend on a module/provider being currently available. Project gameplay discovery records a missing required native generation as an actionable blocking diagnostic while the document remains loadable, inspectable, editable and saveable. Recovery supplies a real compatible module generation through the existing project registry/Play lifecycle; it does not reconstruct stored authoring values from a provider or a VM.

## Compatibility and qualification

This change adds C++ virtual methods to `IScriptModuleRuntime` and a sealed C++ preparation type. Rebuild runtime adapters and all consumers against the matching snapshot. The extension C transport/package ABI remains **1.8**; no old native vtable or wire layout is changed.

The source packet includes genuine Lua/QuickJS regression definitions for held functions/locals, pending async delivery, optional disappearance/recovery, aborted and breaking candidates, batch rejection, concurrent/reentrant Close, signed Headless and actual Editor Play safe points, the real CLI, native last-good/restart diagnostics, and real project Scene/source persistence. These are authored tests until the serialized root build and executions qualify their exact source identities. They do not establish hosted platform or native GUI visual clearance.

## Canonical script error identity migration

This development snapshot changes the textual identities of the17 descriptor/invocation errors below so they can participate in the existing host-owned ErrorCodeRegistry. This is an explicit wire-identity compatibility change. Rebuild consumers and update stored expectations; old textual aliases are not accepted. Existing C++ descriptor symbols, the `horo.extensions` domain, severity, messages, diagnostics, cause chains, retry policy and extension C transport1.8 layout are unchanged.

The CLI composition declares an exact producer table and preserves normal public redaction and trusted-local projection. Registry namespace validation is unchanged; no runtime prefix inference or alternate error envelope is introduced. Other preexisting legacy extension error families are outside this scoped migration and are not claimed as covered.

| Previous textual code | Current textual code |
| --- | --- |
| `script_invocation_invalid` | `extensions.script_invocation_invalid` |
| `script_invocation_thread_violation` | `extensions.script_invocation_thread_violation` |
| `script_invocation_reentrant` | `extensions.script_invocation_reentrant` |
| `script_invocation_unavailable` | `extensions.script_invocation_unavailable` |
| `script_invocation_provider_duplicate` | `extensions.script_invocation_provider_duplicate` |
| `script_invocation_capacity_exceeded` | `extensions.script_invocation_capacity_exceeded` |
| `script_invocation_shutdown` | `extensions.script_invocation_shutdown` |
| `script_invocation_backpressure` | `extensions.script_invocation_backpressure` |
| `script_invocation_cancelled` | `extensions.script_invocation_cancelled` |
| `script_invocation_timeout` | `extensions.script_invocation_timeout` |
| `script_invocation_provider_revoked` | `extensions.script_invocation_provider_revoked` |
| `script_invocation_context_revoked` | `extensions.script_invocation_context_revoked` |
| `script_invocation_abandoned` | `extensions.script_invocation_abandoned` |
| `script_export_descriptor_invalid` | `extensions.script_export_descriptor_invalid` |
| `script_export_descriptor_conflict` | `extensions.script_export_descriptor_conflict` |
| `script_export_descriptor_incompatible` | `extensions.script_export_descriptor_incompatible` |
| `script_export_descriptor_capacity_exceeded` | `extensions.script_export_descriptor_capacity_exceeded` |

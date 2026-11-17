# ADR-059: Script-Consumable Module Boundary

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Authority, trust, binding, import resolution, marshalling, asynchronous work, permissions, errors, reload and shutdown for module APIs consumed by editor tooling and gameplay scripts
- **Issue**: [EXT-005.1](https://github.com/HoroCore/horo-engine/issues/168)
- **Jira**: [HORO-168](https://horo-engine.atlassian.net/browse/HORO-168)
- **Parent**: [EXT-005](https://github.com/HoroCore/horo-engine/issues/167)
- **Related**: [ADR-006](006-lua-5-4-gameplay-runtime.md), [ADR-008](008-error-model-exception-boundary-and-registry.md), [ADR-010](010-job-waiting-and-operation-store-ownership.md), [ADR-054](054-extension-and-package-authority-boundary.md), [ADR-055](055-extension-manifest-v1-typed-model.md)
- **Normative documents**: [Extension Capability Roadmap](../architecture/extensions/extension-capability-roadmap.md), [Extension System](../architecture/extensions/plugin-system.md), [Gameplay Behavior Authoring](../architecture/extensions/gameplay-behavior-authoring.md), [Gameplay Module Boundary](../architecture/extensions/gameplay-module-boundary.md), [Package Lifecycle](../architecture/packages/package-lifecycle.md), [Application Security](../architecture/security/application-security.md)

## Context

Backend modules may expose useful services to editor tooling scripts and gameplay
scripts. The extension manifest already separates a backend service export from a
script API descriptor, and ADR-006 selects a private Lua 5.4 adapter for gameplay
behaviors. The architecture does not yet assign one owner for resolving those
exports into a script runtime, enforcing trust and permissions, marshalling
values, scheduling asynchronous calls, preserving errors, or revoking bindings.

Without one boundary, a runtime adapter could become a second service locator,
bind directly to module function pointers, expose different logical APIs per
language, or keep native callbacks alive after provider shutdown. Tooling scripts
could acquire runtime scene authority accidentally, while gameplay scripts could
bypass the behavior lifecycle and create a second startup, tick, reload, or
serialization model.

The decision must support the selected in-process Lua gameplay runtime without
making Lua syntax, stack objects, native pointers, or a particular process
transport part of the public module API. It must also leave a deliberate path to
isolated providers or future script runtimes without changing service identity or
project-authored imports.

## Decision

### 1. The supported baseline is a host-mediated in-process binding

The initial supported execution model is:

```text
script source / generated import declaration
  -> host-owned ScriptImportResolver
  -> validated language-neutral ScriptApiDescriptor
  -> runtime-owned binding adapter
  -> host-owned ScriptInvocationGateway
  -> authoritative backend ServiceExport
```

The script runtime is embedded in-process behind a Horo-owned sandbox adapter.
The script never calls a provider dynamic library, C function table, C++ object,
IPC transport, service locator, or application singleton directly. Every call
passes through the host-owned invocation gateway after import, context, generation
and permission validation.

An in-process native provider is trusted code only after package verification,
explicit trust, enablement, compatibility validation and host activation. The
sandbox protects the host capability surface from scripts; it is not a security
boundary against a trusted native provider running in the same process.

Provider execution may later move behind an isolated worker or RPC transport.
That transport remains a private implementation of the same service export and
invocation gateway. Project metadata, script imports, descriptors, values, errors
and lifecycle outcomes cannot reveal or depend on provider process location.

### 2. Service, descriptor, adapter and context have separate authority

| Owner | Authoritative responsibility | Must not own |
| --- | --- | --- |
| Backend service provider | Business state, typed operations, domain validation, results, provider cancellation and provider shutdown | Script VM objects, script import policy, editor presentation or gameplay lifecycle |
| Validated service export | Stable backend contract identity, API version, invocation policy, lifetime and required capabilities | Native implementation pointers or runtime-specific syntax |
| Validated script API descriptor | Stable script API identity/version/namespace and language-neutral mapping to one compatible service export | Provider selection, trust grants, VM handles or executable callbacks |
| Host import resolver and invocation gateway | Deterministic provider resolution, context admission, effective permissions, generation leases, scheduling, cancellation routing and error preservation | Business logic or language-specific presentation |
| Script runtime binding adapter | Runtime module/namespace construction, generated glue, VM stack conversion, promise/error projection and VM call-frame containment | Service discovery, permission grants, provider lifetime or a second logical API |
| Script context | Declared imports, approved capabilities, owner-thread/safe-point policy, cancellation group, diagnostic identity and binding leases | Ambient application access, provider ownership or persistent service state |

Descriptors are inert. Decoding or validating a descriptor cannot load a module,
activate a service, create a VM, generate live closures, resolve a provider or
mutate an ambient registry. The application/runtime composition root activates
providers and creates script contexts explicitly.

### 3. Service and script API identities remain distinct

ADR-055's `ExtensionServiceExportDescriptorV1` names the backend-neutral callable
contract. `ExtensionScriptApiDescriptorV1` names an independently versioned script
projection over exactly one compatible service export. Textual namespace equality
does not make the identities interchangeable.

The complete language-neutral export schema is owned by
[EXT-005.2](https://github.com/HoroCore/horo-engine/issues/169). This ADR fixes
its boundary requirements:

- the descriptor references stable Horo IDs, semantic versions and registered
  value/error contracts, never symbols, header names or source-language types;
- namespace and API conflicts are rejected deterministically before binding;
- provider choice is resolved from the verified package graph and exact service
  identity/version constraints, never package load order, filesystem order or
  first response;
- required imports fail context creation when missing, denied, ambiguous or
  incompatible; optional imports remain explicit unavailable results;
- one canonical descriptor digest is the input to every runtime adapter that
  declares support for that contract.

A package with several backends or script APIs declares each module, export and
binding explicitly. A hybrid GUI/backend/script package does not merge their
lifecycle authorities.

### 4. Generated bindings are derived caches, not authority

Runtime-specific glue is generated or materialized only after the descriptor,
provider composition, runtime compatibility and effective permissions validate.
Its cache key includes at least the canonical script API descriptor digest,
service contract/version, runtime adapter identity/version, target profile and
effective binding-policy digest.

Generated source, bytecode, closures, runtime tables and reflection caches are
derived artifacts. They do not become package, API, service or persistence
identity. Invalidating or deleting the cache must reproduce an equivalent logical
API from the same validated inputs. Runtime-specific names may improve idiomatic
syntax, but cannot add functions, relax nullability, change errors or silently
coerce values beyond the shared descriptor.

### 5. Values cross one bounded language-neutral invocation boundary

Calls use copied or explicitly owned Horo values and generation-safe opaque
handles. No native pointer, reference, function pointer, VM object, STL container,
exception, allocator ownership, borrowed module string/view, platform handle or
unbounded object graph crosses the boundary.

The marshalling contract has explicit encoding, depth, element, byte and work
limits. Conversion failure returns a typed error before provider invocation or
before completion reaches the script. Opaque handles name host-owned resources,
record their type, generation and owning script context, and fail closed after
revocation; they are not pointer encodings.

The exact value algebra and async call ABI are owned by
[EXT-005.3](https://github.com/HoroCore/horo-engine/issues/170). Runtime
adapters may project the same values into idiomatic language forms only when the
round trip preserves the logical value and error semantics.

### 6. Asynchronous work remains host-owned

A script call does not create an untracked provider thread or block the VM owner
thread waiting for backend work. The invocation descriptor selects a closed host
policy such as immediate owner-thread work or an admitted asynchronous operation.

An asynchronous invocation receives a host operation identity, cancellation
source, bounded progress/result channel and exactly one terminal result. The
adapter maps that operation to the runtime's promise, coroutine or future shape;
the language object does not become operation authority. Completion is marshalled
back to the script context's declared owner thread and safe point.

Context destruction revokes delivery first. In-flight work is then cancelled and
joined when context-owned, or detached only when the service descriptor explicitly
defines a provider-owned durable operation whose result can outlive the caller.
Detached completion remains in the host result store and never calls a dead VM.
Provider disable or shutdown cannot complete an invocation twice or leave a
pending script promise without a terminal cancellation/unavailable error.

### 7. Effective authority is the intersection of declared policy

Installation makes descriptors available only as inert verified data. A live
binding requires all of:

1. a verified package/install record and compatible validated extension
   descriptor;
2. an enabled and successfully activated backend provider;
3. approved provider trust and permissions;
4. a script source/package declaration naming the import and acceptable API
   version;
5. a script context profile that permits the service capability and operation;
6. a compatible runtime adapter and binding schema; and
7. a live provider generation lease.

The effective permission set is the intersection of product/organization policy,
package envelope, approved module permissions, script trust, declared import and
context allowlist. No layer can grant a permission forbidden by an earlier layer.
Possessing a service/API ID or guessing a namespace never grants import authority.

The detailed permission and context model is owned by
[EXT-005.5](https://github.com/HoroCore/horo-engine/issues/172), but it must
preserve this intersection and revocation rule.

### 8. Tooling and gameplay contexts are different profiles

Editor tooling scripts run in an application/tooling script context. They may
import approved headless application capabilities and editor-safe command/query
adapters. Document or project mutations go through host commands, transactions and
undo policy. A tooling script receives no `BehaviorContext`, runtime scene pointer,
renderer backend, editor widget object, unrestricted filesystem/process access or
ambient extension service graph. Its lifetime is explicitly project, workspace,
operation or tool-session scoped; closing a panel alone does not own or cancel
backend work.

Gameplay scripts run only through the gameplay behavior/runtime contract selected
by ADR-006. A behavior descriptor declares required script APIs alongside its
other dependencies. Imports resolve before behavior activation against the host
composition for that scene/play session. The runtime adapter injects binding
handles into the same behavior instance context that owns lifecycle callbacks.

Calling a module API does not create a second gameplay lifecycle. It cannot add
module-level script `Start`, `Tick`, `Reload` or `Stop` callbacks outside the
existing behavior/system/service schedule. Scene reads and mutations continue
through `BehaviorContext`, declared access and deferred commands; a service API is
not a shortcut to editor state, unrestricted scene mutation or native globals.
Behavior fields and scene identity remain serialized independently from provider
availability.

Shared APIs may be exposed to both context profiles only through one service and
script API identity with explicit operation-level context compatibility. The host
does not infer tooling safety from gameplay availability or vice versa.

### 9. Errors preserve one semantic identity

Providers return Horo `Result`/error values whose domains and stable codes are
declared by the validated extension model. The invocation gateway adds bounded
package, module, service, script API, script source/context and operation evidence.
It does not replace the cause with localized text.

Adapters map the same stable error into an idiomatic language error object while
preserving domain, code, retryability, cancellation and bounded evidence. Native
exceptions and VM exceptions are caught at their owning adapter boundary and
translated once. Secrets, arbitrary provider payloads, source contents, native
addresses and unbounded stack dumps are excluded from ordinary diagnostics.

### 10. Reload replaces immutable generations at safe points

Service exports, script API descriptors and binding sets are immutable generation
snapshots. A candidate package/provider/API composition is fully decoded,
cross-reference validated, permission evaluated, activated and binding-generated
without mutating the current generation.

At a context-appropriate safe point:

- an API-compatible candidate may atomically replace new-import resolution and
  refresh existing bindings when the runtime adapter proves compatibility;
- a breaking candidate keeps the previous generation active until consumers are
  recreated, migrated or report restart required;
- a failed candidate leaves the previous valid provider and bindings unchanged;
- removed or denied APIs revoke bindings and make later calls return a typed
  unavailable/revoked result;
- the old provider remains loaded until all binding leases, handles, callbacks and
  operations for that generation drain.

Native code is not unloaded merely because a registry pointer was replaced. If
callback/job/handle quiescence cannot be proven, the update is restart-required.
Serialized gameplay fields, project assets and tooling configuration cannot depend
on a live provider pointer and remain inspectable when the API is unavailable.

Retention is bounded to the active generation plus at most one draining predecessor
per provider. While that predecessor still owns any lease, later candidates may be
verified and coalesced to the newest compatible candidate, but another generation
cannot activate. Exceeding the configured drain deadline or retained-code budget
marks the newest update restart-required; repeated updates never accumulate an
unbounded chain of loaded native libraries.

Compatibility classification, migration evidence and recovery behavior are owned
by [EXT-005.6](https://github.com/HoroCore/horo-engine/issues/173). Additive
schema evolution may be compatible only when existing call shapes and semantics
remain unchanged; breaking changes require a new major API contract and explicit
consumer migration.

### 11. Shutdown follows the dependency graph in reverse

For every script context and provider generation, shutdown order is:

```text
close import admission
  -> revoke context bindings and suppress new VM deliveries
  -> cancel or detach admitted operations according to descriptor policy
  -> join context-owned work and terminalize pending promises
  -> destroy VM binding objects and invalidate opaque handles
  -> release provider generation leases
  -> stop authoritative backend services
  -> destroy provider module objects
  -> unload native libraries only when quiescence is proven
```

Project close, scene teardown, Play stop, provider disable, package update and
process shutdown use the same ownership rule with their appropriate context
scope. No script finalizer or garbage collector callback is relied on as the
authoritative release signal.

### 12. Direct-native and runtime-specific shortcuts are unsupported

The supported contract forbids:

- script FFI, `dlopen`/`LoadLibrary`, native package loading, light userdata or
  pointer-number encodings used to reach module APIs;
- providers registering Lua/C#/VM closures, stack callbacks or language objects
  as the authoritative export;
- runtime adapters discovering services through globals, native symbol names,
  filesystem scanning or package load order;
- descriptor schemas whose authoritative type system is Lua tables, C#
  reflection, C++ headers, arbitrary JSON or another runtime-specific model;
- synchronous waiting on provider jobs from frame-hot or VM owner-thread paths;
- script callbacks after context revocation, scene teardown, project close or
  provider generation release;
- importing extension GUI objects or renderer/platform native handles;
- using a script module import to bypass behavior scheduling, document commands,
  permissions, trust, package authority or host-owned operation stores.

### 13. Conformance is semantic and lifecycle-complete

The same reference service and language-neutral descriptor must produce equivalent
logical calls, values, errors and cancellation outcomes in every supported runtime
adapter. Conformance covers tooling and gameplay contexts separately, including
missing/optional/denied imports, malformed and oversized values, wrong-thread and
re-entrant calls, provider failure, context revocation, reload, project/scene
teardown and process shutdown.

No test may pass while leaving a live binding to a released provider generation,
an orphan operation, a callback into a destroyed VM, or provider-owned serialized
identity. The full runtime/platform fixture matrix and performance evidence are
owned by [EXT-005.7](https://github.com/HoroCore/horo-engine/issues/174).

## EXT-005.4 Implementation Profile

The module-operation adapters are Lua 5.4.8 and QuickJS-NG 0.18.0. They consume
one existing script-export descriptor/value/invocation boundary through the same
canonical backend and invocation registries. The product owns explicit Headless
and Editor Play operation contexts. QuickJS is a module-operation runtime; it is
not a second project-object behavior authoring language. ADR-006's accepted Lua
behavior decision remains unchanged. This implementation profile does not change
this ADR's **Proposed** status or assert architectural approval not recorded here.

`ScriptModuleBindingCache` retains bounded derived binding recipes, not executable
provider authority. Compiled adapter identity incorporates the exact pinned VM
source, binding implementation, build profile and context profile. Imports retain
actual resolved module/provider generations. The lower native bridge invokes the
existing admitted concrete service member; it never adds a provider registration
or begins a second invocation.

The explicit operation profile uses bounded cooperative execution: Lua's supported
instruction ceilings are positive multiples of its 1,000-instruction hook quantum;
QuickJS uses actual interrupt/deadline/cancellation and bounded owner job draining,
not a claimed exact opcode count. Neither profile admits VM/native loaders,
bytecode, ambient filesystem/process bindings, blocking Atomics or shared memory.
Marshalling accepts bounded copied plain data and generation-safe host handles.

Cold product admission revalidates the current independent signed publisher policy
and exact immutable capture against evidence retained from actual native loading.
An admitted operation has a finite deadline (five seconds by default). Normal
owner Poll and worker calls perform no trust-file I/O; this is an operation-scoped
trust snapshot, not continuous filesystem revocation monitoring. Actual host,
provider, context and Scene scope revocations still close call/delivery admission.

## Compatibility And Migration

ADR-055's existing service export and script API records become the manifest-side
entry to this boundary. The following migration is required before activation is
enabled:

1. Replace ad hoc runtime module names, manually installed globals and native
   binding tables with stable service export and script API IDs.
2. Generate or validate the language-neutral descriptor owned by EXT-005.2 and
   bind it to the verified package resource and canonical digest.
3. Declare script imports and version constraints in the owning script/behavior
   metadata rather than discovering providers at execution time.
4. Route calls through the invocation gateway and value ABI from EXT-005.3;
   preserve existing domain error identities where possible.
5. Place runtime-specific glue behind the registered adapter from EXT-005.4 and
   delete it from public SDK/service contracts.
6. Introduce generation leases, permission intersection and ordered revocation
   before enabling provider reload or disablement.

Legacy names may be accepted only by a bounded, diagnostic migration tool that
produces explicit stable imports. They are never an alternate live resolution
path. Current Lua gameplay behavior APIs remain private behavior-context bindings
under ADR-006 until they are deliberately represented by this service/export
model; this ADR does not reclassify every existing behavior callback as a module
API.

## Consequences

- Backend logic remains usable from GUI, CLI, MCP, tooling scripts, gameplay
  scripts and future isolated hosts without duplicating authority.
- Script runtimes can expose idiomatic syntax while conformance tests compare one
  logical descriptor, value and error contract.
- In-process Lua retains low invocation overhead, but sandboxing and bounded
  marshalling remain mandatory because process isolation is not the baseline.
- Host composition owns more machinery: import resolution, invocation admission,
  operation routing, generation leases, permission intersection and revocation.
- Provider updates cannot unload eagerly. Safe reload may retain an old generation
  or require restart, increasing memory and operational complexity.
- Scripts cannot call arbitrary native libraries or ambient engine services;
  useful APIs must first become explicit backend service exports and approved
  script descriptors.
- Editor tooling and gameplay scripts share infrastructure without sharing
  authority, lifecycle or mutation privileges.

## Rejected Alternatives

| Alternative | Reason rejected |
| --- | --- |
| Bind scripts directly to provider C/C++ functions | Leaks ABI, allocator, lifetime, exception and trust concerns into every runtime and prevents deterministic revocation. |
| Let each runtime define its own module API descriptor | Produces incompatible logical APIs, duplicate version policy and runtime-specific persistence identity. |
| Expose the whole backend service object to trusted scripts | Trust does not justify ambient authority; permissions, threading, cancellation and teardown still require per-operation mediation. |
| Make all scripts and providers out-of-process initially | Stronger fault isolation does not remove the semantic boundary and imposes transport, deployment and latency costs before the in-process Lua baseline is proven. |
| Treat the script API ID as the backend service ID | The script projection has independent compatibility, namespace, permissions and runtime support. |
| Resolve imports by package/module load order | Non-deterministic and unsafe under restore, optional providers, updates and parallel activation. |
| Give gameplay module APIs their own script lifecycle | Duplicates behavior scheduling, serialization, reload and scene ownership and creates inconsistent native/script gameplay. |
| Keep callbacks alive and fail calls only after unload | Calling unloaded code is not a recoverable error; revocation and generation drain must precede provider destruction. |

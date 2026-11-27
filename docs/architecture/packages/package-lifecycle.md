# Package Lifecycle

## Purpose

This document defines package install, trust, enable, activation, update,
uninstall, migration, conflicts, ownership states, transactional staging, and
rollback.

## Lifecycle State Machine

Install, trust, enable, and activation are separate.

```text
Resolve
  -> Download
  -> Verify
  -> Stage
  -> Install
  -> Trust
  -> Enable
  -> Activate
```

| Step | Meaning |
|---|---|
| `Resolve` | Find package version matching request, lockfile, and compatibility. |
| `Download` | Fetch package archive into temporary cache staging. |
| `Verify` | Check hash, signature, manifest, contribution descriptors, and format. |
| `Stage` | Extract with path/resource limits into an incomplete staging directory. |
| `Install` | Atomically record package availability and lock/cache state. |
| `Trust` | User or policy approves code/native/editor contribution execution. |
| `Enable` | Mark selected contributions active for the project. |
| `Activate` | Register contributions in runtime/editor registries. |

Install does not imply trust. Trust does not imply activation. Activation does
not bypass contribution validation.

[ADR-054](../../adr/054-extension-and-package-authority-boundary.md) applies this
same lifecycle to extension-only and hybrid packages. `ExtensionHost` is the
live module/contribution host for an exact verified activation candidate; it does
not own a parallel install, dependency, trust or enablement lifecycle.

[ADR-057](../../adr/057-package-manifest-v1-typed-model.md) supplies the lifecycle
input boundary. Decode and semantic validation are inert; archive publication
requires a `VerifiedPackageBundle` that binds the canonical package-manifest,
file-manifest and archive digests plus verified signature evidence. Lifecycle
state, source policy, trust and selected host artifacts remain separate records.

## Transactional Install

Package install uses the same safety posture as distribution staging:

1. Resolve one deterministic source/artifact record under
   [ADR-058](../../adr/058-package-source-policy.md), then download through its
   ordered approved transports into a temporary location.
2. Verify expected size and hash.
3. Extract with path traversal, symlink, depth, file count, and size limits.
4. Decode and semantically validate the typed package/file manifests, then verify
   every reference, digest, executable mode and required detached signature.
5. Write an install record.
6. Atomically move verified content into the active cache.
7. Remove or quarantine failed staging directories.

The editor/runtime must never load package content from an incomplete staged
installation or from a decoded/partially validated manifest. Parse, validation or
verification failure quarantines staging and leaves the previous install record
unchanged.

The first install-record boundary accepts an immutable complete restore graph,
rechecks each archive against its lock evidence, and writes a project-local
pending record under an exclusive install lock. A durable atomic replacement is
the commit point; cancellation and failures before it leave the prior record and
in-process graph unchanged. A durability error after rename may require journal
recovery before restart can safely select a graph. This record grants availability only. The service
does not infer trust, enablement, or activation from a successful install. Host
activation and restart reconstruction are subsequent lifecycle work.

## Trust And Activation

Data-only assets may be mounted after verification. The following require trust
before activation:

- `ScriptRestricted`
- `GameCodeTrusted`
- `NativeTrusted`
- `EditorExtensionTrusted`

Native code is trusted code, not sandboxed. A native contribution cannot be made
safe merely by reducing permissions.

In non-interactive mode, new trust requirements fail unless a policy allowlist or
previous user-local trust state exists.

### Editor Gameplay Package Approval

The ordinary desktop Editor accepts an explicit startup project, a separate
publisher roots document in the existing signature-document format, and one or
more exact gameplay module approvals.

```text
HoroEditor --project <absolute-project> --gameplay-package-publisher-trust <absolute-roots-json> --approve-gameplay-package <package> <extension> <module> <archive-sha256> <manifest-sha256> <descriptor-revision>
```

The publisher roots document must be outside the project and package cache.
Repeat the approval option for additional modules. All approvals for one package
must identify the same archive, manifest and extension. The archive publisher
signature is reverified from the offline cache. The selected native role also
requires its own signed `<artifact>.signature.json` sibling in the exact
verified contribution inventory. The SDK build fingerprint comes from the host.

`--approve-importer-package <package> <extension> <module> <archive-sha256> <manifest-sha256>`
uses the same project and publisher binding. The Package dialog can activate
the declared importer module. The native owner verifies the detached signature
and admits importer contributions from that exact module. Removal reservations
carry the same immutable approval identities, so reference review can identify
an importer outside the configured owner's scope.
Import, file drop, and Content Browser operations acquire a combined catalog for
the current project. Duplicate contribution IDs fail catalog composition. New
operations see the host catalog when the project module is disabled, and open
import operations retain their captured catalog until they close.

Startup approval makes the role available for explicit activation from the
existing Package dialog. Decision descriptors become visible only after that
activation publishes and disappear when it is withdrawn. The configured package
host is bound to the one startup project. A selected XR package host currently
rejects the combined gameplay package request. The default desktop package
host continues to require configured execution authority.

### Extension Activation Hand-Off

For each enabled extension descriptor, `PackageLifecycleService` lends
ExtensionHost one immutable candidate containing the verified install-record
identity/lease, package and manifest digests, selected declared module artifact,
approved permissions/capabilities and target activation generation. The host
cannot accept a caller-selected raw directory or scan for another descriptor.

ExtensionHost validates the descriptor/ABI/artifact binding, builds the complete
candidate contribution batch and commits live registrations atomically. Its
returned generation/leases prove live activation; only then may the package
lifecycle snapshot publish `Active`. Failure leaves the package installed and
records a typed activation outcome without granting trust or partial registry
state.

Disable, update, rollback and uninstall first detach contributed surfaces and
registries, reject/drain callbacks and jobs, and release module leases. Verified
cache content is not removed while an activation or content lease can still call
or read it.

### Independently Owned Hybrid Roles

`ActivateModules` is the role-aware path through the same `PackageLifecycleService`,
install authority, generation journal and atomic publication point. Applications
explicitly select package, declared descriptor, module, declared role and required
or optional policy. Installed library/assets remain availability records until
selected; package presence never implies executable participation. Backend/library,
headless tooling, script-provider, editor-presentation and runtime/gameplay roles
use the existing closed `ExtensionModuleRole` contract. Hybrid packages use the
union of their actual declared module roles.

All exact inventory/descriptor identities, compatibility and digest-bound trust
are checked before an owning host receives a sealed `VerifiedPackageModule`.
The handoff retains its verified install graph, selected artifact, complete inert
descriptor, dependency identities and revocable capability handles. It grants no
raw filesystem directory, native handle, backend discovery or implicit trust.
Unknown package/module/role selections fail closed, including optional selections.

Applications inject one unambiguous owner for each admitted role.
`NativePackageModuleHost` uses the existing native extension manager and actual
importer registrations; presentation additionally requires an explicit activity or settings/modal page
owner supplied through `PackageLifecycleConfiguration::editorHosts`. The same
`ExtensionEditorHosts` carrier reaches `ExtensionManager`; omitted publication
owners remain unavailable and headless profiles never admit editor presentation. `ScriptPackageModuleHost` uses a native resource receipt
and the existing `ScriptInvocationRegistry`/`ScriptCapabilityContext` invocation
boundary. It does not create a VM, infer script trust, or replace the language
runtime adapters. Each script package/module has an exact application-injected
registry realm: registry generations are module-local, and the provider generation
must equal its capability activation generation. Duplicate realm identities,
shared registry aliases and absent/wrong module realms fail closed. Hosts retain
the realm through retirement; the application retains its normal owner-lane
context and completion responsibilities.

Platform provider packages use the same verified native handoff. Applications pass
`PackageLifecycleConfiguration::platformProviderCommit` explicitly to publish the
copied `platform.services.provider` claim into their Platform Services composition.
Both `Activate` and `NativePackageModuleHost` forward this callback to the existing
extension manager; an omitted callback rejects the provider before native load.
PackageLifecycle checks the provider's copied permission requests against the exact
package approval before invoking the application callback. A broader process policy
cannot grant permissions absent from that package approval. PackageLifecycle
acquires no Platform Services target dependency and never selects or initializes a
provider. The application validates its additional provider policy and owns the
publication registry, lifecycle host and owner-lane drain.

This is an additive configuration field owned by the existing
`HoroPackageLifecycle` public-header boundary. Existing applications keep their
current behavior; provider applications set the callback before constructing the
native role host. The callback's captured owners must outlive every package
retirement. Exact product selection remains application-owned while a different
contract version stages in the registry. A registry rejects a duplicate
capability/version; the application must disable and drain before reusing that
slot. Neither path permits silent fallback or premature unload.
Mixed provider/importer contributions remain rejected under the current atomic
provider-only ABI profile. A failed package attempt revokes its publication through
the normal extension rollback owner, including cancellation and stale-install
failures after native registration.

`GameplayPackageModuleHost` is a dedicated application adapter target linking the
PackageLifecycle and GameplayModuleHost contracts. PackageLifecycle does not depend
on Gameplay. Runtime participants use the actual generated gameplay SDK exports,
exact build fingerprint and descriptor revision, and the separately approved
`runtime.participate` permission/capability before factories. They never masquerade
as native extension ABI contributions. Same-module replacement fails before
mutation while the old generation remains active or retiring; the application explicitly
disables and drains it before loading another gameplay generation, following the
SDK safe-reload rule. Other required-role failures preserve the previous graph.
The host's borrowed SDK view pins the complete verified install and native owner.
Failed Register/Start attempts retain the actual native owner through rollback.

The same dedicated `HoroPackageGameplayHost` adapter also composes the existing
`GameplayLua` runtime for decision-node contributions. This direct dependency
belongs to the concrete host; `PackageLifecycle`, package manifests and the
generic script invocation host remain VM independent. `AcquireDecisionNodes`
returns the actual generated native contribution with its locked package
version, artifact digest and activation generation. `CompileDecisionNode` reads
only a declared, bounded `.horo_script` from that exact verified module inventory
and delegates metadata generation and callback execution to `LuaDecisionNodeProgram`.
It retains the actual native activation's node-generation lease as the Lua
provider parent. Revocation closes new Lua callbacks, and package retirement
cannot unload the native image or remove its verified staged resources until
the Lua programs, executors and cleanup have released those leases. A manifest
role or generic `ScriptInvocationProvider` never certifies a node factory.
After binding its world services, the Gameplay host rechecks the native member
against the verified archive and signature policy before loading the module.
Node descriptor versions are separate from package versions; the existing
locked resolver selects package versions before these APIs can acquire them.

The narrow non-installed `HoroPackageContentStagingInternal` target owns verified
private-root materialization shared by the native-extension and gameplay adapters.
It publishes only its dedicated internal header directory and never a repository
source root; it has no dependency on either concrete host.

Required role failure rolls back the unpublished candidate and preserves the
previous active graph. Optional failure records its original error with package,
module, role, generation and owning-host attribution while admitted siblings may
publish. Headless compositions omit presentation ownership explicitly; an optional
GUI role reports unavailable without withdrawing healthy backend/script roles.
The journal retains unpublished failure reports. Completed phases distinguish
installed, enabled, trusted, compatible, loaded and active evidence.

The resolver's optional selected-root argument checks compatibility for the exact
role dependency closure instead of unrelated optional siblings. The native
manager's optional selected-module argument loads only those explicit native
owners and their contributions. Its caller must retain all required dependency
receipts; the package coordinator provides that closure. Full-package callers
retain the original empty-selection behavior. Descriptor parsing remains inert,
and neither selection path rewrites archive descriptors or invents contribution
ownership.

### Native Callback Exception Containment

Host activation and lifecycle callbacks are an executable-module boundary. Foreign
code may throw arbitrary C++ objects, including values outside `std::exception`.
Each boundary converts that failure to a typed diagnostic while returning or
retaining the already allocated rollback owner. Narrowing those handlers to
standard exception types would allow a rejected partial registration to escape
without its explicit retirement handoff. The `std::exception` handlers preserve
available diagnostic text before the final non-standard exception fallback.
Line-local Sonar exceptions on those handlers document this required containment;
they do not weaken validation, capability admission or retirement prerequisites.
A throwing finalizer is treated as incomplete and its complete resource closure
stays queued for a later owner-lane drain. A failed final queue destruction cannot
release that closure safely and terminates, enforcing the existing explicit-close
precondition.

The retirement queue's mutating operations retain non-const signatures: reserving,
transferring, draining and releasing executable generations change the queue's
lifecycle even though the implementation state is held through a shared pointer.
Line-local const-suggestion exceptions preserve that public mutation contract.

### Explicit Retirement Owner And Migration

Existing native-only `Activate` callers retain their complete-package semantics.
Applications adopting `ActivateModules` must compose `moduleHosts` and an explicit
`PackageModuleRetirementQueue` in `PackageLifecycleConfiguration`. Public ownership
remains `HoroPackageLifecycle`; consumers must link that target. New native/editor
and script host declarations expose only Horo contracts. A selected-module native
loader caller must migrate to explicit required-owner receipt retention; an
isolated module selection is not permission to discard its providers.

The bounded application-owned retirement queue outlives lifecycle services,
contexts, callbacks and every retired generation. A slot is reserved before any
host mutation. Failed activation, replacement, disable and service shutdown move
the entire resource closure into that queue, including concrete host receipts,
required provider receipts and verified install/module resources. Admission closes
before withdrawal. Actual dependents finalize before providers even if the caller
selected consumers first. A host reporting `Finalize() == false` keeps the entire
closure pinned; framework teardown must not treat an activation error as proof of
drain. A host with rejected but still busy work returns an inactive receipt carrying
its original diagnostic rather than dropping that work behind an error-only return.

The application pumps `Drain()` on the creating lane, including after service
shutdown, then calls typed `Close()`. A busy result requires retaining the explicit
drain handle and completing pending work; it is not a successful shutdown. No
orphan registry, process-global discovery or silent resource leak is introduced.
Destroying the final queue owner with reserved/live work violates the lifetime
precondition and terminates instead of unloading executable work. This reuses the
fatal final-owner safety posture of
`src/extensions/registry/BackendServiceRegistry.cpp::QuarantineForRestart` when its
bounded safe retention boundary cannot be maintained. Ordinary busy shutdown is
recoverable through the retained queue and does not terminate. Hosts must not
recreate queues to bypass capacity or discard required shutdown evidence.

Updates replace the sealed install graph and activate a new role generation at a
quiescent boundary. Disable selects an empty or reduced module set. Uninstall
removes package availability only after the same withdrawal/drain policy and
project-reference checks; old resource receipts remain valid until drain. These
operations do not create a second installer or mutate project-owned assets.
The package application rejects an install-graph replacement that would remove a
package still owned by an active or retiring module. The diagnostic identifies
the retained packages. The host can retry after their owners drain.

## Enablement State

Enablement is not the same as trust.

| Contribution | Enablement storage |
|---|---|
| Runtime-required assets/scripts/behaviors/services | Portable project metadata when it affects project behavior |
| Optional runtime features | Portable project metadata or profile-specific project setting |
| Editor extension requested by project | Portable request only |
| Actual editor extension activation | User-local or organization policy-gated state |
| Trust decision | User-local or policy-local state only |

A project may request an editor extension, but activation still requires local or
organization policy approval. Trust decisions are never portable project state.

## Ownership States

| State | Meaning | Update behavior | Uninstall behavior |
|---|---|---|---|
| `MountedPackageAsset` | Read-only asset from verified package cache | Follows package update | Removed when unmounted if no refs |
| `ImportedProjectAsset` | Copied from package and now project-owned | Never overwritten silently | Remains unless user deletes |
| `PackageOverride` | Project override layered over mounted package asset | Preserved | Preserved or detached |
| `SampleImportedAsset` | Sample/demo copied to project | Never managed by package | Remains unless user deletes |
| `GeneratedAsset` | Import/cook/build output | Regenerated from source | Deleted as derived state |

Package updates must not silently overwrite project-owned files.

## Update Policy

Package update flow:

```text
Resolve new graph
  -> compare contributions
  -> compute migration plan
  -> dry-run diagnostics
  -> stage new packages
  -> verify
  -> apply atomically
  -> rollback on failure
```

Update validation checks:

- package ID and source authenticity
- compatibility with engine/editor/runtime
- contribution removals or renames
- migration descriptors
- mounted asset references
- behavior and service usage in scenes
- license changes
- trust level escalation

Trust escalation requires explicit approval.

The resolver's `PackageUpdatePreview` compares the complete current and
candidate lockfiles after validating each against its portable request. It
returns the exact old and new entries for added, removed, or changed packages,
including same-version artifact replacements, and distinguishes request-only
changes. The preview is inert. Artifact verification, reference checks, trust
approval, and migration belong to the transaction that applies the candidate.
The package application checks current project authority and candidate platform
compatibility before returning this preview to a host adapter.

The headless CLI also supports a read-only preview of an already resolved
candidate:

```sh
horo-engine package preview-update --project /path/to/project \
  --candidate-request /path/to/candidate/packages.json \
  --candidate-lock /path/to/candidate/packages.lock
```

The command returns the total number of changes, up to 32 ordered rows and
the next offset. Pass `--offset` to read the following page. A next offset of
`-1` means there are no more rows. The approval fingerprint identifies the
reviewed project and the exact current and candidate documents. A pending package metadata recovery blocks
the preview until a recovery-capable operation repairs the project.

Pass `--package-cache-root` to `preview-update` or `preview-rollback` to
review the cached archives as well as lock metadata. The preview verifies
changed archives and unchanged entries required for offline staging. It reports
missing archives, removed paths, changed bytes or owners, and packages that
require a migration or executable lifecycle owner. It emits an
approval fingerprint only when this review succeeds. JSON output includes the
bounded list of diagnosed package paths. This preview does not claim cache
locks, quarantine artifacts, or write package metadata. Apply verifies the
archives again before publication.
Missing current or candidate archives are reported with their package and role.
The review continues across independent entries up to the diagnostic limit.
During an offline package exchange, a rejected migration declaration still
allows the preview to report independent departing archive and saved-reference
findings.

Behavior rename previews identify each matching attachment with the SHA-256
of its saved Scene or Prefab bytes. Unreadable sources block the review and
appear alongside findings from readable sources within the diagnostic limit.
With a complete asset inventory, the preview also reports missing and
wrong-type child Scene and Prefab sources alongside the confirmed attachments.
It reports recursive child Scene links and cycles through nested and variant
Prefab links from those same saved sources.
Missing object identities and duplicate object or attachment identities make
a Scene's edit addresses unusable. The preview reports the address and keeps matches
from other readable sources.

`horo-engine package apply-update` takes the same project and candidate options,
plus `--approve` with the preview fingerprint and `--package-cache-root` with the
absolute path to an existing verified cache. This path stages offline and
publishes updates that retain existing package references. Newly added packages
must declare asset-only contributions and pass the same archive path and
execution checks as replaced asset packages. An asset-only
replacement can change versions or add files when the package manifest and
lock policy stay identical and verified archives prove that every existing
path keeps its bytes and contribution owner. An offline update can exchange
asset-only packages in one transaction. With a verified cache, preview
checks both archives and reports saved importer references to departing
packages, including when another archive has invalid migration metadata.
Publication repeats the reference checks under the project
mutation lease. The same checks apply to the inverse exchange on rollback.

The update host publishes a previously reviewed candidate only when the exact
base and candidate documents still match the review and the candidate lock
matches a verified staging record for the same project. It uses the project
mutation lease and the package metadata journal. A failed pre-commit write
attempts recovery while holding the lease. If the lock replacement committed
and durability remains uncertain, the journal preserves enough evidence to
confirm the new pair on the next preparation.

The metadata update retains one rollback point containing the exact previous
request and lock. It records the new pair's digests before committing. A retry
recognizes the point only if the current request and lock still match those
digests. After writing the pending rollback point, the host rechecks the
approved base pair before admitting the journal. An external edit detected
there remains in place and the uncommitted rollback point is discarded.
After writing the journal, the host checks the pair again before replacing
either document. A changed pair aborts publication and leaves the external edit
in place. After replacing the request, the host checks that the published
request and prior lock still match before replacing the lock. An independent
edit at that point leaves the journal available for recovery.
Failed updates retain a previous completed rollback point. The point
records package metadata and does not include project document migrations.

`horo-engine package preview-rollback --project /path/to/project` reads the
retained pair automatically. Update and rollback previews recheck the exact
request and lock bytes. A replaced lock invalidates the snapshot even if the
request stays the same. A pending publication also blocks rollback after the
checkpoint has been read. Pass the preview fingerprint as `--approve` to
`package rollback-update`, together with the project and
`--package-cache-root`. Rollback stages the older lock before publication and
applies the same reference-safety boundary as forward updates. A successful
rollback retains the pair it replaced as the next rollback point.
When rolling back an asset-only addition, the host also verifies the departing
archives and checks the saved project references under the package mutation
lease before restoring the exact older documents.

## Uninstall Policy

Before uninstall:

- detect mounted asset references in scenes/assets
- detect behavior type usage in scenes
- detect service dependencies
- close or detach editor panels contributed by the package
- check package dependency graph reverse edges

If required references remain, policy may:

- block uninstall
- convert mounted assets to project-owned copies
- leave explicit missing-reference diagnostics

Silent dangling references are forbidden.

A removal preview can prepare a matching portable request and lockfile without
resolving new versions or changing either project document. The lock drops only
entries unreachable from the remaining roots. Withdrawing the last root produces
a schema-v1 lock with empty `roots` and `packages` arrays. The preview preserves
the artifact and source identities of every retained entry. Reference checks,
approval, and durable publication belong to the uninstall transaction. A
successful removal retains the exact previous request and lock in the same
rollback checkpoint as a package update. The package cache is left intact so
the existing rollback command can stage the previously locked artifacts. A
failed removal attempts to restore the prior pair before releasing the
mutation lease. If repair cannot finish, the journal remains for the next
project preparation.

The editor's application-level preview reports which departing packages still
have active or retiring native modules. It samples the same composition owners
that guard installation, including the retirement queue. It reports native
ownership at preview time. The uninstall transaction still owns the reference
checks and mutation. Application and editor consumers can call `InspectRemoval`
for both graph effects and blockers. `PreviewRemoval` continues to provide the
graph effects alone.

Before a live removal can publish, the lifecycle owner issues a reservation
for the same project and exact departing set. Packages with active or retiring
module owners cannot be reserved. Each departing ID must appear once and match
its verified installed version. Requests during activation preparation are
rejected before the staged modules can publish. The reservation retains the install record
and prevents activation while the transaction inspects references and
publishes metadata. Dropping an uncommitted reservation reopens admission.
Once the lock commits, admission on the old session remains revoked until
the host composes from the new lock.
Lifecycle shutdown revokes the reservation and cancels reference review or
publication before the transaction writes its journal. After the journal is
written, publication uses the same recovery rules as other metadata changes.

When the same lifecycle also owns a native backend through the concrete
importer host, a withdrawal reservation can admit a sealed importer-only
backend alongside gameplay roles. The backend may declare only importer
contributions, with no service imports, exports, local module dependencies,
or additional capability requirements. This path excludes mounted assets. The project still checks
importer sidecars and observes gameplay registrations, and the reservation
waits for live and retiring role receipts to drain. Otherwise, the review
identifies the blocked contribution, module, or archive path and keeps any
confirmed references found in saved sources. Incomplete descriptor or archive
review also preserves independently verified gameplay roles for registration
diagnostics while the unsupported archive remains blocked.
Retained registration history from a replaced artifact is excluded from saved
Scene, Prefab, and Decision attribution. An unobserved role in the current
locked artifact still blocks removal.

The desktop editor can apply a reviewed removal for verified asset packages
and packages owned entirely by the native gameplay runtime. The workspace requires saved documents and stopped Play before
confirmation. The operation binds to the displayed package plan, inspects
saved references on its I/O job, and reopens the package session from freshly
prepared metadata after success. The executable supplies explicit root-bound
read and asset registry factories. Hosts without those factories expose the
preview alone. The editor verifies the complete executable manifest, requires
exact observations of every departing runtime role after retirement, and
checks saved behavior, Decision and service references. Mixed roles, unknown
archive paths and unreviewed component, asset, query or persistence
contributions block removal. The host reads native module identities from the
descriptors in the sealed archive inventory and checks them against the lock.
Each descriptor accounts for artifacts under its declared contribution root.
A locked contribution without a covered descriptor or asset owner blocks
withdrawal. Custom senses block removal when saved authoring sources still
require a binding review. The host decodes each Scene and Prefab once for
behavior, native component and custom-sense review. Exact native component
matches include saved property and topology overrides, including
conflicts and orphans. Child scenes, prefab placements and nested or variant
prefabs must resolve to sources of the expected type in the same reviewed asset
inventory. A missing or mismatched target blocks removal and reports its asset
ID and the source that refers to it. For departing component providers, the review checks
declared reads and writes in observed retained native behavior and system
registrations. A matching access identifies the consumer package, descriptor and
registration. Standalone Scenes without AI or
generic gameplay bindings and standalone Prefabs containing only plain objects
can pass this review. Authored AI, opaque components and unresolved composition
still require review, including disabled attachments. An EQS source is independent of a
departing sense when its complete source is admitted against the engine-owned
point-query schema. Perception queries and queries with unresolved stages still
require review. Unreadable queries are reported with their project-relative
paths and block the operation. An unreadable Scene, Prefab or Decision source
also blocks removal. The review reports its path alongside references from the
other readable documents. When an otherwise decoded Scene contains an unknown
component owner, the review keeps confirmed typed references from that Scene
alongside the unsupported-source blocker. A degraded asset inventory also blocks removal.
Review still reports confirmed references from individually admitted sources
and defers checks that require a complete inventory, such as resolving
composition links. Importer, Scene, Prefab and Decision checks share one
admitted inventory within the removal transaction. The removal dialog shows the source and target of
unresolved Scene, Prefab and Decision links, along with identified modules,
contributions and archive paths that require a lifecycle owner. An unverified
extension descriptor is reported at its archive path. Independent descriptors
and unowned archive members are still reviewed in the same attempt.
The application package project host borrows the
inert EQS descriptor schema and authoring validator from AI Scene integration.
The review does not compose a live Scene or execute queries.

`horo-engine package preview-remove --project /path/to/project --package-id
com.vendor.pack` checks saved asset importer references and reports any locked
contributions that require a live lifecycle owner. When those checks pass, it
lists the departing packages and returns an approval fingerprint. Add
`--package-cache-root` to verify the exact departing archives during preview
and identify any archive members that require a live owner. This check reads
the cache without restoring packages or activating modules. Preview and apply
identify the owning package for each blocking archive member, including when
one root withdrawal removes several packages. Pass the fingerprint as `--approve` to `package
remove` together with the same project, package ID, and
`--package-cache-root`. The command verifies the current locked archives from
the local cache, matches the departing archive entries to the exact lock, and
rechecks the project inventory under the mutation lease.
Offline withdrawal supports asset-only archives. Extensions and other
executable contributions require their live lifecycle owner.

Observed native package registrations include service dependencies and system
service requirements. Within one project session, reactivating the same
verified archive and role retains previously observed service IDs and required
service edges. A failed or over-limit capture prevents removal. The uninstall
transaction checks retained observations against the exact current lock and
reports which retained registration requires a service owned by a departing
package. When a departing package provides a
gameplay service or component, every retained runtime role needs a registration
observation from its exact locked archive, including roles declared by packages
with additional extension contributions. The gameplay-only withdrawal rule
applies to departing packages. The review reports unobserved roles and roles
with unsupported reference classes together, with the package, module ID and
descriptor path for each. The saved request and lock stay intact until coverage
and reference checks pass.

Removal checks also use Decision node IDs observed from the exact native
gameplay registry. Saved Behavior Tree, State Machine, and Utility sources
are decoded with the bounded Decision codec. References report the source
asset and node or action phase. When a departing package owns Decision nodes,
an unreadable graph blocks removal. Authored subplan calls also require an
unambiguous semantic graph identity with a compatible family and schema version
from the same scan. The review reports call resolution only after a complete
scan. When a source is unreadable, the review reports its path and any
independently confirmed provider references. After a complete scan, the caller
path, authored node and target ID identify each call that requires review.
A node containing both a typed call and a legacy subtree reference is reported
as incompatible while the review continues collecting independent references.

## Rollback Contract

Package lifecycle operations write a transaction journal before mutating project
metadata, cache indexes, enablement state, activation state, or document files.
Rollback restores metadata and activation state when failure occurs before
irreversible project document migrations commit.

Uninstall and withdrawal rollback recheck saved references after staging the
rollback point and before writing the metadata journal. The second review uses
the same mutation lease, cancellation and deadline. A late reference blocks the
operation and preserves the previous package metadata.
An update or rollback that drops a package requires the withdrawal publication
path, even when the staged candidate and approval are valid.

A withdrawal rollback may also restore a retained asset package at the same
version and with the same locked source, dependencies and contribution policy.
Preview and publication review the exact older and current archives. The
restored archive must preserve every currently mounted asset path, owner and
payload. Rollback still checks the saved project references to departing
packages before publishing the older request and lock together.

Migrations that modify project-owned scenes or assets are previewed first and
then committed through editor document transactions or explicit file migration
transactions. After a migration commits, rollback requires restoring from the
transaction backup or a user-approved VCS/workspace recovery path.

## Migration

Package updates may require migration:

```toml
[migrations."1.x_to_2.0"]
renamedAssets = [
  { from = "assets/rifle.mesh", to = "assets/weapons/rifle.mesh" }
]
renamedBehaviors = [
  { from = "com.vendor.OldWeapon", to = "com.vendor.Weapon" }
]
fieldMigrations = ["migrations/weapon_fields_v2.json"]
requiresUserReview = true
```

Migration plans must be previewable. Any migration that changes project-owned
scene or asset files uses editor document commands/transactions where applicable.

The cache-backed update preview reads the optional `migrations/asset-renames.json`
file from the candidate's verified inventory. A version-one declaration binds
the candidate package ID and version and lists exact prior artifact selectors.
Compatible asset replacements may carry or retire this archive metadata when
no selector matches the installed artifact and all existing asset paths retain
their bytes and owners. Preview and apply both validate a candidate declaration.
An asset-only addition has no installed predecessor. The same declaration
validation still checks its destination identity, selectors and path rules.

```json
{
  "schemaVersion": 1,
  "packageId": "com.vendor.weapon-pack",
  "toVersion": "2.0.0",
  "transitions": [
    {
      "fromVersion": "1.0.0",
      "fromArtifactSha256": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "renames": [{"from": "assets/rifle.mesh", "to": "assets/weapons/rifle.mesh"}]
    }
  ]
}
```

The preview checks each applicable mapping against both verified inventories and
reports whether the destination also changes bytes. Path collisions, links,
executables, duplicate selectors and missing paths invalidate the applicable
review. This declaration supplies evidence for review. Applying the mapping to
project files and producing an approval for that migration require a document
transaction.

The optional inert `migrations/behavior-renames.json` declaration uses the same
destination and exact source selectors, with stable Gameplay type IDs in
`from` and `to`. Cache-backed preview matches applicable source IDs against
saved Scene and Prefab attachments, including disabled attachments, and reports
the relative path, object and attachment identity. A degraded asset inventory
reports the failing metadata path and retains matches from admitted records.
Matches show possible impact. Invalid migration declarations block approval,
with the declaration and its error reported alongside independently verified
asset and behavior findings.
If multiple packages claim the same saved type, or one package supplies
different destinations for that type, the preview reports ambiguity without
choosing a rename. A preview whose output budget omits any confirmed impact
reports that the review is incomplete.
They require live provider ownership and field compatibility verification before
the host can approve a migration. The preview does not activate package code or
edit project sources.

Legacy extension directories and `.horo/plugins.json` use the bounded ADR-054
migration. It parses legacy metadata only to build a preview, generates a
canonical `horo-package.toml`, `files.manifest.json` and package-scoped extension
descriptor in staging, revalidates the result as one package, requests trust for
the generated digest and atomically converts project requests to
`.horo/packages.json`. The old directory/request state remains recoverable until
commit and is never active at the same time as the new package authority.

## Conflict Policy

Conflicts are contribution-specific:

- package ID conflict
- asset ID conflict
- behavior type ID conflict
- service ID conflict
- schedule node ID conflict
- script API ID, namespace, or compatible service-export conflict
- editor command/menu ID conflict
- input action ID conflict
- shader keyword conflict
- component type ID conflict
- system ID conflict
- asset importer ID conflict
- CLI command path conflict
- MCP tool name conflict
- settings key conflict
- event type name conflict
- file template ID conflict
- project template ID conflict

Possible resolutions:

- fail
- namespace
- override
- skip
- import copy
- explicit user rename

Runtime identifiers should not be automatically renamed because serialized scene
references can break.

Script API conflicts resolve from stable typed identities, compatible version
constraints and the verified package graph under
[ADR-059](../../adr/059-script-consumable-module-boundary.md). Package load order,
runtime globals and automatic namespace renaming are not conflict-resolution
mechanisms.

## Package UI Contract

The package modal is a presentation adapter over `PackageService`. It shows a
precomputed plan and does not own business rules.

The modal must show:

- package source
- exact version
- hash/signature status
- package kind
- contribution kinds
- required trust
- dependencies
- conflicts
- files to import/copy
- mounted vs copied content
- script/native/editor code warnings
- license/notice requirements
- disk size
- restore portability status

## Required Tests

- path traversal and symlink escape in package archive
- oversized archive bomb rejection
- invalid signature rejection
- native contribution without trust rejection
- non-interactive trust failure
- mounted asset update preserves references
- imported copy update is not overwritten
- uninstall with scene references blocks or diagnoses
- package override resolution
- migration preview and rollback
- transaction journal restores metadata and activation state on failed update
- runtime enablement is portable while trust remains user-local
- editor contribution deactivation closes panels safely
- exact install-record/descriptor binding and stale activation generation
- no raw-directory ExtensionHost activation
- legacy extension migration preview, trust review, rollback and resume

## Related Documents

- [Horo Package System](./package-system.md)
- [Package Restore](./package-restore.md)
- [Editor Modal Host](../editor/editor-modal-host.md)
- [Editor Document Model](../editor/editor-document-model.md)
- [Extension System](../extensions/plugin-system.md)
- [Application Security](../security/application-security.md)

## PKG-001.9 Native Activation Boundary and Migration

`HoroEngine::PackageLifecycle` is the application-owned composition bridge. It
links PackageSecurity and Extensions explicitly; neither availability verification
nor the native host gains a reverse dependency. Its two public headers are solely
owned by that target and compiled by its generated standalone header consumer.

Hosts construct `PackageLifecycleService` with the existing install authority,
a separate local/organization `IPackageExtensionTrust`, mandatory native artifact
gate, approved temporary parent, host capabilities and loader. Construction is
inert. The application invokes `Activate` only at startup or after quiescing
runtime consumers, and serializes installation with activation on that owner lane.
The executable regression composition exercises this same public route with real
native ABI fixtures and importer registration, rather than synthetic active state.
No install call activates code.

The present native/importer archive/lock bridge accepts one enabled descriptor per package, with
multiple dependency-ordered modules within that descriptor. The explicit file
inventory contribution root is `extensions/<group>`; `<group>` must also be
selected in the installed lock record. The descriptor is the declared regular
file `extensions/<group>/extension.json`. Its legacy package ID/version assertions
must match the sealed install, and every selected native entry must belong to the
same verified root. Missing artifacts, absent groups, incompatible modules,
duplicate packages and cyclic selected dependencies fail before code loads. This composition
rejects contributions for other extension points because it owns no registry for them.
Package-manifest semantic expansion, independent descriptors per package and
hybrid presentation composition remain subsequent integration work.

The immutable internal candidate binds a sealed install lease, exact archive,
manifest and file digests, descriptor bytes, host-selected entries, external exact
trust approval, capability policy and non-wrapping activation generation. Only
verified declared files are materialized into an exclusively created private
host directory. The existing ExtensionManager reads that exact descriptor and
loads the selected native artifacts through a digest-bound signature gate. It
registers actual contributions into a detached catalog. The lifecycle service
owns real capability admissions and exposes their `ExtensionActivationLease`
proofs only alongside the completely registered sealed catalog.

A transient process-local attempt journal is published before native preparation.
Runtime registration is not portable durable state: failure records the original
typed diagnostic and leaves the last published runtime graph usable. Cancellation,
load/ABI failure, registration conflict, reentrant shutdown and changed install identity retire the
detached host without publishing it. All fallible preparation completes before
one allocation-free publication of graph, generation and catalog. Cancellation
observed after that commit does not reverse success. Durable availability uses
the existing pending-record transaction; no trust grant is written to project
metadata.

Retirement closes real admission leases, withdraws contributions and drains the
existing ExtensionRetirement owners in reverse dependency order. Retained old
catalog snapshots reject new native calls and keep code/content alive until
owner-lane finalization. Bounded retired compositions prevent unbounded repeated
activation. Busy shutdown owners retain private verified files for restart; the
service never deletes content still usable by admitted native work.

`PackageInstallService::ActiveGraph` now returns a frozen owned graph rather than
preserving caller pointer identity. Existing availability consumers need no
signature changes, but must compare content or the new `InstalledRecord` identity
rather than the input restore pointer. This deliberate change prevents a mutable
restore alias from rewriting installation evidence after durable publication.
Old sealed records retain their verified archive snapshots through later installs.

The raw-directory ExtensionManager API remains a legacy authoring/discovery adapter.
Production package activation uses the explicit PackageLifecycle route, which has
no caller-supplied extension directory. It does not infer execution trust from a
legacy directory, publisher verification, successful install or portable enablement.

Rejected native activation retains its actual `ExtensionRetirement` before ABI
callbacks begin. `ExtensionManager::FailedActivationRetirements()` is an additive
owner-lane view for composition owners; existing successful-load callers retain
their contracts. Clean rollback owners are removed during finalization, while
failed teardown owners count against the manager retention limit and require
restart. The package bridge includes those owners when deciding whether private
verified content may be deleted. A failed load never constitutes drain evidence.

### Application Composition And Command Authority

`PackageApplication`, `PackageProjectHost` and `PackageProjectSession` retain their
validating `Create` factories. Their allocation constructors accept private,
non-aggregate construction keys so standard RAII allocation can adopt validated
implementation state without admitting arbitrary client construction. Existing
`Create` callers do not migrate. The non-installed project-session composition
passes the existing typed borrowed host-service bundle instead of separate
scheduler, filesystem, clock and trust arguments; all services remain alive until
host shutdown completes.

The editor modal session exposes `Submit` and `Activate` as mutable command
facets. A const modal view may inspect state but cannot initiate package mutations.
Their shared application owner does not make these operations logically const:
restore, install and activation change the project authorities behind that lease.
Local const-method suggestions at those two definitions are intentional authority
exceptions; private adapter dispatch helpers may be const without widening a
public command facet.

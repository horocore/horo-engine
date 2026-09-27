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

## Rollback Contract

Package lifecycle operations write a transaction journal before mutating project
metadata, cache indexes, enablement state, activation state, or document files.
Rollback restores metadata and activation state when failure occurs before
irreversible project document migrations commit.

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

# Horo Package System

## Purpose

This document defines the core package model for Horo Engine: package kinds,
contribution kinds, source declarations, manifests, dependency resolution,
lockfiles, cache verification, trust levels, identity, and service boundaries.

Detailed restore, lifecycle, and release behavior is defined by:

- [Package Restore](./package-restore.md)
- [Package Lifecycle](./package-lifecycle.md)
- [Package Release Integration](./package-release-integration.md)

## Core Decision

A Horo project must be restorable, verifiable, portable, releasable, and secure
from committed project metadata plus declared package sources.

[ADR-054](../../adr/054-extension-and-package-authority-boundary.md) makes this
package model authoritative for extension-only and hybrid packages as well as
asset, game-library and template packages. ExtensionHost consumes verified
activation candidates; it is not a second resolver, installer or trust store.

[ADR-057](../../adr/057-package-manifest-v1-typed-model.md) defines the only
public package-manifest value model. TOML decoding, semantic validation, bundle
verification and install/source policy are separate stages; only an immutable
validated model bound to its exact file manifest, archive and signature evidence
may enter resolution, lifecycle, activation or release planning.

The package system is not a replacement for the asset importer or the extension
system:

- Asset importers convert individual source assets into project assets.
- Packages distribute resolved sets of assets, scripts, behaviors, services,
  samples, templates, and optional editor contributions.
- Editor/IDE addons are high-trust Extension System contributions carried by the
  same package manifest, verification, lock and lifecycle model.

## Core Distinctions

| Concept | What it carries | Where it lives | Trust level |
|---|---|---|---|
| **Asset Importer** | One media file (`.fbx`, `.wav`, `.png`) | Project asset store | User file |
| **Asset Package** | Many media assets | Project asset store or package mount | User package |
| **Game Library** | Scripts, behaviors, services | Project dependency | Explicit trust |
| **Engine Extension / IDE Addon** | Editor/engine capabilities | Product host | Host-level trust |
| **Hybrid Package** | Assets + game code + optional editor tools | Project + host if trusted | Per contribution |
| **Template Package** | Project template or sample content | Project seed | User package |

An asset package must not silently carry editor host code. Editor tools are
contributed only through the existing [Extension System](../extensions/plugin-system.md)
and require separate trust approval.

## Package Kind And Contribution Kind

`HoroPackageKind` describes package intent:

```cpp
enum class HoroPackageKind {
    Data,
    Tool,
    Extension,
    GameplayLibrary,
    Hybrid,
    Template,
};
```

Package contribution payloads are a closed typed variant rather than an enum plus
an arbitrary property map:

```cpp
using PackageContributionPayload = std::variant<
    PackageDataContribution,
    PackageToolContribution,
    PackageExtensionContribution,
    PackageGameplayContribution,
    PackageScriptContribution,
    PackageServiceContribution,
    PackageSampleContribution,
    PackageTemplateContribution,
    PackageDocumentationContribution>;
```

Contribution payloads are validated independently. A `Hybrid` package may
declare runtime and editor contributions, but editor contributions are not
activated unless the extension trust flow succeeds.

An `Extension` package contains one or more extension descriptors and may include
only their declared binaries, scripts, resources, schemas, licenses and
documentation. A package that also contributes project assets, gameplay code or
services is `Hybrid`; both shapes use the same package authority. `Data` cannot
contain executable/script/module artifacts, and `Tool` execution is always an
explicit host-gated operation rather than an install/restore hook. Complete shape
invariants and legacy-kind migration are defined by ADR-057.

## Package Operations

| Operation | Meaning | Typical use |
|---|---|---|
| `ImportCopy` | Copy package content into the project as project-owned files | Asset packages, samples |
| `InstallDependency` | Add a package as a resolved project dependency | Game libraries |
| `MountReadOnly` | Reference verified package content read-only from cache | Large asset/library packages |
| `ImportSamples` | Copy samples into the project on demand | Any package with samples |

Install, trust, enable, and activation are separate lifecycle steps. See
[Package Lifecycle](./package-lifecycle.md).

## Manifest Format

The signed/distributed `horo-package.toml` is the canonical serialized projection
of `ValidatedPackageManifestV1`; it is not exposed as a TOML DOM or string map to
consumers. Every ID, semantic version/range, kind, scope, platform/architecture,
ABI, capability, module, artifact, contribution, content set, license and
signature requirement decodes into its owning bounded type. Source selection,
credentials, trust decisions, cache/install state and lock resolution are not
package-manifest fields.

The following is an authoring-format example, not canonical distribution bytes.
Authoring tools may preserve its comments and contribution-root shorthand, then
must expand it into the complete typed reference graph and emit deterministic,
comment-free canonical TOML before signing or publication.

```toml
schemaVersion = 1

[package]
id = "com.example.horo.gun-pack"
version = "1.2.3"
kind = "hybrid"
displayName = "Horo Gun Pack"
description = "Weapons, sounds, animations, and recoil behavior."
author = "Example Studio"

[compatibility]
engineMin = "0.9.0"
engineMax = "0.x"
sdkAbi = "horo-sdk-0.9"
scriptRuntime = "lua-1"
assetArchiveFormat = "1"
platforms = ["windows-x64", "linux-x64", "macos-arm64"]

[[dependency]]
id = "input-runtime"
package = "com.horo.input"
versions = "^1.0.0"
requirement = "required"
scopes = ["runtime"]
requiredFeatures = []
requiredContributions = []

[[dependency]]
id = "editor-widgets"
package = "com.horo.editor.widgets"
versions = "^2.0.0"
requirement = "optional"
scopes = ["editor"]
requiredFeatures = []
requiredContributions = []

# Shorthand contribution roots. The verifier expands these into typed
# contribution descriptors before install/activation.
[contributions]
assets = ["assets/"]
scripts = ["scripts/"]
behaviors = ["behaviors/"]
services = ["services/"]
samples = ["samples/"]
extensions = ["extensions/"]
documentation = ["docs/"]

[[contribution]]
kind = "RuntimeServices"
id = "com.example.horo.gun-pack.recoil-service"
root = "services/"
required = true
capabilities = ["runtime.assets", "runtime.input"]

[[contribution]]
kind = "EditorExtension"
id = "com.example.horo.gun-pack.editor-tools"
root = "extensions/com.example.horo.gun-pack.editor-tools/"
descriptor = "extensions/com.example.horo.gun-pack.editor-tools/extension.json"
required = false
capabilities = ["editor.panel", "asset.inspect"]

[license]
spdx = "MIT"
noticeFile = "NOTICE.md"
requiresAttribution = true
commercialUse = true
redistribution = "allowed"

[security]
declaredRisk = "game_code"
requiresSignature = true
```

## Package Archive Layout

Canonical `.horopkg` archives use a declared layout:

```text
package.horopkg
  horo-package.toml
  files.manifest.json
  assets/
  scripts/
  behaviors/
  services/
  extensions/
  samples/
  docs/
  NOTICE.md
```

`files.manifest.json` is the canonical file list and includes per-entry hashes,
normalized paths, size, executable bit, and contribution root. In strict mode,
every file in the archive must be declared. The verifier rejects undeclared
executable files, duplicate normalized paths, unsafe links, path traversal, and
files outside declared contribution roots.

The file inventory has its own immutable typed model. `VerifiedPackageBundle`
binds package-manifest, file-manifest and archive digests plus verified detached
signature evidence. Every module/artifact/content/contribution reference resolves
to exact `PackageFileId` values before publication; author-declared publisher keys
never become trust roots by themselves.

### Portable file identity implementation

`HoroEngine::Packages` owns `Horo/Packages/PackagePath.h`; its only public
dependency is Foundation. Unicode normalization stays private through utf8proc
2.11.0, pinned by commit in `cmake/Dependencies.cmake`. Public-header consumer
compilation verifies that neither Unicode nor archive implementation headers
escape that boundary. No existing extension-loader API is changed by this target.

`PackagePath::Parse` accepts relative NFC UTF-8 file identities with `/` separators,
at most 1024 bytes, 24 components and 255 bytes per component. It rejects empty
components, traversal, absolute/drive/UNC paths, stream syntax, control/format
characters, Windows reserved names and trailing dots/spaces. It rejects rather
than silently normalizes noncanonical archive spelling. A separate Unicode
NFKC/case-folded key detects portable case and normalization aliases; it is never
used as an extraction path. This intentionally conservative collision policy
also applies to directory prefixes when validating the complete file inventory.

Path validation is inert metadata validation, not evidence that an archive,
manifest, signature, contribution graph or installation has been verified.

### File inventory implementation

`ValidatedPackageFileManifestV1::Parse` accepts an exact JSON object with
`schemaVersion: 1` and a `files` array. Each entry has exactly `path`, `size`
(unsigned integer), `sha256` (canonical `sha256:` digest), `executable` (boolean)
and `contributionRoot` (canonical directory path without a trailing slash, or
explicit `null` for package-level metadata). A non-null root must contain the
file. Unknown fields, duplicate JSON keys, excessive nesting, duplicate paths,
file/directory conflicts and differently spelled directory aliases reject the
entire inventory. The immutable model owns all strings and records the digest of
the exact input bytes.

Default inventory policy bounds JSON to 4 MiB, a file to 64 MiB, aggregate
declared bytes to 256 MiB and entry count to 4096. Addition is checked against the
remaining byte budget before mutation. Inventories cannot list themselves:
`files.manifest.json` is bound by its separate digest, avoiding circular hashes.

The inventory is inert metadata, not verified file content. TOML semantics, trust,
contribution/artifact references and installation remain separate responsibilities.

### Archive verification implementation

`ValidatedPackageArchive::Verify` copies the archive and performs a complete
metadata preflight before decompressing its file inventory. Defaults bound
compressed input to 64 MiB, the inventory to 4 MiB, a file to 64 MiB, expanded
content to 256 MiB and entry count to 4096. Aggregate arithmetic is checked before
addition; each decompression output buffer is sized from preflight-bounded data.
CRC and SHA-256 are checked against actual content, not just ZIP header claims.
Callers may provide stricter or larger explicit host-owned limits.

Every regular file, including `horo-package.toml`, must match exactly one inventory
entry by canonical spelling, size, executable mode and digest. The sole inventory
exception is `files.manifest.json` itself: self-hashing is not possible, so its
digest is bound separately alongside the complete archive digest. Inventories
cannot list themselves. Explicit directories do not carry content or installation
authority; their paths still participate in collision validation. Installers must
create directories from validated file paths rather than replay archive metadata.

The private miniz adapter accepts regular files/directories, rejects special files,
links, privilege bits, reparse metadata and encryption, and compares local-header
names with complete central-directory names (including empty entries and names
longer than miniz's diagnostic buffer). Only ZIP64, timestamp and UID/GID extra
fields are admitted; link and alternate-path extra fields reject the archive.
Timestamp/UID/GID metadata does not grant permission to restore host ownership.

Verification does not write/extract to the filesystem, install, grant trust or
load code. Failure destroys only local temporary memory, leaving any prior install
untouched. Success owns an immutable byte snapshot so changing the caller's input
cannot change the verified archive. This result is **not** `VerifiedPackageBundle`:
TOML semantics, contribution/artifact references, source compatibility, signature
evidence and installation remain separate stages. The transitional extension ZIP
marketplace is not silently reinterpreted as a `.horopkg` package consumer.

## Contribution Descriptors

Folder-based contribution declarations are authoring shorthand only. Before
canonical publication, authoring tools expand them into typed module, artifact,
content-set, file and contribution descriptors that declare:

- contribution kind
- stable contribution ID
- root path
- whether the contribution is required or optional
- runtime/editor scope
- entry point or descriptor file
- script runtime when applicable
- behavior, component, system, service, command, tool, or template IDs
- supported platform and architecture for native binaries
- required capabilities

Activation, release inclusion, conflict checks and dependency resolution use the
closed typed payloads and reference graph, not raw folder names. Shorthand is not
present in a validated distribution manifest.

### Extension Descriptor Boundary

An extension contribution points to one package-relative
`extensions/<module-id>/extension.json`. The descriptor owns only module ABI,
entry variants, contributions and requested permissions. It may defensively bind
the owner package ID/version, but the binding must equal the parsed package
manifest and verified install record.

Package identity, version, source, cross-package dependencies, update channel,
trust and enablement remain outside `extension.json`. Every descriptor, binary
and resource is present in `files.manifest.json`; ExtensionHost receives an exact
validated descriptor and install-record lease rather than scanning a directory.
The complete authority and activation hand-off are defined by
[ADR-054](../../adr/054-extension-and-package-authority-boundary.md). The
descriptor is decoded and cross-reference validated against the leased install
record into the immutable typed model defined by
[ADR-055](../../adr/055-extension-manifest-v1-typed-model.md). A decoded value,
unknown required field, unresolved typed identity or mismatched role cannot enter
package composition, trust planning or ExtensionHost.

Audio packages use this same verified descriptor/install-record/trust handoff.
[ADR-069](../../adr/069-audio-extension-capability-and-abi.md) adds Audio-family
descriptors and a stricter RT ABI only after generic activation; it does not create
a second package, resolver, install, enablement, or trust authority. Approval for a
generic module role does not implicitly approve in-process callback execution.

## Package Sources

The package system is source-agnostic, but source identity and precedence follow
[ADR-058](../../adr/058-package-source-policy.md). A package may come from:

- local `.horopkg` file
- local package directory
- direct URL
- Git repository pinned to an immutable revision
- static package index
- optional registry

Only portable sources may be required for project restore. Local path overrides
are user-local and must not be required on another machine.

Portable `packages.json` uses named credential-free sources and explicit
dependency assignments. Map/file enumeration does not define priority:

```json
{
  "sources": {
    "horo.public": {
      "kind": "public-registry",
      "registry": "official",
      "priority": 100
    },
    "vendor.index": {
      "kind": "static-index",
      "url": "https://cdn.vendor.com/horo/index.json",
      "indexSha256": "...",
      "priority": 20
    },
    "project.vendored": {
      "kind": "vendored",
      "root": "vendor/packages/",
      "priority": 10
    }
  },
  "dependencies": {
    "com.vendor.weapon-pack": {
      "source": "vendor.index",
      "version": "2.0.0"
    },
    "com.team.vendored-pack": {
      "source": "project.vendored",
      "version": "1.0.0",
      "artifact": "com.team.vendored-pack-1.0.0.horopkg",
      "sha256": "..."
    },
    "com.horo.audio": {
      "source": "horo.public",
      "version": "^1.0.0"
    }
  }
}
```

Fresh resolution with an explicit dependency source queries only that authority.
An unassigned request snapshots every eligible source in its ordered search set
before selection and compares the chosen package ID/exact version identity across
that snapshot. A lower-priority digest mismatch is a hard conflict; unavailable
required metadata makes the snapshot incomplete rather than silently converting
resolution into first-match behavior.

Mutable Git branches, unpinned URLs, and local absolute paths are not valid
portable restore sources.

### Source Exactness

Direct file and direct URL sources must resolve to one exact artifact and must
include an expected content hash. Version ranges require an index, registry, or
other metadata source that can enumerate versions.

Portable file sources must be project-relative and inside approved project roots,
for example `vendor/packages/*.horopkg`. Absolute local paths are allowed only as
user-local development overrides.

Git sources are advanced package sources. Restore may fetch and verify a pinned
revision, but it must not execute package build scripts or generated code during
restore. If a Git source must be converted into a `.horopkg`, that packaging step
is an explicit trusted authoring/build operation, not an implicit restore side
effect.

MVP portable sources are:

- project-relative vendored `.horopkg`
- direct URL `.horopkg` with `sha256`
- static index entry resolving to `.horopkg`

Git sources, full registries, and marketplace flows are later extensions.

### Package Source Policy

Package source policy separates logical source identity, transport endpoint and
artifact digest. Locked restore uses exactly the `ResolvedPackageSource` recorded
in `.horo/packages.lock`; mirrors/cache/vendored copies may supply its exact bytes
but cannot change authority, version, publisher or digest.

Fresh resolution uses this deterministic order:

1. explicitly enabled user-local development override for the package/profile;
2. portable source explicitly assigned by the project request;
3. explicitly assigned project vendored/static source;
4. organization namespace/package routing;
5. project named indexes by numeric priority then canonical source ID; and
6. product default public source when allowed.

Private is not automatically higher priority than public. Completion timing,
filesystem order and map insertion cannot choose a source. An assigned or locked
source never falls through to another authority when unavailable.

The policy also defines:

- allowed source types
- allowed domains and URL schemes
- redirect behavior
- maximum artifact size
- TLS requirements
- credential requirements and credential scope
- proxy handling
- offline behavior

HTTP without TLS, unbounded redirects, unpinned mutable artifacts, and secrets in
URLs are rejected unless an explicit local development policy allows them.
Availability failure may advance through ordered mirrors of the same logical
source. Hash, signature, publisher, TLS, redirect-scope or authentication-policy
failure quarantines the artifact and stops automatic fallback.

## Dependency Request And Lockfile

Project package metadata:

```text
.horo/packages.json       # portable requested dependencies and sources
.horo/packages.lock       # resolved exact versions and verification metadata
.horo/local/packages.json # user-local dev overrides and trust references
```

`packages.json` contains user intent:

- package ID
- version/range
- source declaration
- desired feature flags
- requested contribution group when needed

`packages.lock` contains resolved immutable identity:

- exact version
- source type and stable source identity
- artifact hash
- manifest hash
- resolved transitive dependencies
- package format version
- compatibility result
- resolved contribution list
- `requestHash`, the hash of canonical dependency-request fields from `packages.json`

`packages.lock` must not contain:

- local absolute paths
- user trust decisions
- credentials or tokens
- user-specific cache paths
- editor workspace state

Trust is user-local or policy-local state. Teams must not commit a trusted-code
decision by accident. Non-interactive restore and release validation fail when
`packages.lock.requestHash` does not match the canonical dependency request in
`packages.json`.

The implemented schema-v1 codec is `ValidatedPackageLockfileV1`. Generation
binds every resolver selection to typed source identity, artifact, package
manifest and file-manifest digests before publishing one canonical package-ID
ordered document. Direct roots and exact-version edges make the stored closure
auditable; parsing rejects unknown or duplicate fields, noncanonical nested
ordering, cyclic edges and unreachable stale entries. Restore validation is a
pure preflight over the request digest, package format and exact host platform;
it performs no cache, project, trust or activation mutation.

## Resolver Rules

The resolver is deterministic:

- same `packages.json`, `packages.lock`, platform, and package sources produce
  the same graph
- semantic versioning is supported but the lockfile pins exact versions
- circular dependencies fail validation
- dependency edges may name required contribution groups (`runtime`, `editor`,
  `assets`, etc.)
- editor-only dependency edges are excluded from runtime release closure
- prebuilt native dependencies are selected by platform, architecture, and SDK
  ABI

A dependency graph is not just package-to-package. Contribution-level dependency
metadata is required so release, restore, and activation can exclude editor-only
or unsupported contributions safely.

## Source And Prebuilt Game Libraries

A game library may be source or prebuilt:

| Type | Build | Trust | Compatibility |
|---|---|---|---|
| Source Game Library | Compiled by the project build pipeline | Medium | engine/script runtime compatibility |
| Prebuilt Game Library | Native binary per platform/arch/ABI | High | engine runtime, SDK ABI, platform, arch |

Prebuilt native libraries require explicit trust. Native code is trusted code,
not sandboxed.

## Package-Scoped Asset Identity

Mounted package assets normally use authoring references without concrete package
versions:

```text
package://com.vendor.weapon-pack/assets/rifle.mesh
```

The lockfile resolves package ID to the exact package version and artifact hash.
Version-pinned URLs are allowed only for explicit compatibility or migration
scenarios:

```text
package-pinned://com.vendor.weapon-pack@1.2.0/assets/rifle.mesh
```

Imported project assets use ordinary project GUIDs. Imported asset metadata keeps
source provenance:

```text
sourcePackageId
sourcePackageVersion
sourcePackageAssetId
```

This preserves update matching without allowing package updates to overwrite
project-owned edits.

## Trust Levels

| Trust level | Code allowed | Risk |
|---|---|---|
| `DataOnly` | None | Lowest |
| `ScriptRestricted` | Scripts in restricted runtime | Medium |
| `GameCodeTrusted` | Gameplay/native code | High |
| `EditorExtensionTrusted` | Editor tools and host capabilities | Highest |
| `NativeTrusted` | Native binary contribution | Highest |

Rules:

- install/restore does not grant trust
- native code is trusted code, not sandboxed
- permissions reduce authority but do not make native code safe
- editor extensions activate through the Extension System
- trust approval, denial, revocation, and activation are audit events

### Trust Metadata Is Not Authoritative

Declared package security metadata is advisory. The host computes required trust
from contribution kinds, expanded contribution descriptors, executable content,
native binaries, editor extension descriptors, source policy, signature status,
publisher identity, and product security policy.

A package cannot lower its own trust requirement. A package that declares
`declaredRisk = "data_only"` but contains scripts, native binaries, or editor
extension descriptors is rejected or escalated to the computed trust level.

## Publisher Identity And Signing Roots

Package signatures are verified against trusted publisher identities, not against
public keys supplied only by the package archive itself.

Trusted publisher keys may come from:

- built-in Horo trusted publisher store
- user-approved publisher trust
- organization/enterprise policy
- local offline trust store

A package archive may include certificate metadata, but that metadata is not a
trust root by itself. Unknown publishers require explicit approval or policy.
Signing key rotation and revocation are handled by the trusted publisher store or
organization policy.

[ADR-178](../../adr/178-application-security-primitive-and-signature-baseline.md)
provides the executable exact-artifact verifier and unforgeable evidence type.
Archive structure/hash success alone cannot construct that evidence; package
composition must bind its manifest/file/archive claims to the selected native
artifact before activation.

## Cache Model

Package cache is a performance input, not a correctness input.

The active package cache is content-addressed by artifact hash and validated
package identity. Package ID and version are metadata indexes, not the sole cache
identity:

```text
~/.cache/horo/packages/by-hash/sha256/<artifactHash>/
~/.cache/horo/packages/by-id/com.vendor.weapon-pack/1.2.0 -> by-hash/...
```

If the same package ID and version resolve to a different artifact hash from a
different source, the resolver reports a package identity conflict unless an
explicit source override policy accepts it.

Every cache hit must still verify:

- artifact hash
- manifest hash
- signature when required
- extracted layout
- package format version
- contribution descriptors

The cache store tracks:

```text
PackageCacheStore
PackageCacheEntry
PackageCacheLease
PinnedPackage
QuarantinedPackage
```

`PackageCacheStore` accepts only an absolute cache root resolved by the host and
uses the host's durable filesystem service for same-filesystem publication and
process-safe digest locks. A verified archive is written to staging, made
read-only, atomically moved to `by-hash/sha256/<digest>/archive.horopkg`, then
read back and independently verified before publication succeeds. A concurrent
publish, read, or cleanup for the same digest fails with a retryable busy result;
loaded archives own their byte snapshot, so later cleanup cannot invalidate a
consumer.

Reads reject links, non-regular files, oversized bytes, malformed archives, and
digest mismatches. Readable corrupt entries are atomically moved out of the
active namespace into a reason-specific quarantine directory. Failed downloads
may be quarantined directly without constructing a validated archive. Each
quarantine entry contains the inert artifact and a bounded diagnostic record
with only its stable reason, expected and actual digests, and byte count; source
URLs, credentials, and host paths are excluded. The diagnostic is published
before the artifact, so an interrupted quarantine never exposes failed bytes as
a cache hit.

Cleanup takes the same digest lock, restores only the owner permission required
for deletion, and durably removes the archive. Missing entries are idempotent
successes. Staging and quarantine retention policy remains host-owned; neither
location is searched as a package source or treated as verified content.

Failed extraction or verification moves data to quarantine, not to the active
cache. Cache garbage collection obeys leases, pins, disk budget, and project
references.

## Credentials And Private Sources

Private sources use credential handles. Raw secrets must not appear in:

- `packages.json`
- `packages.lock`
- command-line arguments when avoidable
- logs
- diagnostic bundles
- job history

Credential handles themselves remain in user/organization credential bindings;
they are also excluded from project files, lockfiles, exported cache metadata,
diagnostics and operation history. A source request resolves one short-lived
credential lease just in time, scoped to the source, endpoint origin and
operation. Redirects, mirrors, subprocesses and package hooks cannot inherit it.

Credential access follows [Application Security](../security/application-security.md)
and [Release Security](../release/release-security.md).

## Package Validation And Authoring Commands

Package production and validation are first-class:

```bash
horopak validate package.horopkg
horopak pack ./MyPackage --output dist/
horopak sign dist/package.horopkg
horopak verify dist/package.horopkg
```

Validation checks:

- manifest schema validity
- declared files exist
- no undeclared files in strict mode
- no path traversal or symlink escape
- package ID namespace validity
- semantic version validity
- dependency graph validity
- contribution descriptor validity
- platform binaries match declared platform/arch
- license and notice files exist
- size limits are respected
- signatures and hashes verify

### Audio Middleware Packages

[ADR-072](../../adr/072-audio-middleware-integration-model.md) governs middleware
package activation. An integration package declares one exact event-bridge or
backend-replacement capability plus adapter/SDK/runtime versions, native modules,
platform variants, cooked banks and bindings, limits, permissions, license files
and redistribution facts. Package discovery and trust do not make that contribution
an active Audio backend.

Audio preflights the adapter, complete binding manifest, all required banks,
effective capabilities and budgets as one private candidate. Any missing version,
bank, mapping, target, trust, permission, license fact or dependency rolls back the
candidate. Runtime never scans vendor directories or loads whichever banks happen
to be present, and package disable/update cannot unload code or banks while Audio
event, callback, tail, queue or profiler leases survive.

## Service Boundaries

| Service | Responsibility |
|---|---|
| `PackageService` | Coordinates high-level package use cases |
| `PackageResolver` | Resolves requests into a deterministic graph |
| `PackageRestoreService` | Restores clean-machine project package state |
| `PackageCache` | Stores verified archives and extracted read-only content |
| `PackageLifecycleService` | Install, enable, activate, update, uninstall, migrate |
| `AssetImportService` | Imports individual assets from packages |
| `GameplayModuleBoundary` | Registers game library modules, behaviors, services |
| `ExtensionHost` | Activates exact trusted extension candidates from verified install records and owns live module/registry leases |
| `TrustService` | Evaluates signature, trust level, and user/policy approval |

Editor modals, CLI commands, MCP tools, and CI jobs are adapters. They call these
shared application services and do not own package business rules.

## Observability

Package operations emit structured records:

```text
PackageRestoreOperation
PackageDownloadOperation
PackageVerifyOperation
PackageInstallOperation
PackageActivateOperation
PackageUninstallOperation
PackageUpdateOperation
```

Records include safe fields such as operation ID, package ID, version, source
type, phase, duration, byte count, cache hit, verification result, and trust
requirement. They must not include URL query tokens, auth headers, raw secrets,
or sensitive local paths.

## Required Tests

- manifest schema validation
- canonical round-trip for pure data, tool, extension, gameplay-library, hybrid,
  and template packages
- package kind/contribution/artifact shape mismatch rejection
- decoded or partially validated manifests cannot enter resolver/lifecycle APIs
- package/file/archive/signature digest binding and trusted-root verification
- package archive layout validation
- package source validation
- direct URL without hash rejected
- absolute local path rejected as portable source
- same package ID/version with different artifact hash conflicts
- lockfile excludes trust, credentials, absolute paths, and cache paths
- lockfile request hash detects stale lockfile
- contribution-level dependency graph resolution
- editor-only contribution excluded from runtime graph
- source vs prebuilt game library compatibility
- cache hit still verifies artifact and manifest
- package-scoped asset identity resolves through lockfile
- package-declared low trust cannot bypass computed trust requirement
- package signature requires a trusted publisher root
- extension descriptor owner/package binding and undeclared-file rejection
- extension-only and hybrid packages resolve through the same request/lock graph
- ExtensionHost cannot load raw directories or resolve/trust package state
- deterministic source precedence under randomized map/request completion order
- locked/assigned source unavailability does not re-resolve through another source
- mirror availability fallback stops on integrity/signature/security failures
- development overrides never enter committed requests, lockfiles or release

## Related Documents

- [Package Manager](../../../mock-studio/designs.md#architecture-packages-package-manager): React mock design for package
  dependencies, sources, restore state, overrides, and lockfile diff.
- [Package Restore](./package-restore.md)
- [Package Lifecycle](./package-lifecycle.md)
- [Package Release Integration](./package-release-integration.md)
- [Asset Pipeline](../runtime/asset-pipeline.md)
- [Extension System](../extensions/plugin-system.md)
- [Gameplay Module Boundary](../extensions/gameplay-module-boundary.md)
- [Application Security](../security/application-security.md)
- [Release Security](../release/release-security.md)

# Release Architecture

## Purpose

The release system transforms a Horo project into a verifiable,
platform-specific distribution artifact. Release behavior belongs to shared
application and pipeline services and is available through:

- the Horo GUI
- the Horo CLI
- MCP in both hosts
- CI automation

All entry points submit the same typed release request and observe the same job
model. GUI, CLI, MCP, and CI do not implement separate build or packaging
pipelines.

## Repository Release Policy

Horo Engine releases are maintainer-driven. The repository does not use any
workflow that automatically opens release PRs, bumps versions, writes changelogs,
creates tags, or publishes GitHub Releases.

The release decision is manual:

1. A maintainer chooses the release version.
2. The maintainer updates `HORO_ENGINE_VERSION` in `CMakeLists.txt` and any
   release notes or changelog content that should ship with the release.
3. Normal CI must pass on the selected commit.
4. The maintainer creates an explicit signed or annotated tag, for example
   `v0.3.0`, and creates the GitHub Release manually.
5. The `Release Binaries` workflow reacts to the published GitHub Release and
   uploads platform artifacts. The same workflow may be dispatched manually with
   an existing tag as a repair path.

This mirrors the release posture used by projects such as Hermes Agent and
OpenClaw: automation validates, builds, signs, and uploads artifacts, but it does
not decide that a release should exist. Manual maintainer intent is the gate.

The release binaries workflow must treat the tag as immutable release input. If
a release is wrong, prefer a new corrective tag over mutating already-published
artifacts unless the maintainer is deliberately repairing an unpublished or
failed release attempt.

### Semantic Version Authority

Release candidates use the full bounded SemVer identity, including build
metadata. Build metadata participates in exact candidate identity but is ignored
for precedence. Engine and game product versions are distinct types and must not
be interchanged.

Before release work begins, boundary adapters read the requested version, tag,
manifest, release notes, source revision, and—only for an engine product—the
persistent project-contract version. A side-effect-free authority validator
requires those claims to agree exactly. The persistent contract deliberately
projects only the SemVer core and prerelease because durable project metadata
does not carry release build metadata. Reading Git, manifests, or release notes
and mutating those sources remain adapter responsibilities; validation never
performs I/O or changes ambient state.

## Product Release Profiles

A release profile defines what kind of product is being produced. Horo supports
at least these product kinds:

- `engine-editor`: HoroEditor desktop distribution.
- `engine-cli`: `horo-engine` and `horopak` command-line packages.
- `sdk`: public development SDK and headers.
- `game-runtime`: packaged playable game.
- `game-dedicated-server`: optional headless server package.
- `developer-diagnostics`: symbols, logs, debug metadata, and validation reports.

Each profile declares:

- included executables and runtime libraries
- asset package/chunk policy
- platform package format
- signing and notarization requirements
- symbol and crash-report artifact policy
- license and notices inclusion
- update and patch eligibility
- store/publication destination eligibility

`ReleaseProfileCatalog` is the Horo-owned schema-v1 persistence and resolution
boundary for this policy. Presets use bounded single-parent inheritance and
resolve into an immutable `EffectiveReleaseProfile`; explicit child fields
replace the corresponding inherited field. Product, artifact class, platform,
and package-format admission reuses the distribution model's authoritative
compatibility table. Toolchain/CMake presets, output paths, credential handles,
private bindings, and publication execution state are separate inputs and are
not fields in persistent product profiles. Unknown fields, duplicate identities,
missing parents, cycles, unsupported required capabilities, and incompatible
overrides fail before a release request is frozen.

For a network-capable game profile,
[ADR-103](../../adr/103-network-project-configuration-and-build-profile-ownership.md)
also requires a typed network release profile naming supported ADR-102 modes,
exact NetworkRuntime/private provider artifacts, protocol/schema compatibility,
security/trust floors, allowed renderer-independent network project profiles,
platform/provider variants, redistribution evidence and permitted runtime override
bounds. The release job freezes those inputs into one `NetworkArtifactPlan` used
by configure, build, cook, package and final verification.

Packaging emits a safe `NetworkProductCapabilityManifest` describing supported
modes/providers and public compatibility/security evidence. It grants no runtime
authority and contains no credential value/reference, private key, authorization
header, machine-local credential path or unrestricted environment value. Public
credential requirements are stable IDs; the release operation resolves their
private bindings through an injected provider only at the stage that needs them.

For an Audio middleware profile, ADR-072 additionally requires the exact adapter
package, SDK/runtime/native libraries, cooked banks and bindings, target variants,
license/notice files, redistribution class, hashes and qualification evidence.
A local SDK or successful editor preview is not redistribution authorization.
Missing, mismatched, prohibited or unknown required content fails before signing
or publication; release does not silently omit the integration or select another
Audio model/backend.

Editor and CLI releases may use Horo-owned update channels. Packaged games own
their product identity, version, update channel, store integration, privacy
policy, and patch policy.

Android product profiles additionally follow
[ADR-171](../../adr/171-android-host-target-and-ownership.md). They freeze the
minimum, compile and target API levels, CPU ABI, GameActivity dependency,
renderer profile, permissions/features and device-qualification identity.
Each ABI is configured and built independently; a multi-ABI package may combine
only verified native artifacts and records their identities and hashes. Store
target-level policy may advance without silently broadening the engine's API,
ABI or graphics support claim.

## Service Model

```text
GUI       CLI       MCP       CI
 \         |         |        /
  +-------- ReleaseService ---+
                  |
          ReleasePipeline
                  |
   validate -> build -> cook -> package
          -> pre-sign verify -> sign
          -> finalize metadata -> final verify -> publish
```

`ReleaseService` owns use-case validation, job lifecycle, cancellation,
progress, and structured results.

[ADR-060](../../adr/060-release-domain-model-and-state-machine.md) defines the
authoritative typed identities, single-target job state, stage attempts,
candidate state, revisioned snapshots, terminal results and ownership rules.
Adapters and the operation store project that model; they do not own or
reinterpret it.

`ReleasePipeline` owns deterministic execution of release stages. It does not
depend on ImGui, terminal formatting, MCP transport, or CI-provider APIs.

In the editor, `BuildReleaseModal` is the GUI adapter. It is hosted by
`EditorModalHost`, owns exclusive editor focus while open, and submits typed
requests to `ReleaseService`. The modal is not the release-job owner.

Closing `BuildReleaseModal` does not destroy or implicitly cancel a submitted
job. The user explicitly chooses whether a supported running job continues in
the background or receives a cancellation request. Reopening the modal queries
active and recent jobs from `ReleaseService`.

Host adapters own only:

- collecting and validating protocol-level input
- presenting progress and diagnostics
- translating the final typed result
- requesting cancellation

## Release Request

A release request contains:

- project root and project identity
- semantic version
- target operating system and architecture
- build configuration
- toolchain profile
- output root
- content and feature selection
- signing profile reference
- archive protection profile reference
- reproducibility and diagnostics options

Secrets are referenced through credential handles. Passwords, private keys, and
tokens are not stored directly in release request objects or persistent job
history.

## Job And Pipeline State

Each target is an independent job under ADR-060. Job ownership and stage attempts
are separate state machines. A job follows:

```text
Queued -> Running -> Succeeded
   |         |  \
   |         |   -> Failed
   |         -> Cancelling -> Cancelled
   |                         -> Failed
   -> Cancelled
   -> Failed
```

The pipeline position for each target follows the planned stage order:

```text
Validating
  -> Configuring
  -> Building
  -> Cooking
  -> Packaging
  -> PreSignVerifying
  -> Signing
  -> FinalizingMetadata
  -> FinalVerifying
  -> Publishing
```

Each stage is a distinct attempt with `NotStarted`, `Running`, `Succeeded`,
`Failed`, `Cancelled`, or preplanned `NotApplicable` state. An attempt never
rewrites a previous attempt, and the job terminal result commits exactly once.
`Cancelling` remains non-terminal until cleanup acknowledges cancellation or
reports failure. A queued job not yet admitted to a worker owns no pipeline
resources and may atomically commit `Cancelled`; an admission/cancel race is
serialized by the release service.

Stage contracts:

1. `Validating` verifies the project, target, toolchain, credentials, output
   policy, and required capabilities.
2. `Configuring` generates the target-specific build configuration.
3. `Building` compiles and links project binaries.
4. `Cooking` converts source assets into runtime-ready target artifacts.
5. `Packaging` assembles the distribution and creates `assets.horo`.
6. `PreSignVerifying` validates the unsigned staged payload, package layout,
   archive readability, forbidden-file policy, and signing prerequisites.
7. `Signing` signs supported binaries and performs required notarization,
   stapling, timestamping, and platform-package signing. This stage may change
   artifact bytes.
8. `FinalizingMetadata` computes the hashes of the signed artifacts and freezes
   the canonical candidate manifest, checksums, signing records, provenance,
   and other supply-chain metadata. Release metadata signatures are produced
   only after their signed content is final.
9. `FinalVerifying` validates the final manifest and checksums against the exact
   signed artifacts, verifies platform signatures and notarization results, and
   runs the configured packaged-artifact smoke checks.
10. `Publishing` transfers only final-verified immutable candidates to a
    configured destination.

Stages publish typed progress events and structured diagnostics. A failed stage
does not silently continue into later stages.

Events are bounded invalidations carrying job revision and typed identities.
Observers query immutable service snapshots for authoritative state. Closing a
modal, terminal view, MCP connection or CI log subscriber releases only that
observer and never changes job lifetime.

## Prepare Release Modal Design

`BuildReleaseModal` is hosted by `EditorModalHost`. The developer first selects
one product profile and one platform/architecture/configuration target, then
supplies the version and project-relative release-notes path. Optional build and
package settings remain available without interrupting the primary path.
Delivery settings specify the local output root and references to configured
signing and archive-protection profiles. The derived candidate path is previewed
as `<output-root>/<version>_<platform>_<architecture>_<configuration>/`.

Before submission, a review step presents the complete request and the checks
the service will perform. Submission starts one service-owned job; the modal
then observes its stage track (Validate → Configure → Build → Cook → Package →
Pre-Verify → Sign → Finalize → Final Verify) and structured activity. The
developer promotes a final-verified immutable candidate to a publication
destination through a separate decision. Editing publication channels does not
silently publish a candidate during construction.

The footer stays visible while only form or activity content scrolls. The modal
owns exclusive editor focus while open. Closing an active job offers an explicit
keep-running or cancellation path; closing never silently cancels the job.

[Prepare Release reference design](../../../mock-studio/designs.md#architecture-release-release-modal-design)

## Target Model

A release job produces one operating-system, architecture, and configuration
combination.

Local GUI and CLI invocations may execute one or more jobs, but each job remains
independent and validates its own toolchain. Cross-compilation is allowed only
when an explicit compatible toolchain profile is configured.

CI creates a service-owned `ReleaseGroupId` with immutable required/optional
membership for a matrix of independent release jobs and aggregates their verified
outputs by that identity. Adapters never infer a matrix from labels, timestamps,
paths or request order. A failed required target cannot be represented as a
successful multi-platform release.

## Output Layout

Release output uses a predictable layout:

```text
<output-root>/
  <version>_<platform>_<architecture>_<configuration>/
    bin/
    assets.horo
    manifest.json
    checksums.txt
    notices/
    symbols/             optional, access-controlled
    logs/
```

The distribution contains runtime artifacts only. Authoring sources such as raw
scene documents, source textures, source models, editor metadata, and temporary
build files are not included unless an explicit developer package profile
requires them.

Output is assembled in a private staging directory and atomically promoted to
its final path only after verification succeeds.

## Artifact Manifest

Every release contains a versioned machine-readable manifest describing:

- engine and project versions
- target platform, architecture, and configuration
- toolchain identity
- build and source identity
- enabled runtime features
- artifact paths, sizes, and cryptographic hashes
- asset archive format version
- signing identity and signature metadata
- reproducibility metadata

The manifest uses a canonical serialization format before hashing or signing.
Unknown required manifest fields cause validation failure. Optional extensions
are namespaced and versioned.

## Runtime Compatibility Contract

A game release declares runtime compatibility separately from editor project
format compatibility.

The release manifest may include:

- a versioned `saveCompatibility` object
- `networkProtocolVersion`
- `modApiVersion`
- `assetArchiveFormatVersion`
- `requiredEngineRuntimeVersion`
- `requiredPluginApiVersion`

When a product supports runtime saves, `saveCompatibility` is required and contains:

- `productSaveCompatibilityVersion`;
- exact `writeArchiveFormatVersion` and `writeSaveSchemaVersion`;
- inclusive direct-readable and migration-source ranges for archive/save schemas;
- every required save participant ID with its direct-readable and migration-source
  `ParticipantSchemaVersion` ranges;
- `minimumSupportedProductSaveCompatibilityVersion`;
- migration policy and the sealed migration-catalog identity; and
- `forwardReadPolicy`, which is `Reject` for HoroSave v1.

Preflight rejects a release when its current writer versions are not directly
readable, a declared migration range lacks a complete bounded path, or required
participant declarations differ from sealed product composition. Engine/build and
product semantic versions are provenance, not save-codec selectors. The legacy flat
`saveFormatVersion` and `minimumReadableSaveFormatVersion` fields are read-only
migration input and are not emitted by new manifests.

A game update must not silently make existing user saves unreadable. If a save
migration is required, the game release profile declares whether migration is:

- automatic and reversible
- automatic but one-way
- explicit user-confirmed
- unsupported

Before product 1.0, each shipped preview declares exact supported ranges and retains
fixtures for those claims. From product 1.0 onward, a stable release supports
migration from at least the previous two stable minor lines and for at least 12
months after each source line's last release, whichever is longer. Patch releases
cannot narrow this window. Removing support requires deprecation in two preceding
stable minor lines, release notes and a final bridge release. Security policy may
block a vulnerable decoder earlier only through an explicit manifest exception and
typed security diagnostic.

Network protocol incompatibility must be represented explicitly so multiplayer
clients, dedicated servers, and tools can reject incompatible connections with a
typed diagnostic instead of failing at runtime.

Mod and plugin compatibility is checked before activation or launch. Unknown or
incompatible mods are disabled, quarantined, or require explicit user action
according to the game product policy.

## Asset Packaging

Release assets are addressed by stable logical asset identifiers and packaged into
`assets.horo` according to typed dependency reachability. Under
[ADR-169](../../adr/169-vtx-producer-terrain-world-streaming-packaging-and-server-ownership.md),
VTX-enabled client products include the exact manifest/page-pack, Material/shader and
declared fallback closure. Dedicated/headless products exclude client-only VTX GPU
artifacts by default; listen-server inclusion belongs only to its local graphical
client scope. Cook-cache directory contents are never package authority.

The archive provides:

- a versioned header
- a deterministic table of contents
- per-entry type and size metadata
- compression metadata
- cryptographic integrity
- optional authenticated encryption
- explicit format and feature compatibility information

Release runtime code accesses content through an asset-provider contract:

```text
logical asset request
        |
    AssetProvider
      /       \
filesystem   archive
development  release
```

Scene, mesh, texture, shader, material, and other runtime loaders do not assume
that release assets exist as raw filesystem files.

Wrong credentials, unsupported archive versions, missing entries, and failed
integrity checks produce explicit errors. The runtime never falls back from a
protected release archive to unprotected authoring files.

## Reproducibility

Release jobs normalize inputs that otherwise vary between machines:

- file ordering
- archive entry ordering
- timestamps included in deterministic content
- locale and timezone
- generated metadata serialization
- toolchain and dependency identity

Given identical declared inputs and a reproducible toolchain, release content
hashes should be identical. Non-deterministic platform signing metadata is
tracked separately from reproducible unsigned content.

## Job History And Logs

Persistent job history contains:

- public request metadata
- target and stage results
- timestamps and duration
- artifact and log locations
- structured diagnostics
- manifest identity

History never contains credentials or raw secret values.

Logs are separated by release job and stage. Log records include timestamp,
severity, subsystem, target, and stage. User-facing adapters may render logs
differently but do not alter the underlying diagnostic identity.

Release records use the common structured schema and carry
`release.job_id`, `release.stage`, and operation context. Job logs are queryable
through the release service; the global process log stores lifecycle summaries
and references instead of duplicating unbounded child-process output.

The authoritative live copy is stored in the release-job log root managed by
`ReleaseService`. The output layout's `logs/` directory is an optional bounded
export or summary for the producer/operator. It is excluded from the
customer-facing game package unless an explicit developer-diagnostics profile
permits it.

## Cancellation And Recovery

Cancellation is cooperative and checked at every stage boundary and during
long-running operations.

On cancellation or failure:

- child processes are terminated through the platform process service
- temporary outputs are removed or quarantined
- the final artifact path is not published
- completed logs and diagnostics remain available
- credentials and sensitive in-memory buffers are released

Publishing and final output promotion are idempotent or protected by an explicit
conflict policy. Existing artifacts are never overwritten implicitly.

## Verification

A release succeeds only when:

- all required stages complete
- the unsigned staged payload passed pre-sign verification
- the manifest matches the produced files
- cryptographic hashes verify
- `assets.horo` can be opened and required runtime entries can be read
- no forbidden authoring assets are present
- signing, notarization, timestamping, and entitlement requirements for the
  selected profile are satisfied and verified after signing
- the packaged executable passes the configured smoke test

Packaged artifacts are the objects tested and published. CI does not publish
artifacts that bypass release verification.

Additional required tests cover:

- platform package format selection
- save-format compatibility warning and migration policy
- network protocol mismatch rejection
- mod/plugin API compatibility rejection
- release candidate promotion without rebuilding artifacts
- SBOM/provenance generation and missing-required-metadata failure
- user settings/cache migration after product update

## Release Candidate And Promotion

Artifact construction and channel promotion are separate operations.

A release candidate is the immutable set of signed artifacts and finalized
metadata produced after `FinalizingMetadata`. It becomes eligible for promotion
only after `FinalVerifying` succeeds. A pre-sign-verified staged payload is not
yet a release candidate and cannot be published.

A final-verified candidate may be promoted to one or more channels only after
policy checks pass:

- required targets succeeded
- required signatures and notarization passed
- smoke tests passed on packaged artifacts
- release notes are attached
- compatibility report is available
- required approvals are present
- publication destination is authorized

Promotion never rebuilds artifacts. It moves or references an already verified
release candidate. If a promotion fails, the candidate remains valid but the
channel state is unchanged.

## Security

Release credentials, encryption, signing, CI trust, transport, logging, and
artifact integrity follow [Release Security](./release-security.md).

## Related Documents

- [Build Output UI Reference](../../../mock-studio/designs.md#architecture-runtime-build-output)

- [Editor Modal Host](../editor/editor-modal-host.md): Build & Release presentation,
  focus, close policy, and job reconnection.
- [Release Modal Design](../../../mock-studio/designs.md#architecture-release-release-modal-design): React mock design for the
  `BuildReleaseModal` workflow surface.
- [Engine Data Bus](../foundation/engine-data-bus.md): release/build lifecycle
  notifications.
- [Release Security](./release-security.md): signing and credential boundaries.
- [Distribution And Update](./distribution-and-update.md): installation
  packages, update manifests, activation, and rollback.
- [ADR-112](../../adr/112-save-archive-container-and-compatibility-policy.md):
  runtime-save container, version axes, release compatibility fields and support
  horizon.
- [Horo Package System](../packages/package-system.md): package lockfile freeze and
  content package to release chunk mapping.
- [Observability Architecture](../observability/observability.md): structured records, job
  context, persistent storage, and diagnostic bundles.

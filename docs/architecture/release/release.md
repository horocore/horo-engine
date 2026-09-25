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

The shared service schedules one frozen target per job and retains bounded active
and recent snapshots after a submitting GUI, CLI, MCP or CI observer exits. Its
stage executor checks the frozen input identities before each worker invocation,
passes only typed outputs to the next stage, and reserves candidate identities
before final metadata is computed. Stage workers use the injected bounded
`ReleaseProcessRunner` for shell-free child invocations when a tool is needed.
The service projects a coarse status to `OperationStore`; release snapshots and
bounded diagnostic IDs remain the authoritative detailed observation path.

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

### Preflight and frozen inputs

`ReleasePreflight` accepts typed intent plus read-only observations captured by
the host for that exact intent. It aggregates independent failures for project
readability, version/source consistency, profile identity, toolchain and target
support, output collision, permission, free space, and credential-handle
availability. The observations echo the requested project and output roots so
facts captured for another request cannot produce a plan. A failed preflight
has no plan and performs no output mutation.

A successful `ReleaseExecutionPlan` owns copies of the request, canonical
project/output paths, and source-tree, dependency-lock, profile, toolchain, and
policy digests. Its machine snapshot is an internal plan artifact: it includes
opaque credential-handle identities but never credential values. Human-facing
summaries omit even the handle identities. Before each stage consumes source or
toolchain inputs, the executor must compare fresh read-only observations with
the plan through `ValidateReleaseInputFreeze` and stop on drift. The plan is
not a substitute for a file lock or immutable source checkout; those belong to
the host execution boundary.

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

`PlanReleaseTargetMatrix` admits each group member separately. Every cell has a
distinct service-assigned job identity, stable target identity, requirement,
single-target preflight plan and validated
target evidence, or field-specific validation failures. The host supplies an
exact toolchain descriptor bound to the preflight toolchain digest; a broad
"cross compiler available" observation alone never admits cross-compilation.
The descriptor names the host and target OS/architecture, an enabled and
compile/link-validated toolchain, an installed target SDK, supported minimum
platform range and package formats. Native builds require an exact host/target
tuple. Cross builds require an explicit compatible profile, including when only
the CPU architecture differs. Non-macOS hosts cannot target macOS. The currently
qualified desktop architectures are Windows and Linux x86_64, and macOS x86_64
and arm64. The product/profile package-format policy and the selected toolchain
must both admit the format. The validated SDK, platform floor, format capabilities
and toolchain digest travel with the frozen single-target plan. Duplicate target
identities, equivalent project/profile/OS/architecture/configuration/toolchain
tuples and colliding canonical output roots are rejected within the group.

The service schedules one job per admitted cell and preserves validation
rejections as terminal cell evidence. `SummarizeReleaseTargetMatrix` derives a
group decision from complete immutable membership and terminal outcomes. It
remains incomplete while any admitted cell lacks a terminal outcome. Results
must match group, job and target identity; duplicate or foreign terminal results
fail closed. Success requires every required member
to succeed with a final-verified candidate; optional failures remain visible.
The summary never replaces a job's terminal result or candidate verification.

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
After a successful rename, a retry may report success only when the original
private stage is gone and the existing final tree still matches the exact
candidate manifest. A conflicting or changed final tree remains a collision;
retries never replace published bytes.

Package production selects exactly one host-installed backend for the validated
product/platform/format tuple. The shared dispatcher verifies the unsigned
Build/Cook source tree against its frozen input inventory before invoking the
backend; a missing or
duplicate format producer fails without fallback. Producers receive a distinct
private output root and report produced files in canonical path order. Native
tool handles and format-specific layout rules remain inside the backend. This
contract is additive; existing release callers do not require a migration until
the host installs concrete package producers.

The input inventory is captured before packaging. A separate inventory of the
packaged private stage is captured after package production for pre-sign
verification; the final post-sign manifest does not exist during packaging.

## Artifact Manifest

The private unsigned stage has its own canonical pre-sign inventory. It records
the candidate ID and exact paths, roles, sizes, and SHA-256 digests before
signing. Pre-sign verification rejects missing, changed, undeclared, or linked
files. This inventory is a separate schema and cannot be parsed or published as
the final candidate manifest; signing may change its recorded bytes.

The signing handoff verifies this complete unsigned tree immediately before it
invokes a host-owned signer. It admits only a nonzero opaque credential handle
already selected in the frozen release plan; credential values remain inside
the signer. A successful handoff is not final candidate verification: signing
may change files, so final metadata and exact post-sign verification still run.
This boundary is additive and does not migrate existing host signing workers.

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

Checksum metadata is a deterministic projection of post-sign artifact records.
It lists each final artifact's SHA-256 digest in portable path order, before
`checksums.txt` and `manifest.json` are written. Neither metadata file may list
itself; the final manifest can then account for the checksum file without an
identity cycle. The projection is additive to the existing manifest contract;
release hosts still own final-byte hashing and verification before publication.

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

`ReleaseBuildProvenance` is the canonical, public-safe unsigned evidence
contract. It records the frozen source, dependency lock, profile, toolchain,
policy, and reviewed notes digests; a build-script digest; sorted runtime
features; named digests of declared non-secret environment values; the source
epoch used by deterministic generators; and sorted relative file sizes and
hashes. Its locale and timezone contract is `C` and `UTC`. Signed bytes and
signing credentials belong only to the final candidate manifest. A comparison
of two provenance values names each changed input field and unsigned file.
The capture boundary streams exact files from a quiescent private unsigned tree
without serializing the host path, and rejects symbolic links, special files,
unreadable content, and portable-path collisions.
Build and cook workers must capture this evidence from actual inputs and bytes
and apply the declared normalization before claiming reproducibility.

## Job History And Logs

Persistent job history contains:

- public request metadata
- target and stage results
- timestamps and duration
- artifact and log locations
- structured diagnostics
- manifest identity

History never contains credentials or raw secret values.

The service accepts an optional host-owned `ReleaseRunHistory` and UTC clock.
The executor receives its stage limits and borrowed synchronous observer in one
`ReleasePipelineExecutionOptions` value. Callers that previously passed separate
limits and observer arguments move both into that value; the release service is
the current production caller, and calls without custom options retain defaults.
When supplied, admission, stage boundaries, and terminal transitions replace a
bounded typed snapshot under an exclusive writer lock. The durable snapshot
contains IDs, revision, stage states and attempts, candidate identity, and UTC
creation, update, and terminal times; it excludes worker messages, arbitrary
paths, and credentials.
Publication uses a durable prepared file and atomic replacement. Recovery rejects
malformed or oversized history and retains the highest candidate ID even after
its job record ages out. Retention evicts the oldest terminal job; active jobs
remain queryable, and admission fails when the store is full of active jobs.
On process restart, a previously nonterminal record is projected as failed with
`interruptedByRestart`; its last stage state remains visible, and no finish time
is fabricated. Schema-v1 records remain readable and are rewritten as schema v2
when the next snapshot is stored.
Hosts that do not supply the optional store retain the existing in-memory
behavior; no existing constructor call needs migration.

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

The candidate verification dispatcher first checks every final manifest file
and byte, requires a host signature verifier when the manifest declares
signing, and then runs each distinct policy-required smoke probe exactly once.
Missing or duplicate required probes fail closed. It checks final files again
after the probes so a probe cannot silently alter the candidate it approved.
Success issues a typed candidate identity carrying the final manifest digest
and checked root; downstream publication must recheck the bytes before upload.
This is an additive dispatch boundary; concrete archive, runtime, install,
launch, and compatibility probes still need host composition and qualification.

The publication dispatcher accepts only that verified identity and the same
final manifest under a preflight-authorized destination. It checks local bytes,
asks the destination adapter to upload only declared files, validates the
receipt identity, asks the adapter to verify remote bytes and signatures, and
rechecks local bytes before committing the channel. The adapter must keep
upload idempotent and channel commit atomic. Concrete destination qualification
and approval policy still belong to host composition.

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

The GitHub Releases destination lives in `HoroReleaseGitHub`, outside the shared
release application target. Its host-owned client resolves an existing tagged
release, uploads only final-manifest files and canonical `manifest.json`, reads
each remote asset back for size and SHA-256 verification, and binds the channel
commit to the same remote release ID. The destination requires a canonical Git
commit SHA in the candidate and peels the existing Git tag to that exact commit.
A missing release, retargeted tag, or changed release ID fails
without creating a tag or release. The `GitHubReleaseCliClient` uses the host's
authenticated `gh` installation through the bounded, shell-free process runner;
it never passes credentials in arguments or emits GitHub CLI diagnostic text.
It treats an existing remote asset as an idempotent retry only after downloading
and hashing its bytes, and changes the stable channel only after verifying the
remote manifest and confirming the release is GitHub's latest. Other channels
fail until they have an explicit remote mapping. `Release Binaries` workflow
composition remains separate host work.

The destination also compares the existing GitHub Release body to the exact
reviewed Markdown in the frozen notes snapshot before upload and at every later
identity check. This extends `GitHubReleaseIdentity` with a bounded body field;
host clients constructing that identity must return the existing release body.
Callers with an older client implementation must supply it or publication fails
closed. Tests cover mismatched and changed release bodies before channel commit.

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

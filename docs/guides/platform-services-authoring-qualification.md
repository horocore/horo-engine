# Platform Services authoring qualification

This records PLS-003 acceptance evidence and remaining integration work. A closed
GitHub child or a successful model test does not establish host, migration or
private-provider qualification.

## Baseline and scope

The delivery starts at `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd` on
`codex/editor-improvements`. The main checkout and its open PR #3457 are separate
from this worktree. Native GitHub sub-issues contain exactly these eight children.
The corresponding Jira parent is HORO-1825; Jira workflow states are retained,
not inferred from GitHub closure.

On 11 November 2026 the delivery branch was rebased onto the local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`. That commit adds only the verified
C11 provider-admission fixture configuration. The parent was not yet published
to origin at the time of this rebase. No remote lookup was used to find it and no
uncommitted main-checkout changes were imported. Hash comparison preserved all
23 source edits; only `tests/CMakeLists.txt` gained the expected parent hunk,
with the PLS target additions retained. Final native validation remains pending
against this rebased worktree.

| Child | GitHub / Jira | Baseline source and regression evidence |
| --- | --- | --- |
| PLS-003.1 | #1888 / HORO-1844 | ADR-132 and the normative Platform Services identity section; #2467 and #2508 merged |
| PLS-003.2 | #1895 / HORO-1851 | `PlatformStableIdRegistry.cpp`, `PlatformStableIdRegistryTests.cpp`; #2692 merged |
| PLS-003.3 | #1896 / HORO-1852 | `AchievementDefinitionRegistry.cpp`, `AchievementDefinitionRegistryTests.cpp`; #2695 merged |
| PLS-003.4 | #1893 / HORO-1849 | `PlatformDefinitionRegistries.cpp`, `PlatformDefinitionRegistriesTests.cpp`; #2698 merged |
| PLS-003.5 | #1894 / HORO-1850 | `PlatformProjectConfiguration.cpp`, `PlatformProjectConfigurationTests.cpp`; #2701 merged |
| PLS-003.6 | #1900 / HORO-1856 | `PlatformProviderManifestCook.cpp`, `PlatformProviderManifestCookTests.cpp`; #3025 merged |
| PLS-003.7 | #1897 / HORO-1853 | Open; shared Project Settings source integration prepared, executable/localized presentation evidence remains required |
| PLS-003.8 | #1898 / HORO-1854 | Open; integrated host, migration and clean/incremental publication evidence remains required |

All seven PLS merge commits supplied by those PRs were verified as ancestors of
the starting HEAD using `git merge-base --is-ancestor`. The transitive
CFG-001.2 dependency #1822 / HORO-1778 is GitHub closed, with #2680 merge
`45623cb8da27f9bb65c909449b881e6ab6b80abd` also an ancestor; its Jira state
remains In Review. PLS-003.1 is Jira Done; the other five closed PLS children
remain Jira In Review.

PLS-003.7 depends on #1894, #1893, #1896 and the open shared modal dependency
#1825 / HORO-1781. PLS-003.8 depends on #1895, #1893, #1896, #1894, #1897 and
#1900. The shared CFG modal work has a separate owner; PLS must use its common
draft lifecycle rather than introduce another settings authority.

ADR-132 still has a Proposed status label. The normative architecture explicitly
references its identity contract, so that label is not permission to change the
algorithm, salt authority, permanent history or private mapping boundary.

## 13 November restart and published-base integration

The user resumed all topic work and authorized traceable topic commits, normal
branch pushes and reviewable draft PRs. Protocol V2 removes the old named-grant /
MAIN-ONLY HOLD veto. The actual canonical OS lock remains mandatory, acquired
nonblocking and retained through a bounded foreground batch, with real Windows
handle, CPU/RAM and serial-worker checks. Live sibling jobs and sources remain
untouched; no background waiter or alternate lock is used.

This worktree fetched and verified published parent
`3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`, then rebased its existing checkpoint
onto it (rewritten local checkpoint `8455527b1`). Before rebase, 84 dirty source
files and the tracked binary diff were backed up under ignored build storage.
All 84 files were restored. Five byte differences were reviewed as published-parent
integration: MSVC UTF-8 options, editor host safety cleanups, existing offline-queue
allocation documentation, existing allocation/CI CMake targets, and the local
WriteScaffolding conflict. That conflict retains both the durable writer/initial
documents and the upstream exhaustive-enum cleanup. The generated four-edge
migration catalog and compatibility header pass again with the unchanged frozen
0.4.0 set hash. This is metadata-generator evidence, not a native C++ test pass.

The old private validation helper had a check-then-block race: it released a probe
lock before entering the actual blocking context. The supported dev helper now has
an additive `blocking=False` keyword (default behavior is unchanged), allowing one
atomic nonblocking context through the existing cleanup-marker checks. A real
native-lock exclusion/retry regression was added and later passed in the Python
regression command recorded below. Local
validation/quality helpers use that context and the existing managed process runner.
The first registry and quality admission attempts both found the physical lock busy;
no compiler, analyzer or waiter was started. The quality admission JSON under ignored
build storage explicitly says checkerExecuted=false and is not an analyzer report.
Required final full Sonar IDE / CLI secrets / Codacy checks remain not run.

## Additive authoring contracts

Ordinary ledger edits use `BuildPlatformStableIdRegistryReplacement` with the
current immutable registry and the edit's captured fingerprint. Initial detached
validation remains `BuildPlatformStableIdRegistry`. Replacement preserves every
primary, numeric ID and alias; active entries may become tombstones with removal
provenance. Tombstones cannot disappear, restore themselves or rewrite their
removal evidence. Project identity and the captured root salt cannot change,
including for empty ledgers. A host must serialize validation with publication;
this pure function does not provide filesystem transaction authority.

Existing callers of initial construction need no source migration. A host editing
an existing ledger must adopt the replacement operation instead of validating only
the candidate. There is no implicit compatibility path for namespace regeneration
or restoration. Those remain separately authorized migration operations.

Diagnostic classification now distinguishes invalid root salts (`InvalidSalt`),
duplicate primary/alias keys (`DuplicateKey`), reserved tombstone reuse
(`Tombstoned`) and numeric collisions between different primaries
(`HashCollision`). Previously an identical duplicate primary reached the numeric
collision check first. Callers matching the older generic classification must
use the refined code. The registry tests and Project codecs are the affected
callers in this change. Canonical document size is now part of initial validation,
so a candidate that would exceed 4 MiB cannot become a snapshot that its own loader
cannot read.

The Project-owned `HoroEngine::PlatformServicesAuthoring` target owns
`Horo/Application/PlatformServicesAuthoring.h`. It exposes bounded ledger
decode/encode operations without leaking its JSON dependency. The canonical
document has exactly `schemaVersion`, `projectId`, `algorithm` and `entries`;
entries have exactly `kind`, `canonicalKey`, `id`, `aliases`, `state` and
`removalProvenance`. Kind tags and ID text follow ADR-132. Output uses two-space
indentation and one final newline, with registry/alias ordering from the shared
validator. The salt is supplied from one captured root metadata authority and is
never serialized in the ledger. Unknown members, duplicate JSON keys, unsupported
algorithms/versions, oversized documents and invalid encodings fail closed.

The canonical `ProjectSourceDocument` parser captures the optional root salt and
`settings.platformServices` JSON values alongside existing metadata. It does not
introduce a second root parser, defaults or provider activation. Its additional
source fields are trailing optional fields, so existing aggregate callers remain
source compatible. Legacy absence remains readable to compatibility inspection;
explicit Platform Services resolution then fails with a typed missing-salt or
missing-policy error until migration supplies the new authoring data.

Portable policy contains exactly `schemaVersion`, `provider` and `services`.
Provider selection is the closed `{mode, key}` object (`null` or `exact`), with
service requirements in exhaustive `PlatformServiceKind` order. The explicit host
profile is supplied by composition, never stored as an authoring override. Root
salt validity is checked before policy admission; trusted-module, exact-provider
and required-capability failures retain the shared validator's typed identities.
The policy encoder excludes host profile, draft fingerprints and private values.

`PlatformProjectSettings.h` contributes the same portable fields to Foundation:
eight keys owned by `horo.platform.services`, legal only at the Project source,
with `ProjectReopen` reload policy. The contribution is inert and has no
environment bindings. `PlatformProjectSettingsValues` and
`ResolvePlatformProjectSettings` bridge the immutable typed policy and the one
Foundation source resolver. There is no separate UI settings state or precedence
implementation. Salt, identity, host profile and mappings are absent from this
editable key set.

Primitive schema validation does not authorize a provider/capability transition.
The shared Settings integration must project and validate the complete candidate
through `ResolvePlatformProjectSettings` before applying its one transaction.
Registering the descriptors alone does not complete that integration, and schema
defaults must not conceal missing required project authoring data.

This codec does not register a new persistent release contract or mutate
existing projects. Host adoption must accompany project creation and an explicit
root-last migration through the existing project migration executor. Read-only
load and cook must never synthesize a salt or repair missing ledger data.

The earlier registry fingerprint hashed a separate binary projection rather than
the canonical ledger document. The corrected fingerprint hashes ASCII
`horo.platform-services.registry-fingerprint.v2`, one zero byte and the exact
canonical UTF-8 ledger bytes. `CanonicalDocument` is the single encoder used by
both the registry fingerprint and the Project codec. Schema-v1 salt derivation,
numeric IDs, primary keys, aliases and tombstones do not change.

This deliberately invalidates old fingerprint-bound definition snapshots, mapping
evidence, cooked outputs and retained drafts. There is no dual-fingerprint
compatibility validator. Consumers must load the unchanged root salt and ledger,
revalidate semantic definitions against the new captured fingerprint and regenerate
provider mappings and both cooked streams as one generation. They must not relabel
old artifacts as fresh. Previously captured immutable values remain alive for their
owners; a host may publish the newly validated generation only at its existing safe
boundary. Persisted authoring adoption remains part of the versioned migration,
and runtime/offline migration cannot silently reinterpret an old binding.

## Validation ledger

The first native Windows Debug pass configured the headless composition, built
`HoroPlatformRequestTests` and `HoroPlatformServicesPublicHeaderConsumer` with
`--parallel 1`, then ran `HoroPlatformRequestTests [stable-id]`: 10 cases and
183 assertions passed. This predates the subsequent codec, UTF-8 and collision
diagnostic edits and is not final-diff evidence.

Every heavy command uses `scripts/dev.py::heavy_build_lock` keyed by the resolved
common Git directory. Before starting, a 30-second CPU average and available
memory are checked; CPU above 60 percent or available memory below 25 percent
defers execution. A busy shared lock causes a no-queue exit. The native host
`LOCALAPPDATA` must be used: a sandbox-redirected cache would not coordinate with
other chats. After acquiring `heavy_build_lock` and before spawning any heavy
command, `psutil.Process().open_files()` must confirm the actual open handle's
Windows path equals the shared native `heavy-build.lock` path. A matching Git
identity or textual cache path alone is insufficient. A different AC/sandbox
handle path or access denial defers work without a private-lock fallback.
The main Windows ALL validation retains priority; a free or bounded slot must be
announced before another heavy job starts. Test concurrency is one.

Final codec tests, affected header consumers, complete platform-service
regressions and the required final-scope Sonar IDE / CLI secrets / Codacy report
are pending. No Linux/macOS, native private-provider, GUI interaction, durable
migration recovery or cloud quality-gate pass is claimed here.

## Semantic source document adapters

`PlatformServicesDefinitionCodec.h` belongs exclusively to
`HoroPlatformServicesAuthoring`; consumers link that target's staged headers.
The adapter is additive and has no prior persisted service-document format to
migrate. Existing semantic builders and runtime contracts remain authoritative.
The branch now registers the 0.4.0 adoption edge and wires editor creation/migration
to these sources. Native execution, durable recovery qualification and complete
GUI/CLI/headless/cook adoption remain unproven; the codec alone is insufficient.

Each service has a separate closed JSON document with `schemaVersion` (1),
`projectId`, `stableIdRegistryFingerprint` (canonical `sha256:` text), and
`definitions`. The complete active set uses canonical `sid1:` IDs, never keys
or provider-native names. The root owner and ledger generation fence the
existing achievement/stat/leaderboard/presence builders; leaderboard loading
also requires a captured compatible stat snapshot. Core typed errors survive.

Achievement entries contain `id`, `authority`, `progress` (`kind`, `total`) and
`presentation` (`titleLocalizationKey`, `descriptionLocalizationKey`, `hidden`).
Stat entries contain `id`, `authority`, `valueKind`, `range` (`minimum`,
`maximum`), `mutation`, `localizationKey`, `hidden`. Leaderboards use `ordering`
instead of `mutation` and add explicit nullable `sourceStat`. Presence entries
contain `id`, `detailPolicy`, `maximumDetailUtf8Bytes`, `localizationKey`,
`hidden`. Enum numbers match the existing typed enum declarations; semantic
validation remains in those declarations' owning builders.

Documents cap input/output at 4 MiB, definitions at 4096, nesting at eight,
object members at 65,536 and member names at 64 bytes. Typed limits may tighten
these caps. Duplicate or unknown members, fractional integers, coerced booleans,
unsigned values outside signed endpoint range, and noncanonical IDs fail closed.
Output uses fixed member order, existing immutable ID order, two-space indentation
and a final newline. Salt and provider-native values are absent.

The authoring regression now serializes and reloads all four semantic documents
before comparing cook output after incremental and clean ledger reloads. New
shape/range/cross-reference tests and the public API signature consumer are
written but await the native slot. Formatting and whitespace checks do not
establish C++ execution or durable publication evidence.

## Creation and recoverable migration implementation

The branch advances the development baseline to `0.4.0`. Its new descriptor freezes
root salt and portable policy plus five canonical source files; all prior release
descriptors and migration definition trees through `0.3.0` remain untouched. The
new convention-discovered sequential edge is
`core.authoring.platform_services_identity` (`0.3.0` to `0.4.0`). Historical surface,
network/navigation and compression edges remain in the complete migration chain.
The journal reader contract is unchanged and is copied byte-for-byte for 0.4.0.

`PreparePlatformServicesInitialDocuments` is the single pure first-generation
builder used by creation and migration. It admits the existing strict root capture,
rejects pre-existing salt/policy, validates injected nonzero salt, and prepares the
root plus an empty ledger and all four semantic documents before returning anything.
It preserves unrelated root values and does not discover entropy or write files.
The existing opaque Project identity ceiling is 256 UTF-8 bytes; Platform Services
ledger, source codecs and default policy limits now preserve that ceiling instead
of forcing an identity rewrite for owners longer than the old module-local 128-byte
bound. Numeric IDs and fingerprints for existing shorter owners remain unchanged.

Editor creation retains an explicitly injected `Security::SecureRandomSource`
through accepted jobs. The existing operation mutex serializes its synchronous,
bounded, nonreentrant Fill calls; no extra mutex or ambient factory is introduced.
Entropy failure, absence or zero salt fails before staging publication. The root
and all platform files enter the existing staged-directory promotion. Typed causes
survive in the terminal snapshot; the loading route uses paired English/Turkish
identity-preparation copy. Host composition selects the native source explicitly.
Old source callers still compile with the empty default, but callers that actually
create projects must inject a provider; empty never selects a native fallback.

Migration execution remains in `HoroApplication`; definition composition remains
in `HoroProjectMigrations`. The latter privately consumes the separate
`HoroPlatformServicesAuthoring` target, avoiding a reverse dependency cycle. Two
existing stable errors, MigrationCancelled and MigrationStageFailed, now have one
public declaration owner in `ProjectMigrationErrors.h`; the former private header
includes it, and definitions/error identities remain unchanged. Independent stages
use this narrow contract rather than gaining a broad Application source include.

`ProjectMigrationExecutionServices::random` is passed explicitly through Prepare,
VerifiedDryRun and the editor transaction. Mutable serialized stages can request
1–256 bytes; const validators and parallel document stages cannot request entropy.
Missing providers, unsupported request sizes and provider failures clear output.
Provider error identity survives annotation. The executor releases its provider
reference after stage execution, before materialization and final validation.
Concurrent operations sharing an injected instance require concurrent Fill support;
the native host provider satisfies that contract. Changed C++ signatures require
rebuilding affected consumers; existing capability-free pipelines keep empty defaults.

The adoption stage generates exactly once only when the entire platform namespace
is absent and the captured schema predates 0.4.0. A required-schema root with erased
identity, partial/case-variant namespace, malformed salt, stale semantic source or
unknown pre-existing platform data fails without regeneration. A complete existing
generation is validated and retained byte-for-byte during preparation. Migration
validates a detached portable policy through the same shape rules as full resolution;
it preserves an exact provider requirement without claiming installation, trust or
availability. Actual activation still resolves the real trusted manifests through
BuildPlatformProjectConfiguration and fails typed if unavailable; no fake manifest
or Null fallback is synthesized. Runtime and authoring fingerprints are not interchanged.

The final target validator checks inherited authoring, all platform documents and
the exact 0.4.0 marker/history after transaction finalization. Existing root-last
publication/recovery owns the commit. Regressions now exercise the real generated
edge on paths containing spaces and non-ASCII names, preparation without source
writes, preservation, partial identity refusal, entropy failure/zero, creation,
root-publication interruption and verified-original rollback. The recovery test
requires reusing the staged salt even when the entropy provider subsequently fails.
**These C++ regressions are written but not executed.** Python code generation and
format checks are separate evidence and do not qualify migration recovery.

## Ordinary-open source ownership and lifecycle fences

`PlatformServicesProjectSources.h` adds a shared host-facing reader under
HoroPlatformServicesAuthoring. It uses the existing bounded Assets source-capture
contract for the root (1 MiB) and five fixed service paths (4 MiB each). It validates
the sole root salt, ledger, actual trusted inert provider metadata and all four
semantic registries, then rechecks every byte identity before returning a sealed
owned snapshot. No native provider instance, cache, source repair or lifecycle
activation occurs. Captured buffers are released; getters retain only immutable
models, resolved policy, source hashes and the root path.

Base compatibility now carries a SHA-256 witness of the same canonical root read
used for classification. ProjectSourceDocument exposes the derived optional witness;
manually assembled candidates have none. This is revision evidence, never a trust
proof. Capture requires Current/CompatibleReleaseLine admission with that witness
and rejects a changed root before interpreting its module source. A user-authored
JSON field named sourceFingerprint cannot select that witness.

ProjectOpenService invokes this shared reader on the serialized background path
for ordinary current projects as well as migrated projects. Its copied context
contains an explicit host profile and approved contribution/trust sets; the editor
composition passes InteractiveDevelopment explicitly. The complete snapshot is
rechecked before any derived-state Install, before ready publication, at reservation
and at final activation Commit. Shutdown revokes activation while retained CPU models
remain owned. Checks are bounded project-load/activation I/O, not normal render-frame
work; one full six-source check may read up to 21 MiB. No performance pass is claimed.
A retained CPU snapshot is not ongoing filesystem authority: callers must still hold
host read/publication authority for explicit revalidation, and cooperating writers
must honor the existing mutation/lease contract.

The old public ProjectSessionCandidate.sourceFingerprint raw JSON string is removed.
The candidate instead retains shared const ownership of the complete platform snapshot;
callers needing the root digest use platformServices->RootIdentity().digest and callers
needing a six-source revision use SourceFingerprint(). The sole repository producer
and activation readers were migrated together. Rebuild Application/EditorServices and
affected consumers for the changed C++ layouts/signatures; no persisted source format
changes for these derived witnesses. HoroPlatformServicesAuthoring publicly declares
Assets, and EditorServices publicly declares Authoring for the exposed narrow types.

Regressions now cover all four host profiles through the shared reader, actual
manifest trust/profile refusal, exact source edits, root admission races, cancellation,
sidecar bounds, nonempty semantic/tombstone source capture and incremental-versus-clean
cook equality. Ordinary-open regressions cover missing required authority without
migration/repair, each of the six edits before installation, ready/reserved staleness,
and shutdown with retained models. **All newly added C++ coverage remains unexecuted**
until the explicitly allocated native slot. CLI validation wiring is described below;
the cook route, Settings modal and durable paired cook publication still require integration.

## Production headless CLI validation (native execution pending)

The actual `horo-engine` registry now declares `platform-services validate` and
grants only its explicit validation command capability. It requires an explicit
absolute `--project` root; it has no working-directory fallback or repair mode.
Its adapter calls compatibility admission, the same six-source capture used by
ProjectOpenService, then exact revision revalidation before returning owned
structured fields. Composition explicitly selects HeadlessServer with the actual
empty linked contribution/trust set. Thus explicit Null can validate where policy
permits, while an exact uninstalled provider fails with ProviderNotFound. No
provider SDK, native provider instance, editor, window or renderer is selected by
this command. Additional private providers require their real trusted composition;
arbitrary command-line mappings cannot manufacture that authority.

The shared capture now also rejects pending marker migration, so direct CLI
consumption cannot silently bypass a root update that editor open must resolve.
The CLI error translator registers exact identity, achievement, other-definition,
configuration and source-capture descriptors. It preserves the original domain/code
and separates validation, unavailable capability, permission, I/O and cancellation
categories. No message-prefix inference is used.

The terminal target privately declares HoroPlatformServicesAuthoring. The existing
HoroCliProductionOutputContract now invokes the real executable with a complete
current project in a Unicode/space path, compares its source digest with shared
headless capture, and exercises malformed, duplicate, unsupported-version,
oversized, missing and exact-unavailable-provider sources without repair. Explicit
translation regressions are compiled into HoroNavigationHostErrorTests, which also
privately declares Authoring. These tests and the changed production host remain
**unexecuted** until the granted heavy slot. The production cook route is now wired
as described below; its actual execution and private-provider qualification remain pending.
The existing Assets generation publisher requires asset-envelope IDs and is not a
drop-in publisher for these two provider-neutral streams. The paired publisher
instead uses the existing cook generation's exact fingerprint preimage, preserving
both stream formats and their existing digest.

## Production CLI cook wiring (native execution pending)

`horo-engine platform-services cook --project <absolute-root>` is now declared in
its actual command registry with a distinct output schema and explicit writes-files
capability. Composition selects Cook policy and injects the native durable writer.
The adapter acquires the existing ProjectMutationCoordinator lease before source
admission and retains it, then acquires the exact destination lease through publication.
This serializes against cooperating save, migration, settings and other project
writers without a second lock protocol. The CLI privately links the existing
Foundation-only ProjectMutationGuard target; its historical Editor header/namespace
does not introduce EditorServices, GUI or renderer dependencies.

The guard now exposes inert ProjectMutationErrorDomain metadata for host error
registration. Its existing `horo.editor.project_mutation` domain and
`project.migration.locked` code remain unchanged; no alias or replacement error is
introduced. Rebuild the guard and affected consumers for the additive declaration.
Native CLI error tests register this exact identity as an operation failure.

The cook adapter uses the same admission/capture helper as validation. It calls the
shared paired publisher and does not duplicate cooking, encoding or publication.
Protocol result and fallback error storage is allocated before commit. After commit
it only moves the owned receipt/error and fills preallocated digest text. A complete
visible pair with uncertain durability becomes a partial-failure result with
`committed: true`, `durabilityConfirmed: false` and the original typed failure;
it is not reclassified as an unpublished cancellation or rollback.

This concrete CLI has no private provider mapping source. It validates actual empty
linked contributions, refuses unavailable exact selection and also explicitly refuses
an exact provider without its private mapping adapter. It never invents mapping
revision/digests or substitutes Null. Real SDK/account/authentication/native-manifest
qualification remains an external evidence gap; a Null cook pass cannot establish it.

The actual-executable protocol contract now cooks, edits semantic policy, compares
incremental versus fresh project outputs and receipts, restores the earlier policy,
and proves hostile/missing/unavailable/migration-required inputs preserve the prior
pair. It holds a real project mutation lease in the parent process to test immediate
child-process busy refusal. A separate real-dispatcher regression injects native
post-commit durability failure, verifies the committed partial result and confirms
both native leases are released. These tests remain **unexecuted**, together with
all required affected-header consumers and the final quality preflight.

## Shared paired cook publication (native execution pending)

`PlatformServicesProjectCook.h` is owned by HoroPlatformServicesAuthoring and exposes
CookAndPublishPlatformServicesProject. It consumes the sealed shared source snapshot,
only with the Cook profile; the caller explicitly supplies mapping policy/revision,
pinned opaque mapping rows, their host-owned freshness callback, expected prior
output identity, exact native destination lease and durable filesystem. Exact
providers fail before staging without their mapping revision witness. No SDK or
provider-native mapping parser is synthesized. Root namespace/mapping lifetimes
remain the caller's authority; the synchronous operation retains no ambient service.

The unchanged v1 generation-fingerprint preimage is now exposed by bounded paired
encode/decode helpers in the existing PlatformProviderManifestCook contract. Its
SHA-256 equals the existing generationFingerprint, so it can serve directly as
one atomic file containing both existing streams. Decoding requires the expected
digest and rejects partial, unknown-domain, trailing, mismatched or oversized input;
it grants no semantic/runtime trust. The full packet ceiling is 64 MiB. Existing
neutral/mapping byte layouts and fingerprint calculation are unchanged. The cook
input appends an optional Foundation cancellation observer; previous stop-token
callers retain their behavior, while application shutdown reaches the same checks.
Rebuild PlatformServices and all input-struct consumers for the changed C++ layout.

Publication uses the existing Assets source-publication primitive and native lease,
not a new journal/current-pointer authority or invented AssetIds. The sole pair is
`build/cooked/platform-services/current.pls-cook`. An injected writer facade adds
domain freshness checks exactly before tracked replacement, after staging and
verification. Original source, mapping, cancellation and I/O failures are preserved.
The existing monotonic native commit receipt handles both typed errors and exceptions
after replacement. Returned ReconciliationRequired means the complete new pair is
visible; callers must not claim rollback. Derived preparation paths may remain after cleanup I/O failure and block a retry
until explicitly reconciled under the same lease. They are never selected as current.
Authored sources and older published bytes are not repaired or removed.
The writer performs requested directory syncs; Windows/storage power-loss gaps remain
as documented for native filesystem operations.

The semantic source regression now performs real native first/incremental and clean
publications, comparing the resulting file bytes/digests and decoding the published
pair. Other new regressions cover every source changing after staging, cancellation,
private preparation and replacement failure, obsolete expected output, missing lease,
wrong profile, mapping witness refusal/revocation, and typed/throwing post-commit
durability failures. Public consumer coverage includes both the application and
pair-codec signatures. **All new C++ remains unexecuted**; no native pass or performance
improvement is claimed. This bounded service recooks deterministically and makes no
incremental cache acceleration claim.

## Creation publication boundary (native execution pending)

ProjectCreationService now requires a host-injected DurableFileSystem, retained by
accepted work. The editor composition explicitly supplies NativeDurableFileSystem.
All staged files use WriteDurable; directories are synchronized before publication.
The operation exclusively creates its sibling staging directory and only cleans a
tree it created. An existing staging tree is refused and retained. Native paths
preserve spaces and Unicode, including explicit UTF-8 conversion for portable
starter-scene names, and normalized destination depth is bounded to 256. Starter
content cannot target the reserved `.horo` authority namespace.

PublishDirectoryNoReplaceTracked extends the filesystem contract with a fail-closed
default. Its native implementation uses Windows MoveFileExW with WRITE_THROUGH,
macOS renameatx_np with RENAME_EXCL, or Linux renameat2 with RENAME_NOREPLACE.
There is no copy or replace fallback. A monotonic commit receipt is recorded before
directory synchronization. A failure or standard exception after commit retains the
published project and reports ReconciliationRequired with the original typed cause
when available; no ProjectCreatedEvent is sent. The localized loading diagnostic
directs the user to inspect existing files. Recreating into that occupied root is
refused. Ordinary successful creation reports Durable after requested OS operations.
Windows SyncDirectory retains its existing no-op behavior; file flushes and the
write-through rename do not establish hardware power-loss guarantees or prove new
ancestor-directory durability on every storage stack.

The constructor appends the durable writer dependency with an empty default for
source-compatible inert construction; actual creation without it fails before
staging. All repository creation callers that execute work now pass the writer.
Rebuild EditorServices/Platform and affected callers for changed C++ signatures,
snapshot layout and virtual table. The public consumer checks the new signatures.
New native and editor regressions cover no-replace publication, malformed paths,
foreign staging ownership, preparation/sync failures and post-commit reconciliation.
They remain **unexecuted**, together with the required quality preflight.

### Exceptional creation boundaries

Accepted creation operations now reserve before/after-publication failure records
before job submission. The worker owns a monotonic publication receipt for its whole
lifetime; cleanup borrows that receipt and never touches a staging name after native
commit. The outer job boundary converts every unexpected host exception into an
authoritative terminal snapshot after stack cleanup, rather than leaving a public
Running operation behind a failed JobSystem record. The private
`project_creation.unexpected_failure` diagnostic identifies untyped host failure
without echoing arbitrary exception text. Typed filesystem failures retain their
original cause. Pre-commit unexpected failure has the appended UnexpectedFailure
snapshot classification; post-commit uncertainty remains ReconciliationRequired.

The success event's owned project identity and fallback diagnostics are prepared
before publication. After confirmed durability the terminal snapshot uses only
scalar assignments and moves. Queued notification or logging allocation failure
cannot change committed truth or hide an already accepted handle; polling snapshots
remains authoritative. Failed staging cleanup is best effort and may retain private
evidence, never published output. No exhaustive allocator-failure qualification is
claimed by the new injected bad_alloc regression.

New preparation/precommit exception regressions require terminal unpublished failure
and owned staging cleanup. Standard, allocation and nonstandard post-commit exception
regressions require the whole project to remain with the original commit witness,
no created notification and a reconciliation diagnostic. New visible summaries use
aligned English/Turkish loading messages. These regressions remain **unexecuted**
pending physical admission for native execution. Existing enum values and persisted project data do not
change; rebuild affected EditorServices/header consumers for the additive enum.

## Current requirement audit

The current issue text for [PLS-003.7](https://github.com/horocore/horo-engine/issues/1897)
requires shared Project Settings drafts, one validated apply transaction, cancel
without persisted changes and no parallel UI settings model. Its dependency
[CFG-001.5](https://github.com/horocore/horo-engine/issues/1825) additionally requires
eligible descriptors without bespoke modal wiring, typed inline diagnostics,
preview rollback, stale-revision rejection and localized layouts in both theme
layers. Both issues remain open at this audit. Existing specialized prefab,
navigation and collision drafts are implementation evidence, not the released
generic contract required by these issues. At the initial restart this PLS
checkout's SettingDescriptor had no validation/presentation metadata seam. The
published CFG draft described below supplied that metadata; the resumed source
integration now extends its existing session for Project values, as recorded below.

| Requirement | Current authoritative source/evidence | Remaining proof or dependency |
| --- | --- | --- |
| PLS-003.1 identity decision | Normative identity contract, ADR-132 references, unchanged v1 numeric derivation; accepted ancestor audit above | No implicit regeneration; current regression execution and reviewed migration/reference disposition where regeneration is explicitly authorized |
| PLS-003.2 ledger/history | Existing registry plus ordinary-replacement fencing, full ledger codec, permanent tombstone/alias coverage | Execute current registry/authoring/public-consumer tests; old baseline runs do not qualify current edits |
| PLS-003.3 achievements | Existing typed builder plus strict source codec and shared source capture | Execute current semantic, malformed, version, identity and host regressions |
| PLS-003.4 stats/boards/presence | Existing typed builders plus strict source codecs and cross-reference validation | Execute current semantic/source and clean/incremental publication regressions |
| PLS-003.5 portable policy | Existing typed validator, inert eight-key Project-only contribution, explicit host profiles/trusted metadata | Current native tests; actual shared-modal apply/cancel integration and genuine provider qualification where available |
| PLS-003.6 cook | Existing cooker retained; unchanged fingerprint preimage paired codec, synchronous application publisher and production CLI wiring | Execute actual current cook/CLI/public-consumer tests; source implementation is not a published-pair runtime pass |
| PLS-003.7 authoring surface | Same shared draft/renderer extended for exact Project keys; private source publication operations and localized labels prepared | Execute common transaction, publisher, modal/lifecycle and public consumer regressions; verify actual UI states |
| PLS-003.8 integrated qualification | Shared six-source capture, ordinary-open fences, creation, recoverable 0.4.0 adoption, paired publication and shared Settings regression sources | Execute all affected native/consumer scopes and actual routes; final Sonar IDE/secrets/Codacy preflight and matching CI |

On 13 November the CFG owner published [draft PR #3556](https://github.com/horocore/horo-engine/pull/3556),
commit `e7b809b3326f493d6642aa4e5e4600fe4d003b2a`. Inspection of that published commit,
without copying dirty sibling files, confirms inert descriptor validation and label
metadata, descriptor enumeration, and ConfigurationService::Prepare. Its shared
ConfigurationSettingsSession::Reload fixes the draft source and candidate selection
to User, filters editability through AllowsSource(User), and Apply persists through
EditorSettingsService::CommitConfiguration. The eight PLS descriptors explicitly
admit only Project, so this draft alone cannot edit or durably publish PLS policy.
The upstream draft alone therefore needs a common source-aware project transaction
and lifecycle boundary. The resumed PLS integration below supplies that extension
in this worktree while retaining Project-only policy. Both the imported native
contracts and the extension still require executable qualification.

Under the 13 November authorization a missing named grant is not a blocker.
Current physical lock contention defers the bounded native and analysis attempts,
while independent source, review and delivery work continues. Do not treat another
worktree's run as qualification of these inputs. Real SDK/account/authentication,
external storage/OS durability and missing compatible display/device evidence remain
explicitly separate from model passes.

Queued creation revoked by scheduler shutdown is now reconciled from an exact
retained Cancelled job snapshot. The service releases its own mutex before calling
JobSystem::Find, preserves the scheduler's typed cause and rechecks terminal operation
state before committing cancellation. Missing/evicted records are not guessed to be
cancelled, and a durable existing terminal snapshot cannot be overwritten. The new
zero-worker Shutdown(Cancel) regression requires no entropy, files or created event,
and exactly one queued revision notice after repeated queries. It remains unexecuted.

## Resumed shared Project Settings integration (execution pending)

The published CFG implementation `fd8aff61b4800b3a9c3984d5498486ab4601e154`
was imported as forward commit `ceecd77a0` without rewriting the published PLS
branch or copying dirty sibling files. The PLS worktree now extends its existing
ConfigurationSettingsSession with exact source-owned keys, revocable availability,
typed validation and persistence callbacks. Incomplete bindings fail closed.
Prepare retains the one configuration writer reservation across persistence;
cross-field-invalid typed drafts remain editable but cannot be applied.

The actual editor composition registers the eight inert Project-only descriptors,
including validation bounds and English/Turkish label identities. Workspace
composition retains the admitted source snapshot and borrows the same immutable
provider/trust context from ProjectOpenService. A private publication owner
supplies weak operations to the generic session; it contains no second UI draft.
The common descriptor renderer exposes a Project destination. Its Apply affects
only that source, preserving a pending User draft for the ordinary stale-review
path. Cancel discards modal drafts without publishing. Workspace leave revokes
old callbacks before source owners retire.

The shared Application publisher retains the existing asset-source transaction,
exact root lease and all six source fences. It prepares preserving root JSON and
the next immutable source generation before replacement. Postcommit uncertainty
retains the new root and its original receipt and blocks another write; no source
rollback, identity regeneration, provider activation or fallback is introduced.
The cook and policy writer share a private precommit filesystem decorator.

New regression sources exercise generic Project cancel/apply, cross-field repair,
source-key isolation, stale/revoked writers, failed persistence, all-six-source
post-staging drift, cancellation and native postcommit uncertainty. Actual modal
regressions cover cancel and Project-only apply through both locales, narrow and
normal widths, both theme layers and increased font scale. These new C++/GUI tests
are **not executed** yet. Public-header consumers and the GUI bootstrap also need
rebuilding for the additive contracts and virtual-table change.

## Resumed CI and analyzer findings

At pushed `cc6c2aad4`, Android Package Qualification passed; CI and Sonar coverage
were terminal failures. Native/coverage configure logs identify the exact missing
0.4.0 CHANGELOG entry in GenerateBuildInfo. Windows additionally encountered a Lua
download TLS/HTTP 500 failure. The version-notes entry and an actual repository
CMake-to-notes regression were added in `873de155f`; the exact freeze command
passed and its Python suite completed with 11 passed, 1 skipped. No C++ compiler
had run in those failed native jobs, and SonarCloud had no completed coverage/
analysis evidence.

The hosted Codacy check on `873de155f` reported 22 findings (including two buffer
access checks, constant false-return helpers and oversized functions/test files).
Its raw GitHub annotations are retained in ignored
`build/pls-codacy-873-annotations.json`. Source fixes now make bounds explicit,
separate preparation/lease ownership from publication, and organize independent
test workflows around shared fixtures while retaining their assertions. A fresh
Codacy result is required; this source work is not a clean analyzer result.

The actual published target advanced to `139f8a5ed63ca8698bb3852c9e0cb1eff421dc8d`
and was fetched. Published PLS history is retained; no force push or merge was
performed. Full preflight must compare this actual target, and remote integrated
CI evidence must match the delivered head. New source work has not been qualified
against the target's complete native matrix.

## Latest local checks and source review

The first resumed Debug headless CMake configure completed successfully on
`e0ec814ec` plus the seven focused C++ edits later committed as `a71e76794`.
Public-header ownership and dependency-direction checks passed. The bounded
runner verified the canonical Windows lock handle, sampled CPU for 30 seconds
(3.09% mean), observed 31.31% available RAM and 9.60 GiB free disk, and retained
identical fingerprints for all 8,616 source inputs before and after the batch.
The evidence is `build/pls-native-configure-1794608555213823400.json`. This is
configure evidence; no new C++ executable has been built or run yet.

The combined Python command passed **95 tests, with 3 skipped**:

```text
python -m pytest -q --tb=short tests/python/test_local_build_cache.py tests/python/test_configuration_settings_localization.py tests/python/test_navigation_settings_localization.py tests/python/test_release_notes.py tests/python/test_project_migration_generator.py tests/python/test_project_compatibility_generator.py tests/python/test_project_python310_compatibility.py tests/python/test_ci_workflow_grouping.py
```

Hosted Codacy on pushed `e0ec814ec` reported nine findings. Eight concern long
C++ functions or test cases; `a71e76794` separates their responsibilities while
retaining assertions and publication ordering. Those fixes require a fresh
analyzer run. The remaining CHANGELOG duplicate-heading finding concerns the
standard repeated release subsection names required by the release-notes
contract. No lint rule, historical release data or release parser was weakened
to suppress it. Raw annotations are in `build/pls-codacy-e0ec-annotations.json`.

The configured offline doctor is `ready_for_local_check` using the actual
portable Code user profile and installed Sonar extension directory. This only
checks prerequisites, not connected authentication, trust or C++ execution.
The online doctor could not read Codacy cloud configuration (exit 2); cloud
configuration drift remains unverified. Full-scope Sonar IDE, CLI secrets and
Codacy analysis have not started because resource admission was busy; the
admission record is not an analyzer report.

The published target is still `139f8a5e`. A review-only integration tree resolves
four source/test-list conflicts while retaining both branches' test membership
and the bounded, non-shadowing JSON parser. It has not been applied. The local
branch retains published history; its current draft PR remains unqualified.

## Outstanding integrated evidence

The 13 November delivery checkpoint is [draft PR #3559](https://github.com/horocore/horo-engine/pull/3559).
Its initial pushed head is `cc6c2aad4e1aab27a0d6f59b9105eb56d7e34031`.
The lightweight Python command
`python -m pytest -q --tb=short tests/python/test_local_build_cache.py`
completed with **26 passed, 2 skipped** on this checkout. It includes the new
nonblocking contention/retry regression using an isolated fixture lock, not the
canonical build lock. These tests launch no compiler or analyzer and do not
qualify the C++ implementation. Required native and full quality admission attempts
found the canonical physical lock busy and started no children; there is still no
final-input analyzer report.

- Execute the new project-creation and 0.4.0 migration regressions on the actual
  native worktree; prepared source and successful Python generators are not C++ evidence.
- Execute the ordinary-open source/lifecycle and real CLI protocol regressions;
  execute the real production cook route with its exact admission context and writer leases.
- Execute the new creation durability/ownership regressions and native no-replace
  publisher tests. External OS/storage durability evidence remains separate from
  successful requested flush operations.
- GUI/CLI/headless/cook must capture the same root and ledger revisions and use
  the shared validators. Pure codec reuse alone does not prove host wiring.
- Shared Settings apply/cancel, stale revisions, localization, theme layers,
  narrow widths and long text must be exercised against the actual modal.
- Clean and incremental cooks must converge after durable authoring reload,
  preserve the previous published pair on failure, and fence mixed revisions.
- Fork/regeneration must provide complete reviewed reference/mapping disposition,
  rollback and derived/offline invalidation evidence. Unknown references must
  block without changing recoverable source data.

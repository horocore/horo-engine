# UI template instancing and reconciliation design

This work implements HORO-811 / #811 / [RUI-012.2] against ADR-083. The
authorized stack parent is PR3392 at
`96a4f6435bf44dc966896892e0314fcddc310dbc`. This document records the design;
it does not claim that the implementation or validation is complete.

## Ownership and boundaries

Template preparation is a bounded load-time authoring/cook operation. Runtime
frame work sees ordinary cooked elements and existing typed style, localization
and binding contracts. It never resolves source templates or allocates an
expansion graph in a frame callback.

The existing `UiTemplateDependencyGraph` owns an immutable metadata/lock snapshot
and verifies exact revisions, packages and dependency cycles. It does not own
template elements, public arguments, slot fragments or linked document records.
The new instancing domain must own those complete inputs and publish only fully
validated candidates; extending graph traversal alone cannot satisfy the ticket.

The document owner persists ordinary content and linked instance records. A
linked record contains stable instance identity, exact accepted asset/revision,
typed public arguments and document-owned slot content. Its expanded subtree is
a derived projection, not a second persisted authority. Inserted/detached content
has fresh ordinary persisted element IDs and no update relationship.

The existing `UiHotReload::Prepare` / `Commit` boundary owns runtime replacement,
typed state reconciliation and last-good retirement. Template code supplies
complete ordinary cooked input through host composition. It must not add a
parallel runtime publisher or edit the concurrently owned hot-reload/overlay
implementation. Source updates only report UpdateAvailable; explicit rebase is
required before producing a new document revision.

## Typed schema integration

Stable local element, parameter, slot and instance IDs are distinct from document
element IDs and transient handles. Linked expansion hashes a versioned encoding
of document identity, complete stable instance lineage and local identity, then
checks the entire target document for collisions. Mutable labels and declaration
order cannot influence identity or canonical traversal.

Public arguments address exact registered property identities and closed value
categories, including the existing style, localization, asset and binding types.
They cannot use arbitrary property paths or silently encode richer values into a
string. Parameter defaults, requiredness, constraints and nested forwarding are
validated before output. Duplicate target writes are errors. Slot content keeps
its document-owned IDs and must satisfy declared root kinds/cardinality after
composition.

The current parent `UiDocument.h` has only boolean, integer, double, string and
reference property alternatives with string schema keys. Its source serializer
and cooked codec support precisely those alternatives. `UiStyle.h` separately
owns stable property IDs and richer closed style values. There is currently no
durable linked-template record in `UiDocument`. These are integration work items,
not permission to narrow ADR-083 to the existing five property alternatives.

A complete implementation must account for authoring persistence, typed property
schema registration, ordinary cooked projection and dependency manifests together.
Any required extension to concurrently owned generation validation/publication
must be reviewed against the stack before editing that boundary.

## Transaction and reconciliation invariants

Preparation pins immutable catalog, schema, package and document revisions and
captures cancellation. The complete candidate contains accepted instance state,
expanded semantic diff and diagnostics. Commit rechecks exact document revision,
owner lifetime and cancellation; no failure publishes a partial subtree.

Rebase matches stable parameter/slot/local IDs, retains all still-valid values
and reports removed/type-changed/constraint-invalid declarations. A removed
populated slot, missing required value, unresolved migration or incompatible
nested revision blocks commit instead of dropping user data. Undo restores the
prior accepted state and document under a new monotonic owner revision; it does
not reuse stale transient runtime handles.

Shutdown closes admission and cancels/drains preparation before releasing pinned
catalog/document leases. Detached snapshots remain owned and readable; late
candidates cannot publish after shutdown or into a newer owner generation.

## Regression and validation plan

Cover reordered declarations and labels, repeated/nested instances, injected ID
collision, malformed hierarchy/schema/targets, every supported parameter kind,
required/default/constraint policies, nested forwarding, slot ownership and
cardinality, exact accepted revision, update notification without mutation,
preview/commit/undo, incompatible rebase with lossless rollback, cook determinism,
cancellation, reload and shutdown. Public header consumers and ownership registry
must accompany new public contracts.

Validation is admitted in explicit bounded checkpoints. The original 15-path
`7f07649c` slice built all four linked targets (530 compiler / 15 archive-link
edges) and passed all 14 matching tests. Its separate full15 secrets scan was
clean; Codacy was incomplete with 26 new complexity/NLOC findings and four
estimated clone groups. Oversized CMake input and analyzer/version limitations
remain recorded in the report. These receipts do not validate subsequent owner,
aggregate-budget or persistence edits; each new checkpoint freezes the complete
scope and rechecks actual own-cache closure, options and dependency pins.

The 20-path `8e40cd98` transaction/aggregate checkpoint subsequently built all
four targets incrementally (5 compiler / 1 archive-link edges, 3.570 seconds)
and passed all 21 matching tests, including seven owner/budget cases. The complete
20-file secrets scan passed. Codacy remained incomplete: 27 advisory CCN findings,
five NLOC findings and six estimated clone groups, all on newly introduced code.
Its oversized CMake skip and Lizard version difference remain limitations. No
receipt from this checkpoint validates later encoder, budget, fixture or flattening
source edits.

## Early implementation slice and header migration

The draft `HoroUiTemplateInstances` target owns `UiTemplateModel.h`,
`UiTemplateCatalog.h`, `UiTemplateExpansion.h` and `UiTemplateDocumentOwner.h`, with explicit dependencies on
`HoroUiTemplateGraph` and `HoroRuntimeUi`. The graph target retains its metadata
contract and gains no dependency on Runtime UI. Source metadata contains no
second nested-edge list; complete authored nested records, including their slot
uses, derive the resolver edges. Registered exact element/property pairs are the
only conversion authority; ordinary codec keys never act as target identities.

The public ownership registry accompanies the draft target. The generated
`HoroUiTemplateInstancesPublicHeaderConsumer` compiles each new header in its own
translation unit. `HoroUiTemplateInstancesTests` exercises the initial model,
catalog admission, deterministic expansion, default-slot identity/reference
remapping, finite numeric constraints and cumulative payload/composed depth
rejection. The initial additions were validated as the bounded slice above;
later source growth requires fresh validation and does not inherit that result.

Admission uses a stack-only bounded walk before allocating catalog validation
containers or an expansion result. It conservatively charges owned defaults and
target writes, traverses nested slot inputs, and checks complete composed parent
depth rather than nested-call depth alone. Ordinary document-owned slot IDs are
preserved; template-authored defaults receive a separate versioned instance/slot
identity domain. The compiled ceilings remain unchanged.

The document owner now publishes prepared immutable source/catalog/projection
states, rejects stale/foreign/cancelled/retired candidates, retains valid typed
customization and supports bounded monotonic undo. Aggregate admission includes
ordinary plus expanded elements, properties and conservative dependency occurrences
under the original catalog's limits. Zero undo is rejected at catalog admission.
These paths have checkpoint coverage; complete semantic host validation remains
part of the full ticket.

The current batch adds closed version-one durable linked serialization and exact
mixed-revision reopening, typed linked/independent insertion and recursive detach
through one document publication, plus bounded undo/cancellation/shutdown behavior.
Detach requires complete fresh persistent identity allocation for derived elements;
host-authored slot IDs remain stable. Source-authored nested slot fragments use the
instance-scoped default domain to avoid repeated-use collisions. Localized rich
attachments retain their exact property IDs throughout persistence and flattening.

Cook emits the existing concrete ordinary cooked document and canonical rich
payload, with cache identity covering durable source, exact registered schemas,
algorithm version, ordinary/rich bytes and a host-verified dependency-manifest
receipt. Asset/package cook remains responsible for verifying complete locked
style/font/image/message/binding/slot resources and focus/accessibility/layout
admission; the template target does no ambient discovery or runtime activation.
The existing typed runtime replacement API remains unchanged.

The narrow non-installed `HoroFoundationJsonInternal` interface exposes only the
existing Foundation JSON guard directory to the template target. It publishes no
repository-wide source root. Public ownership and both header consumers accompany
the target registration. Current source and new rich/persistence/transaction/cook
regressions require fresh native and provider receipts; old checkpoints do not
validate this batch and do not establish full acceptance.

## Application ownership and capture migration

`HoroEngine::UiTemplateHost` is the headless application composition boundary.
Its public header belongs only to `HoroUiTemplateHost`; it depends on the existing
template, Runtime UI, Assets, PackageSecurity and shared project mutation contracts.
`PackageRestore.h` and publisher-verification contracts belong to
`HoroPackageSecurity`; `HoroPackages` alone does not expose that header or restore
implementation. PackageSecurity supplies its ordinary Packages dependency.
`Horo/Editor/ProjectMutation.h` now belongs to `HoroProjectMutationGuard`, which
compiles the existing coordinator implementation once. `HoroEditorServices`
declares that narrow dependency instead of compiling a second copy. Include
spelling and coordinator behavior are unchanged. Standalone host and guard header
consumers accompany the ownership registry; their new source requires validation.

`UiTemplateInputLease::Capture` acquires the coordinator's actual in-process and
native project lease through bounded Assets capture, package qualification and
final control/graph checks. Missing control files are explicit optional values;
regular-file read failures retain their typed diagnostic. Readable control-byte or
graph-publication replacement is stale. Required publisher evidence must satisfy
the existing restore owner's accepted-outcome policy, including exact archive,
package-manifest and file-manifest identities. Captured owned bytes and package
graphs survive later filesystem or catalog replacement.

`PrepareUndo` prepares a monotonic authority without consuming history and can be
paired with runtime replacement. Every prepared transaction pins any evicted
history so final catalog retirement occurs after candidate release outside commit.
Owner operations returning `Result` check their immutable creation-thread identity
before reading mutable state. Nonthrowing owner borrows, shutdown and destruction
require that thread and terminate on violation. Immutable captured snapshots remain
readable on other threads; moving a `unique_ptr` does not transfer owner-thread
authority. Host creation/replacement borrow rvalue owners until admission, and
retirement capacity is allocated before consuming document ownership.

The runtime artifact decoder admits the closed version/algorithm/length protocol,
verifies the exact supplied manifest and cache commitment, and rejects noncanonical
rich JSON, escaped targets and duplicate bindings. That primitive digest check is
not resource authority: application cook derives manifest evidence from the real
captured sources and existing verified package graph. Manifest and rich dependency
storage are checked cumulatively before candidate copies, under the document's
admitted limits.

These are current implementation contracts and new regression sources, not test
receipts. Complete production runtime composition, layout/focus/accessibility
admission and integrated activation/reload lifecycle proof remain required before
full HORO-811 acceptance. The published 28-test receipt predates this source growth.

## Paired publication regression checkpoint

The application regression fixture now encodes the verified ordinary cook output
into an Assets artifact, loads it through `MemoryAssetProvider`, `AssetLoadService`
and `UiRuntimeAssetLoadService`, and composes complete real element trees using
one retained allocator. It does not manufacture a terminal runtime load result.
The host tests cover paired detach and monotonic undo, explicit source-update
rebase with retained arguments, incompatible parameter removal, document/runtime
cancellation, invalid structural cutoff, foreign/stale/moved candidates, retained
generation backpressure and retry, wrong-thread admission, initial allocation
failure, idempotent shutdown and surviving external read leases. A synchronous
allocation probe checks the successful paired commit for allocation and final
deallocation at its structural safe point.

Paired extraction now reads the exact immutable ordinary/rich projection through
`UiTemplateRuntimeLease::CookedDocument`. `UiTemplateHost::IsCurrent` checks both
the existing runtime publication fence and its paired cook identity. Replacement,
foreign-host use and shutdown reject current extraction while retained historical
runtime and cooked data remain readable. Both Acquire and the fence are checked
alongside commit for owner-thread C++ allocation/free regressions; neither grants
a new source resolver or runtime publisher.

Host, candidate and paired-lease members release their runtime owners before
their corresponding cook/package evidence. Lease copy/move assignment and
candidate move assignment explicitly replace runtime ownership first while the
old evidence is retained; default memberwise assignment would reverse that
requirement. Regression sources exercise paired lease replacement after host
destruction and candidate reservation replacement under the real publisher's
two-candidate capacity bound.

The richer load-time host path now calls the existing binding store, style
resolver, layout engine and accessibility extractor. Localization remains a
host-supplied fixed immutable query, with an explicit selected locale and owned
resolved Unicode/spans; this ticket does not implement a catalog, locale engine
or Editor localization authority. The host validates the exact snapshot/key/locale,
Unicode ranges, output bounds, cancellation and actual tree/style/layout lineage.
Application layout adapters are borrowed only during preparation. Published
paired leases retain copied text, computed styles, layout and accessibility
snapshots, never adapter or registry pointers.

Runtime reconciliation remains owned by `UiHotReload::Prepare`. Its new narrow
`Prepared::Candidate()` const owner-thread borrow lets the host refresh semantic
and focus evidence **after** reconciliation. Complete owned declarations include
hidden nodes, so the refresh does not reconstruct policy from a filtered snapshot.
The bare candidate borrow is never retained. Successful consumption and moves
return null; failed commit and cancellation leave the unconsumed candidate
inspectable but grant no active admission. Existing callers need no migration.

Regression sources cover mixed localized/style/binding composition, actual
resource loading, adapter destruction, malformed/mismatched/cancelled text,
layout failure and deterministic preserved-focus accessibility pairing. They do
not claim shipping game/editor wiring or native renderer/text-shaping behavior.
Native builds and analyzer execution for this expanded source batch remain
pending manager admission; earlier ordinary-only receipts do not validate it.

The follow-up failure review pins the Localization identity/locale across the
entire multi-canvas composition, charges localized semantic substitutions to the
same bounded text arena, and copies action names and relation declarations before
adapter destruction. Authored style tokens are resolved by the existing registry
into its admitted literal inline inputs; the original token commitment remains
in the verified cook. Missing tokens preserve the owning style error.

Rich regression sources now include typed domain-error preservation, token
resolution, cancellation and invalid-cutoff retry, allocation/free observations
at paired publication, preserved-focus accessibility, detach/monotonic undo,
retirement draining and shutdown with retained historical projections. Layout
slots are pinned only by the runtime generation/lease: the host projection copies
layout lineage rather than retaining an extra slot that would prevent retirement.

The coherent quality refactor centralizes wire-envelope parsing and codec error
translation, splits captured package/content verification by responsibility, and
separates document publication/lifecycle definitions from preparation. Inert error
definitions live with their existing declaration owner. These changes introduce
no extra public dependency, rule suppression or alternate publisher. Original
cancelled-analysis findings remain historical evidence, not a clean baseline;
their current disposition requires fresh full-scope analysis after native checks.

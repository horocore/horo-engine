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

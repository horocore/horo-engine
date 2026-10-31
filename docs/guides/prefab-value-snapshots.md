# Prefab value snapshots and preview revisions

`PrefabValueSnapshot::Prepare` invokes the existing canonical value resolver once.
The result pairs its authoritative detached resolution with an immutable, bounded
inspection snapshot. It adds no edit, rebase, persistence or runtime publication
authority. Apply, revert and history still use the existing document transactions.
Canonical resolution happens once before optional capture. Capture-only allocation,
budget, invalid presentation ceilings or revision-proof failures return the valid
resolution with `inspectionFailure` and no snapshot. Cancellation still returns a
typed operation failure. Scene publication preserves a valid authored mutation when
only its optional inspection is unavailable.

The snapshot retains typed source/effective values, registry defaults as inspection
metadata, conflicts, orphans, supported component add/remove intent and exact opaque
component evidence. Opaque properties are not decoded or supplied from defaults.
Removed component properties have `RemovedByInstance` disposition: the retained
source value is evidence, not an effective property. An added component retains its
whole canonical payload through the existing component delta; this API does not
invent property assignments for that payload or add new structural override forms.

The resolver records actual outer-to-inner variant-parent and nested-placement
applications on each object. Value provenance copies that lineage. Its final
composition application selects Variant or Nested origin; a scene-instance
assignment/removal reports Instance while retaining the source lineage. Thus a
variant containing a nested variant retains all three actual boundaries. Asset
equality, display names and scope lengths never guess an origin.

Presentation defaults to 256 entries and 1 MiB per instance, with a shared Scene
budget of 4096 entries and 4 MiB. Fixed immutable source/graph revision metadata is
separately bounded by canonical graph policy. Retained presentation bytes charge
owned targets, component types, strings, lineage, vector carrier slack and component payloads before
copying. Values, value conflicts/orphans, component records/conflicts/orphans and
opaque evidence form a deterministic section-order prefix. Coverage always reports
the full total even after the prefix is exhausted. Zero entries or zero bytes
retains no rows. Cancellation and canonical operation limits remain separate.

Source-only Scene projection also captures inspection. Missing schemas yield
transform values and explicit opaque component evidence. A read-only inspection
failure is retained as `valueInspectionFailure` and preserves the valid source
expansion. Actual authored override failures continue to prevent publication through
the canonical resolver. Inspector presentation is prepared from these immutable
rows; Draw performs no document reads, payload parsing, source I/O or formatting.

`PrefabValueSnapshotRevision` commits the full source resolution, actual canonical
source commitments, schema revision and exact authored delta. An absent delta has
zero digest, distinct from a present empty envelope. Independently captured schema,
delta or source changes make `IsCurrent` false, including changed source bytes with
an unchanged caller-claimed source revision.

The existing preview service asks providers for an optional revision before cache
lookup. Providers without this capability keep their original root-payload cache
behavior. Required revision failure never selects a weaker root-only key. The same
proof joins the cache key and generation input and is recaptured before either a
cached image is delivered or a generated image is cached. Provider exceptions remain
contained by the worker host.

The prefab provider includes its real source/sidecar closure, immutable schema and
delta capture, and explicit owner publication in that proof. Rendering applies
supported deltas through the canonical resolver. Value-capture callbacks must be
safe for worker reads of owned immutable publications and must never read a mutable
document. Asset cards have no scene-instance delta by design. Their host copies
its current schema into the request and compares schema revisions on the owner
lane, cancelling and requeuing both pending and ready cards on a change. Existing
source/settings publication generation fences remain in force. Worker validation
is capture-scoped; final owner publication fencing is required, and no OS-atomic
current-at-display guarantee is claimed.

Migration: `ScenePrefabInstanceProjection.valueProvenance` is replaced by the
immutable `valueSnapshot` lease. Consumers read `Values()` and check `Coverage()`
rather than treating omitted rows as absence. `Inherited` remains a legacy enum
value, while canonical resolutions now emit precise Variant/Nested origins.
`IAssetPreviewProvider` gains a default virtual revision method. Existing provider
subclasses remain source compatible, but their vtable ABI changes: rebuild native
provider modules and SDK consumers together with the host before loading them.
Source compatibility does not permit loading a previously compiled provider ABI.
Source and cooked formats, stable identities and authored delta encodings are
unchanged. No CPU/GPU preview or native visual verification is implied by headless
contract tests.

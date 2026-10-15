# Streaming cook artifact integration

HORO-1521 / #1521 / [WST-004]

## Ownership and compatibility

`HoroWorldStreaming` owns inert canonical `world.index`, `HOROCELL` and the
separately versioned dependency-bundle contracts. `StreamingCellEncoding` accepts
already cooked provider-owned bytes; it never calls a provider or serializes
native objects. `WorldIndexArtifact` encodes and decodes the existing ADR-023
192-byte header, 32-byte layers, 64-byte volumes and 96-byte cells unchanged.
Hard edges cannot be inserted into reserved bytes or silently omitted from a
complete published world. A decoded index is an inert projection, not an
activation or complete package-admission receipt.

The Scene owner retains CoreEcs component semantics. The final headless cook
composition belongs above Scene, WorldStreaming and Assets; none of those owners
acquires a reverse dependency on another owner's private implementation. Assets
retains the durable generation-publication transaction and current pointer.

## Canonical index boundary

The index writer normalizes names to NFC with the existing pinned utf8proc
implementation, privately linked by the format owner. No Unicode library types
enter public headers. The reader requires canonical NFC rather than silently
repairing externally supplied bytes. String and record ceilings precede table
allocations. Unsupported versions, nonpacked offsets, duplicate or unordered keys,
nonzero reserved bytes, negative zero, invalid float values, corrupt hashes and
oversized inputs are rejected before any external publication.

The current descriptor authority uses exact ADR-026 millimeters. Version-one
float meters must round trip to those exact millimeters; coordinates that cannot
be represented are rejected with an explicit unsupported-format result. No
coordinate rounding changes the authored world. The inert index retains all four
ADR-023 coordinate conventions; runtime provider admission remains responsible
for supported conventions and is not selected by this codec.

`manifestHash` is the normative SHA-256 with its embedded field zeroed.
`artifactHash` is separately named SHA-256 over the final complete index bytes.
The dependency sibling must bind the latter and the index schema and partition
identity, so an old sibling cannot be attached to a changed index generation.
Static volumes and layer metadata retain all version-one fields; layer-mask bits
refer to canonical layer ordinals, never numeric layer IDs.

## Caller migration

Existing descriptor, assignment, dependency-plan and cooked-manifest APIs retain
sole semantic authority. Tooling may now turn an already validated cooked
manifest into canonical bytes and decode those bytes for bounded validation.
Existing callers need no migration. Complete-world publication must additionally
validate its matching dependency sibling and actual package coverage before
committing through the Assets publication owner; parsing an index alone does
not satisfy those requirements.

The focused codec target and real external public consumer are registered for
Windows build and test discovery. Codec tests exercise independent fixed-offset
and CRC oracles, negative cell identities, normalized names, static-volume order,
malformed rehashed records, version rejection, exact bounds and cancellation.
Authored tests and partial compile checkpoints are not execution evidence for the
remaining whole-world cook integration.

## Scene provider bytes

`HoroSceneCellPayload` now owns `SceneCellPayloadCodec` and its reviewed version-one
logical format. The existing canonical Scene content-field map is cv-preserving:
const key hashing and writing, and mutable decoding, share exactly one member
order. Existing key bytes are unchanged. The independent field-order tests also
exercise writer/reader reconstruction for every mapped type. The actual decoded
baseline still passes through the existing Scene cook owner with captured schemas
and expected identity; opaque wire bytes cannot approve their own schema or bypass
stale/publication checks. See the owning Scene CoreEcs codec contract for format,
logical allocation budgets and caller migration. Existing semantic-only cook
callers remain source-compatible; byte producers/consumers opt into the new API.

## Incremental cache and durable publication

`IncrementalSceneCellCook::Prepare` is the Scene-owned transaction boundary for
aggregate artifact producers. It invokes the same key and baseline cook algorithm
as `Cook`; the latter now calls Prepare, Reserve and Adopt synchronously. Existing
Cook callers retain their source and successful-result behavior. There is one
cache authority and one retained revision, with no Assets or filesystem dependency
inside Scene.

A prepared receipt owns its detached report and replacement storage and retains
its exact cache incarnation. Its weak reservation blocks competing Cook/Prepare
requests until adoption or destruction. Failed encoding, package validation or
external preparation destroys the receipt without publishing the cache revision.
The caller checks its captured source authority and calls Reserve immediately
before the Assets publication point. Reserve rechecks cancellation, closed owner,
exact cache revision and reservation identity. Successful reservation allocates
nothing; the subsequent Adopt swaps preallocated storage without allocation or
failure. A receipt cannot be retargeted to another cache.

An Assets result reporting committed output with a durability error still requires
Adopt: its current pointer has changed. The caller preserves that publication
knowledge and its error cause; it cannot claim rollback or delete the committed
generation. Owner shutdown before Reserve rejects admission. Shutdown after
Reserve closes future admission and releases retention, but Adopt still records
the committed revision. `Adopt()` returns void; `ReportLease()` retains the prepared
report through an aliasing immutable shared owner without allocating or moving
its vectors. Receipt lifetime
never borrows a destroyed cache object. The cache and receipt remain confined to
their owning thread; this is no permission for concurrent Scene cache access.

## Complete headless composition

`HoroWorldCookIntegration` is a new non-installed explicit tooling composition
above SceneCellPayload, WorldStreaming and Assets. It owns no renderer, editor,
runtime activation, installer or additional serialization authority. Its public
facade is `Horo/WorldCook/WorldCookPublication.h`. Applications link this target
deliberately; existing lower-level callers do not acquire a reverse dependency.

`PublishWorldCook` captures borrowed immutable source and release intent for one
synchronous owner-thread operation. Scene preparation is detached. Its actual
baseline is encoded through the Scene codec, then the existing HOROCELL and index
writers, followed by the independently versioned index-bound dependency sibling.
The ADR-023 index does not encode hard edges; package admission reconstructs the
existing validated manifest with the sibling's independently rederived complete
edges. No synthetic digest or runtime operation fence is created to break that
ordering. Existing assignment, spanning and dependency plans remain authority.

Every planned spatial placement requires one actual cell-local entity binding;
every encoded entity must have one such binding. A nonspatial object instead
requires an exact already cooked provider envelope in the Base chunk, with its
independently captured complete-envelope hash. Providers retain their own payload
semantics. The aggregate verifies complete envelope bytes, target, identity and
package membership rather than inventing a native serializer. Unresolved soft
references remain explicit; hard target and release prerequisites cannot be
omitted. This facade currently produces exactly one required CoreEcs provider per
cell. Other provider compositions continue using their owning APIs; the validator
rejects additional unrequested provider payloads rather than silently adopting
them.

The complete inventory is wrapped using Assets' existing envelope and CacheKeyV1
writer. The envelope-packager source is the exact provider artifact bytes;
metadata is empty, schema versions are one, contribution is
`horo.world.artifact-envelope`, version is one, and host settings/target/type/ID
are explicit inputs. This key describes packaging, not a replacement for Scene's
semantic incremental cache. All generated and external release assets must be
present exactly once before staging. Entries and complete envelopes are sorted
together by AssetId for byte-stable generation manifests.

The mandatory host `beforeCommit` callback validates and reserves the exact
captured source revision through return. It runs before Scene's final cache
reservation. Assets retains the exact native writer lease through pointer commit
and `afterCommit`; the aggregate adopts the preallocated Scene receipt there,
then invokes the existing host callback. `prepareCommit` retains its existing
precommit allocation role. A committed durability error remains a committed
publication value with its original typed cause. Validation and Scene reports are
immutable shared leases prepared before Assets I/O. The aggregate allocates a
publication storage control block before entering Assets and directly initializes
its retained Result from the Assets return prvalue; after commit it creates only
aliasing shared leases. No new aggregate-owned vector/string construction or move
is added after commit. The existing Assets implementation and its own return
boundary retain their independent contract; this change does not claim to repair
or strengthen those lower-level allocation guarantees. Existing by-value Cook
callers retain their original result boundary. No aggregate cleanup removes
committed generations. Cancellation before commit, source replacement, shutdown,
encoding failure, missing content and package failure preserve the previous cache
revision and durable pointer.

`ValidateWorldCookArtifacts` consumes actual detached bytes. It independently
checks index binding, dependency projection, complete package/base inventory,
HOROCELL integrity, captured Scene schemas and semantic content, object placement
and required references. Missing, corrupt and unsupported artifacts become owned
BuildOutput findings paired with their original typed errors. Cancellation and
invalid admission remain typed failures. Diagnostic stores are not mutated.

The real public consumer performs a complete single-cell publication using native
durable files and an exact writer lease. The dedicated regression target covers
actual two-cell publication, clean versus reused bytes, malformed/missing/version
content, final source replacement, cancellation, shutdown, precommit I/O failure
and committed durability failure. A real durable commit callback measures Scene
adoption and immutable report/result lease transport without C++ allocations.
These are authored coverage requirements;
execution evidence must be recorded separately on the final source freeze.

## Codec argument and receipt authority refinement

`EncodeSceneCellPayload` and `DecodeSceneCellPayload` now borrow
`const SceneCellPayloadCodecLimits&` for their synchronous call instead of
copying the limits value. Ordinary calls, including temporary limit values, keep
their behavior. Rebuild callers and update stored function-pointer types to the
const-reference parameter; no parallel value-taking overload remains. The public
Scene payload consumer pins both exact signatures and compiles against the owning
header boundary. The private inspection constructor likewise borrows its scalar
header only until it copies those facts into the returned owner.

Prepared receipts keep `Reserve`, `Adopt` and internal reservation release
non-const. These operations mutate cache admission or committed publication state,
even though an immutable report lease shares the backing allocation. Exactly those
three implementation locations have a documented `cpp:S5817` exception; changing
them to const would admit mutation through a const authority token. Public consumer
assertions reject const invocation of the mutators, while runtime reservation,
receipt destruction, shutdown and zero-allocation adoption tests retain the actual
lifetime contract. `Report` and `ReportLease` remain const observers.

# Terrain collision and navigation producer snapshots

`HoroEngine::TerrainProducerIntegration` owns
`Horo/Terrain/TerrainProducerSnapshot.h`. Hosts link this explicit integration
target; TerrainApi does not acquire WorldStreaming or consumer-native dependencies.
No existing caller, serialized schema, or Assets publication contract is replaced.

At the Terrain owner safe point, the host pins the already selected Assets/Terrain
generation lease, its `TerrainPayloadManifest`, exact cook-issued roots and foliage
definitions. It captures one coherent header containing the runtime incarnation,
all four Terrain revisions, request generation, world, optional complete cell fence,
origin binding, authored transform, capabilities, target and manifest digest. These
are correlation evidence, not PHY/NAV preparation or readiness receipts.

`CaptureTerrainProducerSnapshot` projects only exact manifest members for the
requested collision or navigation role. Same-tile geometry from another dataset,
generation or cook root is rejected. It copies canonical indexed surfaces, hole
exclusions, seam/source provenance and exact authored foliage primitives and integer
placement. Cylinder is never silently replaced with capsule. Consumers retain
responsibility for capability admission, native cooking, body/NavMesh lifetime,
safe points and readiness acknowledgement. No global scheduler or task is installed.

Capture is synchronous bounded load/preparation work, not a frame-hot extraction.
Finite lowered ceilings cover meshes, clusters, every selected instance (including
visual-only instances), vertices, triangles, neutral storage and examined work.
Hash work is counted in at-most-4-KiB chunks; membership scans, copied elements and
conservative sorting work are also charged. Storage charges cover root metadata,
copied vector elements and temporary selection indices, not allocator bookkeeping
or consumer-native allocations. Cancellation and failures return no partial snapshot.
The host retains borrows through capture and joins/cancels any host-scheduled work
before releasing the input generation. `std::bad_alloc` unwinds detached capture.

`TerrainProducerSnapshotOwner` is an optional integration-owned consumer-input cache,
not the Terrain/Assets publication authority and not an aggregate ActivationTicket.
Calls are confined to the integration owner lane after the host validates the existing
generation lease. Replacement requires an exact expected header, a non-wrapping next
request and non-regressing semantic/origin/cell generations within the same host/cell
incarnation. A new incarnation requires a new cache. Cancellation before adoption
preserves the current input. Shutdown closes the cache and drops only its lease.
Readers retain copied memory but must compare complete authoritative current evidence
with `ValidateCurrent` before consumer adoption; retained memory never proves freshness.
Moved-from leases have invalid headers and empty views and cannot be cached.

`Publish` borrows its optional expected header by const reference only for the
synchronous call; it never stores that reference. Temporary headers remain valid
inputs, and candidate snapshots still transfer an owned immutable lease. This
changes the initial by-value signature's ABI: rebuild linked consumers and update
any member-function pointer signature to the const-reference parameter. No caller
must retain the expected header after return, and no publication authority changes.

Focused regression and public consumer targets are
`HoroTerrainProducerSnapshotTests` and
`HoroTerrainProducerSnapshotPublicHeaderConsumer`. Their implementation depends on
the immutable manifest delivered by HORO-1897; they must be validated on the aligned
published stack, not against a duplicated local manifest implementation.

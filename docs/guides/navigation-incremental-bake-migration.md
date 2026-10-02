# Incremental navigation tile baking

NAV-003.4 adds halo-aware dependency keys and immutable tile payloads to
HoroEngine::NavigationApi. Every new public header belongs to that target
or to the narrow HoroEngine::NavigationBakeService application target;
generated public-header consumers compile each header independently.
Navigation targets retain their Foundation-only API dependency direction.
The application service explicitly composes NavigationRuntime, Assets and
Platform and does not select or discover a provider.

Existing isolated BuildTile callers retain a zero border. Incremental callers
use PrepareNavigationBakeTile and pass its exact geometry, selected inputs and
borderSizeCells to the builder. The tile size must be an integral horizontal
voxel count. The sampling halo is the rounded-up agent radius plus three cells,
matching Recast region border treatment. Native portal flags become neutral
boundary edges; they never become invalid local polygon references.

Dependency keys include selected source provenance and actual triangle values,
intersecting modifier identity/mode/area/bounds, referenced area semantics,
profile/filter/surface/grid identity, exact resolved geometry, coordinate policy
and three nonzero host-resolved compatibility digests. Those digests must cover
the provider source/options/compiler/architecture/floating-point identity, every
schema/format and all definition/target/settings/catalog/package-lock/cooker
policies. Do not substitute an unversioned provider display name. Scene and
request generations fence adoption without invalidating unrelated tile content.
Always recompute the entire requested layout; movement, deletion and empty tiles
must replace old footprints rather than overlaying only newly occupied tiles.

The service is one host-thread authority per project/definition/scope/target.
Submit a complete sorted layout, immutable bake snapshot and complete current
source observations. Every source change must call Invalidate or submit its
new coherent capture before that change becomes authoritative. This is the host
freshness contract: workers must not query mutable editor or producer objects.
Requests expose queued operation identities immediately; one active operation
with identical complete input can be joined by another caller. A differing active operation
drains while at most one pending replacement is retained. Call Pump from the
host update loop to start that replacement without waiting. Close admission
before draining the process scheduler; the scheduler and OperationStore must
outlive accepted work.

Each attempt gathers dependencies, reuses last-valid immutable tiles or verified
AssetCookCache envelopes, builds misses through the injected provider, validates
the complete neutral closure and encodes one definition-rooted core.navmesh
artifact. Generated tiles are internal partitions and receive no authoring
AssetIds. Publication takes the OS-held .cook-writer.lock under the canonical
target root, pins and verifies the current generation there, carries unrelated
artifacts forward and stages through the injected durable filesystem.
Replacement entries require the canonical <AssetId>.cooked filename. Carried
entries are written under their own canonical asset-ID filenames in the new
generation, preserving encoded bytes and hashes. This prevents valid older
filename aliases from colliding with the navigation replacement, including on
case-insensitive filesystems. Pinned older generations keep their original paths.
All other writers to that output root must honor this same lock.

The final publication policy runs after staging current.json and immediately
before its atomic replacement. It checks cancellation, the complete captured
source/revision fence and atomically adopts the desired generation. A source
edit, supersession or shutdown ordered before that barrier rejects adoption.
Changes ordered after adoption affect the next attempt; they cannot relabel
the committed attempt as cancelled. Failure preserves the last published lease.
If atomic replacement succeeds but directory durability confirmation fails,
the published generation carries durabilityError and remains a succeeded
operation. The writer verifies the exact current bytes before reporting this
committed outcome; a failure before replacement preserves the prior pointer.
Invalid current authority fails closed; no older generation is silently selected.
Immutable cache writes may survive cancelled attempts without activating them.

HNT1 tile bytes and HNS1 complete-set bytes use explicit little-endian fields,
canonical zero floats, bounded counts and content digests. No native structure,
allocator capacity, operation ID or timing statistic enters portable identity.
The new set payload is decoded with DecodeNavigationCookedTileSet; it does not
replace the existing NavMeshData schema or claim native Detour tile-pack loading.
Regression queries weld shared planar portal vertices from actual cooked
neutral output and pass that topology through the production Detour query API.
Editor/CLI command adapters and native tile streaming remain their own tickets.

# Incremental navigation tile baking

NAV-003.4 adds halo-aware dependency keys and immutable tile payloads to
HoroEngine::NavigationApi. Navigation headers belong to that target, the service
contract belongs to HoroEngine::NavigationBakeService, and Assets owns the narrow
cooked-generation replacement contract. Generated public-header consumers
compile each header independently.
Navigation targets retain their Foundation-only API dependency direction.
The application service explicitly composes NavigationRuntime, Assets and
Platform and does not select or discover a provider.

Existing isolated BuildTile callers retain a zero border. Incremental callers
use PrepareNavigationBakeTile and pass its exact geometry, selected inputs and
borderSizeCells to the builder. The tile size must be an integral horizontal
voxel count. The sampling halo is the rounded-up agent radius plus three cells,
matching Recast region border treatment. Native portal flags become neutral
boundary edges; they never become invalid local polygon references.

NavigationTileBuildLimits::IsValid is the shared native-budget contract used by
Recast and NavigationBakeService::Create. The service now rejects zero,
undersized polygon corner limits and above-hard-limit values with BakeInputInvalid
before scheduler estimates or cache lookup. Previously such host configurations
could be accepted even though the native provider rejected them. Hosts must keep
all six ceilings within the documented ranges; this also prevents unsigned work
and resident-byte estimate overflow. Valid configurations retain their behavior.

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
Submit a complete sorted layout, immutable bake snapshot and complete source
observations. NAV-003.7 requires `NavigationBakeServiceConfig::sourceAuthority`:
compose one `NavigationBakeSourceAuthority` shared with the host source transaction.
Supply `newOperationId` from the host's native secure entropy provider; operation
UUIDs choose only private staging and never grant authored asset identity.
Call `UpdateCurrent` with complete proposed revisions and source observations before
that source change becomes authoritative. If it returns contention, defer the
source mutation; the call never waits on the GUI/render thread. A successful source
transaction then invalidates/submits its new capture. A captured request cannot
validate itself as current. Background publication acquires an exact source lease
under the AssetCook writer and retains it through pointer replacement and live
adoption. Source capture/edit revision fences remain separate from compatibility
and halo dependency fingerprints.
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
Replacement entries require the canonical `<AssetId>.cooked` filename. Carried
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

All publishers use the common `.cook-writer.lock`, including unrelated artifact
writers. Navigation waits only on a background worker, observes cancellation and
current source revisions while waiting, and stops at `writerWaitTimeout`. After
acquisition, AssetCook verifies the then-current inventory before carrying it
forward. A fresh empty output root can bootstrap; a missing current pointer with
existing immutable generations requires explicit repair/recook intent.

AssetCook owns `.cook-staging/<operation-token>` and publishes a complete verified
directory before the selector. Restart recovery runs under the same native lock,
validates the current pointer and every envelope, and removes only recognized
bounded owned staging entries. It neither follows symlinks nor recursively removes
unrecognized paths, picks an orphan, or deletes immutable generations. Retained
reader contents and old generation paths therefore survive replacement. This is
retention, not a garbage collector.

`ResolveNavigationBakePublication` is the reusable host reader for Editor/runtime
composition: it resolves AssetCook's selector, pins its exact manifest, checks the
target/definition/type and standard envelopes, then decodes every portable tile.
It returns owned immutable content, including stable keys, links, provenance and
diagnostics. It does not load source or invoke a builder. Native provider activation
still occurs at the ordinary Scene safe point.

Navigation bake jobs with irreversible publication now supply one fresh
`NavigationBakePublicationReceipt` and exactly one final Publication work item.
Record it inside the writer lease only after pointer replacement is confirmed.
Cancellation before final adoption prevents publication; a raced cancellation
after committed replacement cannot relabel disk truth. All accepted child jobs,
including cancelled queued children, drain before a terminal operation releases
its resources. A drain timeout requests cancellation and retains ownership until
callbacks close; it does not abandon a live writer.

Affected callers are the host `NavigationBakeService` composition, the generic
`AssetCookService` publication host and their filesystem test harnesses. All
generic cook requests now inject shared `publicationFiles` and
`newPublicationOperationId`; an empty registry publishes one verified empty
manifest through the same writer lease. `PublishCookGeneration` and recovery
require `policy.writerLease` to reference the real native writer capability for
the chosen root; `ExclusiveFileLock::ProtectsPath` verifies that capability against
the exact lock path. Null-policy publication refuses to write. This removes the old
unlocked/empty-registry selector bypass while keeping Assets backend neutral.
Existing isolated tile builders and no-publication
`StartNavigationBakeJob` consumers remain compatible. The source-authority header
belongs only to `HoroNavigationBakeService`; runtime/provider targets gain no
application dependency. Public-header consumer coverage includes the new header
and changed NavigationRuntime/Assets contracts. GUI/CLI/MCP command registration
remains NAV-003.10, not an alternate publication implementation.

HNT1 tile bytes and HNS1 complete-set bytes use explicit little-endian fields,
canonical zero floats, bounded counts and content digests. No native structure,
allocator capacity, operation ID or timing statistic enters portable identity.
The new set payload is decoded with DecodeNavigationCookedTileSet; it does not
replace the existing NavMeshData schema or claim native Detour tile-pack loading.
Regression queries weld shared planar portal vertices from actual cooked
neutral output and pass that topology through the production Detour query API.
Editor/CLI command adapters and native tile streaming remain their own tickets.

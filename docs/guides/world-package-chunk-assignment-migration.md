# World package chunk assignment migration

WST-004.10 adds `WorldPackageChunkAssignment.h`, owned only by `HoroWorldStreaming`
in the public-header registry. Its dependencies are the existing public Foundation
and Assets targets; it exposes no installer, archive implementation or native types.
`HoroWorldPackageChunkPublicConsumer` verifies the staged public-header boundary.

Release/runtime composition now constructs an assignment from the exact
`CookedWorldIndexManifest`, validated `AssetChunkPlan`, verified base-manifest digest
and finite storage limits. The descriptor remains the only cell/artifact authority;
Assets remains the only release membership and mount-compatibility authority.
Construction copies inputs and does not mutate a package, mount content or start jobs.

The host allocates a stable assignment owner and non-zero assignment revision and
partition epoch. Changing any manifest, chunk plan or verified base-release input
requires a fresh revision or owner identity; exhaustion closes admission rather than
wrapping. Any availability change similarly advances the independent availability
revision. A complete snapshot explicitly names every chunk as Installed,
Installable, Downloadable, Installing, Unavailable or Failed. Installed is an acknowledgement of
verified and mounted Assets content, not merely downloaded bytes or a registry entry.

Before packaged reads, call `EvaluateWorldCellContent` with current host context.
A normal partial result names every missing required chunk and state. An absent
unrelated optional chunk does not fail another cell. The package-aware
`RequestStreamingCellAssets` overload uses the same evaluation and rejects missing
content before submitting any child. Its `WorldPackageCellAdmission` carrier borrows
the assignment and availability only during the synchronous admission call, and
copies the current host fence. Async child work retains none of those references.
The overload also validates exact candidate artifact/hash
and partition epoch. No download or fallback is triggered implicitly.

Standalone/editor callers keep the existing explicitly composed provider overload.
No existing signature or serialized `world.index`/`.wcell` representation changes.
New packaged compositions use the new overload rather than treating registry presence
as installation evidence. Cancellation and shutdown close new content admission;
already admitted asset requests retain ordinary structured cancellation and joining.
Retained immutable assignments/snapshots are readable after replacement, but current
admission rejects their obsolete revisions. The host owns safe removal of mounted
content and must retire outstanding Asset read leases before unmounting it.

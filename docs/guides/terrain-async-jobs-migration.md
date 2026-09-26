# Terrain Async Jobs Migration

TRF-001.5 introduces `HoroEngine::TerrainRuntime` and
`Horo/Terrain/TerrainAsyncJobs.h`. There is no production TerrainRuntime caller
to convert in this change; TerrainApi's inert metadata registry remains its
own target.

Host composition creates one `TerrainAsyncJobs` owner for an exact live terrain
incarnation and injects the process `JobSystem`, current four-part Terrain
revision, registry publication, finite limits and capability grants. Submit
immutable cook, load or edit-preview preparation with an owned candidate or
lease, parent cancellation and optional operation/configuration correlation
through `SubmitCook`, `SubmitLoad` or `SubmitEditPreview` respectively.
Worker callbacks return typed `Result<void>` and acknowledge cooperative abort
with `JobCancelled()`; ordinary typed failures remain failures.

At the Terrain owner safe point, `Advance` checks the exact fence and runs only
the successful current candidate's atomic publication callback. That callback
receives the cancellation token for its final commit check. A replacement calls
`ReplaceFence` before new submissions. It cancels old candidates, which can no
longer publish even if their workers finish late. Replacement rejects revision
rollback within a live incarnation; capability grant changes must carry a new
capability revision. `BeginShutdown` closes admission
and publication, and `IsDrained` reports worker completion without an owner-thread
wait. Keep provider, candidate and consumer leases until their own retirement
acknowledgements. `Forget` releases a completed record after its Foundation job
has terminated.

The new contract contains Horo typed identities and errors only. Backend-native
handles, global schedulers, implicit capability fallback and worker-side live
Terrain mutation are outside it.

Concrete tile cooking, cache/residency loading and authored preview pipelines
are later TRF-002/TRF-006 producer stages. This coordinator executes their
owned callbacks and gates publication; it does not implement those algorithms.

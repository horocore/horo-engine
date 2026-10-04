# Runtime Scene Cell Payload Integration

HORO-1569 adds `HoroEngine::SceneCellPayload`, which owns
`Horo/Runtime/Scene/RuntimeSceneCellPayload.h`. Hosts that cook or consume cell
baselines link this narrow integration target. Existing `RuntimeScene` and
`WorldStreaming` consumers keep their existing independent target boundaries.

Build a complete runtime-typed cell snapshot after offline prefab expansion and
component projection. Supply its exact durable partition/cell/Scene identity,
non-zero source revision, expected cook publication, mandatory count/storage
ceilings and explicit supported gameplay schemas to `CookRuntimeSceneCellPayload`.
Keep external parent references out of standalone cell baselines unless the parent
is included. Treat any rejected required component or hierarchy as a cook failure;
do not drop content to obtain a successful baseline.

At runtime, retain a shared `SceneCellPayloadAuthority` implementing the host's
current publication checks, and call `QueueRuntimeSceneCellPayload` with the
immutable baseline, exact streaming fence and cancellation observer. The authority
lease is retained through pending preparation, so destroying the caller's lease or
payload cannot shorten its lifetime. Inspect `TakeOperationError` after
`CommitDeferredLifecycleChanges` for deferred stale/cancellation or Scene asset
failures. Existing active state survives failed publication. The host still owns
streaming reservations, provider publication and committed cell eviction.

`RuntimeSceneService::QueuePreparationWithPublicationCheck` adds an optional owned
`ScenePublicationCheck`. The original `QueuePreparation` signature is preserved; existing calls require no edits. The predicate is pure and
rechecked at admission and commit. It cannot perform provider work or publish state;
Scene activation participants remain responsible for prepared resource publication.
No existing serialized schema, persistent ID, or ADR-023 bytes change. This seam
supports one cell Scene domain and does not claim aggregate multi-cell merging.

Regression evidence lives in `RuntimeSceneCellPayloadTests` and the standalone
`SceneCellPayloadPublicHeaderConsumer`, covering source ownership, stable hierarchy,
strict gameplay schemas, bounded/cancelled cook, deferred Scene consumption,
replacement, late cancellation/stale attempts, unload and shutdown.

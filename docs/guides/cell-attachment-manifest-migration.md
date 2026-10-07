# Cell attachment manifest migration

WST-004.7 replaces caller-supplied attachment readiness facts with exact immutable
references and feature-owned prepared Scene candidates. Existing CoreEcs-only callers
keep their existing Scene baseline path; streamed cells containing the five supported
feature TOC rows must construct complete `CellAttachmentManifest` membership.

1. At the validated cell-artifact handoff, supply every feature artifact's stable
   AssetId, semantic subresource, immutable revision, matching TOC schema, exact bytes
   and digest. Required rows remain required; optional promotion is explicit.
2. Resolve bytes through Assets and retain immutable `AssetPayloadLease` values.
   Compose exact feature identities/revisions and factories before Scene startup.
   Factories receive the reference plus verified bytes and must fully prepare their
   own resources. Return native `SceneActivationCandidate` ownership, never a boolean.
3. Register `SceneCellAttachmentParticipant` through `AddActivationParticipant`.
   Queue the cell's existing source-free Scene payload with its authoritative current
   attempt check. Scene validates every candidate and publishes only at
   `CommitDeferredLifecycleChanges`.
4. Advance complete manifest revision on replacement and close the participant on
   cancellation or mounted-attempt invalidation. Invalid replacement preserves the
   prior owner. Pending old candidates cannot publish; active native cleanup remains
   attached to Scene unload/replacement/shutdown.
5. Read `ActiveStatus` for optional capability availability and exact failure causes.
   Optional missing data cannot silently become ready. Late optional publication
   requires its own admitted attachment transaction.

Header ownership remains narrow: `CellAttachmentManifest.h` belongs to
`HoroWorldStreaming`; `SceneCellAttachments.h` belongs to `HoroSceneCellPayload`;
`PhysicsCellAttachments.h` belongs to `HoroPhysicsSceneIntegration`. The Physics
integration adapter now explicitly depends on SceneCellPayload, which already depends
on RuntimeScene and WorldStreaming. Physics kernels and native backends gain no reverse
streaming dependency. Consumers must rebuild; no public signature or wire layout is
removed. Public consumer compilation covers both newly exposed adapter headers.

The concrete Physics bridge verifies/acquires real cooked shape resources and calls
existing Physics Scene preparation. Current native cooked-collider realization gaps
remain typed capability failures; this change does not recook or claim native readiness
for unsupported data. Terrain, Foliage, Navigation and Audio own their schemas and
native preparation behind the shared factory contract. A required provider absent from
host composition is rejected before any factory starts work.

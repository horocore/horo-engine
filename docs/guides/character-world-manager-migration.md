# Character World Manager Migration

## Purpose

`Horo/Physics/CharacterWorld.h` replaces ad hoc ownership of inert Character
controller descriptors with one bounded manager per active scene generation. This
is a new runtime contract; there is no legacy controller owner to preserve in
parallel.

## Host migration

During aggregate scene candidate preparation:

1. Capture and validate one immutable `CharacterWorldSettings` snapshot.
2. Capture collision-filter and local-origin generations from the application-owned
   `PhysicsSceneActivationAuthority`; do not synthesize generation literals.
3. Obtain the paired `PhysicsWorldId` from the owning `PhysicsRuntime`. Its identity
   stream survives participant recreation and consumes failed preparation attempts.
4. Supply the exact scene, paired world, collision-filter, and local-origin generations
   in a `CharacterWorldPreparationDescriptor`. The manager issues a never-reused
   process-local `CharacterWorldId`; callers cannot select or recycle it.
5. Call `CharacterWorld::Prepare`. This allocates the complete controller slot table
   and returns an unpublished candidate with its completed owner descriptor.
6. Create candidate controllers with descriptors bound to the same three owner
   generations. A failure leaves existing slots and generations unchanged.
7. Fully finalize the detached Physics and Character worlds, then revalidate the
   captured authority evidence immediately before one no-fail aggregate publication.

On unload or failed aggregate publication, call `Shutdown` before retiring the
paired Physics world. Shutdown is idempotent and drains every owned controller
record. Destruction remains a final safety net.

## Handle migration

Store `CharacterControllerHandle` only as process-local runtime binding state.
Resolve persistent authored identity again after reload or world replacement.
Destroying and reusing a slot increments its generation, so all previous handles
remain stale. A slot at the generation ceiling is permanently retired; replace the
Character world when all slots report `CharacterErrors::GenerationExhausted`.

`ControllerDescriptor` returns an owned copy rather than a pointer into slot
storage. Controller creation and destruction are admitted only on the preparation
thread while the world remains unpublished. Active-world changes are rejected
until the tick-addressed safe-point command ingestion introduced by CHR-001.4 is
available.

Production hosts link `HoroEngine::PhysicsSceneIntegration` and inject its
`PhysicsSceneActivationParticipant` into `RuntimeSceneService`. The aggregate service prepares detached paired Physics and
Character worlds, preserves the old bundle when participant preparation or final
evidence validation fails, and shuts Character down before Physics during
replacement, unload, and host shutdown.

## Physical surface consumption

`CharacterControllerContracts.h` remains owned by `HoroEngine::Physics`; no Audio,
VFX or native dependency is added. Existing sweep adapters may omit the additive
`subshape` field for primitive geometry. Compound/mesh adapters must copy the exact
Physics authored child ID. The controller copies this provenance to contacts and
ground results; contacts from different children no longer collapse together.

Results now distinguish an adapter-supplied material (`Query`) from an absent
binding resolved by the descriptor (`DescriptorFallback`). Custom movement result
producers must populate the provenance and selected `groundPoint`/`groundSubshape`
consistently. Fallback records must equal the captured descriptor's physical asset,
generation and slot; present malformed values are rejected rather than defaulted.
Airborne producers must clear ground point/child and reset provenance to `Query`.

Post-commit adapters can call `BuildCharacterGroundSurfaceFact(snapshot, descriptor)`
for bounded copied support and exact tick/sequence/state/publication correlation.
An airborne snapshot yields an absent fact. Missing/deleted downstream mappings
suppress presentation; lookup cannot alter movement or reinterpret copied old
material generations after reload or shutdown. Physical references are not
semantic surface IDs. The ADR-181 semantic producer/catalog remains a separate
unimplemented prerequisite; do not infer surfaces from material names or media.

# Structural graph cook migration

DFR-002.6 adds `StructuralGraphCook.h` as a public header owned by
`HoroDestructionCook`. Consumers link that target and use its staged header.

For each accepted `ChunkMeshArtifact`, capture its exact `content` and
`integrityDigest` with the graph owner's current revision and a nonzero structural
policy revision. Supply one `StructuralChunkInput` per mesh chunk in stable-ID
order. Contact indices refer to
that table and must be unique, lower-before-higher and lexicographically sorted.
The contact weight is an explicit positive finite source value; changing its
derivation or the anchor/required/parent policy requires a new policy revision.

The cook returns a detached immutable graph. Publish it through
`StructuralGraphCookOwner::Accept` with the captured owner revision and the current
content, mesh digest and policy revision. Invalidate the owner when any of those
inputs changes; release a failed or cancelled candidate without publication.
Existing mesh and graph snapshots remain readable after replacement or shutdown,
but runtime admission must compare their exact identities and revisions again.

The graph output is a companion semantic product and does not change chunk mesh
bytes, Physics shapes, Render resources or the source import schema. Consumers
that require cooked support must request the `CookedSupport` feature explicitly;
there is no implicit tier substitution.

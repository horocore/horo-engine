# HORO-963 / #963: captured Physics surface semantics

`HoroPhysicsModel` owns `SurfaceMaterial.h`. `SurfaceMaterialId` is a distinct
canonical nonzero UUID, with no implicit conversion from a physical material or
asset identity. The immutable catalogue retains the exact opaque Project identity,
its revision, canonical sorted IDs and bounded display metadata. Project persistence
owns `.horo/surfaces.json`; Physics never discovers or rewrites that file.

`PhysicsWorldSimulationBinding::surfaces` captures the catalogue, host mapping
generation and explicit optional Project default. Worlds retain this immutable
policy through retirement. Its fixed-field fingerprint includes catalogue identity,
mapping generation and default presence/UUID, so default-only policy changes differ.
This is admitted world policy evidence; it does not claim an implemented checkpoint,
replay or network compatibility protocol. Existing seed/determinism encodings are
not silently reinterpreted as a complete scene or surface mapping identity.

Collider schema 2 adds `surfaceBindings`, an array of at most 64 objects containing
unsigned nonzero `slot` and canonical UUID string `surface`. This is an independent
semantic capacity; the legacy physical binding limit remains unchanged. Physical bindings remain
in `materials`. Duplicate, unknown, missing physical slots and catalogue membership
fail before native construction. Schema-1 colliders without semantics remain valid;
they never infer a surface from a name, coefficient, renderer material or physical ID.
Rigid-body and constraint schema versions remain unchanged. The new Project release
adopts legacy projects with an empty catalogue and absent default, without inventing
semantic content or changing the old release/migration bytes.

Whole-Scene and structural plans decode the actual canonical physical material
payload and copy its declared source revision as content-generation evidence. The
semantic mapping is separate. Supported analytic colliders have one used physical
slot; native compounds preserve the exact authored collider contributor using bounded
private native child user data. The native child ordinal is only an index into the
prepared Horo table, never a persistent identity. Query and contact copies retain the
authored collider slot independently of physical material and optional semantic data.
Contact endpoints each retain their own values. No catalogue or consumer lookup occurs
in solver callbacks, and native coefficients are unaffected by semantic selection.

Canonical native cooked mesh/triangle realization remains explicitly unsupported by
the existing scene activation path. This work does not guess triangle provenance or
introduce runtime cooking. Native physical friction/combine coefficient realization is
also a separate existing gap: decoding genuine physical asset evidence here does not
claim that missing physical-response integration is implemented.

A surface evidence value proves historical catalogue association only. `Make` and
`MatchesPhysicsSurfaceEvidence` never authorize live access or extend world lifetime.
Live use requires the enclosing exact body/shape identities and current query-capability
publication revision. Same-world structural replacement allocates new, never-reused
resource handles, retires the old mappings and invalidates query/event publication.
The old copied association remains inspectable, while its old query command is stale.
No mutable latest-catalogue lookup can reinterpret a copied result.

Query ordering includes the new copied collider and semantic evidence. Callback
observations include complete evidence in their canonical comparison; contradictory
semantic values for one endpoint pair reject the joined candidate instead of selecting
callback arrival order. Publication remains atomic and preserves the previous event
block on failure. Bounded loss/overflow receipts retain their existing meanings.

The editor and project CLI capture the real Project catalogue explicitly before
Canonical world preparation. An explicit Null composition supplies no physical or
semantic contact evidence. Source and C++ layout changes require rebuilding affected
Physics, Scene, Character and Gameplay consumers together with the current Gameplay
SDK fingerprint. No native rendering, media/effect binding, checkpoint, cross-platform
or manual editor interaction qualification is implied by source implementation.

# Cooked world dependency sibling

HORO-1521 / #1521 / [WST-004] proposes this separate version-one wire contract for
review together with its writer, reader and package admission. It does not change
ADR-023 `world.index` bytes or imply this addition was ratified before this change.

## Authority and binding

The WorldStreaming owner projects the existing validated spatial assignment,
spanning-object and dependency plans. Those plans remain the semantic authority;
this sibling is an inert captured result, not a scheduler or another topology.
It binds the partition, index schema 1.0, and SHA-256 of the **final complete**
`world.index` artifact. The ADR-023 embedded digest with its own field zeroed is
not interchangeable with this artifact digest. Changed cell revisions, provider
bytes or index metadata therefore invalidate a sibling from an older generation.

The decoder recomputes every derived hard cell edge from captured coverage,
placement and co-load components and compares the result with the encoded rows.
Single-owner coverage cells require their actual owner. Every actual placed cell
of a hard co-load component requires every other placed cell of that component.
Nonspatial objects remain explicit base-content objects; an empty placement does
not authorize dropping their content. Unresolved soft references remain explicit.
Package admission checks complete base content and every required hard target
before durable publication. Parsing either sibling alone is not that admission.

## Version-one envelope

All integers are little endian. Padding and reserved bytes must be zero. The
192-byte header is followed immediately by eight packed tables, without gaps or
trailing bytes. Counts and offsets are unsigned 32-bit values; the file length is
unsigned 64-bit. All ranges use checked prefix offsets in **records**, never bytes.

| Header offset | Bytes | Value |
|---|---|---|
| 0 | 8 | ASCII `HORODEPS` |
| 8 | 2 + 2 | Major 1, minor 0 |
| 12 | 4 | Reserved flags, zero |
| 16 | 16 | Exact partition GUID bytes |
| 32 | 2 + 2 + 4 | Index major 1, minor 0, reserved zero |
| 40 | 32 | SHA-256 of final complete index artifact |
| 72 | 8 × 4 | Table counts in the order below |
| 104 | 8 × 4 | Exact packed byte offsets in the same order |
| 136 | 8 | Complete file length |
| 144 | 32 | SHA-256 of complete sibling with bytes 144–175 zeroed |
| 176 | 16 | Reserved zero |

| Table | Row bytes | Logical fields |
|---|---|---|
| Objects | 64 | Endpoint32; placement32; coverage offset/count32 each; placement offset/count32 each; reserved12 |
| Coverage cells | 16 | Existing canonical 15-byte serialized cell tuple followed by one zero byte |
| Placement cells | 16 | Same cell representation |
| Hard bundles | 8 | Member offset/count32 each; at least two members |
| Bundle members | 32 | Endpoint |
| Soft references | 64 | Source endpoint32 then target endpoint32; kind is implicitly Soft |
| Cell rows | 24 | Cell16; hard-target offset/count32 each |
| Hard targets | 16 | Cell |

An endpoint contains page AssetId16, page-scoped nonzero object identity64 and
positive authored revision64. Objects are ordered by page/object address. Every
coverage and placement slice is in canonical layer/LOD/Z/Y/X cell order with no
duplicates. Hard bundles are ordered by first member; their ordered unique member
sets are disjoint and refer to exact known object revisions. Soft records are
ordered by source address then target address, with no duplicate endpoints or
self edge. A known target must have its exact captured revision. Soft edges within
the same hard component are invalid, matching the source plan contract.

Cell rows match the complete index cell set exactly, including zero-edge cells.
Each hard-target slice is canonical and unique, excludes its own cell, and equals
the independently derived closure. Missing hard targets, reordered or aliased
slices, overlap, count/offset overflow, malformed identities, unsupported versions,
nonzero padding, corrupt hashes and stale index bindings fail before publication.

## Bounds, lifecycle and migration

Caller limits bound artifact bytes, aggregate decoded logical storage, objects,
cells, coverage, placements, members, soft references and hard cell edges. A
separate derivation-step ceiling accounts candidate edge work including duplicates.
Cancellation is observed during capture, derivation, copying and hashing. The
writer returns detached bytes; it performs no registration, I/O or activation.
The downstream cook aggregate validates both siblings and actual package content,
then uses the Assets durable generation transaction. Scene cache adoption follows
the committed publication outcome, including a committed durability error.

Existing index and plan callers keep their APIs and bytes. Complete-world writers
and readers add this sibling and reject a missing or mismatched one; they cannot
treat an index-only decode as complete hard-dependency or package qualification.
Future major/minor changes require an explicit decoder migration and new golden
bytes. No fields may be hidden in ADR-023 reserved data.

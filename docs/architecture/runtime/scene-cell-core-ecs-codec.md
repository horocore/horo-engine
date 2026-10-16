# Scene CoreEcs cell payload codec

HORO-1521 / #1521 / [WST-004]

## Owner and scope

The Scene cell-payload owner defines the versioned logical CoreEcs provider bytes
inside an ADR-023 HOROCELL archive. This contract introduces no live Scene state,
backend selector, native handle, callback, VM reference or editor document format.
The enclosing archive retains provider identity/version, CRC and artifact SHA
responsibility. Existing Scene source/definition types and semantic validation
remain authoritative.

## Version-two representation

The fixed 16-byte header contains ASCII `HOCORECS`, uint16 little-endian major2,
uint16 little-endian minor0, and four zero reserved bytes. Other versions and
nonzero reserved bytes are rejected. The complete body contains, in order:

1. `SceneCellPayloadIdentity`: partition, cell tuple, Scene ID and source revision.
2. The complete ordered runtime-entity vector.
3. The canonical complete asset-dependency vector.
4. Canonical object-ordered portable trigger migration evidence.

The existing Scene-owned `SceneCellContentFields.h` version-two member mapping is
the single definition of member order for all core components and their nested
values. Const hashing/encoding and mutable decoding borrow the same declared
fields. Adding, removing or reordering a field requires a deliberate codec version
and semantic-key migration; native struct order is irrelevant. The independent
field-order regression lists the expected members and also checks the actual
writer/reader round trip for each mapped type.

Every integral scalar occupies eight little-endian bytes. Signed values use
canonical sign extension of their two's-complement value; narrowing outside the
owning type's range is invalid. Boolean values are exactly zero or one. Enums use
their integral value and remain subject to the existing owner's semantic-mode
validation. A float stores its uint32 IEEE bits in the low half of an eight-byte
unsigned scalar, whose upper half must be zero; a double stores uint64 IEEE bits.
These are explicit logical representations, never copies of native objects.

Strings store an unsigned eight-byte byte count followed by exactly those bytes,
without a terminator. Collections store an eight-byte element count followed by
values. Byte/uint8 collections store their explicit bytes directly after that
count. Fixed arrays require their exact declared count. GUID/AssetId values use
these length-prefixed persistent 16-byte sequences with no native GUID swapping.
Optional values store a Boolean presence scalar and the value only when present.
Variants store an unsigned alternative index followed by that known alternative;
unknown indices are invalid. Empty marker alternatives contribute no extra bytes.
Typed numeric/string identities use their existing factory representation. Exact
default sentinels are preserved for disabled fields and still undergo final Scene
semantic validation before a baseline can be exposed.

Opaque gameplay and behavior data is copied as its existing typed fields and
payload bytes, not reinterpreted as C++ layout. Component/behavior schema approval
is deliberately absent from the wire. A host must supply its captured approved
schema snapshots to decode; the same bytes cannot grant their own capability.

## Bounds, identity and publication

The decoder checks the complete encoded byte ceiling before constructing fields,
charges logical vector-element/string storage before allocating, and bounds work
units independently of zero-width marker values. Root entity/dependency counts
also obey their existing semantic ceilings before table allocation. Truncated,
trailing, malformed and unsupported input yields a typed failure with no baseline.
Identity-factory failures retain their owning error information.

The decoded model is passed through `CookRuntimeSceneCellPayload` with the exact
host-captured expected identity, topology, schema capabilities and semantic bounds.
That owner continues to reject stale revisions, missing external parents, invalid
components and unsupported schemas. No decoder activates a cell or changes an
incremental cache, package inventory or durable output pointer. Cancellation is
observed during preparation and before final owner publication. A failed decode
cannot partially alter the source or a live Scene.

Version two adds required semantic trigger binding constraints, not runtime state.
Version-one provider payloads require recook and are rejected; evidence is never
silently discarded. `SceneCellPayloadCodecVersion` is the one provider-version
authority used by the Scene writer/reader and WorldCook envelope validation.
Runtime/filter activation generations and process leases are not serialized.

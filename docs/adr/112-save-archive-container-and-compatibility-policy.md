# ADR-112: Save Archive Container and Compatibility Policy

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Portable `.horosave` framing, canonical logical-state serialization, archive and schema version axes, state/content/publication identities, release compatibility declarations, migration support and forward rejection
- **Issue**: [SAV-002.1](https://github.com/HoroCore/horo-engine/issues/1410)
- **Jira**: [HORO-1410](https://horo-engine.atlassian.net/browse/HORO-1410)
- **Related**: [ADR-003](003-artifact-identity.md), [ADR-008](008-error-model-exception-boundary-and-registry.md), [ADR-057](057-package-manifest-v1-typed-model.md), [ADR-060](060-release-domain-model-and-state-machine.md)
- **Normative documents**: [Save Game And Persistence](../architecture/runtime/save-game-and-persistence.md), [Release Architecture](../architecture/release/release.md), [Distribution And Update](../architecture/release/distribution-and-update.md)

## Context

Runtime save authority and transactional capture/restore are defined, and the save
architecture proposes a single-file envelope. It still uses archive revision and
content identity ambiguously, treats one module version axis as both whole-save and
participant schema, and lets release metadata collapse compatibility to one
`saveFormatVersion`. Those ambiguities cannot become shipped user data.

The same logical state can be encoded with different compression or presentation
metadata. Identical archive bytes can be replicated without being a new logical slot
publication. A new durable write of unchanged state must still be distinguishable
from the publication it replaced. One digest or timestamp cannot safely answer all
three questions.

Portable persistence also cannot inherit C++ object layout, host endianness, locale,
unordered-container iteration or compiler floating-point behavior. Readers need an
explicit answer for direct readability, migration and newer unsupported input before
allocating or interpreting participant payloads.

## Decision

### 1. `.horosave` is an explicitly encoded single-file container

HoroSave v1 uses the 32-byte preamble, bounded deterministic entry table, stored
entry payload and integrity trailer defined by the normative save architecture. All
multibyte envelope and entry-table integers are explicitly encoded little-endian.
Implementations read and write fields individually; native structs, bitfields,
`sizeof(T)`, padding, pointer values, native enum width and in-memory container layout
are forbidden format inputs.

The payload contains deterministic metadata plus independently coded data entries.
Entry identity, order, lengths, codec and required/optional status are explicit.
Unknown flags, duplicate entries, gaps, overlaps, trailing bytes, invalid lengths and
unsupported required codecs fail before decoding. Filesystem paths never occur in the
entry table.

JSON metadata is a deterministic bounded interchange surface, not the canonical
logical-state representation. It uses UTF-8, sorted keys, duplicate-key rejection,
locale-independent numbers and schema-defined required fields. Data entries use
participant codecs registered by stable typed identity.

### 2. Canonical logical state has a storage-independent byte form

Every required save participant produces a canonical logical byte stream before any
compression, encryption, entry packing, signature or publication metadata is added.
The whole-save canonical stream is composed in ascending `StableTypeId` byte order
and includes stable project/world/base-scene and base-dataset identities, the
`SaveSchemaVersion`, each participant ID and `ParticipantSchemaVersion`, and each
participant's canonical state.

Canonical codecs obey these rules:

- fields have stable numeric IDs and schema-defined presence/default rules;
- integers have explicit width and signed representation; booleans are exactly 0 or
  1; enums serialize their declared stable value;
- floating values use declared IEEE 754 binary32/binary64 encoding, normalize
  negative zero to positive zero and reject non-finite values unless a participant
  schema defines one exact canonical non-finite representation;
- strings are length-delimited validated UTF-8 scalar sequences with no locale,
  platform or implicit normalization; semantic normalization must occur before the
  codec under a versioned participant rule;
- maps/sets sort by canonical encoded key bytes and reject duplicate canonical keys;
  sequences retain schema-defined semantic order; and
- padding, addresses, runtime handles, unordered iteration, timestamps, display
  names, compression choices and publication identities never enter the stream.

Changing a canonical rule is a save or participant schema change. A reader never
reconstructs canonical state by reserializing untrusted JSON with its local library.

### 3. State, content and publication use three typed identities

```cpp
struct CanonicalStateHash { Sha256 value; };
struct ArchiveContentHash { Sha256 value; };
struct SlotGenerationId { Opaque128 value; };
```

`CanonicalStateHash` is the domain-separated SHA-256 of the canonical whole-save
stream. It answers whether two snapshots represent the same logical state under the
same semantic dependencies and schemas. It excludes slot ID/generation, capture and
wall-clock times, display name, play duration, entry layout, codec/compression,
signature and other publication metadata. It is useful for equivalence and
determinism evidence, not archive integrity or slot concurrency.

`ArchiveContentHash` is the domain-separated SHA-256 of the final v1 preamble and
payload bytes, excluding the integrity trailer that carries it. It is computed only
after metadata, entry layout, codecs and `SlotGenerationId` are frozen. It answers
which immutable archive content was verified or transferred. Any covered byte change
requires a new value and signature. It does not mean logical-state equality.

`SlotGenerationId` is a nonzero opaque 128-bit identity allocated by the storage
authority for one intended durable publication to one logical slot. Publication
records the expected parent generation for compare-and-swap/conflict checks. An ID
is never derived from time, path, counter display, either hash or content. Failed
pre-publication attempts may consume an ID but never publish it; successful overwrite,
migration or explicit import gets a new ID even when logical state is unchanged.
Replication, download and recovery copies of the same logical publication retain its
generation. Copying into a new logical slot requires a new finalized archive.

The archive manifest stores `CanonicalStateHash`; the header stores the logical slot
and `SlotGenerationId`; the trailer stores `ArchiveContentHash`. Catalog, operation,
cloud and restore APIs carry the typed values without calling any of them a generic
archive revision. Timestamps are presentation/provenance metadata only.

### 4. Four version axes remain independent

| Version | Authority |
|---|---|
| `ArchiveFormatVersion` | Horo-owned envelope, entry table and trailer codec |
| `SaveSchemaVersion` | Whole-save canonical root, required roles and cross-participant composition |
| `ParticipantSchemaVersion` | One `StableTypeId` participant's canonical payload and migration chain |
| `ProductSaveCompatibilityVersion` | Product release's declared compatibility matrix and policy epoch |

Base asset/dataset revisions remain dependency identities rather than schema
versions. Engine version, project build ID and product semantic version are bounded
diagnostics/provenance; none selects a codec or implies compatibility.

Archive metadata declares its archive/save/participant data versions and the
producing product compatibility version. A release declares its product compatibility
version plus exact write versions, inclusive direct-readable and migratable ranges,
required participant ranges, migration policy and forward policy. The product version
is incremented whenever that matrix or required participant set changes incompatibly;
it does not replace any codec version or enter `CanonicalStateHash`.

### 5. Compatibility preflight is explicit and fail-closed

Readers preflight in this order: framing/limits, `ArchiveFormatVersion`, outer
integrity and trusted signature policy, metadata/save schema, required participant
set and versions, dependency identities, then decoded participant hashes. A version
is directly readable only when every required axis is in the release's declared
direct range. A version is migratable only when every required axis is in a declared
migration-source range and the sealed registry contains one complete deterministic
bounded path to a directly readable version.

Any newer archive, save schema, required participant schema or product compatibility
version outside the declared ranges is rejected with a typed unsupported-newer error.
Readers do not best-effort parse forward data. Unknown optional participants may be
skipped only when the manifest marks them optional and no required participant
declares a dependency on them. Missing/unknown required participants always reject.
Optional unknown data is conservatively preservable for migration and save copies;
only an explicit stable owner ID in the sealed release's droppable list permits loss.
Preservation carries exact stored chunk bytes and their codec/length/alignment/digest
metadata. A repacked candidate may change offsets but must prove the protected
records survived before publication. Unknown required data reports its stable owner;
unknown required feature flags report their bit mask before restore activation.
An unsupported newer schema of a known optional owner follows the same opaque
preservation policy unless a required participant depends on it.

Migration verifies the source first, operates on detached staging, preserves the
source unless an explicit replacement policy was captured, and emits current writer
versions with a new `SlotGenerationId`, `ArchiveContentHash` and signature. It may
preserve `CanonicalStateHash` only when the canonical logical state and semantic
dependency identities are unchanged.

### 6. Release manifests publish a complete save policy

The release manifest contains a versioned `saveCompatibility` object with:

- `productSaveCompatibilityVersion` and the exact archive/save versions written;
- inclusive direct-readable and migration-source archive/save ranges;
- required participant IDs with direct and migration-source schema ranges;
- the migration policy (`AutomaticReversible`, `AutomaticOneWay`,
  `UserConfirmed` or `Unsupported`) and a migration-catalog identity;
- `minimumSupportedProductSaveCompatibilityVersion`; and
- `forwardReadPolicy`, which is `Reject` for v1.

Preflight validates that current writer versions are directly readable, all declared
migration ranges have complete registered paths, and required participant declarations
match the sealed product composition. The old flat `saveFormatVersion` and
`minimumReadableSaveFormatVersion` fields are legacy input only and cannot be emitted
by new manifests because they cannot represent the four independent axes.

### 7. Stable releases have a minimum migration and deprecation horizon

Before product 1.0, every shipped preview declares its exact support window and keeps
fixtures for every version it claims; no compatibility is inferred across previews.
From product 1.0 onward, each stable release must migrate saves from at least the
previous two stable minor release lines and for at least 12 months after each source
line's last release, whichever is longer. Product policy may extend this window.

Patch releases may not narrow direct-read or migration ranges, remove a required
migration path or increment the product compatibility version. Dropping support in a
later minor/major release requires deprecation in two preceding stable minor release
lines, release notes and a final supported bridge release capable of writing the next
current form. A major release may reject older input only after that horizon; rollback
must retain the untouched source archive. Security policy may block a vulnerable
decoder sooner, but the release must declare the exception and present a typed
security rejection rather than silently treating the save as corrupt.

Fixtures cover the oldest supported, every migration boundary, current writer and
one-newer rejected forms for each version axis. Golden canonical-state vectors run on
all supported platforms/compilers; archive fixtures also cover byte-exact framing,
endianness, hashes, signatures and generation conflict behavior.

## Consequences

### Positive

- Compiler ABI, host endianness and unordered iteration cannot define save format.
- Logical equivalence, immutable archive integrity and slot concurrency are no longer
  conflated.
- Releases can truthfully preflight direct read, migration and forward rejection.
- Compatibility removal has a measurable horizon and fixture obligation.

### Costs

- Canonical codecs and migration registries need golden cross-platform fixtures.
- Release composition must validate participant ranges and migration coverage.
- Every durable publication must finalize archive metadata before hashing/signing;
  mutable catalog labels/categories remain outside the archive transaction.

## Rejected Alternatives

### Serialize C++ structs directly and version the build

Rejected because padding, endianness, enum width and compiler ABI are not portable
or stable format contracts.

### Use one save format version for container and all participants

Rejected because independent participant evolution would force unrelated migrations
and still would not describe product compatibility.

### Use one digest or timestamp as revision, content and state identity

Rejected because equivalent state may have different archive bytes, while repeated
publication of equivalent state still needs a distinct conflict generation. Clocks
also do not establish causality.

### Parse newer versions best-effort

Rejected because unknown required semantics can produce plausible but invalid state;
forward incompatibility must fail before live-runtime mutation.

# Animation Graph Source Contract

ANI-002.1 owns the durable graph authoring schema, explicit migration, detached
compiler and instance access/allocation plan. Assets owns the sidecar `AssetId`,
project paths, byte acquisition, publication and cache authority. Animation owns
node semantics, stable local IDs, typed pins, dependency roles and source maps.

`HoroEngine::AnimationGraphSourceCodec` depends on `HoroEngine::AnimationApi` and
privately uses the existing pinned JSON library. No JSON/native type crosses its
public header. The codec performs no I/O, service registration, ambient lookup or
publication. These are load/tool-boundary operations and may allocate bounded
storage; immutable program/occurrence access is allocation-free.

## Envelope And Canonical Bytes

Canonical output is UTF-8, has no BOM, insignificant whitespace or escaped ASCII
identifiers, and ends in exactly one newline. The top-level field order is:

```json
{"schemaVersion":2,"assetType":"core.animation.graph","contractVersion":{"major":1,"minor":0,"patch":0},"dependencies":[],"payload":{}}
```

The version-1 Animation graph semantic contract is exact `1.0.0`. The independent
source schema version is `2`; any unknown semantic or source version fails.
The graph's own identity is injected from the sidecar by the decoder and is never
repeated in payload bytes. Runtime generations, pointers, registry revisions,
source paths, native handles and ABI-dependent object layouts are not persisted.

`dependencies` contains exactly one `core.animation.skeleton` dependency and all
unique `core.animation.clip` references. Each record has fields `assetType`, then
`assetId`; canonical order is asset type then canonical asset identity. The
skeleton value and clip references in payload must match that typed dependency
set exactly. Missing, extra, duplicate, mistyped or self-role-conflicting records
fail rather than being inferred from a project scan. Source field/collection order
may vary; decoding and re-encoding produces canonical order. Asset IDs use exact
lowercase hyphenated UUID text. Local IDs use unsigned non-zero 64-bit JSON
integers, never float, signed values, text or vector indexes.

`payload` fields are `skeleton`, `entry`, `parameters`, `definitions`. Parameters
are sorted by their stable asset-local ID and contain `id`, `name`, `type`,
`default`. Names are non-empty ASCII identifiers of at most 64 bytes; the first
character is an ASCII letter or underscore. Names remain advisory binding keys.
Types are `float`, `boolean`, `integer`, `trigger`. Defaults are finite binary32,
JSON boolean, signed 32-bit integer, or false respectively. Numerical input for
float defaults is rounded to binary32; overflow/non-finite values fail. Serialization
normalizes signed floating zero and emits the exact stored binary32 value.

Definitions are sorted by stable ID and contain `id`, `inputs`, `nodes`,
`connections`. Definition inputs are sorted by ID and contain `id`, `type`.
Nodes are sorted by stable ID and contain `id`, `kind`, `reference`, `pins`.
Kinds/references are:

| Kind | Reference |
|---|---|
| `clip` | Canonical clip `AssetId` |
| `blend` | JSON null |
| `parameter` | Stable parameter ID |
| `output` | JSON null |
| `input` | Stable definition-interface input ID |
| `call` | Stable asset-local definition ID |

Pins are sorted by stable ID and contain `id`, `role`, `type`, `output`,
`interface`. Roles are `value`, `firstPose`, `secondPose`, `weight`, `interface`,
`result`; direction is a JSON boolean; `interface` is a stable interface ID only
for an interface role and otherwise null. Pin types additionally admit `pose`.
The compiler owns the exact shape of every node and rejects extra/missing or
contradictory roles. It never interprets an unknown node as an opaque extension.

Connections are sorted by source endpoint, destination endpoint and type. Their
fields are `source`, `destination`, `type`; each endpoint has `node`, `pin`.
Every input has exactly one same-type source; no coercion or implicit defaults
repair a malformed edge. Node cycles, self/indirect call recursion, unused cyclic
definitions, unbound interfaces and unreachable nodes all fail.

## Migration And Hostile Input

Schema 1 has the same envelope, typed nodes/pins and stable IDs, but connection
objects omit `type`. `DeserializeAnimationGraphSource` admits it only with the
explicit `MigrateVersion1` policy. Migration derives each type from its source pin
and validates the complete destination/node/call/parameter contract. It neither
repairs IDs nor changes persistent identity. The result is a detached canonical
schema-2 candidate. `SerializeAnimationGraphSource` requires schema 2; callers
must explicitly migrate older in-memory data first. Current-schema migration is
an idempotent validated canonical copy.

Parsing rejects duplicate keys at any depth, unknown/missing fields, malformed
UTF-8, BOM, embedded NUL, trailing non-whitespace data, invalid enums, non-canonical
identities, count/number overflow and unsupported versions. Before parsing, complete
input is limited to 4 MiB. Parsing additionally caps nesting at 16, callback values
at 1000000, decoded strings at 64 bytes, and all typed graph counts at the captured
compiler limits. Typed arrays are budgeted before their storage is reserved.
Serialization checks the same JSON depth/value policy and encoded-byte ceiling.
Policies may only lower ceilings; bounded parser/serialization work may allocate
within the independent implementation byte/count ceilings before a lower requested
output-byte budget is found insufficient.

Admission and Foundation cancellation are checked at bounded checkpoints and
before returning. Shutdown, cancellation, malformed input and migration failure
return a stable `animation.graph.*` error with no partial candidate or callback.
The caller keeps the last good source/program and commits a successful detached
candidate only after rechecking its authoritative dependency snapshot.

## Compiled Binding, Occurrences And Memory

The compiler requires validated immutable skeleton/clip snapshots captured and
pinned by their caller, together with an exact skeleton publication generation.
Every clip descriptor must target that skeleton identity and generation, and the
supplied dependency set must be exact. The immutable result copies compatibility
values, never borrowed views; retaining/retiring runtime dependency generations is
the future instance/publication owner's responsibility.

Definitions compile in stable-ID ready-node topological order. The entry occurrence
map distinguishes repeated calls by stable root-to-leaf call-node paths and maps
every operand to a producer occurrence and typed working slot. Calls appear at completion, after all occurrences in their callee. Input, Output and Call are aliases,
not working-storage writes: `outputProducer` identifies the actual Clip, Blend or
Parameter writer and every operand points directly to that writer. Interface
bindings retain both the slot and this producer across every nested call, including
passthrough definitions; no alias or call-entry marker pretends to produce a pose. Clip occurrences
have distinct player indexes and a dense immutable clip-binding index. Parameter
reads address the canonical parameter block; graph nodes never write it. Input,
Output and Call alias exact interface/result slots; Clip/Blend own pose slots.

The plan exposes exact counts for five working storage types, joint transforms,
clip players, parameters and call depth. Expanded instruction work, call-path entries
and each storage category have hard and caller-lowerable budgets. Budget failure
rejects the entire detached program before an instance can allocate it. This
compiler provides the access/allocation plan; ANI-002.2/3/4 still own evaluation,
parameter/trigger policy and state-machine execution.

## Assets Cooking Prerequisite

These authoring bytes and immutable in-memory tables do **not** claim a packaged
cooked graph artifact or a runtime asset publication. Graphs depend on skeleton
and clip content. Assets `CacheKeyV1` cannot cover dependency contents; graph cook
admission must fail explicitly until the dependency-aware Assets key/snapshot
extension and a versioned graph cooker/decoder exist. It must not reuse an
under-keyed cache, synthesize compatibility, or encode C++ tables as raw bytes.

The graph codec's typed dependency projection is the source of truth that a future
host contribution must report through Assets' dependency sink. Generation-safe
runtime publication/reconciliation remains a separate owner. Failure preserves
last-good publications instead of silently falling back or partially replacing
source/program state.

See [Animation Architecture](./animation-architecture.md) and
[Animation Asset Pipeline Contract](./animation-asset-pipeline-contract.md).

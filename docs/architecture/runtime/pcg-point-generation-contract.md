# Deterministic point generation contract

HORO-2015 / #2061 [PCG-3.3] extends the CPU node library described by
[Procedural Generation Architecture](procedural-generation-architecture.md).
The catalog is the sole authority for type identity, settings schema, capabilities,
numeric policy and complete admission cost. These kernels read one immutable captured
spatial root and produce point values; they acquire no scene or provider authority.

## Settings and algorithms

SnapshotGrid keeps its existing node type, version and exact eight-byte network-order
`SpatialElementId` payload. Canonical order is x fastest, then y, then z. Its coordinates
use double intermediates before the captured Float32 conversion, including large
origin/spacing cancellation. Existing callers with no identity attributes remain valid.

The three added types use schema version 1, encoded as a four-byte network-order
version followed by the eight-byte source identity and fixed typed fields:

| Type | Fields after source | Payload size | Point count |
| --- | --- | --- | --- |
| Grid jitter | Binary64 fraction | 20 bytes | Exact grid dimension product |
| Box stratified | Three UInt32 stratum counts | 24 bytes | Exact stratum product |
| Box Poisson style | UInt32 count, UInt32 attempts, Binary64 separation | 28 bytes | Exact requested count |

Jitter fraction is finite in [0,1], with signed zero canonicalized to positive zero.
Each grid point defines a half-spacing window multiplied by the fraction. Sampling
uses the intersection of that window with the complete captured bounds, rather than
clamping an out-of-domain generated point. Fraction zero retains the grid coordinate.
Stratified sampling uses one stable row-major stratum identity and one independent
sample per positive-axis stratum. Float conversion may place a point on a shared
stratum boundary; identity comes from the stratum, never the rounded position.

Stratified and Poisson settings select an exact immutable box volume. Spheres and other
provider primitives are not silently approximated by bounding boxes. Provider capture
owns finite, complete, non-degenerate canonical geometry admission.

Poisson style is bounded rejection sampling, rather than an unbounded neighbor-growth
algorithm. Each attempted sample owns its identity before acceptance. Separation is
measured between the final stored float positions using double normalized deltas;
this avoids overflow or underflow from squaring a finite separation. A half-separation
cell partition gives a conservative maximum density: a cell diameter is below the
separation, so at most one accepted point can occupy a cell. Requests exceeding that
upper bound fail with `pcg.generation.density_impossible`. Other infeasible requests
may exhaust the finite attempt budget and fail with
`pcg.generation.attempts_exhausted`. Both paths discard partial output; success always
contains the exact requested count. Zero count is a valid empty successful output.

## Seed and identity evidence

The existing `PCGProvenance::Seed(SourceSampleId)` is the only random-stream root.
Fixed integer draw expansion supplies a fixed axis draw order. Floating interpolation
is classified by the existing profile-deterministic numeric contract and its required
nonzero profile evidence; it does not claim portable floating-point certification.

Point identity schema 1 consists of five unsigned-integer attributes:

- `pcg.source_root_0`, `pcg.source_root_1`, `pcg.source_root_2`, `pcg.source_root_3`:
  four network-order UInt64 words copied directly from `PCGProvenance::Key()`;
- `pcg.source_sample_id`: nonzero semantic lattice-cell, stratum or attempted-sample ID.

No second hash schema is introduced. The existing canonical key retains graph/node,
effective settings, provider/source/revision/origin, world and numeric-profile evidence.
Every retained tuple must reproduce
the stored seed through the unchanged schema-1 `DerivePCGSampleSeed` encoder; this
checks consistency and does not authenticate host-supplied provider digests.
All five fields must be present with the exact type, or all must be absent for legacy
SnapshotGrid. The three added generators require the complete schema. An acceptance
index or filtered-output index is never substituted for the semantic sample ID.
Filtering and forwarding preserve the composite unchanged. Merge preserves distinct
roots even when sample IDs coincide and rejects repeated complete `(root,sample)`
identities with `pcg.generation.identity_invalid`. Partial or mistyped identity evidence
is an error, rather than a weaker legacy fallback.

## Complete cost and lifecycle

Payload-dependent costs require the exact validated cooked node, so the catalog cost
query accepts `const PCGCookedNode&`. Every owning caller migrates with this change;
a type-only query must not underprice an attempted-sample budget. The built-in catalog
capacity changes deliberately from four to seven while preserving all existing enum
values and type IDs. The host still explicitly registers and captures its inert catalog.

Admission precedes workspace allocation and includes checked output records, source
search, all attempted samples, worst-case attempt-by-accepted-count pair comparisons,
accepted-position scratch, identity validation, and retained/replacement overlap.
Merge identity validation also contributes its complete bounded temporary storage and
comparison work. Integer arithmetic overflow is rejected before allocation.

Kernels poll the existing operation cancellation token during samples and pair checks.
They borrow only the captured root and operation-owned workspace. The evaluator owns
failure cancellation, completion, replacement fencing and drain; a kernel failure never
seals a partial successful candidate or publishes scene state.

## Migration and regression obligations

`PCGPointGeneration.h` belongs to HoroPCG and exposes fixed typed authored settings;
private decoding, geometry and execution remain target-private. Public-header ownership,
consumer coverage, built-in schema/cost descriptors and all evaluator admission callers
change together. Existing Grid v1 payload and identity-free output behavior have golden
coverage. New tests exercise exact schema bytes and skew, count/work overflow, complete
identity evidence, deterministic sample/root perturbation, distinct-root merge and
same-root duplicate rejection, numeric boundary coordinates, density and attempt
failures, cancellation, replacement and operation drain. Hosted platform qualification
remains separate from local validation evidence.

Point transformation and gradient-noise nodes preserve these identity columns; see [the transformation contract](pcg-point-transformation-contract.md).

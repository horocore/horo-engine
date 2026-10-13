# Deterministic point transformation contract

HORO-2032 / [PCG-3.5] extends the inert CPU catalog with Transform (12),
SurfaceAlignment (13), RandomOffset (14), PerlinNoise (15), and SimplexNoise (16).
IDs 0–6 retain their existing type IDs. IDs 7–11 and settings schemas 5–9 belong
to the point filters already delivered on main. Transformation schemas 10–14
are version 1; the combined capacity is seventeen and counts registered descriptors,
never an inferred registration from the largest enum value. Unknown kinds remain
typed unsupported errors, and supported but unregistered kinds remain unavailable.
No descriptor construction or registration selects a backend or invokes a provider.

## Coordinates and records

Every operation preserves input count, order, local bounds, density, stored seed,
and every source attribute. Transform adds translation in snapshot axes or in
point-rotation axes (without applying point scale), composes normalized rotations
on the left for snapshot axes or right for local axes, and multiplies component
scale. This is an explicit component operation, not affine matrix decomposition;
nonuniform scales do not create a silently discarded shear component. Result
components and the composed matrix must be finite. Invalid output cancels the
whole unpublished workspace; the previous immutable candidate remains valid.

SurfaceAlignment selects immutable captured triangles whose inclusive snapshot
XZ projection contains the point. Vertical projections provide no coverage.
Selection minimizes absolute vertical separation, ties by stable triangle ID,
and requires the authored maximum distance. The oriented unit normal becomes
the rotated local +Y axis through a shortest rotation computed from a double
cross magnitude/dot and atan2 half-angle. Near-antiparallel lateral components
remain significant. Only exactly zero cross with a negative dot uses a fixed X tangent is used unless nearly parallel, then Z. Translation, scale and
local bounds stay unchanged; absent coverage fails explicitly. No provider or
terrain callback occurs during evaluation.

RandomOffset expands the original stored semantic sample seed with SplitMix64
(constants 9e3779b97f4a7c15, bf58476d1ce4e5b9, 94d049bb133111eb), xor the
authored salt, drawing x/y/z in that order. The upper 53 bits select [0,1),
then map to the symmetric authored half-extent. Point-local offsets use rotation
without scale. Row index and worker assignment never select the stream. The
five source-identity attributes and their matching stored seed remain unchanged.

## Gradient noise and numerical tier

Both kernels sample snapshot translation times positive frequency. The field
seed is the existing node provenance seed for semantic sample 1, combined with
an authored salt and a fixed per-channel mix. It is constant across the point
set; point identity is retained rather than used to make a discontinuous field.
Scalar, Vector2, Vector3 and Vector4 columns are exact typed destinations. A new
key adds exactly one field; an existing key requires the same type. Integer,
boolean, source-identity destinations, missing output fields, dropped input
fields and unrelated added fields fail during admission.

Perlin uses the twelve diagonal gradients (±1,±1,0 and axis permutations), eight
integer lattice corners and quintic fade 6t^5−15t^4+10t^3, interpolated x/y/z.
Simplex uses the same gradients, 3D skew 1/3 and unskew 1/6, four corners,
stable x/y/z ranking at ties, and 32·max(0,0.6−r²)^4 gradient contributions.
All lattice hashing is fixed unsigned integer arithmetic; there is no mutable
permutation table, global RNG, renderer backend or ambient registration.

Octaves run in ascending order, with 1–16 iterations, persistence [0,1] and
lacunarity [1,8]. Every octave coordinate must be finite and within ±1048576
before floor or signed conversion. All requested octaves are validated even
when later weights are zero. Weighted noise divides by the complete positive
weight, clamps to [-1,1], optionally maps to [0,1], then multiplies by finite
nonnegative amplitude. Vector values narrow once to finite float storage;
scalar remains double. No nonfinite value is replaced by a default.

These five nodes are profile-deterministic: numeric-profile provenance is
mandatory. Structured workers write disjoint stable row ranges with the same
per-point channel/corner/octave order for all worker counts, so a qualified compiler/platform profile produces identical
stored values. Cross-profile floating-point bit identity is not promised;
reference fixtures specify their tolerance. Portable legacy nodes keep their
existing determinism declarations.

Perlin's x/y/z interpolation uses C++20 `std::lerp`. Numeric profile certification
includes the kernel implementation, compiler and standard library: the earlier
unmerged handwritten interpolation is not bit-compatible evidence for this
implementation. A host upgrading that implementation must requalify its numeric
profile and issue a different `numericProfile` fingerprint (or its versioned
numeric policy) before reusing cached results. Settings bytes and node identities
do not change; the independent rational fixtures retain their original tolerances.

## Admission and lifecycle

Before workspace allocation, schemas and capacities are checked, and checked
arithmetic charges complete record visits, every surface selection, every
channel/octave/corner, and bounded settings scratch. Noise settings keys are
bounded by the existing 96-byte canonical key contract. Execution reads only
retained cooked plans, catalog snapshots, spatial snapshots and borrowed inputs
whose synchronous lifetime encloses evaluation. The owner opens preallocated columns before submitting workers and joins every
accepted child before sealing, cancelling, revoking writers or destroying borrowed
settings/spatial/source data. Child workers write only disjoint value elements;
column structure and the writable phase stay unchanged until that join. Submission
failure cancels and drains previously accepted children. Cancellation is checked
for every point, surface and octave; any failure cancels partial workspace state.
Catalog replacement/removal/close does not invalidate already retained roots.

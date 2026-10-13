# Typed bounded point filter contract

HORO-2018 / #2064 [PCG-3.4] extends the explicitly composed immutable PCG CPU
node catalog. These operations borrow the captured spatial root and operation-owned
point workspace. They perform no provider calls, scene mutation, residency requests,
or global registration.

## Settings and semantics

`PCGPointFiltering.h` belongs to HoroPCG. Five settings-backed kinds extend the
catalog to twelve entries without renumbering the seven existing kinds/type IDs.
Each new kind has one PointSet input and one PointSet output. The existing
DensityFilter with a Scalar threshold input retains its schema and semantics.
Hosts explicitly register the selected new kinds; catalog creation remains inert.

| Kind | Typed settings | Inclusive boundary and failure policy |
| --- | --- | --- |
| DensityRangeFilter | Binary64 minimum and maximum | Finite ordered interval within [0,1]; retains core densities in the interval |
| BoundsFilter | Six Float32 box components | Finite ordered snapshot-space box, including degenerate boxes; tests transform translation, not point-bound intersection |
| SlopeFilter | Binary64 minimum and maximum degrees | Finite ordered interval within [0,180]; uses captured surface normals against snapshot Y-up |
| DistanceFilter | Binary64 minimum distance | Finite nonnegative snapshot-unit distance; stable greedy separation against previously accepted transform translations |
| AttributeFilter | Canonical attribute key, comparison, exact typed value | Missing keys, wrong types, unsupported comparisons, or nonfinite values fail without coercion |

All outputs preserve relative input order and copy every core field and typed
attribute unchanged. Filtered positions never acquire new seeds or source identities.
Distance equality is accepted. A rejected point never constrains later acceptance.
Double `hypot` over the stored Float32 coordinates avoids threshold-square overflow
and underflow, including positive subnormal distances between coincident positions.
No approximate neighbor index or unchecked quadratic work is introduced.

Terrain slope performs a bounded scan of immutable surface triangles at each point's
XZ position. Projected barycentric containment includes triangle boundaries. Vertical
triangles with zero projected area do not supply an XZ height. Among overlapping
surfaces, the nearest vertical height wins; ties retain the first stable surface ID.
Slope is `acos(clamp(normal.y,-1,1))` in degrees. No containing surface, including a
snapshot with no surfaces, is an error rather than an empty successful result.
Floating terrain slope and separation use ProfileDeterministic numeric evidence; it does not claim
portable transcendental certification. The other three new filters use the catalog's portable
comparison policy over captured finite values.

Attribute predicates retain Boolean, SignedInteger, UnsignedInteger, Scalar,
Vector2, Vector3 and Vector4 separately. Numeric scalar/integer values support exact
equality, inequality, less, less-or-equal, greater and greater-or-equal. Boolean and
vector values support equality and inequality only. Vector equality compares every
component exactly. Integer predicates preserve their full 64-bit width. Missing keys use `pcg.point.attribute_unknown`, wrong types use the existing typed
attribute mismatch error, and missing terrain samples use the spatial coverage error.
Schema
validation precedes point allocation and applies even when selection is empty.

## Canonical envelope and admission

Every new payload starts with a four-byte network-order version 1. Density and slope
append two Binary64 fields (20 bytes total); bounds append six Float32 fields
(28 bytes); distance appends one Binary64 field (12 bytes). Attribute payloads append
one-byte key length, the validated canonical ASCII key, one-byte comparison enum,
one-byte exact type tag, then Boolean byte, Int64/UInt64/Binary64 word, or two/three/four
Float32 components. Tags match the seven ordered PCGAttributeType values. Signed
integers preserve their two's-complement bits; floating signed zero is canonicalized.
Truncated, oversized, unsupported-version, invalid-value, negative-zero and trailing
payloads fail exact decoding. No source-format change or implicit authored migration
occurs.

The evaluator proves identical input/output schemas before allocation. Catalog cost
charges selection and copying, the complete worst-case N-squared separation visits,
N times the bounded surface count for terrain visits, and N accepted indices plus a
bounded codec/metadata allowance. Checked arithmetic and the existing point/snapshot
visit limits reject overflow and excessive work before workspace creation. Terrain
accounting charges the captured surface count separately; the existing generation
lookup envelope continues to count grids and volumes without unrelated surfaces.
The selection allocation is operation-local and is included in scratch, aggregate,
and replacement overlap accounting.

Each selection, pair and surface loop polls cancellation. Only successful selection
opens the exact bounded output. Owner-thread copying seals the entire result; errors
or cancellation cause the evaluator to cancel the operation workspace and return no
candidate. Catalog replacement, withdrawal, owner close and spatial replacement do
not mutate historical roots retained by existing plans/candidates. New evaluations
use new operation workspaces; old/new candidate retention remains charged by the
existing replacement admission contract.

## Migration and regression obligations

Consumers may continue registering the original seven kinds with unchanged schemas.
The default capacity grows from seven to twelve; explicitly lowered capacities remain
valid. New public-header ownership and consumer coverage accompany the header.
Regression fixtures exercise exact column preservation, integer precision, stable
compaction, inclusive boundaries, empty selections/inputs, invalid settings/envelopes,
missing or mismatched attributes, terrain coverage failures, checked work admission,
cancellation, and retained historical roots across replacement and close. Native and
hosted qualification remain separate evidence.

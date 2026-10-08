# Offline Terrain Source Artifacts

## Ownership and composition

`HoroEngine::TerrainCook` owns `CookTerrainSourceArtifacts`. The Assets cooker host
passes one immutable canonical source, an exact tile/target/toolchain profile,
dependency artifact digests, explicit collision/navigation LODs and a cooperative
cancellation token. The result includes the existing verified tile set and actual
indexed geometry for every visual LOD and the two selected consumer LODs. It is not
a renderer demo or a reference to geometry that has not been generated.

The contribution performs no registration, storage, filesystem access, scheduling,
GPU allocation, solver-native shape cooking, PhysicsWorld installation, navigation
provider bake or NavMesh activation. Assets owns cache keys, complete generation
staging and atomic publication. Consumers own their separately keyed native
conversion. A source artifact does not prove consumer readiness.

Before publication the host rechecks the authoritative source with `ValidateCurrent`
and its selected dependency/target fingerprint. Assets stores the complete artifact
closure and manifest in its ordinary generation transaction. Loaded consumers match
tile/role membership against that verified manifest, then call
`VerifyTerrainSourceArtifactPayload` with the manifest's expected digest. Computing a
new expected digest from untrusted bytes is not integrity verification.

## Canonical geometry and legal representations

Vertices are right-handed Y-up meter positions in double precision, with the exact
local/projected coordinate provenance retained. Imported heights are already in
meters: normalization scale and offset are not applied again. Each source-grid quad
uses the fixed upward-wound diagonal `(a,c,b)` / `(b,c,d)`, with its original integer
source coverage recorded on both triangles. Partial tiles retain the final source
edge. Triangle indices and finite positive XZ area are checked before output.

All roles omit a coarse quad if any original sample in its closed coverage is a hole.
Thus an unsampled hole cannot become solid geometry at a coarse collision or navigation
LOD. This v1 policy is deliberately conservative, rather than inventing interpolation
through a hole. Unreferenced grid vertices are retained; only solid triangles form the
surface. An all-hole artifact is explicit empty topology, not a silent fallback.

Visual LODs bind existing edge signatures and require same-LOD neighbors. There is no
skirt, morph, automatic neighbor clamp or mixed-LOD permission. A renderer must reject
an incompatible selection, not invent a crack repair. The v1 geometric-error bound is
zero for the exact finest grid and otherwise the maximum enclosed solid-quad height
range. Both original and coarse piecewise-linear surfaces lie within this range, so
it is conservative even where their diagonals intersect. It is not a measured optimal
screen-space error bound or a performance qualification claim.

## New schemas and migration

Existing `HTIL` / `HTMF` schema-v1 layouts are unchanged. New independent schemas are:

- `HTSG` v1: sized magic, little-endian schema, tile ID, role, capability, source
  asset/revision/digest, tile-cook and source-artifact fingerprints, tile manifest
  digest, exact coordinate metadata, four seams, error bound, same-LOD requirement,
  vertex/triangle counts, float64 XYZ vertices and uint32 indexed triangles/coverage.
- `HTSM` v1: sized magic/schema, tile-manifest digest, complete source-artifact
  fingerprint and canonical tile/role entries containing byte size and SHA-256.

The source-artifact fingerprint closes over the exact tile fingerprint/manifest,
consumer LODs, artifact schema/algorithm and finite limits. Dependency input order is
canonicalized by the existing tile cook. Source revision/capability, hole semantics,
coordinate policy and all geometry bytes remain traceable. Changing an algorithm or
wire layout requires a new schema/key, not reinterpretation of old bytes.

Existing tile-only consumers may continue to consume their unchanged tile schema.
Hosts requiring neutral geometry must cook this new complete contribution and include
all selected artifacts; they cannot relabel an old tile-only generation as geometry
ready. This API addition has no native dependency or existing consumer migration.

## Bounds and lifetime

This is a tooling/load-time operation, never a frame-hot call. Tile cooking retains its
own qualified sample/payload work limits. Additional geometry work is admitted against
the exact tier's vertex, triangle and sample-visit ceilings. Peak candidate byte
admission includes retained tile/vector/payload/CRS capacities, geometry objects and
arrays, encoded payload/manifest capacities and transient coordinate/grid storage.
The final actual-capacity audit rejects a candidate that exceeds the ceiling. Allocator
bookkeeping and caller-owned source storage are not reported as contribution bytes.

Cancellation is checked during geometry rows, source-quad rows, vertex/triangle encoding
and before the final return. SHA and structural verification operate on independently
bounded tile/artifact byte buffers. Allocation exceptions unwind detached candidates;
no partial output becomes a cache entry. The host cancels and joins its operation
before releasing borrowed inputs. No task, callback, global registry or source borrow
is retained in the move-only result. Completed results survive source replacement and
host retirement; stale revision/capability or same-revision changed bytes fail source
revalidation. The contribution has no independent runtime publication/shutdown owner.

## Delivery identity

This change is **HORO-1899**, Jira immutable ID **12304**, GitHub **#1943**,
**[TRF-002.4]**. Historical merged PR3017 incorrectly included this Jira key while
closing GitHub #1899 / [PLS-004.2] and delivering only achievement coordinator files.
The authoritative achievement Jira identity is HORO-1855 (immutable ID12260).
PR3017 did not deliver this Terrain AC. Historical PR/Jira development links are left
untouched; this delivery uses the current verified three identifiers only.

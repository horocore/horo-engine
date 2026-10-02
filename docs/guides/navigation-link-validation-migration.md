# Grounded off-mesh link validation and suggestions

`NavigationLinkValidationSnapshot` belongs to `HoroEngine::NavigationApi`.
`NavigationSceneLinkCapture.h` belongs to the existing
`HoroEngine::NavigationSceneIntegration` target. Public-header consumer generation
covers both contracts through their registered owners. No provider, Physics,
Asset Pipeline or editor dependency was added to NavigationApi.

The shared `NavigationLinkKind` and `NavigationLinkDirection` now live in
`NavigationLinkTypes.h`. Existing `Runtime::NavigationLinkKind` and
`Runtime::NavigationLinkDirection` spellings are source-compatible aliases, with
the same enum values and Scene serialization vocabulary. This prevents the bake
and Scene contracts from acquiring competing direction semantics. Existing Scene
callers require no source migration. The enum C++ type identity has moved; rebuild
dependent modules rather than mixing old and new binaries. Scene schema values
and serialized names are unchanged.

## Bake composition

1. Validate one committed Scene revision with the existing Scene-wide navigation
   validator. Capture that revision in `NavigationBakeInputSnapshot`.
2. Use `CaptureNavigationSceneLinks` for each selected grounded profile. It copies
   enabled links and transforms both ordered endpoints to canonical metres.
   Radii already use metres. Disabled/unselected links remain authored and do not
   enter that partition. No Scene pointers survive capture.
3. Pin the host-composed query backend for that exact profile's immutable built
   topology. Supply its world/topology identities and an admitted nearest-point
   requirement with one result and finite work/search bounds. The host must not
   substitute another profile's topology.
4. Supply available traversal descriptors and the exact captured area/filter
   registry. Descriptor kind collisions fail rather than selecting by input
   order. Missing kinds reject the affected links. Both endpoint partitions and
   projected hits must resolve to the selected profile and exact surfaces.
5. Supply immutable collision-owner clearance observations for the **projected**
   ordered endpoint pairs. Each observation must carry the bake fingerprint and
   profile, and measured free radius/height. Bidirectional traversal needs evidence
   for both directions. Missing evidence fails closed. A connection radius alone
   is not free-space evidence, and navigation does not assume an airborne gap is
   unobstructed. The collision owner remains responsible for measuring the actual
   traversal corridor appropriate to Jump, Ladder, Door or Teleport.
6. Assemble a `NavigationLinkValidationRequest` containing the pinned input, registry,
   backend, exact projection context, authored links, descriptors, clearance and limits.
   Pass it with the cancellation token to `NavigationLinkValidationSnapshot::Validate`.
   Omit generation to validate
   only authored links. `CaptureNavigationLinkBoundaryAnchors` can derive stable
   edge-midpoint anchors from validated neutral candidate tiles using exact
   polygon-range-to-surface bindings, without publishing an intermediate mesh. Internal
   edges are excluded. Optional generation receives those stable anchors, explicit
   kind/direction/distance policy and qualified limits. Anchors sort by identity;
   one-way generation considers ordered pairs, while bidirectional generation
   considers each pair once. Cross-surface candidates pass the same full validator
   and clearance checks as authored links. Generated anchor identities never
   become authored `NavigationLinkId` values.
7. Inspect `Authored()`, `Suggestions()` and `Diagnostics()` separately. Every
   rejection retains both ordered endpoint values and its typed failing rule;
   failed endpoint projection retains the original typed provider error. No
   generated suggestion is a Scene command or cooked link.
8. Call `PrepareCookedLinks` with an explicit `AuthoredOnly` or
   `IncludeValidatedSuggestions` policy. There is no default policy or implicit
   conversion from suggestions to `NavMeshOffMeshLink`. Any authored rejection
   blocks the complete conversion; rejected suggestions do not poison valid
   authored acceptance. The current neutral link schema represents traversal cost
   through its area/filter policy, so an authored cost that cannot be represented
   exactly is diagnosed instead of silently dropped. The returned dependency
   fingerprint includes canonical rows, stable source identities and cook policy;
   include it in the host's complete navigation cook key.
9. Partition returned neutral rows by the tile owning `startPolygon`, retaining
   the canonical global `endPolygon`. Feed those rows to the existing
   `NavMeshArtifactView`/`NavMeshData::Create` validation path. Asset Pipeline
   staging and publication remain host-owned. Conversion rechecks the existing
   bake publication fence, complete source observations, current request,
   profile/world/topology identity and lifecycle state. It does not publish an
   artifact or update a current pointer.

## Lifetime and bounds

This is a synchronous background/tooling bake kernel. It creates no scheduler,
operation store, callbacks, registry mutations or runtime topology changes. The
caller pins backend, registry, input and clearance storage during validation; the
returned move-only snapshot owns its acceptance/diagnostics and retains no such
pointers. Moving invalidates cooked conversion on the source.

Hard and caller-lowered bounds cover authored rows, anchors, pair attempts,
suggestions, clearance rows, retained storage and deterministic work charges.
Projection charges the entire admitted node ceiling before dispatch. Sorting,
evidence lookups and duplicate checks are charged as well. Pair/work/output/byte
exhaustion returns a typed capacity error and no partial validation snapshot.
Cancellation is observed around provider calls and each bounded work charge.
Replacement, supersession, cancellation, failed operations and shutdown cannot
cross cooked conversion; the operation owner still performs its final atomic
publication barrier under ADR-106.

Regression coverage includes real Detour projection into neutral artifact rows,
policy exclusion/inclusion, malformed and finite values, exact clearance/direction
boundaries, duplicate identities/traversals, profile/descriptor compatibility,
input-order determinism, count/work/storage exhaustion, cancellation during
projection, capability/topology/request replacement and source-storage teardown.

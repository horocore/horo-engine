# Navigation Scene Components Migration

Scene navigation authoring now uses `NavigationSurfaceComponent`,
`NavigationRegionComponent`, `NavigationModifierComponent`,
`NavigationLinkComponent`, and `NavigationAgentComponent` from
`Horo/Runtime/Scene/NavigationSceneComponents.h`.
Importers must assign non-zero stable surface, region, modifier, and link identities instead of
deriving identity from object order, names, paths, entity slots, or pointers.

Surface components reference the exact navigation-definition `AssetId`, one or
more grounded profile identities, a version/generation, and either the owning
object subtree or explicit finite local bounds. Region components reference an
existing surface identity and declare finite local bounds, include/exclude mode,
and explicit-contributor or bounded-static-collision source selection. Provider
handles, generated polygons, and backend flags are not valid authored fields.

Modifier components reference one exact surface and use a finite object-local box
or Y-axis cylinder. Their operation controls the complete payload shape:
`Exclude` carries neither area nor cost, `OverrideArea` carries only a valid area,
and `OverrideAreaAndCost` carries both a valid area and finite non-negative cost.
Link components own ordered finite start/end endpoints, an explicit kind and
`StartToEnd` or `Bidirectional` direction, a finite non-negative traversal cost,
and a bounded unique profile set present on both endpoint surfaces. Coincident
endpoints are rejected rather than guessed.

Agent components own only provider-neutral intent: a non-zero profile identity,
query-filter identity, optional finite positive radius override, grounded movement
capability, schema version, and enabled state. They never persist a runtime crowd
handle, world identity, entity slot, provider reference, or backend flag.

All mutations go through `SetSceneNavigationSurfaceCommand`,
`SetSceneNavigationRegionCommand`, `SetSceneNavigationModifierCommand`, or
`SetSceneNavigationLinkCommand`, or `SetSceneNavigationAgentCommand`. These commands validate the complete committed
Scene identity/reference set before changing history. Removing a referenced
surface therefore fails until its regions, modifiers, and link endpoints are
removed or explicitly retargeted. A malformed command fails atomically and leaves
the otherwise valid surface and document revision unchanged.
Object duplication regenerates component identities and retargets a co-located
region or modifier that referenced the duplicated surface. It also retargets each
endpoint of a co-located link that referenced that surface; references to external
surfaces remain exact and unchanged. Prefab source-object duplication likewise
issues new component identities in the prefab document. Placing another prefab
instance does not mutate source-owned identities: the existing pinned prefab-
resolution step materializes instance-qualified identities in the containing Scene
snapshot before validation, and overrides then use the same typed commands. Nested
instances do not inherit or discover ambient surfaces.

Persistence stores these typed values under `navigationSurface`,
`navigationRegion`, `navigationModifier`, `navigationLink`, and `navigationAgent`.
Modifier shape and
operation names, link endpoint order, kind, and direction are explicit strings;
one-way and bidirectional links therefore round trip without inference. Runtime
conversion copies only enabled values from one
committed `DocumentStateId`, which becomes the `SceneDefinitionRevision`; it never
reads editor previews or an in-progress modal/property draft.

`HoroNavigationRuntime` owns `NavigationAgentRegistry`, a bounded owner-thread
registry whose handles carry an exact non-reused world and slot generation.
`HoroNavigationSceneIntegration` prepares a complete detached agent population
from the unpublished `RuntimeScene`, and the aggregate scene commit publishes it
only after every activation participant validates. Failed capacity, duplicate-owner,
stale-generation, or shutdown admission leaves the active scene and registry
unchanged. Prefab expansion recognizes the canonical provider-neutral
`game.horo.navigation_agent` serialized component envelope; other opaque payloads
retain the existing typed-projection failure behavior.

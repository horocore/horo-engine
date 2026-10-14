# Gameplay navigation capability

HORO-1265 / [NAV-004.11] adds an explicit host adapter above NavigationRuntime.
`HoroEngine::GameplayNavigationIntegration` owns `GameplayNavigationContext`; no
Navigation target acquires a Gameplay or Lua dependency. Native SDK boundary 10
requires rebuilding project modules and descriptor bundles. Native authors opt in
with `find_package(HoroEngineGameplay REQUIRED COMPONENTS Navigation)` and link
`HoroEngine::GameplayNavigationIntegration`; the default SDK remains navigation-free. Existing behavior
runtime calls keep their default omitted navigation grant.

Composition binds the exact module ID, Scene/world incarnation, path caller,
filter, capability revision, finite query policy and optional per-agent movement
controller. Query and movement grants are independent. No service is discovered
globally and no Scene or provider pointer is exposed to scripts. The host calls
`Refresh` with the committed observation at its navigation safe point, commits
canonical coordinator results, updates movement, and refreshes terminal delivery.
`Submit` is owner-thread only, bounded to the prepared maximum retained records.
Request cancellation and owned Telemetry operation context propagate into every
canonical path/repath; terminal status and provider errors remain typed.

The same `GameplayNavigationRequest` and opaque `GameplayNavigationHandle` serve
native callers and Lua `ctx.navigation.path`, `move_to` and `follow`. Lua arguments
are primitive copied coordinates and generation-tagged target IDs:
`path(sx, sy, sz, dx, dy, dz)`, `move_to(dx, dy, dz)` and
`follow(dx, dy, dz, targetIndex, targetGeneration)`. Handles expose
nonblocking `poll` and `cancel`; a missing service returns typed unavailable, a
withheld operation returns permission denied, and retirement returns scope revoked.
Each Lua VM owns a cancellation source revoked before reload/close; no VM callback
or borrowed behavior context is retained by navigation.

`GameModuleHost::PrepareReload` and shutdown revoke navigation before calling
module code. Behavior shutdown and activation rollback do the same. The composition
root retains the adapter and canonical owners, keeps committing/draining after
`Revoke`, and calls `DrainRetired` until true before releasing the borrowed services.
Revoke never waits or calls module/script code. Final provider unload follows
Foundation JobSystem shutdown; cancellation alone is not proof that jobs joined.

Rollback requires stopping play and restoring one coherent engine/SDK/module set.

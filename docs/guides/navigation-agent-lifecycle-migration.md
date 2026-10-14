# Navigation Agent Lifecycle Migration

## Purpose

Use the Scene-qualified `NavigationSceneAgentLifecycle` owner for real MoveTo,
Follow and crowd velocity work across teleport, compatible NavMesh/profile
replacement, disable, entity destruction and Scene unload. This is an explicit
host composition API, not automatic Scene activation or a second navigation
provider. It replaces host-side resets that cancel a query but leave its result
reservation or avoidance history owned by a disappearing consumer.

## Prerequisites

- Link `HoroNavigationSceneIntegration` and include
  `Horo/Navigation/NavigationSceneAgentLifecycle.h`. The lower
  `HoroNavigationRuntime` target does not depend on Scene.
- Publish the Scene agent population and provider through their existing
  transactional activation boundaries. Use exact runtime Scene/entity,
  registration, world, topology, profile and caller incarnations; never derive
  runtime identity from names or addresses.
- Keep the registry, world, coordinator and canonical Foundation `JobSystem`
  alive until all lifecycle owners and retained worker partitions are drained.
- Select a real grounded provider explicitly. Headless integration tests use
  `HORO_BUILD_NAVIGATION_RECAST_DETOUR=ON`; no renderer or extra worker pool is
  needed. Provider/backend selection remains in the host.

## Workflow

### 1. Compose and reconcile committed authority

Create one lifecycle owner outside fixed-tick work for each enabled exact Scene
entity and published registration. `Create` validates the entity and composes
the actual movement controller and velocity pipeline. Failed creation does not
mutate the registry. Pass the exact host-published entity, registration, caller
and Character revision together as `NavigationSceneAgentLifecycleDescriptor`:

```cpp
auto owner = NavigationSceneAgentLifecycle::Create(
    registry, world, coordinator, scene, {entity, agent, caller, characterRevision});
```

The descriptor is inert admission metadata; it owns no services or Scene storage.
The borrowed authorities remain explicit factory parameters. Scene views are borrowed only for each call; do not retain
their pointers in worker captures.

After `CommitDeferredLifecycleChanges`, invoke `ReconcileAtSafePoint` for every
remaining owner once in the canonical lifecycle phase, including Idle,
TargetLost and otherwise inactive owners. Supply a fresh Scene view, or
`std::nullopt` only after committed unload. A stale borrowed view is an error,
not evidence of unload. Missing, disabled or replaced entities permanently
retire the owner and its logical registration. Re-enable through a fresh
registration and owner, not by reusing the retired instance.

Reconciliation services obsolete movement requests only while that owner has
exact retained handles. Ordinary Start, Update, Capture, Prepare, Commit and
Feedback authority guards do not poll global job partitions. The host also
calls `NavigationCoordinator::CollectRetiredAtSafePoint` once at its global
owner lifecycle phase, including phases with no surviving Scene owners. This
keeps abandoned partitions draining after their consumer has been destroyed.

### 2. Capture, execute and publish real movement

Start MoveTo/Follow through the lifecycle owner and supply current bounded
movement observations. Follow targets are checked against committed Scene
storage; stale host target facts cannot revive a destroyed target.

Character remains the sole transform and achieved-velocity authority. Pass its
committed velocity to `CaptureMotion`; never estimate velocity from positions
on opposite sides of a teleport. Build the canonical crowd snapshot from those
samples. `PrepareVelocity` requires that exact sample and returns owned,
immutable work for the host-selected avoidance backend and existing JobSystem.
Workers must not capture the lifecycle owner, mutable Scene or borrowed views.

At NavIntentCommit, pass a fresh Scene view, same-tick observation and current
capture to `CommitVelocity`. Publication revalidates Scene, target, registration,
motion, Character revision and pipeline currency. A late result is not motion
authority. Failed publication requires the host's normal zero-motion/error
policy; `CancelVelocity` discards an exact pending avoidance request without
authorizing motion. Feed actual Character feedback through `ObserveFeedback`.

### 3. Reset or rebind discontinuities explicitly

After Character commits a teleport or replacement, call `TeleportAtSafePoint`
with a strictly newer Character revision and the post-reset observation,
without old feedback. It invalidates the old corridor and pending avoidance
work, clears accumulated motion/feedback history, and installs a zero-velocity
sample. The reset tick rejects a nonzero sample, rather than converting the
position jump into artificial crowd speed.

For a compatible published NavMesh or profile replacement, call
`RebindAtSafePoint` before ordinary reconciliation would retire the obsolete
registration. Supply the new registration, strictly newer caller incarnation,
new Character revision and exact committed replacement observation. The same
Scene/entity owner must remain live. Admission validates the new world/topology
and query capabilities before changing ownership. Accepted semantic goal,
operation sequence, exclusive deadline and total accepted-query budget are
preserved; rebind forces a fresh route and zero avoidance history. It does not
replenish retries, implicitly adopt another entity or mutate Scene authoring
components. Failed admission leaves the existing movement state unchanged.

### 4. Cancel and drain physical ownership

`NavigationCoordinator::Cancel` preserves a terminal result for its consumer.
When a consumer will never take that result, use `Abandon` instead. It requests
cancellation, suppresses publication and relinquishes the result reservation
idempotently. It does **not** free executing slots or provider/tile leases.
Those remain owned by the coordinator's immutable worker partition until its
exact terminal JobSystem fence is observed. `IsRetained` tests that exact
generation without polling jobs or advancing publication.

Movement destruction transfers outstanding retirement to the surviving
coordinator. While a movement owner remains alive, canonical reconciliation
retains its obsolete handles until physical retirement, even if the host no
longer calls Update after TargetLost. Logical disable is therefore not proof
that a provider can be destroyed. Close admission, request cancellation, use
the host's bounded JobSystem shutdown policy, collect terminal coordinator
partitions, then collect world retirement before destroying borrowed services.
Safe-point collection itself neither waits nor joins workers.

## Troubleshooting

- `AgentRegistryStale`: inspect the exact Scene/entity incarnation and published
  registration/binding. Use explicit compatible rebind or a fresh owner; do not
  retry a destroyed owner with a replacement entity.
- `StaleSnapshot`: inspect Character reset revision, capture tick, current
  world/topology, stale feedback and the current borrowed Scene view. Recapture
  committed facts rather than weakening currency checks.
- Retained provider after TargetLost: continue canonical reconciliation for the
  live inactive owner. After owner destruction, continue global coordinator
  collection; release is permitted only after the worker terminal fence.
- Old completion after teleport/reload: discard it. Neither cancellation success
  nor the existence of a worker result proves current publication authority.

## Limitations

This API does not register ambient callbacks, choose a backend, install an app
activation path, own Character transforms, or schedule an independent worker
pool. Cross-Scene replacement requires fresh ownership, not compatible rebind.
The application must implement the explicit canonical phase and keep borrowed
services alive throughout shutdown. Durable navigation source schemas and Scene
authoring serialization are unchanged.

## Validation Record

Linux headless Debug validation on 2026-10-13 built the runtime, real
Recast/Detour provider, Scene integration and both public-header consumers.
The current canonical-retirement implementation passed 118 runtime, 22 provider
and 10 Scene Catch cases (3,894 assertions), plus both consumers: 152 registered
CTest runs passed without skips. Thirteen new cases use the actual JobSystem,
Scene and provider boundaries, including running/queued abandonment, terminal
lease retention, inactive-owner-only reconciliation and unrelated idle-owner
polling. The owner-thread C++ allocation probe covers the tested capture,
publication and reset path; it does not measure native allocator calls or other
threads. These are source-stage results, not final PR or cross-platform gates.
GPU and Python suites were not qualified by this run.

## References

- [Navigation and AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md)
- [Ownership and Resource Lifetime](../architecture/foundation/ownership-and-resource-lifetime.md)
- [Concurrency and Jobs](../architecture/foundation/concurrency-and-jobs.md)
- [Navigation Scene Components Migration](./navigation-scene-components-migration.md)
- [Grounded Navigation Provider Composition](./grounded-navigation-provider-composition.md)

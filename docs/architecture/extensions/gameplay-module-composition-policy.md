# Gameplay Module Composition Policy

Through 1.0, each project may publish one primary native gameplay module. Project
authors can split their code into libraries and use approved packages without
adding a second project lifecycle owner. The application selects the accepted
sources, artifact, and package roles before composing a runtime generation.

## Supported composition

| Use case | Admission and ownership |
| --- | --- |
| Project C++ gameplay | Build one primary module with the exported SDK. The application owns its activation, registration transaction, and reload or shutdown. The module owns its allocations and callbacks for the accepted generation. |
| Reusable C++ source libraries | Restore the locked package graph and explicitly approve the exact source contribution for compilation. The build compiles those sources into the primary module under the same SDK and toolchain. The primary module owns their registrations and lifetime. |
| Project-authored Lua | The project host admits scripts through its existing behavior discovery or explicit source selection. The host owns the script runtime, allowed bindings, and accepted program generation. Behavior instances use the scene lifecycle and host-provided capabilities. |
| An approved prebuilt gameplay package role | The package host selects the declared role from a verified contribution and checks execution approval, target, and exact SDK fingerprint. Its activation owns that role and its retirement. It publishes the contributions allowed by the role, such as decision descriptors, through the existing host admission boundary. |

The [gameplay module overview](./gameplay-module.md) defines source selection and
build inputs. The [package lifecycle](../packages/package-lifecycle.md) defines
approval and activation for prebuilt roles, including the desktop Editor path.
The number of artifacts in a package does not change the project's primary module
limit. Packaged-player support follows its own declared distribution profile.

## Trust and isolation

Project ownership, discovery, successful restore, and a valid signature each
provide different evidence. The author or host policy must explicitly admit
execution. Source compilation approval and prebuilt role activation apply to
the exact selected contribution. A shipped build packages only the project's
declared, admitted artifacts for its target. Loading from a mod directory or
downloading executable code during play is outside this policy.

Native code shares the process and its privileges. SDK validation,
capability checks, and lifetime leases enforce composition and compatibility.
Native code that may be hostile requires a separate isolation boundary before
admission can be considered. Project Lua receives the bindings exposed by its
host and is scoped to its accepted program and scene. The project Lua contract
makes no security guarantee for arbitrary player-supplied scripts.

Independent project modules registering into each other's registries, replacing
engine services, or overriding published IDs are unsupported. A collision
rejects the candidate through the owning compiler, generator, or registry.
Filesystem order, package install order, and last-writer priority cannot select
which registration wins.

## Resolution, ordering, and failure

1. Resolve and restore the project's declared package graph before compiling
   imports. Bind selections to the locked versions and exact approved artifacts.
   Use the SDK's canonical source order and include all imports in its build
   identity. A prebuilt role passes its own compatibility and trust checks.
2. Stage and validate the full registration set before exposing it to a scene.
   Start project services provider-first by their declared dependencies and use
   the runtime's typed schedule for systems. A missing provider, cycle, or
   incompatible registration fails the candidate.
3. Bind instances and callbacks to the accepted generation. On shutdown, stop
   dependents before providers and retire callbacks before unloading native
   code. Reload follows the existing quiesce, snapshot, replace, and restore
   transaction. If safe retirement cannot be proven, require a restart.
4. Retain stable IDs, schema versions, and opaque authored payloads when their
   owner is unavailable. Keep the editor inspectable with actionable diagnostics
   and block Play when a required contribution cannot be composed. Upgrades that
   change serialized structure use the owning type's registered migration.

See the [native module boundary](./gameplay-module-boundary.md) for registration,
reload, and ABI rules, and the [behavior authoring contract](./gameplay-behavior-authoring.md)
for script and scene binding.

## Migration and scope after 1.0

Existing projects can keep their primary module and serialized IDs. To reuse
code, move implementation into namespaced libraries and select reusable sources
through the locked package graph. Keep behavior and component IDs and schemas
stable during the move. The primary module still owns their registration.

Multiple independently activated project modules, player-installed code mods,
and separately distributed script mods are intentionally deferred until after
1.0. Any future proposal must specify typed module identities and dependencies,
namespaced registrations, deterministic ordering and conflict rejection,
versioned save compatibility, and a distribution and update policy. The host
must resolve a complete candidate before activation and define rollback or
restart when removal would invalidate a live generation. Untrusted code also
requires a separately qualified isolation model and an explicit capability
allowlist. Existing single-module projects must continue to open with their
original lifecycle and stable IDs.

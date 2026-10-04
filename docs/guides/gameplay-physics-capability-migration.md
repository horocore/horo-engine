# Gameplay Physics capability migration (HORO-891 / PHY-006.5)

## Native boundary 9

`GameRuntimeContext` gains an explicitly injected Physics context, and the behavior
backend gains its corresponding context accessor. These change native layout/vtable
contracts. The SDK fingerprint changes from `gameplay-sdk8` to `gameplay-sdk9`;
rebuild every gameplay module and generated descriptor bundle with the same engine,
compiler, platform and configuration. Old boundary/fingerprint artifacts are
rejected before factory invocation. Do not preserve mixed SDK generations.

Affected consumers include native module `Start`/`Stop`, native behavior callbacks,
the Lua behavior adapter, editor project/module loading and play-session behavior
activation, the headless product world owner, and external SDK module projects.
Existing default loader/runtime call expressions remain valid and now receive an
explicit unavailable Physics context. They do not activate Physics.

External native projects opt in through the existing build-tree SDK package:

```cmake
find_package(HoroEngineGameplay REQUIRED COMPONENTS Physics)
horo_add_gameplay_module(MyGameplay MODULE_ID game.my_game SOURCES Gameplay.cpp)
target_link_libraries(MyGameplay PRIVATE HoroEngine::GameplayPhysicsIntegration)
```

Include `Horo/Gameplay/GameplayPhysicsContext.h` through that target's staged public
headers. Acquire from the injected holder using the exact module, Scene ID and
activation generation in its immutable binding. The returned type is precisely
`PhysicsQueryEventCapability`; its existing command/revision/error contracts apply.
Neither a world pointer nor native solver API is exposed to gameplay. Physics and
its reviewed private static-library closure are exported only for the explicitly
requested SDK component; the minimal gameplay package does not acquire those link
edges. This is a build-tree SDK, not a new relocatable installed SDK distribution.

`PhysicsRuntime::IssueWorldIdentity` is now public for application composition.
It keeps its existing owner-thread, never-wrap/never-reuse runtime authority and
consumes identities even when candidate activation fails. Hosts must use it instead
of deriving world identity from an authored or runtime Scene ID.

## Headless production composition

The existing bounded reference product accepts an optional explicit suffix after
its role/frame/endpoints:

```text
--game-module <absolute artifact> <module id> <descriptor revision> <grant|deny>
--game-script <absolute source> <absolute sidecar> <module id> <grant|deny>
```

This is trusted local host policy, not permission declared by the loaded module or
script. Artifact metadata still validates against the exact engine SDK fingerprint
and descriptor revision; scripts still use their canonical sidecar identity.
Absent selection runs no gameplay module. No ambient module discovery, implicit
permission, solver fallback, network authentication or authored-project loader is
introduced. The reference Scene attaches an explicitly selected script through its
normal generated descriptor and behavior lifecycle.

Each Scene/Physics factory pair owns a distinct candidate record and activation
generation. Factory pairing happens before preparation; equal Scene IDs cannot
overwrite routing. Gameplay activates only after its exact world activates.
Module unload, Scene unload and host/play stop close admission before storage
retirement. A failed world invalidates Physics clients before returning failure;
the headless owner then closes gameplay admission before releasing the world.

Editor composition is deliberately unavailable: its existing Null selection and
core-only play clone remain unchanged. Neither this migration nor sdk9 claims editor
play-world activation, Physics simulation or UI support.

## Lua adapter

`ctx.physics.acquire()` returns VM-owned storage containing the exact Physics
capability, or `nil, {domain, code}` preserving the stable typed failure. The
context also copies the host's exact `scene` and `scene_generation` routing evidence;
neither field selects a world or grants permission.
The
private Lua projection delegates `identity`, `submit`, `submit_batch`, and
`read_events` to the existing Physics implementation. Ray marshalling is bounded
to 64 hits; event copies are bounded to 64 records. Batch `poll` is non-blocking
and `cancel` delegates to Physics. No VM object, raw pointer, world ownership or
independent query/event service crosses the public SDK boundary. Further geometry
marshalling can extend this private adapter without defining a second Physics API.
Acquisition closures and retained clients hold revocable admission, not a borrowed
callback context. Garbage collection is never the authoritative revocation signal.
The headless owner executes one queued batch after Gameplay and before the next
Physics tick replaces the completed publication. The query batch owns its commands;
scripts do not schedule native work or drive the world themselves.

Pending batches terminate on module revocation or world retirement. A completed
batch's committed result is immutable: later revoke, cancellation or world
destruction cannot replace success or an earlier terminal error. Batch polling may
run on any thread and reads only the immutable admission pin's cancellation token
under the existing terminal-publication mutex, never owner-thread world state.

## Verification

`HoroGameplayPhysicsTests` covers permission/unavailable/disabled policy, native and
Lua injection, separate retirement paths, world failure, same-Scene-ID simultaneous
and replacement worlds, and retained clients/batches. Its real headless factory
regression prepares both equal-ID Scenes before either Physics service.
`HoroGameplayPhysicsSdkContract` copies an independent external project into its
test build directory, finds the exported SDK Physics component, configures/builds
the native module with `--parallel 2`, then activates it through the real loader.
That CTest performs a native build and requires the same manager build grant as
ordinary native validation. Public-header consumers and existing Physics,
GameplayRuntime, ModuleHost, Lua and headless product tests remain required.

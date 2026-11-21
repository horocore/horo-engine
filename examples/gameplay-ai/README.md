# Gameplay AI guard example

The native **Hold Waypoint** task (node type `83002`) waits after a guard reaches
a patrol waypoint. Place it after a movement task in a Behavior Tree sequence.
Its **Hold ticks** property defaults to 30 and accepts 1 through 600. A task
started at tick 10 with the default completes at tick 40. The fixed simulation
clock drives the wait and the task releases its state on cancellation or
completion.

Copy this directory to a writable location. Configure it with the
`HoroEngineGameplay` SDK built for the same compiler, architecture and
configuration as your project.

```sh
rtk proxy cmake -S <example-copy> -B <example-build> -DHoroEngineGameplay_DIR=<engine-build>/sdk/gameplay
rtk proxy cmake --build <example-build> --config Debug
rtk proxy ctest --test-dir <example-build> -C Debug --output-on-failure
```

For a single-configuration generator, set `CMAKE_BUILD_TYPE` to the SDK's
configuration when configuring. The example uses `HORO_AI_NODE` and the SDK's
generated descriptor bundle. The check compiles `Assets/HoldWaypoint.horo_bt`
with the native descriptor and the authored `Assets/Guard.horo_bb` schema.
It encodes and reloads the cooked plan, then runs the graph once per fixed
tick against a frozen Blackboard snapshot.
An engine build with tests enabled can run the same checks from a fresh copy
using `ctest --test-dir <engine-build> -R '^HoroGameplayAiGuardSdkQualification$' --output-on-failure`.
Copy each asset with its paired `.horo` identity file when reusing it.
The schema's **Target visible** key starts at `false`. To inspect that key in a
project running this schema, launch cooked Editor Play with
`--play-ai-inspect-key 83003 1`.

`Assets/PatrolPoints.horo_eqs` generates a three-by-three grid around the
querying agent. The points are one metre apart. The distance stage retains
points within 1.1 metres of world origin and ranks the nearest first, returning
up to four results. Open the asset in the query editor and preview it with a
guard near the origin. Moving the guard changes the grid center. When the query
has no candidates in range, move the guard closer to the origin or adjust the
distance stage. The engine's EQS test suite cooks this asset and exercises both
the successful Blackboard write and cancellation when the Scene changes.

The same module publishes **Guard proximity** through `HORO_AI_SENSE` (sense
`83010`, stimulus `83011`). Its listener reports candidates within three metres
and publishes sensing loss for admitted candidates outside that radius. The
provider uses the host's captured positions and processes at most 32 candidates
per poll. For a cooked Editor Play project, add the sense and its stimulus to
the Scene's native Perception policy and pass its project-relative path via
`--play-ai-perception-policy`. Inspect the selected agent's stimuli in the Game
panel. If no stimuli appear, check the listener's sense ID, layer and mask,
and whether the policy's candidate radius reaches the target.

Use `Assets/RecoverTarget.horo_bt` for the recovery branch after losing a
target. Keep `Assets/Scripts/RecoverTarget.horo_script` and its `.meta` sidecar
under the project's scripts folder. The Editor discovers the Lua node
automatically. The script reads **Target visible** from the same Blackboard
schema as the wait task. It succeeds after three Decision updates by default,
so the caller can resume patrol. If the host sets the key to `true` at
BlackboardSync during that window, the recovery branch fails and a reactive
selector can give pursuit priority. The project's Perception-to-Blackboard
policy supplies the key.

For the pursuit example, attach **Guard target beacon** to a root target
entity and **Guard pursuit** to the guard. Copy the Lua behavior and its
`.meta` sidecar together. Set the same `channel` on both behaviors and use
one beacon per channel. Run them in a host that grants Navigation movement
to the guard under `game.example.guard` and supplies the current target
observation. The beacon publishes
the target's live entity generation and position after each fixed update.
The Lua behavior submits one follow request, retains its opaque handle, and
cancels when the signal stops or its target generation changes.
The Navigation host test loads the shipped behavior on a real runtime Scene
and exercises request admission and cancellation.

The cover sample pairs each captured cover handle with Navigation
reachability and Physics visibility from the same Scene publication.
`host/GuardCover.cpp` filters out exposed, unreachable and occupied points,
chooses the lowest Navigation cost, and reserves the winner on the Scene owner.
Equal costs favor the lower authored cover ID. Retain the returned receipt
while moving to the selected point. Release that exact receipt if movement
fails, the task is cancelled, or the guard leaves cover. When no point is
selected, inspect reachability, exposure and the current guard reservations.

With the engine configured using `HORO_BUILD_EXAMPLES=ON`, run the
deterministic cover fixture. It demonstrates the selection policy with
captured sample facts. The native Navigation and Physics integration test
also runs the same helper against actual provider results.

```sh
rtk proxy cmake --build <engine-build> --target HoroGuardCoverSample
rtk proxy <engine-build>/examples/HoroGuardCoverSample
```

If the node is unavailable, verify that the gameplay module built successfully
and that the loaded module matches the project's SDK fingerprint. In cooked
Editor Play, select the guard and open the Game panel's **AI agent inspector**.
Use `--play-ai-debug` to enable single-step and observe when the task leaves the
running state. Re-cook the decision graph after changing its task configuration.

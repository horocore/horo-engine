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
Copy each asset with its paired `.horo` identity file when reusing it.
The schema's **Target visible** key starts at `false`. To inspect that key in a
project running this schema, launch cooked Editor Play with
`--play-ai-inspect-key 83003 1`.

If the node is unavailable, verify that the gameplay module built successfully
and that the loaded module matches the project's SDK fingerprint. In cooked
Editor Play, select the guard and open the Game panel's **AI agent inspector**.
Use `--play-ai-debug` to enable single-step and observe when the task leaves the
running state. Re-cook the decision graph after changing its task configuration.

# XR Simulator development sample

This is a synthetic developer workflow. It does not discover OpenXR, require a
headset, or qualify a physical runtime, hand provider, renderer or GPU. The sample
has an explicit tracking origin (object 1), a Lua-driven cube (object 2), and a
light. It deliberately has no authored camera: the Game view uses an actual
consumed simulator view.

Build `HoroEditor` and `HoroXRSimulatorSample` in an existing configured build.
Copy this directory to a new writable project directory before recording or
editing it. In a single-configuration build, launch:

```sh
rtk proxy <build>/apps/HoroEditor --project <project-copy> --renderer opengl --xr-simulator --xr-tracking-root 1
```

Select **XR Simulator** in the workspace and start Play. The simulator's movement
control enters the real `gameplay.move` Axis2D action in `editor.workspace`. The
sample Lua behavior reads that committed action and submits canonical Scene
transform commands. Movement is applied once per fixed update; it is deliberately
a simple interaction sample, not a physical motion model.

Use the normal Play pause/resume and single-step controls from the simulator
panel. Edit head position/orientation to change the consumed Game view. Controller
pose editing and anatomical hand-joint authoring are independent from scalar
movement; the simulator invents no gestures. **Lose focus**, **Lose tracking** and
**Lose session** expose lifecycle failures. Missing evidence hides the previous
Game texture. **Reset simulator session** creates a fresh session and rebinds the
actual Input coordinator; stop and restart Play to reset the gameplay Scene.

To replay, enter `Scenarios/synthetic-movement.json` or
`Scenarios/recorded-movement.json` as the project-relative scenario and import it,
then select replay. Both use the same bounded codec and real admission path.
Sequential cold session replacements require an actual Input opportunity between
them so the outgoing source's neutral state is committed. Separate Editor clicks
naturally cross that cutoff; the headless runner explicitly drives one real
RuntimeHost frame after import while the imported simulator remains paused.
After the last step, synthetic Input becomes neutral and the consumed preview
becomes unavailable. Reset the simulator session to return to live controls.

To record, select **Record interaction**, change controls while Play runs, then
**Finish recording** and **Export synthetic recording** before stopping Play.
The output is under `.horo/xr-simulator/captures/` in that project. A partial
capture remains labeled partial in its artifact; cancellation exports nothing.
Project-relative imports use the retained project source capability. Exports use
exclusive staging and atomic replacement in that fixed derived namespace. No
native pose/history capture or consent grant is available.

Run the same project without the Editor, SDL or a renderer:

```sh
rtk proxy <build>/apps/HoroXRSimulatorSample <project-copy>
rtk proxy <build>/apps/HoroXRSimulatorSample <project-copy> Scenarios/synthetic-movement.json
```

The runner drives one real RuntimeHost, Scene service, shared simulator host and
Lua gameplay composition. It reports the actual cube position and writes a
synthetic capture to the same derived namespace. It runs a finite 32 opportunities;
EOF remains neutral rather than replaying the last scalar value.

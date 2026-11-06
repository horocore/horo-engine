# Editor improvements delivery evidence

This log records scoped delivery evidence for the shared `codex/editor-improvements`
branch. Local builds, hosted quality checks, manual Editor interaction and native
device qualification are separate evidence streams.

## HORO-2207 / #2268 / XRA-007.9

Published implementation:
[`32c40bceef6701241607e5d9702dd58bad2c8f66`](https://github.com/horocore/horo-engine/commit/32c40bceef6701241607e5d9702dd58bad2c8f66).

The headset-free workflow uses the canonical fake XR lifecycle, bounded simulator
controls, anatomical hand data, failure scenarios and synthetic interaction
capture/replay. The Editor and terminal sample share actual Scene, Input and Lua
composition. Project-relative artifacts retain the admitted source capability;
capture publication uses exclusive staging and atomic replacement.

See the [runnable sample](../../examples/xr-simulator/README.md) and
[contract migration](xr-simulator-contract-migration.md).

### Local validation

- The Mac Debug build completed for the Editor, terminal simulator sample, owning
  regression targets and public-header consumers.
- Fresh case executions passed: workflow 9, artifact publication 10, simulator 26,
  Play controller 36 and network host integration 54. There were no failed or
  skipped cases in those runs.
- Forty UI component cases carry their earlier passing result because the actual
  executable SHA-256 was unchanged; these were not rerun in the final batch.
- The actual terminal executable ran in three independent writable project copies.
  Interactive input moved the Lua-controlled actor to `(16, 0, 0)` in 32 frames;
  three recorded steps produced `(1.5, 0, 0)` and three synthetic steps produced
  `(0.75, 0, 0)`. EOF remained neutral. Exported captures retained their synthetic
  developer provenance and bounded step counts.
- The final formatting delta changed whitespace in three runtime files and sorted
  two includes in a standalone header consumer. Runtime evidence carries through
  the verified whitespace-only changes; the header consumer was compiled afresh.
- Twenty-seven compiler-cache regression cases passed. Consumer operation preserved
  MAIN cache file contents and counters; ccache may still refresh LRU timestamps.
- Clang-format 19 accepted 92 C++ files. Local Lizard measurements found no new or
  changed function above cyclomatic complexity 10 across 549 measured functions.
- A separate GUI-off/live-UI-off CMake configuration succeeded and retained the
  sample/workflow targets. Its sample link dependency rule excludes GUI, SDL,
  concrete renderer and OpenXR libraries. No fresh GUI-off executable link was run;
  the actual CPU executable above came from the GUI-enabled build profile.
- The normal commit hooks passed. Unrelated user staging and stashes were retained.

### Limits

These results qualify the CPU simulator workflow. They do not establish manual
Editor interaction, physical-headset, native hand-provider or GPU qualification.
Local managed Sonar was waived by the user. Lizard metrics are not a complete
Codacy analysis or a hosted quality-gate result. Hosted checks remain independent.

## HORO-1749 / #1792 / VFX-007.2

The typed spawn pipeline connects admitted native/Lua gameplay and the unchanged
cinematic event registration contract to the same canonical Scene-owned VFX World.
Bindings are strict data, asset availability and scalar schemas are checked cold,
and hot reload publishes only at a safe source boundary. Committed fixed attempts
retain their original tick/delta until actual Scene transaction receipts resolve.
Expected effect-capacity outcomes remain observable without shutting down gameplay.

See the [contract and SDK migration guide](vfx-spawn-pipeline-migration.md).

### Local validation

- The macOS Debug build completed for the owning libraries, eight regression
  executables and standalone public-header targets. Libraries used at most two
  compile jobs; executable links and execution were serialized.
- Qualified case executions: codec 21, source sealing 11, scoped context 16,
  cinematic integration 11, host commit/abort 5, native/Lua pipeline 6,
  configured product integration 1 and existing gameplay runtime 74: **145 passed,
  zero failed, zero skipped**. The final configured-only fixture changes preserved
  the exact binaries and production inputs of the other 144 passing cases.
- The configured product test cooks a real project, activates canonical native
  Physics and two independent VFX-selected Worlds, drives real scheduler catch-up
  and Scene publication, observes extracted particles, exceeds the source queue's
  per-cutoff count across successive ticks and retires one World while the other
  continues. It retains the old immutable extracted frame.
- Real Lua allocator failures exercise userdata construction/finalization and
  shared-owner release. A separate real VM exhaustion after emission verifies
  exact-attachment cancellation. Prior sealed work, other entities and independently
  committed cinematic commands retain their source facts.
- A fresh external build-tree optional Vfx SDK consumer configured, compiled,
  linked and ran successfully against boundary 29. Trusted host/Scene/cinematic
  composition headers remain outside that exported client component. Two further
  public-header smoke executables passed; the module-host contract target compiled.
- The earlier 130 core VFX cases retain their recorded passing result with the
  exact executable unchanged; they were not rerun in the final batch.
- Clang-format 19 accepted the actual C++ delta. New and changed functions were
  measured with local Lizard and kept at cyclomatic complexity 10 or below.
- Consumer builds and the external SDK check preserved the 412 MAIN cache files'
  contents and counters. Cache LRU timestamps may change; no cache hit-rate or
  comparative speed measurement is claimed.

### Limits

This is macOS owning-library/test qualification. It does not establish a fresh full
Editor executable link, interactive Editor behavior or GPU/backend pixel/performance
acceptance. The broader Network suite was not qualified: its bounded earlier run
expired, and a targeted stack sample showed active canonical Physics executable
hashing rather than demonstrating a deadlock. No full-suite success is claimed.

Local managed Sonar was waived. Format and raw complexity measurements are not
full Codacy, coverage, hosted-gate or vulnerability clearance. Cold preparation,
Lua VM allocation and Scene command/receipt allocation retain their separate
allocation scopes described in the migration guide.

## HORO-394 / #394 / RND-013.3 — scene color integration ready for review

The backend-neutral contracts now feed a genuine Metal Editor graph: unexposed
FP16 mesh shading, bounded GPU histogram/adaptation, optional AP1 creative LUT,
pinned analytic ACES 2 SDR100 output, display-linear grid/selection and final SDR
encoding. Both genuine forward and clustered lighting families are integrated.
Requested unsupported profiles and incompatible project restarts fail explicitly.

Local macOS Debug evidence, kept as separate scopes rather than an aggregate:

- 18 producer unit tests and actual pinned OCIO analytic/look data generation.
- 11 look-loader JUnit entries, including original two-mode producer data;
  307 frontend entries including 901/1024/1025 retention boundaries; 108 captured
  shader-reflection entries and 12 viewport resource entries: zero failures/skips.
- Four full viewport native cases cover both lighting recipes, raw/final pixels,
  HDR values, both nonidentity LUT modes and selection/grid depth/meter separation.
  Six common-owner native cases cover real upload readiness, cancellation, reset,
  in-flight resize, signed/HDR identity LUTs and next-frame GPU adaptation. All pass
  with Metal validation and zero skips.
- Actual Editor runs opened a Metal project and its default scene with one mesh.
  Clustered/trilinear and forward/tetrahedral each completed 60 presented frames
  and 52 accepted color/exposure generations. An incompatible OpenGL project
  returned the expected typed profile-preservation denial.
- All 125 changed C++ paths pass format19; all 760 new/changed functions have
  cyclomatic complexity at most 10. MAIN cache contents/counters remain unchanged.

See [current integration, API migration and limitations](scene-color-pipeline-migration.md).
The implemented output is an explicitly diagnosed unmanaged SDR preview; physical
calibrated HDR/PQ/EDR and hardware device-loss injection are not qualified. Local
managed Sonar is waived; no full Codacy, coverage, hosted-gate or vulnerability
clearance is claimed. Earlier exposure-component evidence remains scoped to its
original source and native fixture.

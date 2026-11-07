# Character property and malformed-input qualification

`CharacterPropertyFuzzTests.cpp`, `CharacterGeometryPropertyTests.cpp`, and the
property group in `CharacterSceneSourceTests.cpp` use the test-only
`CharacterPropertyCorpus.h`. Each group runs 48 recipes for each SplitMix64 seed
`0x994`, `0x434852`, and `0xdeadbeef` (144 recipes). The generator algorithm and
integer-to-float mapping are explicit and independent of platform random engines.

| Group               | Generated behavior                                                                                 | Rejection / invariant                                                                                                                                                                                                                                         |
|---------------------|----------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Descriptor          | Capsule dimensions, position, slope, contact capacity and crouch profile                           | Nonfinite and extreme/negative/zero dimensions, zero basis, invalid bindings, capacity overflow, unknown policy and malformed scene identity return the exact typed category.                                                                                 |
| Request             | Positive tick/correlation, controller identity, finite velocity and explicit heading               | Zero order, nonfinite intent, nonunit heading, malformed handle and unknown stance return the exact typed category.                                                                                                                                           |
| Locomotion snapshot | Correlated movement and transform identities, ticks, revisions and geometry                        | Mixed ticks, zero revision, unbounded contact count, invalid geometry, nonfinite publication and unknown discriminators fail before indexing active prefixes.                                                                                                 |
| Durable component   | Disabled valid authored component with varied identity/capacity                                    | Past and future real Character component schema versions reject through the canonical validator.                                                                                                                                                              |
| Scene wire          | Configured Character, generated capsule/capacity/identity, full value and canonical-byte roundtrip | Scene schema 1 containing Character, past/future component schema, malformed type/vector/asset, oversized capacity, missing geometry and truncated bytes fail through the canonical decoder.                                                                  |
| Analytic wall       | Production CharacterWorld fixed tick through the public capsule-query seam                         | Capsule stays behind a wall, tangential travel survives, results remain finite and bounded, two fresh worlds replay identical evidence; cancellation, malformed count/normal, stale query revision and finite extreme velocity preserve the last publication. |
| Canonical wall      | Actual PhysicsRuntime Canonical world, query fixture and CharacterPhysicsQueryAdapter              | Same movement/replay bounds using real native capsule queries; stale exact Physics publication revision rejects without changing committed Character state. Compiled only with `HORO_BUILD_PHYSICS_NATIVE=ON`.                                                |

The wall oracle generates geometry evidence only; the production Character solver
owns all movement, contact reduction, state transitions, and publication. No
alternate controller simulation or backend is introduced. The native group makes
an independent real-backend claim; the analytic group is backend-neutral seam
coverage. Each movement recipe executes two fresh worlds, one successful tick and
one rejected successor per world, with small immutable work/storage budgets.

On failure the harness re-evaluates at most 128 candidate recipes, shrinking one
integer field toward zero while preserving the original failure message/category.
It prints stage, original seed/case and the complete reduced eight-word recipe.
Replay with `HORO_CHARACTER_REPLAY='word0 word1 word2 word3 word4 word5 word6 word7'`
and the exact reported Catch2 test name. No filesystem corpus, wall clock, mutable
random state, environment-specific RNG or external service controls generation.
The minimizer and generator replay have a separate synthetic boundary regression.
The reducer is bounded and does not claim a globally smallest counterexample.

CTest entries `HoroCharacterPropertyFuzzTests` and
`HoroCharacterWirePropertyFuzzTests` select only these groups, serialize execution,
and set a 90-second process timeout. Each test binary runs in its own process;
a native fault fails that invocation and the watchdog bounds a stuck invocation.
Inputs are bounded before reaching native code: arbitrary bit patterns are used
only at canonical validation boundaries, never unchecked native geometry calls.
This is deterministic finite-corpus qualification, not an unbounded coverage-guided
fuzzer, sanitizer run, or proof against every crash.

The existing public Character state is an immutable locomotion publication.
`CharacterWorld` has no canonical checkpoint byte encoder/decoder or aggregate
restore operation. This work tests genuine durable Scene/component schema skew
and state-publication invariants; it cannot validate HORO-981 checkpoint format,
restore rollback, delta history or state-hash compatibility before those exist.
Platform/build/backend execution evidence belongs to the delivery validation log;
compilation of a native-gated case alone does not establish its execution.

## Local execution receipt (2026-10-31)

Executed on macOS arm64, Debug, `BUILD_TESTING=ON` and
`HORO_BUILD_PHYSICS_NATIVE=ON` in the existing `build/dev-editor` composition:

- `cmake -S . -B build/dev-editor`: passed after the concurrently owned Prefab
  settings target topology was made coherent; public-header/dependency guards remained enabled.
- `cmake --build build/dev-editor --target HoroPhysicsTests HoroSceneSourceTests --parallel 2`: passed.
- `ctest --test-dir build/dev-editor -R '^HoroCharacter(Property|WireProperty)FuzzTests$' --no-tests=error --parallel 1 --output-on-failure`:
  both watchdog invocations passed, 0.60 seconds total. Physics executed seven
  test cases (3,029 assertions); wire executed one generated test case. Seven
  generated groups completed 1,008 recipes in total, including real Canonical queries.
- The emitted reduced native recipe `0 0 0 0 0 0 0 0` replayed successfully in
  the exact native test (15 assertions), under a 90-second subprocess timeout.
- CTest JSON inventory confirmed both entries select their actual binary with
  `[character-property]`, `RUN_SERIAL=true`, and `TIMEOUT=90`.
- Changed C++/header clang-format verification and `git diff --check` passed.

The reducer was exercised by a genuine test-fixture failure: a native Physics
world initially had no published tick, then rejected a mismatched setup quantum.
The final fixture advances the real admitted 16,666,667 ns Physics quantum before
capturing its query publication. Those setup corrections changed no production
code or expected controller behavior. The first reduced failure and final replay
were retained in external validation logs, not committed generated output.

This receipt does not claim Linux/Windows execution, a native-disabled build,
sanitisers, unbounded fuzzing, GPU tests, full unrelated unit suites, hosted gates,
or checkpoint/restore coverage. Quality preflight results are reported separately
by the owning delivery task.

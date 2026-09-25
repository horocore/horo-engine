# Cinematic Foundation Contract Qualification

This is the qualification record for [CIN-001.7 #1715](https://github.com/horocore/horo-engine/issues/1715). It covers the implemented identity, source schema, cook, scalar sampling, transform and property tracks, and the foundation evaluation seams. Later playback, authoring, camera and event capabilities have separate delivery tickets.

## Contract matrix

| Contract | Executable evidence | Pass condition |
|---|---|---|
| Stable identity and byte order | `CinematicIdentityTests.cpp` | Typed domains remain distinct; generations do not wrap; non-palindromic 12-byte network-order vectors decode and re-encode exactly, independent of host endianness. |
| Hostile source and path independence | `SequenceAssetTests.cpp` | Duplicate and unknown fields, malformed and oversized JSON, excessive depth, invalid UTF-8, embedded NUL, invalid integer encodings, noncanonical UUIDs, POSIX, Windows and UTF-8 path-shaped references fail with typed errors. Persisted references remain stable `AssetId` values. |
| Cook admission and bounds | `SequenceAssetTests.cpp` | Compact, Standard and Large profile limits; reference availability/type/cycle; aggregate track, key, reference and depth ceilings; repeated cook plans are equal. |
| Random-access deterministic sampling | `CurveSamplingTests.cpp` | Exact key time, mapped time and segment metadata; fixed-fixture repeated and reordered replay results are equal in one executable. Numeric cross-platform equality is bounded by declared tolerance, not bitwise identity. |
| Transform/property plans | `TransformTrackTests.cpp`, `PropertyTrackTests.cpp` | Stable track ordering, scene-generation fencing, root/child origin handling, typed binding validation and random-access output remain valid after different seek histories. |
| Frame evaluation | `SequenceEvaluationTests.cpp` | Stable player order, exact rational advancement, directed endpoint/loop/turn events, failed-boundary atomicity and capacity rejection. |
| Frame-hot allocation | `CurveSamplingTests.cpp`, `TransformTrackTests.cpp`, `PropertyTrackTests.cpp`, `SequenceEvaluationTests.cpp` | Prepared sampling/evaluation windows make zero C++ `operator new` calls; the curve replay fixture covers 4,096 reordered samples after setup. |

The allocation probe counts `operator new` calls in the test process. It does not measure native allocators, total process bytes, time, or GPU work. The budget is zero counted calls in the named prepared windows. The separate `HoroCinematicCurveSamplingBenchmark` is a Release-only timing tool with 4,096 keys, 4,096 samples per batch, 1,000 measured batches and a 750 ns/sample P99 budget. Its result is machine-specific and is not inferred from unit tests or debug builds.

## Platform qualification

| Platform | CI evidence on the PR head | Status |
|---|---|---|
| Linux / GCC | `Test · Linux / GCC` runs both cinematic test targets through the full suite. | Pending hosted run. |
| macOS / Clang | `Test · macOS / Clang` runs both cinematic test targets through the full suite. | Pending hosted run. |
| Windows / MSVC | `Cinematic Foundation · Windows / MSVC` builds and runs both targets in a focused job, because the full Windows matrix is disabled. | Pending hosted run. |

Run the focused local pass with `cmake -S . -B build/skeleton -DBUILD_TESTING=ON`, `cmake --build build/skeleton --target HoroCinematicModelTests HoroCinematicRuntimeTests --parallel 2`, and `ctest --test-dir build/skeleton -R '^HoroCinematic(Model|Runtime)Tests::' --output-on-failure -j 2`. The local result qualifies only its own compiler and platform. Hosted pass status must be checked against the final PR head SHA.

Local evidence (2026-09-26, Linux x86-64, GCC 15.2.0, headless `build/skeleton`): configure with `BUILD_TESTING=ON`, editor GUI/OpenGL/OpenTelemetry off succeeded; both cinematic test targets built with `--parallel 2`; the focused CTest selection passed 116/116 with `-j 2`. The allocation assertions passed inside that test selection. This was a default local build, so no Release benchmark timing result is claimed.

## Deviations and follow-ups

No deviation from the implemented foundation contract is accepted by this record. If a platform lane or bounded test fails, mark its matrix row unqualified and fix it or file a named follow-up with an owner before claiming all-platform qualification. Full Windows suite re-enablement remains outside this focused foundation contract; the focused Windows lane above supplies the required MSVC evidence for this ticket.

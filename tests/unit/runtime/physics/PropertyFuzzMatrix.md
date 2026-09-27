# Physics property and malformed-input matrix

`PhysicsPropertyFuzzTests.cpp` is a bounded, deterministic unit suite. It uses
SplitMix64 with fixed seeds `0x922`, `0x504859`, and `0xdeadbeef`; each seed runs
32 cases per descriptor/asset group. The native lifecycle group runs eight cases
per seed. Generated inputs use no wall clock, platform RNG, or external corpus.
A failure prints the seed and case; asset cases also print the asset ID,
subresource, source context and stage, and lifecycle cases print the world
generation, body target, sequence and stage. Replay with the `[property]` Catch2
tag and find the reported seed and case in its output.

| Boundary | Generated valid path | Injected failure | Pass threshold |
| --- | --- | --- | --- |
| World descriptor | Finite gravity within 16 m/s² and positive bounded delta | NaN gravity; arbitrary 32-bit float gravity | All 96 valid cases pass; all NaNs fail; arbitrary values match an independent finite/magnitude oracle. |
| Authored body | Dynamic mass and velocities within CanonicalV1 limits; resolve into exact world/shape | Infinite linear velocity; arbitrary 32-bit float mass | All 96 valid cases validate and resolve unchanged; infinities fail; arbitrary masses match an independent finite/range oracle. |
| Convex asset | Closed cube with varied translation and extent; cook and load; rename diagnostic context without changing identity or payload | NaN at varied vertex, truncated payload at varied byte count | All 96 valid sources cook/load and keep identical bytes after context rename; all source failures return `ShapeCookSourceInvalid` with source, asset and vertex; all truncated loads return `ShapeArtifactInvalid`. |
| Triangle asset | Two-face quad with stable material and subshape IDs; cook and load; rename diagnostic context without changing identity or payload | Out-of-range index at varied face/corner, flipped payload byte | All 96 valid sources cook/load and keep identical bytes after context rename; all source failures return `ShapeCookSourceInvalid` with source, asset and triangle; all corrupted loads return `ShapeArtifactInvalid`. |
| Canonical lifecycle and affinity | Active world with varied body target and producer sequence, queued command, reset to a new generation, unload | Zero producer sequence, worker-thread admission, old-generation command, admission after unload | All 24 cases reject malformed and foreign-thread commands without queue growth, clear the queue on reset, reject the old generation, and keep unload idempotent. |

The suite stops on the first assertion failure and has zero permitted unexpected
acceptances, mismatched error codes, or input mutations. Each generated collection
stays small: eight hull vertices, four mesh vertices, two triangles, one command,
and one world per lifecycle case. It is a regression sampler, not an unbounded
fuzzer or a proof of memory safety. Sanitizer and larger fuzzing jobs can use the
same seeds and invariants separately. The lifecycle group compiles only when
`HORO_BUILD_PHYSICS_NATIVE` is enabled; portable descriptor and asset groups run
in headless compositions without the native solver.

The source cook errors are required to carry the asset, source context and specific
vertex/triangle. Artifact integrity errors identify the failed stage; the test
diagnostic supplies the asset and seed. Lifecycle assertions print the world
generation and body target. This suite does not add a world or asset identifier
to low-level error messages that currently have only a stable error code; callers
that present those errors must retain their owning operation context.

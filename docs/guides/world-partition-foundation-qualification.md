# World Partition Foundation Qualification

## Purpose

This guide records the qualification boundary for the WST-001 world-partition
foundation. The qualification composes the existing backend-neutral contracts; it
does not add a streaming scheduler, cell activation implementation, cooking
pipeline, editor authoring UI, or native backend dependency.

## Prerequisites

- A configured Horo Engine build with `BUILD_TESTING=ON`.
- The headless `HoroWorldStreamingTests` target.
- The public World Streaming headers and their declared Horo module dependencies.

## Qualification matrix

`WorldStreamingFoundationQualificationTests.cpp` covers the parent capability
acceptance boundary:

| Contract | Qualification evidence |
| --- | --- |
| Stable identities | World, cell, source, and generation values round-trip through canonical bytes; non-wrapping generation advances and exhaustion are checked. |
| Deterministic spatial ownership | Negative coordinates, exact half-open boundaries, and an absolute coordinate near the signed 64-bit limit produce stable floor-quantized cells; subtraction overflow is typed. |
| Immutable descriptor and registry | A validated descriptor is published into a generation-pinned snapshot; inclusive bounded queries are ordered, replacement fences old handles, and capacity failure leaves caller output unchanged. |
| Authored references | Stable page/object revisions are assigned to canonical cells; hard dependencies form a co-load bundle while a soft reference remains deferred, and a stale revision is rejected. |
| Capability and baseline provider | Exact packaged settings are admitted against one capability revision; version-skewed and unsupported package requests fail, while Null and SingleCell fallback modes preserve explicit non-streamed behavior. |

The focused unit suites remain the detailed contract coverage for malformed
descriptor fields, all lifecycle branches, dependency-cycle policy, and registry
concurrency. The qualification test proves that their public boundaries compose
without editor paths, live pointers, or native API types.

## Running the qualification

From the repository root:

```bash
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --target HoroWorldStreamingTests --parallel 2
ctest --test-dir build/skeleton -R 'HoroWorldStreamingTests::World streaming foundation' --output-on-failure
```

Run the complete headless World Streaming target when reviewing the full
foundation test set:

```bash
ctest --test-dir build/skeleton -R '^HoroWorldStreamingTests::' --output-on-failure
```

Expected result: the qualification cases pass without enabling a display server,
GPU, physics backend, navigation provider, or editor process.

## Ownership and limitations

World Streaming owns identity, topology, registry publication, spatial policy,
and fallback demand. The descriptor and dependency-plan values are inert and do
not perform I/O, registration, provider selection, Scene mutation, or lifecycle
callbacks. Registry snapshots own their immutable publication independently from
later replacement; handles are meaningful only through the snapshot that issued
them. The fallback provider exposes desired input only and never claims that a
cell is Loaded, Resident, or Active.

This qualification does not prove asynchronous scheduling, provider activation,
cooked byte production, editor authoring workflows, or cross-platform CI matrix
results. Those remain the responsibility of their owning WST capabilities and
hosted validation.

## Validation Record

On 2026-09-21, Linux x86_64 with GCC 15.2.0, CMake 4.4.3, and the Release
configuration completed:

- `cmake -S . -B build/skeleton -DBUILD_TESTING=ON`
- `cmake --build build/skeleton --target HoroWorldStreamingTests --parallel 2`
- `ctest --test-dir build/skeleton -R '^HoroWorldStreamingTests::' --output-on-failure` (299/299 passed)
- `ctest --test-dir build/skeleton -R 'HoroWorldStreamingTests::World streaming foundation' --output-on-failure` (3/3 passed)
- `build/skeleton/tests/HoroWorldStreamingTests "[qualification]"` (93 assertions in 3 cases passed)

CTest discovers Catch2 cases as `HoroWorldStreamingTests::...`; the unqualified
aggregate name is not registered. Local Linux evidence does not replace the
hosted Windows/macOS matrix.

## References

- [World Streaming Architecture](../architecture/runtime/world-streaming-architecture.md)
- [World Partition Capability Profile Migration](./world-partition-capability-profile-migration.md)
- [World Partition Registry Snapshot Migration](./world-partition-registry-migration.md)
- [World Spatial Object Descriptor Migration](./world-spatial-object-descriptor-migration.md)
- [Repository Working Contract](../../AGENTS.md)

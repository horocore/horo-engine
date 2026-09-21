# Physics Reference Scene Corpus

## Purpose

This guide defines the reproducible Physics reference fixtures for PHY-009.4. The
corpus is a small backend-neutral observation manifest used by
`HoroPhysicsTests`; it locks event lifecycle ordering and query identity ordering
without exposing Jolt types or native handles. Native query execution is an
additional qualification check when the private CanonicalV1 solver is enabled.

## Prerequisites

- A C++20 build with the `HoroPhysicsTests` target and Catch2 test registration.
- The headless Physics target, which does not require a window or renderer.
- The private CanonicalV1 Jolt dependency only for the optional native query case;
  it is pinned to Jolt v5.6.0 by `cmake/HoroPhysicsDependency.cmake`.

## Workflow

### 1. Prepare

The manifest lives in
`tests/support/PhysicsReferenceSceneCorpus.h` and is intentionally test-only.
Each entry has a stable scene ID, corpus version, required capability, support
status, bounded observation fields and a fixed 64-bit fingerprint. The fixture
tests are in `tests/unit/runtime/physics/PhysicsReferenceSceneCorpusTests.cpp`.

The current matrix is:

| Scene | Current outcome | Fixture and exact observation |
| --- | --- | --- |
| `contact-lifecycle` | Supported | `ContactBegin@1`, `ContactPersist@2`, `ContactEnd@3`; Horo event projection, `WorldCreation` admission context |
| `trigger-lifecycle` | Supported | `TriggerEnter@1`, no event at tick 2, `TriggerExit@3`; Horo event projection, `WorldCreation` admission context |
| `ccd-tunnelling` | Expected unsupported | Requires `RigidBodies`; no observation is allowed when that capability is unavailable |
| `stacking-and-sleep` | Expected unsupported | Requires `RigidBodies`; no observation is allowed when that capability is unavailable |
| `fixed-joint` | Expected unsupported | Requires `Constraints`; no observation is allowed when that capability is unavailable |
| `query-ordering` | Supported | Two closest-first hits: body slot/generation `0/1`, `1/2`, at exact float bit patterns `0x40900000`, `0x41180000`; requires `ImmediateQueries` |

Supported contact and trigger entries qualify the Horo-owned copied-event and
lifecycle projection contract. They do not claim that the current runtime admits
solver rigid bodies. CCD, stacking/sleep and joints remain explicit capability
gates until their owning runtime contracts are implemented and qualified.

### 2. Configure and run

From the repository root, run the Physics test target using the repository's
normal CMake configuration. The focused Catch2 filter is:

```text
[physics][reference]
```

Run the same filter in both the portable/headless composition and the supported
CanonicalV1 native composition when the native dependency is available. Native
execution must not be replaced by a different solver or by an implicit fallback.

### 3. Verify

The test is a pass only when all of the following hold:

1. The manifest version and scene IDs are complete, unique and in canonical order.
2. Every manifest fingerprint equals the hash of its own observation.
3. Supported fixtures match every active observation field exactly.
4. Expected-unsupported fixtures report the declared
   `RequiredCapabilityUnsupported` reason and never produce a fabricated result.
5. A bounded event overflow returns `CapacityExceeded` and leaves the prior
   published tick intact.

The comparison policy is intentionally exact. Enum values, event counts, event
ticks, body slots, handle generations and query distance float bit patterns have
zero tolerance. Fixed-capacity array entries outside the active count remain zero
and are included in the canonical little-endian FNV-1a fingerprint. There is no
epsilon-based pass, learned baseline, callback-order comparison or native-ID
comparison.

Every failure is reported with the stable scene name. Native query failures also
retain the world identity, scene generation and typed Physics error at the owning
boundary; unsupported cases identify the missing capability rather than being
silently skipped.

## Troubleshooting

| Symptom | Check | Recovery |
| --- | --- | --- |
| A supported event fixture changes order | Inspect `PhysicsEventProjection` output and the scene's expected event ticks | Preserve canonical Horo pair ordering and update the versioned corpus only with a reviewed contract change |
| Query distance bits differ | Check the exact fixture pose, shape dimensions, fixed tick and active world generation | Do not add a tolerance; repair the fixture or qualify a new observation schema/profile |
| An unsupported scene reports a result | Inspect the runtime capability snapshot and the scene's required capability | Fail closed and return explicit capability evidence; never fall back to another solver |
| Overflow publishes a partial tick | Check the event overflow policy and prior publication revision | Use `FailTick` for the failure fixture and retain the previous coherent publication |

## Limitations

This corpus is not a cross-platform determinism qualification or a rollback/replay
contract. Those claims require the capability and evidence gates in ADR-088. The
current native fixture path covers only the admitted analytic query path; live
rigid-body, CCD, stacking/sleep and constraint scenes remain unsupported until
their corresponding Physics capabilities and lifecycle contracts are qualified.

## Validation Record

The repository's Physics qualification lane records the platform, compiler,
solver build fingerprint, capability revision and exact corpus version for each
run. Local changes must report which portable and native filters were actually
executed; a missing native lane is not reported as a native pass.

## References

- [Physics Architecture](../architecture/runtime/physics-architecture.md)
- [ADR-084: Canonical Physics solver, units and tolerances](../adr/084-canonical-physics-solver-units-and-tolerances.md)
- [ADR-088: Physics determinism capability and support tiers](../adr/088-physics-determinism-capability-and-support-tiers.md)
- [Testing Architecture](../architecture/delivery/testing-architecture.md)
- [Jolt Physics v5.6.0](https://github.com/jrouwe/JoltPhysics/tree/v5.6.0)

# Character movement qualification

`HORO-952` / `[CHR-002.8]` qualifies the movement contract through
`HoroPhysicsTests`. The continuous-support proof fixes low capsule contacts being mistaken for stairs; no public API changes are required.

The canonical reference corpus in `CharacterMovementQualificationTests.cpp`
uses actual Physics fixtures and the ordinary `CharacterPhysicsQueryAdapter`,
spawn, command admission, fixed-tick resolution and publication path. Each
scene runs in a fresh world twice. The corner reverses collider creation order;
other scenes repeat the same authored geometry. Terminal position tolerances
are 5 mm per component, with replay agreement within 0.01 mm. These are
correctness tolerances, not performance measurements.

| Scene | Authored geometry and independent expectation |
| --- | --- |
| Ramp | 30 degree plane; 0.6 m horizontal travel and matching plane elevation |
| Stair | 0.15 m box riser; 1 m horizontal travel and one riser of elevation |
| Ledge | Finite support ending at x = 0.25 m; 1 m travel followed by 0.04905 m gravity displacement |
| Corner | Perpendicular walls at x/z = 1 m; capsule radius and diagonal skin bound both axes |
| Seam | Coincident coplanar walls; unchanged tangent travel |
| Ceiling | Plane at y = 2 m; upward travel stops below capsule extent and skin |
| Thin wall | 0.02 m thick box; 10 m requested travel stops at the near face |
| Filtering | Thin trigger before endpoint; no physical blocking |
| High speed | 128 m requested travel at the admitted displacement boundary; continuous wall stop |

Every successful native scene validates its locomotion snapshot, grounded state,
complete termination, query count, movement iteration bound and matching
publication revision. Separate native cases cover query exhaustion without
publication, a one-iteration conservative stop, stale Character query-context revisions and
copied snapshots surviving teardown. Null compositions must return explicit
`CapabilityUnavailable` without a spawned transform.

The existing exact headless geometry, ramp, step and airborne oracles additionally
capture metrics on each helper-driven attempted tick. Actual sweep callback counts (plus step landing overlap-clearance callbacks)
must equal captured query counts and stay within the prepared query and movement
iteration budgets. Their existing tighter terminal tolerances, threshold
variants, malformed evidence, rollback and cancellation cases remain applicable.

Run the native corpus and the headless threshold suites after building
`HoroPhysicsTests` in the chosen Physics composition:

```sh
ctest --test-dir build/skeleton -R HoroPhysicsTests --output-on-failure
build/skeleton/tests/HoroPhysicsTests '[movement-qualification]'
build/skeleton/tests/HoroPhysicsTests '[character][geometry],[character][slope],[character][step],[character][airborne]'
```

Native cases are compiled only when `HORO_TEST_PHYSICS_NATIVE` is enabled.
Passing a Null-only composition cannot qualify canonical native movement.

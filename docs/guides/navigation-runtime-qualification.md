# Navigation runtime qualification

`HORO-1266` / `#1266` / `[NAV-004.12]` qualifies integrated headless runtime
behavior using `HoroNavigationRuntimeQualificationTests`. The executable composes
actual Recast/Detour queries and crowd sampling with Navigation's coordinator,
movement and velocity pipeline, the public Navigation/Character bridge, canonical
Character fixed ticks, and the CanonicalV1 Physics query world. Renderer selection
has no role. No test backend, movement callback, synthetic achieved displacement,
or direct Transform assignment substitutes for the movement path.

## Workload and budgets

`RuntimeQualificationSmall` is a bounded regression workload, not a claim that the
entire ADR-109 CrowdSmall scale or a deterministic cross-platform crowd mode has
been qualified. Its single moving Character and three query callers fit below
CrowdSmall's 64 logical/provider agents and 64 avoidance evaluations per tick.
The dimensions are authored from the invariants being tested: two queries per
caller distinguish per-caller service from FIFO, three callers expose rotation,
and sixteen requests per caller exercise eight service rounds and a rejected
seventeenth reservation. They are independent of measured execution speed.

| Dimension | Authored value and oracle |
| --- | --- |
| Query reservations | 64 slots; 16 retained requests per caller; request 17 returns `AdmissionRejected` |
| Query work | 4 jobs, 4 requests/job, at most 8/tick; 2/caller/tick |
| Search budget | 64 nodes/query; 512 nodes/tick, 128/caller/tick |
| Selection work | 4096 candidate probes/tick, compiled bounded ceiling |
| Fairness corpus | 48 grouped requests (16 caller 1, then 16 caller 2, then 16 caller 3); exactly 2 dispatched native queries/caller in each of 8 rounds; repeated Dispatch cannot reset quotas |
| Character | One controller, 4 queued command slots, canonical 60 Hz quantum (16,666,667 ns) |
| Immutable capture | 4 agents, 16 boundary segments, 32 cell entries, 16 neighbor/boundary facts, 16 pair/64 boundary checks |
| Avoidance | Real Detour sampling; radius 0.25 m, 4 m neighborhood, at most 16 neighbors/boundaries, 2 s horizon |
| Movement | 2 m/s, 0.2 m acceptance, 600-tick deadline, at most 16 queries/operation |
| Physics | 16 bodies, 32 shapes, bounded contacts/constraints and 1 MiB scratch |

The test prints platform/architecture, compiler, build mode, sample count and
wall-clock p50/p95 for the velocity commit, and end-to-end query completion
latencies including the test host's polling and publication rounds. Grouped
admission deliberately opposes round-robin service. The test records each real
partition's Foundation JobId at dispatch, waits for that exact terminal fence,
and correlates eventual provider completions back to their dispatch wave and
caller. Every wave must contain exactly two queries for each caller. Partition
service latency comes from the real scheduler's submitted/finished timestamps
and includes queue time plus native execution. Query publication latency comes
from owner consumption, including the stable-prefix delay. This proves
service fairness without misrepresenting stable-prefix publication as per-caller
completion fairness: later caller results must wait behind earlier eligible
admissions, as required by [path batching](navigation-path-batching.md). Timing is
advisory: no wall-clock speed threshold determines correctness. The ten-second
wait is an infrastructure deadline for a dispatched job fence, not a performance
budget or a production blocking API.

`AllocationProbe` measures owner-thread C++ allocation requests/bytes for query
admission and immutable crowd capture separately. Velocity commit must allocate
zero C++ storage, as required by the existing pipeline contract. The measurements
exclude worker-thread and direct C/native allocator calls, resident memory,
fixture construction and Catch assertions. They must not be reported as an
allocation-free query/provider or complete process memory measurement.

## Reference corpus and exact outcomes

| Case | Expected outcome |
| --- | --- |
| Connected 10 m floor, start (1, 0.77, 5), goal (9, 0.77, 5) | `Arrived`, Character root within 0.2 m of goal, actual sampled avoidance while Following |
| Two separated floor polygons | `Unreachable`; Character root unchanged |
| Same connected floor with a Physics-only wall at x=4 | Actual Character side contact, no crossing x=3.75, no fabricated arrival over 180 fixed ticks |
| Goal above root plane, y=1.2 | `OperationUnsupported` from planar velocity admission; no subsequent Character tick or movement |
| 24 alternating retargets then final connected goal | Old operation identities never authorize a new publication; final operation reaches `Arrived` |
| Cancellation before/after native dispatch | `QueryCancelled` exactly once, reservation drained, stale slot generation cannot cancel its replacement |
| Eight alternating connected/disconnected topology replacements after native query completion | `InvalidWorld` because the old read lease is revoked, not a corridor authorized by the replacement topology |
| Scene-generation reload with pending native jobs, old Character grant/controller/agent and bridge work | Old grant revoked, old handles/work rejected, old coordinator completions cancelled and drained, replacement native host reaches `Arrived` |
| Pending work at shutdown | Admission closed, abandoned completions unobservable, all coordinator and provider pins drained |

The floor is at y=0 while the capsule collision root is at y=0.77 (radius 0.25,
half-segment 0.5 and skin). Requested path endpoints retain the exact root height;
navmesh projection does not become a Character root assignment. The off-plane
case deliberately proves that unsupported vertical steering is not silently
flattened. This corpus does not claim to qualify arbitrary sloping portal
corners or off-mesh link execution.

Each tick observes the root before query/safe-velocity/Character admission and
again before Character's fixed tick, then after Character feedback and query dispatch. Position, source tick and publication
revision must remain unchanged across admission. After the canonical tick the
bridge consumes the exact command correlation and feeds achieved movement back
to the same operation/path/tick/Character revision.

## Lifecycle and composition evidence

The host closes the bridge and movement owner, cancels and joins its process
JobSystem, collects coordinator retirement fences, and shuts down Character.
Tests require empty movement/coordinator state, zero resident controllers and
`Closed` Navigation with zero retired worlds. Topology replacement and actual Scene-generation reload retain an explicit external
lease across revocation, verifies the old generation remains pinned, then drop
that lease and require reclamation. Scene reload explicitly retires the old agent
candidate and Character world, replaces the dynamic registry binding, and creates
a new scene-owned coordinator so Character's one-based ticks do not roll back a
previous coordinator's monotonic publication clock. The old coordinator retains
only owned cancellation/completion state until its physical fences drain. This preserves the lifetime fence rather
than weakening it to a logical-revocation check.

The target is registered in the native Windows headless and Linux/macOS default
build/test closures (both providers ON). Linux also runs three explicit omitted
profiles; these are configure, build and actual CTest steps, not merely target
registration:

| CI preset | Navigation | Native Physics | Executed evidence |
| --- | --- | --- | --- |
| `ci-navigation-null` | OFF | ON | Existing Navigation suite including qualification availability |
| `ci-navigation-physics-null` | ON | OFF | Qualification availability and focused bridge/coordinator public consumers |
| `ci-navigation-providers-null` | OFF | OFF | Qualification availability and focused bridge/coordinator public consumers |

The two focused profiles also compile Character/bridge/Physics public-header
consumers as OBJECT targets. These three generated targets have no CTest; only
the qualification cases and two registered direct consumers execute.
Each profile writes its own JUnit report under `build/ci-navigation-*/ctest.xml`,
covered by the existing CI report upload. Empty test discovery is an error.
When either native provider is disabled, movement corpus sources are excluded;
the availability case still calls the actual factories and verifies typed
unsupported/unavailable results. These passes do not qualify native movement.
Report configure, compilation, discovery and executed test results separately.

Native and omitted-provider results are not recorded by this source document.
Delivery evidence must record the exact source freeze, target base, cache
producer base (if reused), commands and terminal results. No performance or
native-pass claim follows from source registration alone.

Navigation world IDs are never reused. Each topology replacement and Scene reload
issues a new world incarnation and binds that exact ID into the real Detour
provider, requests and provenance. A request retaining the old world is rejected
as `InvalidWorld` before topology comparison; this preserves exact rejection
precedence rather than expecting a stale-topology error from an unacquirable world.
The eight replacements still exceed the four-retired-world capacity over time,
with each prior generation reclaimed only after jobs and explicit leases release.

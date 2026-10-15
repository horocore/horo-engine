# Streaming source planner integration

`StreamingSourcePlanner` implements the integrated WST-002 planning boundary. It is
owned by the existing application's `StreamingAuthorityRole`, and may be supplied as
the existing runtime composition's explicit Planner service instance. The host keeps
that unique owner alive through the borrowed composition. No descriptor constructs
or discovers a planner, camera, player, network session, provider or world.

## Ownership and inputs

Create the planner from an owning immutable `WorldPartitionRegistrySnapshot`, explicit
priority/stability/prefetch policies, shape support, positive storage/work ceilings,
and a runtime environment. Enabled layers are copied from the host's current
runtime-layer selection; this is a projection, not another layer-state authority.
Profile, platform bit, gameplay tags and optional exact grid/layer/LOD source selectors
are explicit inputs. A different target grid is filtered, not silently replaced.

Each camera, player, gameplay component, network relevance adapter or preload owner
keeps one move-only `StreamingSourceRegistration`. Player intent uses the existing
Gameplay category. Registration issues a source owner slot/generation. Its destructor
withdraws exactly that generation. Reused slots advance without wrap. No source
registration owns cell residency, entities or replication policy.

`Replace` changes a complete hard source revision, preserves its ID/category and
immediately makes the previous plan unavailable. Closing an owner, replacing the
runtime environment, changing the immutable partition snapshot or shutting down also
revokes the publication before returning. World replacement withdraws all old source
receipts; retaining an old receipt cannot remove a successor generation. A receipt
may outlive an explicitly shut-down planner, but must still be closed on its original
lane. `Close`/`Shutdown` reject foreign-lane calls before mutable storage access.
Destruction/move-assignment on a foreign lane violates the documented lifetime
precondition and terminates instead of racing source storage.

## Motion and publication age

Moving camera/player/gameplay/network owners call `SampleMotion` with canonical
position, velocity and a newer unscaled timestamp. It translates the existing shape
without changing its dimensions or source configuration. A box uses its minimum as
anchor, a path its first point, a sphere its center and a frustum its anchor. Prediction
is opt-in through the registration request and is supported only for existing camera
and Gameplay intents. Predicted demand is Loaded at most and never Pinned.

Motion samples coalesce without cancelling an in-flight cycle. Each source is visited
in stable source-ID order and captures one sample for its direct and predicted
traversals; later samples feed its next cycle. The complete plan retains exact sample
timestamps on source/cell contributions. This is an explicitly bounded-age plan,
not a claim that all sources were sampled simultaneously. The oldest captured sample
bounds publication age. Expired cycles fail typed; expired previous plans become
unavailable. Hosts must choose a plan-age limit and work budgets compatible with their
service cadence. Tiny budgets do not promise completion before any arbitrary deadline.

Preload destinations use hard `Replace`, not motion sampling. Their destination and
readiness revision therefore cannot silently change while a travel request is polling.
A moving camera cannot restart an unrelated fixed preload's planning every frame.

## Bounded service and consumers

`Service` uses independent source-visit, BVH-node, leaf-candidate and stability/output
row budgets. It retains a fixed-stack spatial continuation across calls. Construction
allocates the immutable-manifest-sized dense lookup once; generation stamps avoid
per-cycle whole-world clears. Bounded tracked rows and provenance use preallocated
heaps, with logarithmic work per insertion/removal. Publication is incrementally
canonicalized; no entire-world or entire-source-registry scan is hidden in a quantum.
The storage ceilings bound retained rows, and each budget bounds work even in dense
queries or when no candidates match. The plan sequence advances on each complete
publication, including motion-only cycles.

Priority uses the existing numerical policy, bounded queue age and stable source/cell
ties. Its explicit distance input is exact integer Chebyshev distance from the source
anchor to the cell AABB, avoiding floating-point global-position conversion. The pure
ranking API's numerical behavior is unchanged. Source residency/pin reduction uses the
same allocation-free algebra as the existing owned reduction. Margin evidence is
explicit threshold evidence for existing hysteresis, not an invented metric distance.

Consumers capture a complete borrowed plan only after `Service` returns. The view is
valid until the next owner mutation/service call. Check typed unavailability rather
than retaining old intent during recomputation. Provenance contains only current live
sources. Policy-only linger rows have no priority source or pin floor: they represent
bounded stability retention, not continued authority from a destroyed source. Disabled
runtime layers are removed without linger. The existing scheduler remains responsible
for reservations, I/O admission, actual cell generations, commit and retirement.

## Travel evidence

`PollTravel` performs no wait, I/O, activation or resource acquisition. The host supplies
a bounded canonical `StreamingTravelResidencyEvidence` captured from the current
partition authority at one safe point, including actual exact-generation residency
facts, publication revision and unscaled timestamp. It must never synthesize these
facts from a desired plan. These values are a trusted in-process host projection,
not an authenticated network proof or a second residency ledger. The planner checks
partition/epoch/publication binding, structural generations, ordering and freshness;
the authority producing the projection remains responsible for selecting current
attempt generations.

Loaded travel requires actual Resident or Active evidence; Activated travel requires
actual Active. Missing/loading/evicting facts remain Pending, a failed destination is
Failed, and replaced owner/source/world or stale evidence fails typed. Readiness uses
complete direct source/cell provenance independently of whether hysteresis admitted
all destination demand, so suppressed rows cannot create vacuous readiness.

## Existing callers and qualification

Existing `WorldPartitionRegistrySnapshot::Query` remains synchronous and performs its
full count/write traversals plus canonical sort. Its output ceiling is not a visited
work ceiling. New frame-bounded callers use additive `BeginQuery`/`QueryNext`, retain
the exact owning snapshot and consume deterministic BVH batches. Batches are not
canonical until the caller reduces/orders them. Existing Query callers need no source
migration. Grid/layer accessors borrow immutable snapshot data only.

The new public header belongs to `HoroWorldStreaming` and its real public-header
consumer compiles service/travel operations using only that target's boundary. Native
regressions cover order independence, lifetime/world replacement, filters, actual
residency-ledger readiness, moving-source progress, prediction, bounded queue age,
oscillation and atomic capacity failure. The benchmark is a real default-build CTest qualification on Linux/macOS and the focused Windows suite. Its stdout is retained in the CI JUnit artifact. It records compiler, platform, architecture and build mode with sparse/dense workload measurements; registration does not establish a performance result before execution:

```sh
cmake --build build/skeleton --target HoroWorldStreamingTests HoroWorldStreamingPublicHeaderConsumer HoroStreamingSourcePlannerBenchmark --parallel 2
ctest --test-dir build/skeleton -R '^HoroWorldStreamingTests::' --output-on-failure
build/skeleton/tests/HoroStreamingSourcePlannerBenchmark 256
```

The benchmark constructs equal-size sparse and dense partitions, measures old Query
and equivalent bounded spatial traversal, and separately measures integrated planning.
It emits compiler/configuration, total milliseconds, worst quantum, work counts and
result agreement. Record the actual platform/CPU and output with the delivery evidence;
a benchmark source file or an unrun command is not a performance result.

## Service cadence and bounded-age qualification

There are no production default limits: the host supplies every positive work
ceiling and a maximum age in 1..60000 milliseconds. One Service call is one
host-controlled work quantum; the API does not infer a frame cadence or prove
that a configured age can accommodate an arbitrary query. A profile whose full
cycle exceeds that age fails with PrefetchStale at completion. Plan rejects an
aged previous publication while staging continues; it never substitutes stale
intent. Repeating an incompatible profile does not promise eventual publication.

The 65536-cell benchmark intentionally reuses service timestamp 1000. Its JSON
clockBasis identifies CPU/work measurements, not once-per-render-frame elapsed
time. The measured 1535-quanta first dense cycle would take about 25.6 seconds
at one 60Hz quantum per frame, so it is incompatible with a two-second age bound.
Native advancing 16/17ms-clock regressions separately qualify continuous motion
with a 60-second/128-work profile, fail-closed two-second/128-work behavior with
a real previous publication, and a separate two-second/4096-work profile.
No test changes immutable limits mid-cycle, relaxes freshness, sleeps, or
performs unbounded work in one Service call. Their actual runtime results must
be recorded separately from benchmark CPU timing.

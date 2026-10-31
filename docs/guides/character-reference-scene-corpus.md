# Character reference scene corpus

HORO-993 / CHR-007.6 owns corpus version 1. Its stable, canonically ordered scene
IDs and finite schedules are in `tests/support/CharacterReferenceSceneCorpus.h`.
All geometry is authored in C++ with public Horo descriptors. There are no imported
assets, window, renderer, random generator, mutable baseline or filesystem scene
lookup. The shared native fixture header is the single author of the original nine
movement-qualification scenes and their independent SI golds; the original test
names, filters and boundary regressions remain available.

| Scene ID | Authored expectation |
| --- | --- |
| ceiling | Upward travel stops below the y=2m plane at capsule extent plus skin |
| corner | Two perpendicular walls bound both diagonal axes |
| crowd-query-only | Eight controllers, four crossing pairs over four ticks; each follows its own 1m/s query path |
| dynamic-awake-obstacle | Resident awake dynamic box blocks at x=0.48m; four Character ticks never change body pose/velocity/activity |
| dynamic-sleeping-obstacle | Same obstruction with a sleeping body; Character does not wake or push it |
| filter | Trigger geometry does not physically block travel |
| flat | Four 100ms ticks at 1m/s end at x=0.1/0.2/0.3/0.4m on level support |
| high-speed | 128m requested travel stops continuously at the wall |
| ledge | Finite support ends; airborne displacement follows the authored gravity quantum |
| platform-rotation | Resident native box and predicted query frame carry an attached root through (1,0), (0,-1), (-1,0) XZ endpoints |
| platform-translation | Resident native box and predicted targets carry the root to x=1/1.25/1.5m |
| ramp | 30-degree support preserves 0.6m horizontal travel and matching elevation |
| seam | Coincident coplanar barriers preserve tangent travel |
| stair | 0.15m riser produces one authored step elevation |
| thin-wall | A 0.02m box stops 10m requested travel at its near face |

The corpus bounds execution to eight controllers and four successful ticks per
recipe. Each scene runs in a fresh Canonical world twice; command insertion and
fixture insertion are reversed where meaningful. Logical controller ordinals and
owned-body equality normalize identity, while native handles/world allocation
counters are never golds. Each successful tick checks authored position, grounding,
termination, command sequence, valid copied locomotion evidence and bounded query
work. Replay additionally compares achieved velocity and support membership.
Position tolerance is 5mm; same-build replay tolerance is 0.01mm. These are explicit
correctness tolerances, not cross-platform bitwise determinism or performance
claims.

The platform scenes exercise Character's real native resident-body predicted-pose
query frame, before Physics integration. Host-authored kinematic targets move the
query geometry and Character root; they must not mutate the captured Physics body
or its publication during Character resolution. After each target tick the fixture
explicitly stages a canonical Physics velocity mutation, advances the real native
60Hz Physics tick and refreshes Character's query snapshot before the next tick.
Physics alone integrates the platform to the same authored target, which is checked
independently. Both platform phases use the same 16,666,667ns quantum. Translation
uses 15m/s and quarter-turn rotation uses approximately 94.247rad/s, below the
existing 100rad/s limit. Reusing a prior Physics tick for a later predicted frame
is rejected by the existing freshness contract; no rule is bypassed by this fixture.
Dynamic bodies are real resident Canonical bodies under zero-gravity Physics;
Character sees an obstacle but supplies no solver impulse or contact proxy. The
crowd consists of independent query-only controllers in one Character world;
controller capsules are not peer colliders, so crossing does not imply avoidance.
ADR-090 push/proxy/reaction modes and peer avoidance remain outside the currently
implemented production contract and are not qualified here.

Principal failure coverage shares the existing movement-qualification cases for
one-iteration conservative stops, query exhaustion and stale snapshot rejection.
A cancelled successor query after four committed ticks additionally retains the
whole prior publication and copied state through shutdown. No validator or native
simulator is reimplemented by the oracle.

Configure/build the ordinary `HoroPhysicsTests` target in a supported headless
composition, then select only this bounded group:

```sh
ctest --test-dir build/dev-editor -R '^HoroCharacterReferenceSceneCorpusTests$' --no-tests=error --output-on-failure
build/dev-editor/tests/HoroPhysicsTests '[character-reference]'
build/dev-editor/tests/HoroPhysicsTests '[movement-qualification],[character][platform][frame]'
```

The named CTest entry is serial with a 90-second outer timeout. The manifest always
runs. Canonical scenes compile only with `HORO_TEST_PHYSICS_NATIVE`; a Null-only
composition checks explicit capsule-query `CapabilityUnavailable` and reports
native scene execution skipped. A Null pass is not native movement evidence.
Validation results must name actual platform, build mode and backend, selected
cases, failure/skip reasons and corpus version. A previous unbounded full-Physics
run exceeded its outer timeout; this guide does not claim that suite passed.

Validation: macOS arm64, Debug/Ninja, headless CanonicalV1 native Physics with the
pinned Jolt 5.6.0 backend. The named `HoroCharacterReferenceSceneCorpusTests` CTest
passed 1/1 in 19.31 seconds; actual Catch execution passed 7 cases and 3,164
assertions with zero skips. This includes all 15 recipe generators, the manifest,
explicit Null capability rejection and the selected lifecycle/capacity/cancellation
boundaries. The existing `[movement-qualification],[character][platform][frame]`
groups separately passed 17 cases and 854 assertions with zero skips in 17.34
seconds. The whole Physics suite and native Windows/Linux execution were not run
for this change. These results do not qualify cross-platform bitwise determinism,
peer avoidance or Character-to-body push.

# Blend spaces and instance-scoped synchronization

HORO-467 / ANI-002.6 extends the existing graph compiler and instance. It does not
replace the two-pose `GraphBlendNode`, the state-machine clocks, or clip traversal.

## Authoring and migration

Graph schema 5 / semantic contract 1.3 adds `GraphBlendTreeNode`. Its sole pin is a
Pose Result. Coordinates refer directly to exact Float parameter declarations in
the existing parameter namespace. Curves and invocation overrides therefore use
the same candidate parameter capture as every other node.

Schema 4 / contract 1.2 and earlier admitted source contracts require explicit
`MigrateAnimationGraph` or source `MigrateSupported`. Existing pose/state payloads
retain their semantics. A tree cannot be labeled as an older schema. Current
canonical output changes its version envelope; pre-tree payload fields stay the
same. Existing builders default to the new constants. No second graph format or
runtime discovery path exists.

A tree has 1..32 stable-ID samples. In 1D their x positions must be distinct;
y must be zero. The runtime selects bracketing positions independently of sample
ID order. In 2D an author/cook supplies at most 64 triangles in stable sample IDs.
Compilation rejects absent/repeated vertices, zero-area/duplicate/overlapping
faces, unused vertices, and unsplit T junctions. A disconnected authored mesh is
allowed. Interior points use barycentric weights; an outside point either fails
the complete evaluation or projects to the closest authored edge, according to
`GraphBlendOutside`. Equal-distance ties use authored triangle/edge order.

Weights are nonnegative and normalized. At most two 1D or three 2D samples
contribute to a pose. Canonical sample-ID order determines the successive
normalized SLERP/linear pose composition; it does not depend on source-vector
order. Missing tracks retain the same skeleton-reference sampling and authored
coverage authority as ordinary clips. This node admits absolute clips only;
reference-relative additive composition remains the existing explicit layer API.

## One graph instance, many groups

An explicit `GraphSyncGroupId` is an instance-wide authoring name. The same name
in called definitions deliberately joins their actual distinct player occurrences.
There is no implicit per-call name mangling, global registry, or cross-instance
clock. An absent group ID creates a separate group for that exact tree occurrence.
All authored nodes sharing an explicit name must agree on sync and event policies,
including inactive branches; conflicts fail compilation.

Only positive-weight members in active branches participate. A frozen transition
source retains its actual cursor and cannot lead or follow an advancing group.
The typed `LocalSampleWeightWithStateContribution` leader policy selects the
highest-weight eligible member. Its score uses its normalized local
sample weight multiplied by canonical active state-transition guard contributions;
this does not alter the tree's normalized pose weights. Other pose-layer operators
do not create implicit group weighting; authors can use explicit leader eligibility
when grouping otherwise independent trees. Ties use canonical compiler occurrence
order, then canonical sample ID. `canLead=false` makes a member a follower while
any active eligible member exists; if all active members are followers, the same
stable highest-weight rule chooses the fallback leader.

The leader advances its existing `GraphPlayerState` through `ScaleGraphDelta` and
`AnimationClipAsset::TraversePrepared`. Followers adapt their actual cursor travel
to that leader. A group stores membership, not another time accumulator. Exact
integer phase scaling uses bounded portable 64-bit long division; no floating
clock or platform-specific 128-bit integer is required. Reverse playback and
multiple loops pass through the same atomic traversal/boundary budget.

Synchronized members must use Loop wrapping; a non-Loop clip fails with typed
GraphTypeMismatch. Authors can select `Independent` explicitly to retain ordinary
Once/ClampForever/PingPong behavior. There is no silent change to a clip's wrap
policy. The shared held-PingPong path maps its phase to the actual reverse-half
sample time without changing its cursor or terminal flag.

## Markers and event policy

`AnimationClipData::syncMarkers` is optional immutable clip metadata: at most 256
nonzero semantic marker IDs, strictly increasing exact times in `[0,duration)`.
It is validated by the actual ClipAsset owner and retained with that exact clip
ID/generation publication, not independently injected into the runtime. Marker-empty
clips keep their existing contract and behavior. This is an additive in-memory
metadata extension; there is no new persisted clip envelope or competing codec.
An actual clip revision must change its generation when its markers change, as
for its transform tracks.

Marker synchronization requires at least two markers and the same ordered semantic
ID sequence. Corresponding intervals, including the interval crossing clip zero,
are mapped piecewise. Missing/incompatible sequences use normalized whole-cycle
phase and report `markerFallback` on the actual follower observation.

`LeaderOnly` is the default event filter; `AllActive` and `Suppress` are explicit.
`CopyBlendMembers` returns publication-scoped weights, leader, eligibility, fallback,
sample time, directed advance, alignment, and boundary crossings in caller storage.
A group join/re-alignment is included in actual cursor advance, but its separate
`alignment` amount must be excluded from event emission. This ticket supplies the
filter/traversal contract, not clip notifies: ANI-002.9 owns point/duration events
and their queues. No event is synthesized by reading a pose or member snapshot.

## Preparation, transactions, and lifetime

The compiler admits aggregate sample/player counts, exact clip descriptors,
skeleton coverage, scratch pose slots, and every inactive occurrence. Instance
creation admits complete pose/key-search, geometry, marker/group control work,
and all candidate/publication storage before allocating it. Group membership and
all scratch/publication vectors are fixed at creation. Evaluation, commit, abort,
and copies perform no I/O or dynamic allocation.

Graphs without trees preserve their existing execution stream. A graph containing
trees first stages existing StateBegin controls in parent-before-child order,
then plans groups across the resulting active source/destination branches, then
samples/composes the original pose stream. StateBegin is not run twice. Nested
entry, interruption, retirement and reentry reset/traverse all original tree
players through the same state-owner path.

Every cursor and control write remains a candidate until the existing tick and
cancellation Commit fence. Abort, malformed coordinates, traversal limits, or
cancelled commit preserve the last-good publication and all committed clocks.
Owner thread, exact generation, two-publication residency, transactional Replace,
and Shutdown rules are shared with `AnimationGraphInstance`. Observations have no
independent lifetime or publication authority.

## Verification scope

Focused cases use real skeletons/clips, compilation, instance Evaluate/Commit,
source round trips, shared calls, state transitions/nesting, and publication
copies. Allocation-counter scopes exclude test assertions/setup. These are CPU
animation contracts; they do not claim editor authoring UI, clip-notify dispatch,
or native GPU animation execution. Qualification receipts are reported separately
from authored tests.

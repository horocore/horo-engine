# Prepared animation graph runtime

HORO-463 / ANI-002.2 adds `AnimationGraphInstance.h` to the new narrow
`HoroEngine::AnimationRuntime` public boundary, downstream of `AnimationApi`.
Instance consumers explicitly link `AnimationRuntime`; compiler and clip consumers
continue linking `AnimationApi`. The owning animation test suite and graph public
header consumer now declare the runtime dependency. No source codec, gameplay, Physics,
renderer or platform dependency is introduced.

Hosts prepare immutable graph/skeleton/clip publications outside fixed-tick
execution, assign an exact runtime/component/slot generation, and call `Create`.
Pass the actual asset owner's skeleton publication generation beside the pinned
skeleton, matching the compiled program. The transient clip span is copied into
canonical retained dependency ownership. Existing compiler callers do not change.

For each ordinary attempted fixed tick:

1. Supply typed parameter overrides, fixed delta and player-local millionths rate.
2. Call `Evaluate`; retain the last published handles if staging fails.
3. Call `Commit` only after the enclosing tick succeeds, otherwise call `Abort`.
4. Copy committed local poses into the consumer's own complete storage with
   `CopyPose`; never retain pointers into instance-owned storage.

A rate of 1,000,000 is normal speed; zero holds and -1,000,000 reverses player
traversal without reversing simulation. Signed exact residuals survive successful
rate changes; abort and failure cannot consume time or a fractional remainder.
Authoritative simulation rate belongs to the fixed scheduler, not this rate.

Reload uses `Replace` at the control boundary with the same runtime/component/slot,
the next slot generation and exact replacement snapshots for the same graph ID.
Preparation failure leaves prior state and handles valid. Successful replacement
retires old handles. Shutdown closes mutation but retains the last two committed
pose copies until destruction. Operations and destruction stay on the creator's
thread; there are no worker callbacks or scheduled borrowed references.

Hot operations return `GraphInstanceOutcome` rather than owning diagnostic strings.
Call `DescribeGraphInstanceFailure` outside the hot window when presenting/logging
failures. Clip consumers that also need allocation-free failures can migrate from
`Traverse`/`Sample` to `TraversePrepared`/`SamplePrepared`, checking `HasValue` before
reading the value. `DescribeAnimationClipFailure` retains the original code,
domain, severity and literal operation context. Legacy clip calls still use the
same validators and kernels; their failure projection may allocate as before.

All immutable asset preparation and logical instance storage/work limits remain
explicit. These APIs do not activate scene authoring components or implement new
state-machine, event, root-motion or additive graph semantics. Their owning tickets
must consume this runtime seam and provide their own versioned semantics.

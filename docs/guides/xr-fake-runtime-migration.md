# Deterministic XR Fake Runtime

`Horo::XR::XRFakeRuntime` is an additive XRRuntime public contract for headset-free
host and regression tests. Link `HoroEngine::XRRuntime`; OpenXR, a graphics device,
a display and a loader are unnecessary. The harness owns its resource adapter,
`XRSessionLifecycle`, `XRFrameLifecycle` and copied script in lifetime order.
There is no change to existing production callers or target dependencies.

Activate using the same capability snapshot and accepted feature plan as a
production host. The returned exact session starts Ready. Load a script for that
owner containing event-only steps and predicted-frame steps. Start, visibility
and focus come from explicit script events, rather than automatic activation.
Each frame supplies validated presentation-purpose `XRPoseSample` values and a
positive, strictly increasing `XRRenderPredictionTime`. Actions carry a separate
explicit `XRRuntimeTime` sample timestamp. Simulation timestamps
are not inferred from prediction time.

Scripts are copied at admission, bounded to 256 steps. Views and backend-neutral
boolean/float/vector2 action samples use fixed-size owned arrays. Action IDs are
session-owned XR IDs; Input retains ownership of semantic action mapping. The
harness admits action kinds only when the accepted feature plan enables them.
Focus or view tracking loss neutralizes actions without retaining old poses.
Script admission rejects stale owners, duplicate actions, nonfinite values,
invalid timestamps/enums, unsupported kinds and overflow before replacing the
prior script. Allocation failure also preserves the prior script and cursor.

`Step` fences the exact session before consuming evidence. Its outcome reports
whether the step was consumed, the next cursor, the current session, the injected
failure point and frame evidence captured before cleanup/end. No frame remains
open after the call. Illegal events are rejected without consuming their step; a
legal event followed by failed frame admission remains explicitly consumed.
Exhaustion returns Unavailable with no consumed step. Publications are owned
values that stay memory-safe after restart; their session IDs must still be
revalidated for live use.

Failure injection covers wait, begin, locate, acquire, submit, release and end.
Wait failure cancels its reservation; later failures abort, retry fake cleanup,
release any acquired image and end with zero layers. This proves production gate
ordering without manufacturing native/GPU completion evidence. Fake resources
have no external leases and cleanup retries always complete synchronously.
Activation can fail at every preparation stage, exercising production rollback.
Successful replacement retires old resources and clears its script; runtime
restart uses a new runtime generation. Frame sequences are never reset/reused.
Shutdown permanently closes both gates and is idempotent.

The public header is owned solely by `HoroXRRuntime` in the header registry and
is compiled by `HoroXRFakeRuntimePublicHeaderConsumer`. Deterministic evidence does
not establish physical runtime compatibility or satisfy hardware qualification.

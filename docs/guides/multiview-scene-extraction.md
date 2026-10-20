# Multiview scene extraction

`MultiviewSceneSnapshot` owns renderer presentation values, with one instance table
shared by every view. It never owns or mutates a runtime scene. Each instance names
an exact resident mesh and material generation; those identities do not retain GPU
resources. The frontend must resolve and pin them using its existing resource and
material completion lease before native submission.

The public headers belong to `HoroEngine::RenderApi`. Existing mono scene and
material submission callers retain their contracts. This additive surface exists
because `RenderSceneView` describes one borrowed camera and CPU mesh spans, while
multiview needs immutable frame-owned transforms and generation-safe resident
identities shared by a complete runtime-ordered view set. No public native handle
or XR session type crosses into Renderer.

The caller supplies the canonical scene/frame/origin/view and selected-device
publication fences. A retained snapshot remains memory-safe after scene teardown;
`Validate` rejects use against shutdown or replacement. The XR adapter must retain
and revalidate the exact `XRViewRenderPlan`, session/configuration generations and
current acquired-image evidence. A matching numeric view revision alone does not
replace XR identity validation. Presentation poses must be explicitly transformed
from their declared reference space into the canonical scene origin; local/stage
coordinates cannot be silently treated as world coordinates.

## Work and shader indexing

Extraction has fixed storage ceilings of 32 views and 128 instances. Caller limits
independently bound view count, instance count, instance/view cull tests and visible
draw expansion. Admission checks counts before traversing payloads. Each cull test
transforms eight box corners and conservatively checks six homogeneous clip planes;
touching/straddling geometry stays visible. Both clip-depth conventions are explicit.
Finite input whose matrix product or corner transform overflows is rejected.
No normal successful extraction or invocation lookup allocates, waits, calls native
code, changes producer state, or repeats simulation. Stack/value storage is bounded
by the public fixed ceilings. Errors use the standard owned diagnostic mechanism.

Every visible draw references one instance plus a view mask. `Invocation` maps a
packed visible-view ordinal to the same canonical instance and dense shader index
for multipass and instanced execution. It rejects invalid ordinals rather than
wrapping them into another view. Runtime semantic roles remain with the XR producer;
dense shader index zero is not an assertion that a view is the left eye.

`RequireInstanced` requires both indexed-shader support and instanced target routing.
`PreferInstanced` may select explicitly supported multipass; an unsupported complete
view count is never truncated. Capabilities are effective backend facts for the
captured device/capability generations, not feature claims made by this extractor.

## Integration still required for HORO-2095

The extraction core and tests do not complete the ticket's native render/shader
acceptance. The remaining integration must:

- Consume HORO-2094's acquired external-image lease, exact subresource ranges,
  initial/final state and completion evidence. Renderer borrows the image and never
  destroys or releases the runtime allocation.
- Extend the existing material draw/execution request with bounded instance count,
  dense view mapping, exact target subresources and prevalidated transform bindings.
- Realize and consume reflected per-view matrix/array packing in actual shaders;
  merely selecting an enum or adding a shader fixture is insufficient.
- Qualify native instanced execution and admitted multipass fallback against the
  same immutable presentation. Resolve stale resources and complete all input
  admission before encoding any view of the frame.
- Use HORO-326's existing submission validator/timelines and
  `BeginShutdown`/`PollDrain` completion authority. Preserved/discarded graph contents
  do not prove a foreign native image import or matched external queue release.

No completion, native support, visual parity or performance qualification is claimed
by the source-only extraction tests. A full local build is not authorized for this
worktree; executable evidence must be recorded separately when granted or hosted.

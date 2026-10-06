# Immutable voice render-state adoption

`HoroEngine::AudioVoiceRender` is an additive host-composed boundary over
AudioPlayback, AudioMixer and the existing AudioCommands staging. It owns
`Horo/Audio/AudioVoiceRenderRuntime.h`; no backend, Scene, editor or package
discovery is linked into it. Existing standalone playback/mixer callers retain
their contracts.

## Preparation and ownership

Prepare the registry, command staging, source and mixer on control. Resident
creation copies PCM and retains the canonical registry; its initial gain/pitch
must be unity because the complete voice generation applies those values once.
Streaming creation reads actual admitted decoder facts, retains the service and
the sole retirement-pinned port, and prepares Linear clip-to-mix conversion.
The service's JobSystem and package source still outlive service shutdown.

The host transfers exclusive registry/processing ownership to one callback lane;
shared ownership pins are not permission for concurrent registry mutation. Source
and listener requests contain complete copied numeric values, not Scene/ECS
references. Publish resolves a stable bus into a pinned stereo graph generation
on control and owns the prepared target/pitch bank before staging its command.
Busy/full/retry accepts nothing. At most one state publication awaits completion,
and the explicit state budget covers fixed scratch/metadata and two pitch banks.
PCM, stream ring/package reservations and the mixer retain their separate budgets.

## Callback and lifecycle

One bounded host dispatcher consumes the existing FIFO. It dispatches graph
commands to the mixer and voice-state/control/gain records to the voice owner.
After Render, pass its borrowed `MixerVoiceInput` to MixerGraphRuntime. Call
EndBlock **after** the mixer's last access; command consumption is not reclamation
proof. Control Reconcile then releases replaced state/converter storage. Streaming
conversion retains unconsumed PCM between blocks, renders silence on starvation
and never waits for fills. Resident conversion is not applied a second time.

Only adjacent unpublished updates for the same scoped gain parameter coalesce
through existing staging. Full state/route publications, voice lifecycle controls,
resource releases, unload and reset barriers never coalesce or reorder. A foreign
epoch, old handle or terminal voice cannot adopt a new generation. The dispatcher
must retain/reconcile unaccepted required work and apply unload/reset to every
affected voice before acknowledging those barriers at their owning boundary.

Close retains all callback-visible owners. After exact-epoch native stop/join,
CompleteShutdown releases the stream retirement pin and processing resources on
control; only then may the host Retire/Shutdown the stream service. False
detachment, queue consumption, silence or timeout never authorizes destruction.
Legacy RenderPort callers retain their explicit host-detachment obligation;
RetainedRenderPort adds an enforceable pin without changing that legacy contract.

## Public consumer migration

AudioCommands adds `AudioPublishVoiceStateCommand` to its closed payload variant;
exhaustive visitors must handle this retained reference explicitly rather than
discard it. Structural normalization validates identities but grants no liveness.
CoreStereoSpatialRenderer adds a numeric prepared-target adoption operation for
the exclusive render lane; scene geometry and target preparation stay on control.
MixerRenderPlan exposes immutable rate/frame limits for control admission.
AudioVoiceRender, AudioApi, AudioCommands, AudioDsp, AudioPlayback and AudioMixer
public-header consumers must be validated alongside their regression suites.

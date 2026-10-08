# Mixer snapshot integration

`MixerSnapshot.h` is additive and belongs to `HoroEngine::AudioMixer`; existing
MixerAsset versions and ordinary automation remain unchanged. Public consumers
receive it through that target's declared header boundary.

Control stores version-one named presets with stable mixer asset, bus, send,
effect and parameter IDs. Runtime handles and editor solo state have no persisted
representation. Use the bounded codec; unknown versions and trailing fields fail
closed. Resolve against the currently retained physical bindings and prepare an
exact sample target through `PrepareMixerSnapshot`. Its typed
`MixerSnapshotTransitionRequest` retains scope, sample target, increasing identities,
priority and curve/duration policy as one owned intent.

Publish `prepared.batch` with `MakeScheduledAudioBatchCommand` through ordinary
retained command transport. Retain the **entire PreparedMixerSnapshot** sidecar
under its CommandStorage handle until callback acknowledgement. Pass the
resolved sidecar and its controller to `MixerGraphRuntime::RenderAutomated`; the
runtime advances the borrowed sealed engine and dispatches `Apply` at the exact
interior sample boundary. Direct ApplyBatch
is ordinary automation and does not apply snapshot priority/replacement policy.
Reconcile rejection explicitly. Never release retained storage on queue consumption
alone, or rebind an old stable ID to a new graph generation.

Before publication, call `MixerRenderPlan::BindAutomation` with the same sealed
engine descriptors. Bus/send linear gain uses parameter ID 1 and range [0,16];
DSP IDs and bounds come from the compiled node descriptor. Initial values match
compiled defaults. Successful binding is final and charges physical projection
storage and worst-case work to the profile. Replace the plan and engine together
when the graph generation changes. `RenderAutomated` validates exact ownership
and resolves opaque selectors once per block, then reads/project values without
per-sample identity discovery or allocation. It processes aligned one-frame DSP
blocks while preserving voice source offsets and node state. Static `Render`
keeps its existing block behavior.

One whole
preset owns at most eight trajectories. Higher priority wins; equal priority later
transition IDs replace; completion releases priority. Cancellation holds current
values rather than restoring a prior preset. Transition IDs and automation request
IDs are separate increasing sequences, and rejection preserves both for retry.

`AudioParameterAutomation::HasRequest` is an additive callback-owner observation
used to exclude completed or replaced requests from cancellation transactions.
It is not a cross-thread snapshot or proof of physical target liveness.

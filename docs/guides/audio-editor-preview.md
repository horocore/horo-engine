# Audio editor document and preview foundation

`AudioEditorDocument` supplies the shared AUD-009.1 asset-document preview owner.
Clip inspectors, mixers, procedural tools and recording surfaces retain their
source/history authority and use this foundation for document identity,
interaction focus, source/preview revision fences and transport. It never writes
source, dirty/save state, solo, meters, device policy or preview overrides.

## Composition and playback

The application host chooses one output peer explicitly and obtains its discovery
snapshot before constructing a preview. It prepares ordinary
`AudioVoiceRenderRuntime` resident PCM or an admitted stream,
`MixerGraphRuntime`, immutable `MixerRenderPlan` and `AudioCommandStaging` in one
exact runtime/epoch/scene scope. Preparation copies or pins source data; mutable
authoring objects never enter Audio. Source/device preparation runs on control or
workers, never in the draw loop or callback.

The build-tree-only `HoroAudioFrontendComposition` interface exposes
`AudioFrontendComposition::Create` to the host. It validates detached candidates
and stages initial voice/graph publications. It consumes owners only on success
and never opens output, starts callbacks, discovers backends or registers ambient
services. A failed partially staged candidate remains caller-owned and requires
detached close/reconciliation before destruction.

`HoroEngine::AudioFrontend` owns one prepared playback lane, mixer, FIFO and
selected output. It reuses existing playback, decoding, resampling, DSP and native
adapters. Callback dispatch is bounded to 64 records and executes the existing
voice/mixer; voice acknowledgement follows the mixer's final borrowed-input use.
It performs no allocation, reclamation, logging, worker I/O, locks or discovery.

`Start` requests Open, validates negotiated facts against preserved request,
discovery and prepared graph, then requests Start. Only matching Ready and Started
proofs permit `CommitRendering`; priming is silence. Transport uses ordinary FIFO
admission with explicit retry/full outcomes, never a claim of audible execution.
Unadmitted formats fail without implicit conversion, fallback or graph rebuild.

Each admitted transport retains an exact sequence and terminal Applied, Rejected
or Cancelled result. The fixed 64-slot receipt buffer applies backpressure until
the control consumer acknowledges results with `DrainTransportResults`. Callback
application is published after the block's final borrowed-input use; cancellation
is published only after native detachment. Retirement never discards accepted
transport outcomes or waits for the presentation consumer.

## Document lifecycle

The host opens an Asset `DocumentIdentity` at a nonzero authoring revision and
attaches its isolated frontend for that capture. Stale attachment retains caller
ownership. Existing `DocumentIdentityRegistry` owns persistent routing and
focus-existing semantics; no new tab registry or serialized session handles exist.

`SetFocused` records interaction routing. Start/seek/resume require focus; Stop,
Pause and Cancel remain available after focus loss. Focus never activates a device
or resumes playback. The documented EditorPreview default continues existing
playback on mere focus loss. Native interruption/device loss instead closes this
frontend, without automatic recovery or fallback.

Every command is source-revision fenced. `Reload` takes a strictly newer externally
committed revision, invalidates preview correlation and closes the old owner.
Replacement waits for retirement. Concrete authoring documents retain source,
history, save and dirty authority; preview commands cannot mark source saved.
Retired transport results remain available through the document after reload or
close. Drain these bounded results before attaching a replacement preview.

## Shutdown and failure ownership

`Close` closes new admission and requests silence from existing voice/mixer
owners. The host continues owner-thread `Pump` during reload, tab, project and
application teardown. Closing pending Open never starts a callback. Pending Start
waits for exact Ready/Started before its Quiesce transaction.

Matching Quiesced proof precedes Stop. Only matching Stopped proves no current or
future native callback entry. Device Close releases native output; control then
releases voice/mixer generations, reconciles cancelled FIFO work, drops the stream
port, cancels/joins its admitted stream and releases its provider/package lease.
`Closed` appears only after those operations succeed, not on silence or timeout.

Streams carry a host dependency lease retaining jobs/provider/code through
retirement. Closing one lane never shuts down an unrelated shared stream service.
Interrupted worker joins preserve the stream for retry. Original output failures
remain in snapshots. Failed/Retained output preserves the full ownership island
under ADR-062; the host retains it and applies process-fatal policy. Premature
destruction of attached/incompletely retired output terminates, matching existing
`AudioStreamingService` lifetime guards, instead of silently leaking a playing
device or freeing unjoined callback memory.

## Additive migration and qualification

Rebuild consumers for AudioFrontend and EditorServices' AudioEditorDocument.
Only application roots opt into the non-installed composition interface. There
is no persistent format or existing playback API change. Later audio tools supply
their source captures and commands through these owners instead of native APIs.

`HoroAudioEditorPreviewTests` covers real resident/stream PCM through Null,
transport, retry, callback allocation safety, partial startup, revision fencing,
focus, reload and native/worker retirement. It and AudioFrontend's public consumer
join Windows Debug and Release audio qualification. EditorServices' consumer
compiles the document contract from staged declared dependencies. Native tests
require hosted qualification when local resource limits prohibit a build.

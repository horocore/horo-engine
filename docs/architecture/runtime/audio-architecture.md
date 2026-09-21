# Audio Architecture

## Purpose

This document defines audio asset loading, device ownership, mixer and voice
models, scene integration, streaming, real-time thread rules, lifecycle,
editor tooling, middleware boundaries, and observability.

[ADR-062](../../adr/062-audio-runtime-ownership-and-update-order.md) is the
single normative owner of process/control/callback authority, runtime and device
state transitions, scene-context barriers, suspend/recovery, fatal failure, and
teardown order. This document owns the broader audio subsystem model and summarizes
that lifecycle decision.

## Core Decisions

- Engine audio APIs are backend-neutral.
- One audio control runtime owns lifecycle and exactly one selected output
  composition. The selected adapter owns native device objects; the integration
  model declares whether Horo or middleware owns the final mixer/voices.
- The audio callback performs no heap allocation, blocking I/O, logging, or
  contended locking.
- Canonical mixer/DSP blocks use 64-byte-aligned planar binary32 samples and an
  explicit Horo speaker, Ambisonic, or discrete channel order under
  [ADR-063](../../adr/063-audio-sample-format-and-channel-layout.md).
- Game and scene code use typed handles and enqueue commands.
- Decoding and streaming I/O occur outside the real-time thread.
- Audio assets use the common asset identity, cooking, and cache contracts.
- Spatial profiles distinguish optional provider fallback from required-capability
  activation failure under
  [ADR-066](../../adr/066-spatial-provider-and-required-capability.md).
- Headless hosts may omit audio or use a null backend.
- Core audio targets a practical Unity/Godot-level foundation: sources,
  listeners, buses, 2D/3D playback, streaming, basic effects, and profiling.
- The 1.0 product includes the typed extension contracts, basic spatial and
  environmental baseline, native backends, authoring, and qualification. Capture,
  procedural synthesis, advanced acoustic propagation, adaptive music systems,
  and middleware implementations remain Post-1.0 capabilities.
- Procedural graphs compile to separate immutable sound generators and reuse the
  ordinary AudioFrontend, voice, spatialization, mixer routing, and lifecycle
  contracts under [ADR-071](../../adr/071-procedural-audio-graph-ownership.md).
- Middleware selects either event bridge or backend replacement before runtime
  construction; one runtime has one final mixer/device owner under
  [ADR-072](../../adr/072-audio-middleware-integration-model.md).

## Product Boundary And Roadmap

This document is the normative target architecture, not an implementation-status
inventory. An M0 architecture decision may define a safe extension seam for a
Post-1.0 feature without promoting that feature into 1.0. Likewise, a parent
capability milestone records the product checkpoint that requires the integrated
outcome; it is not dependency order. Native `blocked by` relationships, not issue
numbers or milestone numbers, own technical execution order.

[AUD-001.12](https://github.com/HoroCore/horo-engine/issues/691) is the M0
reconciliation checkpoint for this document. The parent capability milestones are:

| Capability | Product checkpoint | Boundary summarized here |
|---|---|---|
| [AUD-001](https://github.com/HoroCore/horo-engine/issues/524) Real-time runtime, clock and commands | M3 — Alpha | Process/control/callback ownership, bounded transport, clocks, Null and teardown |
| [AUD-002](https://github.com/HoroCore/horo-engine/issues/536) Assets, codecs and streaming | M4 — Beta | Audio/AST cook authority, resident/streamed media and codec contributions |
| [AUD-003](https://github.com/HoroCore/horo-engine/issues/550) Voice playback and concurrency | M4 — Beta | Voice identity, admission, priority, virtualization and completion |
| [AUD-004](https://github.com/HoroCore/horo-engine/issues/561) Mixer, automation and DSP | M5 — 1.0 | Constrained bus DAG, effects, ramps, latency, tails and publication |
| [AUD-005](https://github.com/HoroCore/horo-engine/issues/576) Spatial audio | M5 — 1.0 | Core renderer plus typed optional/required provider policy |
| [AUD-006](https://github.com/HoroCore/horo-engine/issues/586) Environmental audio | M5 — 1.0 | Basic raycast occlusion, material contribution, zones and environment sends |
| [AUD-007](https://github.com/HoroCore/horo-engine/issues/594) Devices and backends | M5 — 1.0 | Qualified native desktop, SDL3 reference and Null peers |
| [AUD-008](https://github.com/HoroCore/horo-engine/issues/606) Timeline and music transport | M5 — 1.0 | Sample-clock transport and cross-system scheduling; not adaptive-music policy |
| [AUD-009](https://github.com/HoroCore/horo-engine/issues/615) Authoring and mixer tools | M5 — 1.0 | Production-path preview, asset/source/mixer/settings/debug workflows |
| [AUD-010](https://github.com/HoroCore/horo-engine/issues/626) Diagnostics and qualification | M5 — 1.0 | Bounded metrics/faults, signal/RT tests and platform qualification |
| [AUD-011](https://github.com/HoroCore/horo-engine/issues/637) Extension contracts | M5 — 1.0 | Versioned codec/DSP/spatial/acoustic/service capability and Audio RT ABI seams |
| [AUD-012](https://github.com/HoroCore/horo-engine/issues/644) Capture and voice I/O | Post-1.0 | Reserved permission-aware capture/recording/NET/speech boundary |
| [AUD-013](https://github.com/HoroCore/horo-engine/issues/654) Procedural audio | Post-1.0 | Reserved compiled sound-generator and editor graph boundary |
| [AUD-014](https://github.com/HoroCore/horo-engine/issues/664) Advanced acoustics | Post-1.0 | Rooms, portals, diffraction, baked/geometric propagation and advanced reflections |
| [AUD-015](https://github.com/HoroCore/horo-engine/issues/672) Adaptive music | Post-1.0 | Gameplay/package orchestration over the 1.0 music transport |
| [AUD-016](https://github.com/HoroCore/horo-engine/issues/681) Middleware | Post-1.0 | Reserved event-bridge/backend-replacement, bank and distribution boundary |

The 1.0 extension ABI must be capable of hosting later packages, but passing its
contract does not make AUD-012 through AUD-016 required 1.0 implementations.
Post-1.0 sections below define compatibility and ownership constraints only.

## Normative Contract Map

Detailed ADRs are the single owners of their decisions. This document projects
them and must not redefine a competing rule:

| Concern | Single normative owner | Delivery boundary |
|---|---|---|
| Runtime/control/callback lifecycle, clock mapping and teardown | [ADR-062](../../adr/062-audio-runtime-ownership-and-update-order.md) | AUD-001, 1.0 foundation |
| Processing sample representation and channel layout | [ADR-063](../../adr/063-audio-sample-format-and-channel-layout.md) | AUD-001, 1.0 foundation |
| Audio/AST import, cook, cache and runtime media boundary | [ADR-064](../../adr/064-audio-asset-and-cook-boundary.md) | AUD-002, 1.0 |
| Mixer bus/send/return topology and deterministic compilation | [ADR-065](../../adr/065-mixer-topology-and-constrained-dag.md) | AUD-004, 1.0 |
| Spatial provider identity, required capability and fallback | [ADR-066](../../adr/066-spatial-provider-and-required-capability.md) | AUD-005, 1.0 |
| WASAPI/Core Audio/PipeWire/SDL3/Null backend strategy | [ADR-067](../../adr/067-platform-audio-backend-strategy.md) | AUD-007, 1.0 |
| Music transport and cross-system clock/semantic ownership | [ADR-068](../../adr/068-music-transport-and-cross-system-ownership.md) | AUD-008 transport in 1.0; adaptive policy Post-1.0 |
| Audio extension capability identity, RT ABI, trust and unload | [ADR-069](../../adr/069-audio-extension-capability-and-abi.md) | AUD-011 contract in 1.0 |
| Capture, recording, network voice and speech boundaries | [ADR-070](../../adr/070-capture-and-voice-io-ownership.md) | AUD-012, Post-1.0 |
| Procedural graph assets, compilation and generator execution | [ADR-071](../../adr/071-procedural-audio-graph-ownership.md) | AUD-013, Post-1.0 |
| Middleware event bridge and backend replacement | [ADR-072](../../adr/072-audio-middleware-integration-model.md) | AUD-016, Post-1.0 |

## Layer Model

```text
Scene / Gameplay / Cinematic / Animation / Editor / Streaming
                         |
                         | owned typed snapshots and intent
                         v
                  AudioFrontend / API
                         |
                         | validate, normalize, map clocks, stage
                         v
              Audio control runtime (owner thread)
               assets  voices  graphs  device state
                         |
                         | immutable generations + bounded commands
                         v
                 real-time render core
        voices -> generators/DSP/spatial -> mixer Master
                         |
                         v
         selected platform/middleware/Null output adapter
```

Producers never share mutable ECS/editor/asset state with Audio control or the
callback. Control is the only state-transition and publication authority. The
callback consumes one preallocated render epoch and emits bounded facts; it does
not perform policy, discovery, recovery, compilation or ordinary diagnostics.
Backend device APIs and native handles remain private to the selected adapter.

## Authority Matrix

| Concern | Authority | Callback access |
|---|---|---|
| Product composition, omitted/Null/device/model selection and fallback list | Application/process host | None |
| Runtime/device/scene-context states, command validation, graph publication and completion reconciliation | Audio control runtime under ADR-062 | Immutable epoch plus bounded commands/acknowledgements only |
| Persistent asset identity, cache, staging, publication and package delivery | AST/PKG/Release | No registry/file/package access |
| Codec/layout/loop/gapless/cook/runtime-media semantics | AudioModel/AudioCook under ADR-064 | Prepared decoded/stream blocks only |
| Bus topology, routes, DSP order, buffers, latency and tails | Mixer compiler/control under ADR-065 | Immutable compiled plan and preallocated state |
| Scene transforms, physics/world queries and acoustic facts | Scene/Physics extraction and admitted providers | Prepared numeric source/listener/acoustic inputs only |
| Native device objects, format conversion and callback registration | Selected backend adapter | Exact private native buffer/callback boundary |
| Editor documents, undo/redo, selection, solo/meters and preview UI | Editor | No widget/document state |
| Metric storage/export, formatting and profiler UI | OBS/control adapters | Preallocated counters and bounded records only |

## Clock And Scheduling Authority

The output callback's generation-scoped `AudioSampleClock` is authoritative for
the sample at which audible state changes. It is not wall, simulation, sequence,
animation, media or device-policy time. Audio control owns immutable correlation
snapshots that map admitted producer clock evidence into the current sample epoch.
Only resolved sample/buffer targets enter the callback.

Device reset, suspend/resume, backend/runtime replacement and transport seek create
named discontinuities or new generations. The same integer frame in another epoch
is not the same time. Producers retain semantic occurrence/request IDs and their
own clocks; callback observations are evidence returned through control, not
authority to rewrite a gameplay, sequence or animation playhead. Wall time is for
deadlines/diagnostics and never schedules audible work directly.

## Runtime Ownership

```text
AudioRuntime
  +-- AudioDevice
  +-- MixerGraph
  +-- VoiceRegistry
  +-- StreamBuffers
  +-- CommandQueue
  +-- EventQueue
```

This tree is the native/event-bridge composition. A backend-replacement adapter
may privately own the final mixer/voices and omit corresponding native runtime
objects only after ADR-072 capability preflight; it still participates in the same
AudioRuntime state, frontend, scene-context and completion authority.

The process host owns `AudioRuntime`. A game runtime may create one scene audio
context per active scene or listener policy.

The host resolves `Omitted`, `Null`, or `Device` composition before construction.
The audio control runtime is the only authority that commits runtime/device
transitions and owns scene contexts, registries, graph generations, command
normalization, and completion reconciliation. A selected backend owns native
device objects and callback registration. The callback owns only its currently
published preallocated render-core epoch and bounded acknowledgements/events.

Implementation status on 12 September 2026: `AudioLifecycleReconciler` is the
bounded control-owner record for accepted operations and callback-visible voice/
resource generations. Scene unload closes its exact context until the ordered
barrier is acknowledged. Device reset and shutdown close global admission and
require matching callback-quiesced evidence plus native-detachment proof before
releasing tracked references. Pending operations receive retained typed terminal
outcomes before scene/runtime ownership disappears; partial startup and repeated
shutdown follow the same state machine without invoking a device from destruction.

Scene, gameplay, cinematic, editor, asset, and streaming systems are typed
producers. They submit owned value snapshots and commands through Audio API or an
application audio capability; Audio Runtime does not include/query editor, GUI,
MCP, CLI, gameplay, mutable ECS, or Scene Runtime internals. Scene contexts are
generation-checked client handles and unload through the ordered barrier defined
by ADR-062.

## Handles

```cpp
template<typename Tag>
struct AudioHandle {
    AudioRuntimeId owner;
    uint32_t slot;
    uint32_t generation;
};

using AudioClipHandle = AudioHandle<AudioClipHandleTag>;
using AudioVoiceHandle = AudioHandle<AudioVoiceHandleTag>;
using AudioBusHandle = AudioHandle<AudioBusHandleTag>;
```

Handles are process-local, registry-owned, and generation checked. The non-zero
`AudioRuntimeId` owner prevents a numerically equal slot/generation from another
runtime replacement from aliasing current state. Slot generations never wrap;
an exhausted slot is retired permanently. Asset IDs remain the persistent
identity for clips and sound definitions through distinct `AudioClipId` and
`AudioSoundId` wrappers. `AudioBusId`, `AudioParameterId`, and `AudioEventId` are
distinct stable numeric identities resolved before real-time processing.

Malformed, foreign-owner, stale, capacity-exhausted, and generation-exhausted
handles produce distinct `horo.audio` errors. Operation-specific error messages
carry bounded owner/slot/generation context; callers branch only on the stable
descriptor code. Native device identifiers never enter these public values;
`AudioDeviceId` is a generation-scoped Horo handle.

## Internal Processing Format

[ADR-063](../../adr/063-audio-sample-format-and-channel-layout.md) is the single
normative owner of real-time sample representation and channel layout. Mixer,
voice, bus, DSP, spatializer, resampler, and extension processing consumes
borrowed `AudioPlanarBlockView` values: one contiguous binary32 plane per ordered
semantic channel, at least 64-byte plane alignment, common valid/capacity frame
counts, and positive-zero silence/padding. Persistent descriptors and prepared
graph generations own `AudioChannelLayout`; callback blocks carry only an
`AudioChannelLayoutView` whose span is bounded by the retained graph epoch.

Speaker presets have exact Horo orders; channel count alone is never identity.
Ambisonics uses ACN order and SN3D normalization (AmbiX), with orders 0–3 admitted
only when profile/provider limits permit. Discrete channels carry no speaker
meaning. Native/file/middleware layouts map explicitly at adapters and never leak
their format enums into runtime contracts.

Sample rate is an explicit checked value in every block and published render plan;
there is no universal hidden 44.1 kHz or 48 kHz mixer constant. Source, cooked,
decoded, effective mixer and native-device rates remain distinct with provenance.
Layout conversion never changes rate, and resampling never reinterprets channel
roles. Rate conversion is prepared outside the callback and executes only through
a bounded admitted resampler plan.

### Resampler admission and implementation boundary

`HoroAudioApi` owns `AudioResamplerPlan`: immutable control-thread metadata, not a
device, callback, state allocator or working audio renderer. `Prepare` owns a copy
of the request and reserves per-instance sample-product, history-byte and
look-ahead limits before a future processing state can be published. Failure does
not mutate a live plan. Existing identity/error consumers require no migration;
the new header is additive and owned only by `HoroAudioApi`.

The baseline accepts explicit input/output rates from 8,000 through 384,000 Hz,
1–64 unchanged semantic channels, and 1–16,384 output frames per call. Clip pitch
is a finite multiplier in [0.125, 8]; the resulting input-frame step
`inputRate / outputRate * pitch` must be in [1/64, 64]. No implicit device rate,
channel remapping, clamping, fallback quality or silent budget reduction is allowed.
Mix-to-device conversion accepts only unity pitch. Pitch changes playback duration;
independent pitch-preserving playback speed remains unavailable and non-unity
requests return `audio.operation.unsupported`, including invalid/non-finite speeds.

The explicit quality choices are two-tap unfiltered Linear and windowed-sinc
Sinc32/Sinc64. Linear is not an anti-aliasing quality guarantee. Sinc support expands
by `ceil(max(1, inputStep))` during downsampling, up to 4,096 taps, to retain its
input-domain low-pass support. Look-ahead is half the effective tap count in input
frames; history storage is `channels * taps * sizeof(float)`. The processing
reservation is `channels * maximumOutputFrames * taps` sample products, computed
in 64 bits after bounds checking. These are deterministic admission costs, not
wall-clock timing or a whole-runtime CPU/memory budget. Caller input/output planes
and any prepared coefficient storage require separate ownership/reservations.

Plan creation implements admission only. The DSP target below supplies scalar/SIMD
streaming processing, reference validation and an opt-in CPU qualification harness.
Each target host/profile still requires its own deadline admission. No device or voice
path may advertise a configured runtime based solely on successful plan creation.
A SIMD implementation must use the same
plan, limits and channel/phase order and pass the scalar reference suite before
explicit non-callback dispatch selection; no callback feature probing is allowed.

The following numerical primitive in private `HoroAudioDsp` prepares 1,025 phase
rows (1,024 intervals) outside the callback. Its bank costs
`1025 * taps * sizeof(float)` separately reserved bytes, independent of channel
count. Sinc rows use a Hann window with cutoff `0.9 / max(1, inputStep)` relative
to input Nyquist and per-phase DC normalization. Linear rows contain only
`1-phase, phase`. Processing interpolates adjacent rows and accumulates a channel's
circular history in double precision without allocation, locks or transcendental
calls. The owner must validate history width, finite samples, ring index and phase
before this private primitive; it must also perform the ADR-063 output safety
policy. The bank never clips intermediate headroom.

This is the finite windowed-sinc/table-interpolation approach described in
[Julius O. Smith's Windowed Sinc Interpolation](https://www.dsprelated.com/freebooks/pasp/Windowed_Sinc_Interpolation.html),
with Horo-owned window, cutoff, table resolution and budget choices. Reference
tests exercise individual impulses against direct double-precision coefficients,
unity DC, exact linear ramps, circular ordering, and a 96-to-48 kHz fixture that
passes a 1 kHz tone and attenuates a 30 kHz tone. Those fixtures do not certify a
universal passband/stopband specification or real-time CPU deadline.

`AudioResampler` is the additive public processing owner in `HoroAudioDsp`; it
does not make AudioApi depend on DSP. Control creates its coefficient bank and
plan-reserved history, then transfers exclusive ownership to one processing
thread. Moves/replacement/destruction require quiescence; replacing storage is
off-callback work. The object never retains input/output spans and never queries
a device, layout service or ambient runtime. Callers preserve one semantic layout
in plan channel order. Buffer prefixes require ADR-063's 64-byte plane alignment,
sufficient storage and nonoverlapping output/input and output/output regions.
Malformed calls return fixed-size status without consuming input, writing output,
or changing phase. General error formatting belongs outside the callback.

Input is consumed only to satisfy the next output's look-ahead. The caller retains
and re-submits unconsumed frames. A zero-capacity output call is a no-op and does
not adopt an end marker. Fractional input position is shared by all channels and
remains in [0,1); only bounded relative integer advances are retained, eliminating
ever-growing counters and callback-boundary phase resets. Input starvation does
not insert silence or advance the timeline. An explicit end marker is adopted
only after all input on that call has been consumed. Subsequent empty-input calls
drain at most `taps` virtual zero input frames and retain the finite filter tail;
Complete is terminal until Reset. Empty streams produce no output. Reset clears
history, fraction, look-ahead and end state without reallocating and deliberately
discards the previous tail; it does not reconfigure rate/pitch/quality.

Process bounds output by the admitted maximum and offered input by
`maximumOutputFrames * 64 + taps`. Actual input consumption is demand-driven;
sample sanitization touches only consumed input and produced output. Buffer
alias validation is at most quadratic in the admitted 64-channel limit. Reset
clears at most `channels * taps` samples. These additional bounded costs must be
included in measured callback admission, not confused with the kernel's
sample-product reservation alone. Non-finite input and overflowing/non-finite
output become silence with a bounded per-call count; subnormal and signed-zero
samples become positive zero without erasing normal low-level audio or ordinary
over-range internal headroom. Unproduced output storage is untouched.

Regression coverage includes partition-invariant output across qualities/pitches,
channel isolation, headroom, reset/move, starvation, zero-length blocks, empty and
terminal streams, overlap/alignment/size rejection, fault/silence policy and zero
ordinary C++ heap allocations in Process/Reset.

`AudioResamplerExecution` selects Scalar or Simd explicitly at Create. Unsupported
or unknown execution requests return `audio.operation.unsupported`; the default is
the scalar reference, not automatic backend selection. Preparation stores one
immutable evaluator pointer. `SupportsSimd` reports the compiled baseline: ARM64
NEON where enabled and SSE2 on supported x86 builds; no callback CPU probing or
dispatch mutation occurs. Native types/intrinsics stay private to AudioDsp. The
implementations follow the [Arm intrinsic reference](https://arm-software.github.io/acle/neon_intrinsics/)
and [MSVC x64 ISA baseline](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64).
Each ring half is processed with bounded two-lane loads and a scalar odd tail,
never by loading across its end. Scalar/SIMD summation order differs; qualification
allows 2e-12 absolute error for normalized kernel fixtures and 3e-6 absolute error
for normalized streaming fixtures, while each mode is independently bitwise
partition-invariant. Scalar and SIMD are not promised bit-identical across CPUs.

### Local CPU qualification

`HoroAudioResamplerBenchmark` is an explicit `EXCLUDE_FROM_ALL` target, not a noisy
default CI timing test. Run it in a headless Release build without coverage or
sanitizers. It preallocates and warms the state for 100 calls, then measures 1,000
complete Process calls per mode/quality/profile with a steady clock. Input is
synthetic repeated finite blocks; preparation and reporting are outside timing.
Each call produces 256 frames at 48 kHz (5,333.33 microseconds). The harness rejects
a fixture whose measured P99 exceeds 10% of that period per instance. This envelope
does not reserve the remaining callback for an arbitrary number of voices: the
host must admit the aggregate cost of voices, mixing, validation and other DSP.
Rates, channels, pitch ratios, block sizes and machines outside these fixtures
require separate qualification and appropriately bounded plan reservations.

Recorded on 2026-09-04: Apple M3, macOS 15.5, AppleClang 17.0.0, CMake Release
(`-O3 -DNDEBUG`), headless, no coverage/sanitizers, scalar versus ARM64 NEON.
All Linear/Sinc32/Sinc64 fixtures passed the 10% P99 envelope. Sinc64 results:

| Profile | Scalar mean / P99 (µs) | SIMD mean / P99 (µs) | SIMD P99 / callback |
|---|---:|---:|---:|
| Stereo, 44.1→48 kHz, 64 taps | 46.728 / 54.417 | 14.733 / 17.583 | 0.330% |
| 8 channels, 96→48 kHz, 128 taps | 316.168 / 336.583 | 94.731 / 105.625 | 1.980% |

These are local wall-time observations, not worst-case execution-time guarantees
or x86 performance claims. Core scalar/SIMD regression tests run in ordinary CI;
platform deadline qualification remains an explicit host/release activity.

```bash
cmake -S . -B build/audio-resampler-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DHORO_BUILD_EDITOR_GUI=OFF -DHORO_BUILD_RENDER_OPENGL=OFF \
  -DHORO_BUILD_RENDER_METAL=OFF -DHORO_BUILD_EXAMPLES=OFF
cmake --build build/audio-resampler-release --target HoroAudioResamplerBenchmark --parallel 8
build/audio-resampler-release/tests/HoroAudioResamplerBenchmark
```

Internal buses may exceed nominal `[-1, +1]` full scale. Declared DSP/limiter
nodes own intentional saturation; one finite safety clamp occurs immediately
before native output conversion. Denormals normalize under the approved callback
floating-point environment, non-finite samples enter bounded fault policy, and
no per-sample logging occurs. ADR-063 owns the complete conversion, silence,
tail, denormal, clipping, and validation rules.

`AudioApi` now owns the additive `AudioFormat.h` value boundary: owning and borrowed
channel layouts, exact speaker presets, canonical ACN/SN3D metadata, checked
processing-format admission and backend-neutral native PCM representation facts.
The processing profile admits 8–384 kHz and at most 64 channels, matching the
resampler baseline. `AudioChannelRole` is a typed speaker/discrete/ACN variant;
equal numeric indices across families do not share meaning. Speaker adapters may
report explicit unique native role order, while canonical presets retain ADR-063
order. Layout conversion and resampling remain separate operations.

Layout views borrow retained immutable owners and reject direct temporary-owner
construction; callers still own epoch lifetime enforcement. Native PCM metadata
describes coding, packing, byte order, significant bits and low padding without
native pointers or enums. This does not implement sample conversion, planar block
storage, callback lifetime tracking or a device negotiation engine. Existing
callers need no migration; the public-header consumer and headless format tests
cover this additive boundary.

## Audio Assets

[AUD-002](https://github.com/HoroCore/horo-engine/issues/536) delivers this
resident/streaming asset baseline by M4 — Beta as part of the 1.0 product.
[ADR-064](../../adr/064-audio-asset-and-cook-boundary.md) is the single
normative owner of the Audio/AST import, cook, cache, publication, and runtime
media boundary. AST owns stable identity, admitted source access, generic
orchestration, dependency-aware cache and atomic publication. Audio owns source
extraction, codecs, schemas, profile semantics, transforms, deterministic domain
fingerprints, logical cooked outputs, and runtime compatibility validation. The
Assets implementation does not depend on Audio; the application host registers
the Audio contribution through the generic catalog.

Together, the generic pipeline and Audio contribution import and cook:

- decoded short clips
- streamed music or ambience
- channel layout and sample-rate metadata
- loop points
- loudness and normalization metadata
- per-platform compression, quality, and streaming policy
- duration
- original and cooked sample rate
- bit depth and sample format
- channel count
- seek tables for streamed or compressed audio
- encoder delay and padding for gapless playback
- loop start/end expressed in sample frames

Runtime loading validates cooked format and backend capability. Source files are
not decoded synchronously inside the audio callback.

Core asset support includes `wav` and `ogg` source audio. Additional formats may
be provided by packages or platform-specific importers, but scenes and packages
reference the cooked `AudioClip` identity, not decoder-specific files.

Loop metadata is sample-frame based, not millisecond based. Music loops and long
ambience must preserve encoder delay and padding information so packaged builds
can perform gapless looping where the backend supports it.

Dialogue and localized voice clips are resolved before they reach the audio
runtime. Locale-aware asset resolution belongs to the asset/configuration
pipeline; audio components and commands receive the resolved `AssetId` for the
selected locale.

### Audio Variation Container

A single `AudioSourceComponent` may reference either a single `AudioClip` or an
`AudioVariationContainer`. The container owns multiple clip references and a
selection policy so that repeated one-shot playback does not repeat the exact
same asset:

```cpp
enum class VariationSelection {
    Random,
    RoundRobin,
    Shuffle,
    WeightedRandom
};

struct AudioVariationContainer {
    std::vector<AssetId> clips;
    VariationSelection selection;
    float minPitchDelta;
    float maxPitchDelta;
    float minGainDeltaDb;
    float maxGainDeltaDb;
    uint64_t deterministicSeed;
};
```

The runtime resolves the container to a concrete clip at voice creation time,
before the clip enters the audio callback. Variation containers are small assets
that live next to `AudioClip` assets and are authored through the same import
pipeline.

The M2 schema baseline represents resident clips, streamed media and deterministic
variation containers as versioned, bounded Audio-owned values in `HoroAudioApi`.
Validation is side-effect free and rejects invalid semantic formats, sample-frame
ranges, duplicate local identities, unordered seek anchors, non-finite loudness or
variation values and unsupported schema versions before cook publication. This
baseline contains no source path, cache key, native format, decoder pointer or live
audio handle; import, codec selection, cooking and runtime streaming remain separate
responsibilities.

### Loudness And Metering Metadata

Cooked audio assets carry loudness metadata for mixing, normalization, and
platform compliance:

```text
- integrated LUFS
- short-term LUFS (optional)
- true peak dBTP
- RMS
- normalization gain
- broadcast compliance tag (optional)
```

The mixer may use this data to auto-level sources or to report loudness to
platform certification tooling. Loudness values are computed during import/cook,
not on the real-time path.

### Decoder Plugin Interface

Codec support is extensible through the asset cooking pipeline, not by adding
decoder-specific branches to runtime playback.

```cpp
class IAudioSourceReader {
public:
    virtual AudioByteCount Size() const = 0;
    virtual Result<AudioByteCount> ReadAt(
        AudioByteOffset offset, std::span<std::byte> destination) = 0;
};

class IAudioDecoder {
public:
    virtual bool CanDecode(AudioFormatTag format) const = 0;
    virtual DecodeResult Decode(IAudioSourceReader& source,
                                AudioDecodeOutput& output) = 0;
};
```

This C++ interface illustrates the internal strategy shape; it is not a stable
third-party binary ABI. Package codecs use the versioned capability/ABI and owner-
lease rules in ADR-069 while preserving the same semantic boundary.

Audio owns the decoder registry and its typed codec/container semantics. Import
and cook invoke a pinned registry snapshot through the Audio contribution while
AST retains source limits, cancellation, cache, staging, and publication
authority. Runtime loaders consume cooked audio payloads and validated stream
metadata. Packages may add Opus, ADPCM, MP3, platform-native codecs, or
middleware bank decoders by registering Audio decoder plugins without changing
Assets or runtime scene code. Registration order is not selection policy, and a
plugin cannot publish files or retain the AST-owned source reader. The reader is
valid only for the current invocation, enforces admitted total/read/seek budgets,
uses caller-provided bounded destination storage, rejects overflow and out-of-range
reads, and never implies that the complete source is resident or contiguous.

`AudioMediaFormatRegistry` is the inert metadata boundary beneath that future
decoder registry. Container and codec identities are independent, PCM carries an
explicit `AudioPcmFormat`, and resolution admits only an exact registered tuple.
Consequently, detecting WAVE or Ogg never claims that an embedded codec is valid.
The snapshot copies bounded contribution metadata and contains no probing logic,
decoder callbacks, source extensions, registration side effects, or selection
policy.

## Audio Components

### AudioSourceComponent

`AudioSourceComponent` is the core scene emitter component. It describes a
playback source without exposing backend voice handles. The implementation uses
the typed Audio reference and playback policy values below:

```cpp
struct AudioSourceComponent {
    AudioSoundReference sound;
    AudioSoundPlaybackDefaults playback; // bus, gain, pitch, loop, priority, concurrency and spatial mode
    AudioSceneLifecyclePolicy sceneLifecycle;
    bool enabled;
};

struct AudioPlaybackRequest {
    AudioSceneContextHandle sceneContext;
    AudioSoundReference sound;
    AudioPlaybackSettings playback;
    AudioSceneLifecyclePolicy sceneLifecycle;
};
```

Runtime playback creates voices through the audio frontend. Persistent scene
data never stores `AudioVoiceHandle` or another prepared voice/resource handle.
Transient `AudioPlaybackRequest` values carry only the generation-checked scene
context and semantic playback policy; the Audio control owner resolves resources
and admits a voice. Native clip sources resolve `sound` to a
cooked `AudioClip` payload, either directly or by expanding an
`AudioVariationContainer`. Middleware event sources resolve through the
middleware event bridge described below.

`AudioSpatialMode::TwoD` is a complete direct-to-bus path. It does not require a
listener, spatial provider, provider capability or spatial-provider activation;
only `ThreeD` requests enter the provider-resolved spatial path. A legacy
two-dimensional source may still carry its authored doppler bit, but that bit
does not make the request depend on a spatial provider. Scene teardown uses the
request's scene context and lifecycle policy, so closing a context rejects late
requests and unloads its admitted voices through the ordered Audio barrier.

Scene files written with the former boolean `spatial` field are migrated to the
typed `spatialMode` value on load; new files always write `"2d"` or `"3d"`.

In **event middleware** mode the native Horo mixer remains active. The bridge
must reserve a proxy slot in `VoiceRegistry` for each middleware event so that
core voice limits and virtualization policy apply to the combined native +
middleware voice budget. Any private vendor playing handle is owned by the
bridge and never leaves it; Horo exposes only `AudioEventInstanceId`. The proxy
slot carries the same priority, bus, and virtualization metadata used by native
voices. If the bridge cannot create a proxy slot, the event is rejected according
to the same policy as a native voice. Backend
replacement mode may own physical voice allocation, but it still consumes the
effective Horo budget and reports admission, steal, virtualization and completion
through stable Horo observations.

Event bridge does not open a second final output. Its prevalidated vendor stems
enter ordinary Horo buses as bounded source blocks while Horo retains Master and
device ownership. Backend replacement owns the final vendor mixer/device; native
Horo sources coexist only when the adapter preflights explicit feature mapping or
`HoroSubmixInput` support.

### AudioListenerComponent

`AudioListenerComponent` defines the listener transform used for 3D audio,
usually attached to the active camera. A scene may contain multiple authored
listeners, but the runtime listener policy selects the active listener
deterministically.

Common policy:

```text
Camera
  [x] Main Audio Listener
```

If no active listener exists, 3D sources either fail validation or play through a
documented fallback policy. They must not silently use an arbitrary camera.

## Mixer Graph

[ADR-065](../../adr/065-mixer-topology-and-constrained-dag.md) is the single
normative owner of legal bus/send/return topology, feedback policy, deterministic
processing order, graph compilation, publication, and retirement. The mixer is a
constrained DAG: every non-Master bus has one primary parent route, sends add
explicit cross-tree routes, and all audio plus same-block control dependencies
must remain acyclic. Baseline feedback paths, including implicit one-block-delay
feedback, are rejected before a graph can reach the callback.

The default mixer template contains named buses:

```text
Master
  +-- Music
  +-- SFX
  +-- UI
  +-- Voice
  +-- Ambient
```

The default core bus/category set is:

| Bus | Purpose |
|---|---|
| Master | Final output control. |
| Music | Music and adaptive music stems. |
| SFX | Gameplay sound effects. |
| UI | Runtime game UI sounds. |
| Voice | Dialogue and voice lines. |
| Ambient | Long-running ambience and environmental beds. |

Buses use stable `AudioBusId` identity; their names and authoring positions are
presentation data. One `MasterOutput` root feeds the output adapter. Sends carry
stable route IDs, typed pre/post-fader taps, gain, and explicit destinations;
return buses are ordinary typed buses with one primary route toward Master.

Buses support typed gain, mute, pause, routing, and effect descriptors. The Audio
control runtime compiles complete immutable render-plan generations off the
callback. Compilation validates the rooted primary tree, combined DAG, layouts,
providers, and budgets, then derives one stable-ID topological order and canonical
incoming-route/voice/effect accumulation order. Graph changes commit through a
bounded `SwapMixerGraph` command at a buffer boundary. Failed or stale builds
leave the last good generation active, and old plans retire only after matching
callback acknowledgement and owner-lease/tail policy completion.

Rendering uses the ADR-065 pull model. Each source publishes immutable pre/post-
fader taps into compiled storage; a destination clears its own accumulator, then
pulls completed source taps in canonical route order. Sources never push into a
destination accumulator, so destination clearing cannot erase upstream signal.

[ADR-066](../../adr/066-spatial-provider-and-required-capability.md) is the single
normative owner of spatial provider identity, typed capabilities, profile
resolution, activation preflight, fallback, runtime failure, and observability.
Each output/listener context resolves one `SpatialRenderProfile` to a prepared
provider before callback publication. Buses and voices select modes supported by
that provider; they do not discover providers during rendering.

The profile may resolve the core stereo panner or a custom spatializer:

```cpp
enum class SpatialProcessStatus : std::uint8_t {
    Rendered,
    RenderedSilence,
    Fault,
};

struct SpatialProcessResult {
    SpatialProcessStatus status;
    AudioSpatialFaultCode fault;
};

class IAudioSpatializer {
public:
    virtual SpatialProcessResult ProcessSpatial(
        const VoiceSpatialInput& input,
        VoiceSpatialOutput& output) noexcept = 0;
};
```

This virtual interface is an internal semantic sketch, not the third-party ABI.
External spatial providers use ADR-069's family-specific versioned Audio RT table;
backend-native types and compiler-specific C++ ownership never cross the seam.

The core provides an explicit deterministic stereo provider. Packages may register
HRTF, Ambisonic, platform-native, or middleware spatializers with typed stable IDs,
capabilities, limits, layouts, ABI, owner leases, and real-time declarations.
Spatializer processing consumes prevalidated per-voice spatial input and ADR-063
blocks; it must not query scene, physics, configuration, files, package discovery,
or network from the real-time thread.

`SpatialProcessResult` is a fixed-size allocation-free callback result, not a
general diagnostic object. `Fault` carries a stable bounded code; the callback
overwrites the admitted output range with ADR-063 positive-zero silence and emits
the detailed preallocated fault record owned by ADR-062. Providers never allocate,
format a message, log, or invoke control/host failure handling from `ProcessSpatial`.

A spatializer computes per-voice 3D positioning, such as panning, HRTF, or
object-based spatial rendering. DSP nodes process bus-level signals such as
filters, reverb sends, or ambisonics decoding. A spatializer that needs bus-level
processing may internally use DSP nodes, but its public contract to the mixer is
per-voice positioning.

An optional profile may use core stereo only when its persisted policy explicitly
allows fallback and core satisfies every remaining hard content/layout limit. The
result is a durable observable `ActiveFallback` generation naming the requested
and selected providers and reason. A required missing/incompatible provider or
capability fails activation preflight; it never starts unnoticed with stereo.
Runtime faults follow the same policy through control-owned prepared generation
replacement. The callback emits a bounded fault and compiled silence behavior; it
does not select or instantiate a fallback.

User volume settings are configuration values. Per-frame GUI controls do not
directly edit backend state.

The editor may expose mute, solo, volume, routing, meters, and preview controls.
Only gain, mute, pause, routing, and effect changes are runtime mixer state.
`Solo`, meter visibility, and preview overrides are editor-session/debug state
and must not be serialized as packaged-game mixer behavior unless an explicit
debug profile requests it. Runtime-affecting controls submit mixer graph changes
through the audio frontend; they do not edit backend state directly.

## Effects And DSP Boundary

[AUD-004](https://github.com/HoroCore/horo-engine/issues/561) delivers the
core mixer/DSP baseline for M5 — 1.0. Built-in nodes may use private C++ strategies;
third-party nodes use ADR-069 rather than this illustrative virtual interface.

Core effects are intentionally small and deterministic:

- gain
- low-pass filter
- high-pass filter
- reverb send

Core effects and custom submix effects use the same DSP node contract:

```cpp
class IAudioDSPNode {
public:
    virtual Result<void> Prepare(const AudioDSPContext& context) = 0;
    virtual void Process(AudioBuffer& buffer) = 0;
};
```

`Prepare()` runs outside the real-time callback and may validate descriptors,
allocate fixed buffers, and build immutable processing state. `Process()` runs
on the real-time path and must not allocate, block, log, access files, or invoke
unbounded user callbacks. Nodes that do not declare this contract at graph
build time are rejected by the mixer graph validator.

Graph validation runs whenever a mixer graph is built or modified:

- in the editor when a bus graph or effect chain is authored
- at scene load time
- when a package hot-reloads an effect node

Validation is a contract check: the node must advertise whether it allocates,
blocks, or calls user code during `Process()`, and the validator rejects nodes
that do not match the real-time-safe contract. Static verification is preferred
where possible, but the minimum requirement is explicit declaration and
runtime rejection before the node reaches the audio callback.

Advanced effects such as convolution reverb, granular synthesis, procedural
node audio, platform-specific spatial audio, and middleware DSP are package or
extension features. The real-time callback may execute prevalidated DSP state,
but it must not allocate, block, or invoke unbounded user code.

[ADR-071](../../adr/071-procedural-audio-graph-ownership.md) is the single
normative owner of procedural graph asset identity, compilation, deterministic
inputs, extension nodes, editor boundaries, runtime limits and retirement. A
procedural graph is a separate compiled sound generator for one voice; it is not a
mixer graph, bus DSP chain, adaptive-music state machine or executable editor
document. The contract is fixed at M0 for compatibility, while implementation and
product delivery remain AUD-013 Post-1.0.

### Procedural Audio Graphs

AST provides stable identity, dependencies, cache, staging and publication under
ADR-064. AudioModel owns stable node/pin/link/parameter/trigger IDs and typed graph
semantics; AudioCook validates an immutable source snapshot and emits a versioned
`CompiledSoundGenerator`. Editor node position, selection, undo state, preview
history and widget IDs are excluded from runtime semantics and cook identity.

The baseline graph is an acyclic typed signal/control plan. Its compiler uses
canonical node and link IDs for order, resolves dependencies, constants, layout/
rate adapters and fixed instance/scratch storage, and rejects hidden/unbounded
work. Random/noise nodes consume an explicit instance seed and stable stream ID;
ambient entropy, wall time, thread ID and global random state are forbidden. The
same artifact, format/block schedule, parameters/triggers, seed and resources must
produce the profile's declared deterministic numerical result.

Audio control prepares each generator instance outside the callback, pins its
asset/provider generation, admits declared state/scratch/work/latency/tail/event
and voice costs, then publishes it through the ordinary voice boundary. The
callback writes the prepared generator into a borrowed ADR-063 output block and
continues through normal voice gain, pitch, spatialization and ADR-065 bus routing.
It cannot compile, allocate, perform I/O, discover services or mutate topology.

Parameters and triggers use stable typed IDs and the normal generation-tagged,
sample-boundary AudioFrontend scheduling path. Gameplay or an adaptive-music
package may push values and create a generator voice, but the graph cannot read
gameplay/ECS state, choose music transitions, schedule other voices or own a
clock. It may receive admitted music phase/sample evidence only as an explicit
typed input under ADR-068.

Extension nodes use the closed versioned `audio.generator.node` family layered on
ADR-069. Native callback code must satisfy the Audio RT ABI, trust, resource limit,
fault and lease contracts; a generic extension callback or scripting/editor object
cannot execute in the callback. A generator cannot create buses, sends, returns or
effects, and a mixer DSP capability does not implicitly grant generator-node use.

The Editor owns graph document lifecycle, node presentation, commands, undo/redo,
localized diagnostics and preview controls. Audio owns schema validation,
compilation, runtime admission and faults. Preview compiles a revision-pinned
candidate and instantiates it through the same AudioFrontend/voice/mixer path used
by packaged games; stale results are discarded when the document revision changes.

### Bus Sidechain And Auto-Ducking

Bus-to-bus automatic ducking (for example, dialogue ducking music when a voice
line starts) is not part of the core mixer. The snapshot system supports
manually triggered ducking through gameplay code. Automatic sidechain ducking
is provided as a package/extension DSP node:

```cpp
struct AudioSidechainDescriptor {
    AudioBusId sourceBus;     // bus whose level is measured
    AudioBusId targetBus;     // bus whose gain is modulated
    float thresholdDb;
    float attackMs;
    float releaseMs;
    float duckAmountDb;
};

class IAudioSidechainDuckNode : public IAudioDSPNode {
public:
    virtual void Configure(const AudioSidechainDescriptor& desc) = 0;
};
```

The node reads a buffered RMS/peak measurement from the source bus, computes a
control signal off-thread, and applies gain reduction to the target bus on the
real-time path. Core ships no built-in sidechain; packages provide the node when
a project needs broadcast-style auto-ducking.

## Voice Model

A voice represents one active playback instance with:

- clip, stream, or prepared sound-generator handle
- playback state and cursor
- gain and pitch
- loop policy
- spatial parameters
- bus destination
- priority and virtualization policy

Voice limits are explicit. When capacity is reached, the frontend applies the
configured reject, replace, or virtualize policy deterministically.

Voice priority and virtualization are core features. A low-priority distant SFX
may be virtualized or rejected before a high-priority UI or voice line. Policy
decisions are observable through metrics.

## 2D And 3D Playback

Audio sources support two spatial modes:

| Mode | Behavior |
|---|---|
| 2D | No spatial attenuation; routed directly to the selected bus. |
| 3D | Uses source/listener transforms, distance attenuation, and panning. |

Core 3D audio provides:

- distance attenuation
- panning through the preflight-resolved spatial provider and output layout
- optional per-source doppler pitch shift
- configurable min/max distance and rolloff curve

Scene extraction provides either explicit velocity or previous/current
transforms for sources and listeners. The audio runtime does not query physics
state directly from the real-time path.

Doppler is core because the required source/listener velocity data already
exists in scene extraction and the calculation is deterministic. It is controlled
by `AudioSourceComponent::enableDoppler` so projects that do not need it pay no
per-source policy cost. HRTF, Ambisonic/object and platform-specific spatializers
remain optional providers behind the 1.0 ADR-066 contract. Basic raycast occlusion,
acoustic-material contribution, zones and environment sends are the AUD-006 M5 —
1.0 environmental baseline; rooms/portals, diffraction, baked/geometric
propagation and advanced reflections are AUD-014 Post-1.0.

### Occlusion Provider Interface

Occlusion, obstruction, and diffraction are not computed by the audio runtime.
Packages may provide an occlusion service that runs during scene extraction or a
non-real-time audio update phase:

```cpp
struct AudioOcclusionQuery {
    Vec3 sourcePos;
    Vec3 listenerPos;
    AudioOcclusionHandle outHandle;
};

class IAudioOcclusionProvider {
public:
    virtual void RequestOcclusion(std::span<AudioOcclusionQuery> queries) = 0;
};
```

The interface is an internal semantic sketch. Package providers use their typed
ADR-069 acoustic capability and may not expose Physics/native types through Audio.

The 1.0 baseline ships a null/reference provider plus a qualified basic physics-
backed raycast provider. It may raycast outside the real-time callback, then feed
the result back at most one game frame later as ramped gain, filter, or send
parameters. Rooms, portals and diffraction are not part of this provider baseline.
Missing a deadline is a provider bug: the runtime applies the last known valid result
ramped toward a documented safe fallback and emits a staleness metric. The audio
callback never performs raycasts or scene queries.

Observable occlusion/zone metrics:

```text
audio.occlusion.stale_value_age_ms     // age of the last applied occlusion result
audio.occlusion.deadline_miss_count    // deadline misses since startup
audio.occlusion.active_query_count     // queries pending off-thread
```

## Parameter Automation, Fades, And Snapshots

Audio parameter changes may be immediate, timestamped, or ramped. Gain, pitch,
filter cutoff, send level, and bus gain changes should use buffer-boundary-safe
or sample-safe ramps to avoid clicks and pops.

```text
SetBusGain(Music, -6 dB, fade = 250 ms)
SetVoicePitch(voice, 0.8, fade = 100 ms)
SetLowPassCutoff(SFX, 1200 Hz, fade = 500 ms)
```

The core supports simple audio snapshots as named mixer-state presets:

```text
pause_menu
underwater
combat
dialogue_focus
```

Snapshots apply bounded parameter ramps to buses and sends. Example:

```text
Dialogue starts
  -> Music -6 dB over 250 ms
  -> SFX   -3 dB over 250 ms
  -> Voice unchanged
```

Advanced adaptive music and procedural modulation remain package or extension
features. Adaptive-music product delivery is AUD-015 Post-1.0; the 1.0 core
snapshot and music transport systems provide predictable ducking, mix-state
transitions and sample-clock scheduling without owning gameplay state policy.

Adaptive music systems are higher-level orchestrators over this command and
snapshot model. They may run state machines, vertical remixing, stem crossfades,
or transition rules in gameplay/package code, then submit scheduled audio
commands through the normal command queue. They do not require a separate
real-time path.

## Music Transport And Cross-System Ownership

[ADR-068](../../adr/068-music-transport-and-cross-system-ownership.md) is the
single normative owner of the sample-clock music transport and its Gameplay,
Cinematic, Animation, Localization, Accessibility, and Save boundaries. Audio
owns generation-scoped sample time, transport cursors, tempo/marker-to-sample
conversion, exact command batches, prepared media, and playback observations.
It does not own gameplay/adaptive-music decisions, narrative progression,
sequence or animation clocks, locale selection, caption presentation, or the
save archive.

Cross-system clients submit bounded generation-tagged intent with stable request
and semantic occurrence/cue IDs, source-clock evidence, timing target, and late/
discontinuity policy. Audio control validates and maps the intent to the current
sample epoch; only the callback applies the prepared batch at its sample boundary.
Render sampling never emits authoritative sounds, and callback observations reach
domain owners only through control-owned bounded queues at their allowed boundary.

Localization resolves voice/media assets before Audio. Dialogue/gameplay fans
semantic caption cues independently to Accessibility/UI, so mute, virtualization,
missing media/device, or Audio failure cannot suppress subtitles. Save participants
persist only semantic music/narrative state and optional stable section/musical
position; voice handles, device sample epochs, queues, decoder/DSP state, buffers,
and native handles are never serialized.

## Audio Zones

`AudioZoneComponent` is not required for the initial M1/M2 prototype foundation.
It is part of the AUD-006 M5 — 1.0 environmental
baseline and owns area-based environment sends such as reverb routing, low-pass
regions, or bus overrides. A product checkpoint that has not yet delivered zones
must not expose a create action or claim AUD-006 qualification; reverb sends remain
controllable by ordinary source/bus parameters meanwhile.

Audio zones should integrate through the same non-real-time query/update model
as occlusion. Scene extraction determines source/listener relation to zones,
then applies ramped send, filter, or routing changes through normal audio
commands. Zone intersection tests must not run inside the audio callback. Like
occlusion, zone-driven parameter changes should be applied at most one game
frame after the query, unless the provider documents a longer latency policy.

## Scene Integration

Audio source and listener components contain backend-neutral state. During
runtime update, the scene extracts an audio snapshot or commands:

- source transform and velocity
- listener transform
- playback requests
- parameter changes
- stopped or destroyed sources

Authoring documents never store live voice handles.

Editor create menus may expose:

```text
Create > Audio > Audio Source
Create > Audio > Ambient Sound
Create > Audio > Music Player
Create > Audio > Audio Listener
Create > Audio > Reverb Zone   (shown only when audio zones are enabled)
Create > Audio > Audio Bus / Mixer Asset
```

`Audio Listener` is normally created through the camera inspector rather than as
a free-floating object, but the component remains a first-class runtime type.

`Ambient Sound` and `Music Player` are presets over `AudioSourceComponent`, bus
selection, and streaming/loop policy. They are not separate playback systems.

`Reverb Zone` appears only when `AudioZoneComponent` is implemented or when an
audio-zones package/feature is enabled. It must not be shown as an available
create action before the feature can create valid runtime data.

## Real-Time Command Queue

[ADR-120](../../adr/120-cinematic-event-dispatch-and-audio-coupling-boundary.md)
specializes the cinematic producer seam without changing Audio ownership. EventTrack
gameplay occurrences use the application-owned `CinematicEventDispatcher`; an
AudioTrack instead submits a bounded generation-tagged schedule/cancel/seek/preroll
bundle through the Audio/Cinematic adapter into `AudioFrontend`. Sequencer supplies
stable occurrence identity, source `SequenceTime` and intent. Audio alone validates
prepared media, correlates source time to sample time, admits the schedule, publishes
callback commands, owns voice/transport lifetime and reports actual observations.

The seam consumes AUD-001.1/ADR-062 for runtime/callback phases,
AUD-002.1/ADR-064 for cooked media and readiness, and AUD-008.1/ADR-068 for transport,
sequence-to-sample mapping, seek/preroll, late policy and acknowledgement. CIN does
not create alternate Audio rules. Required unavailable media fails cinematic
activation with a typed result. Optional silence/skip must be authored; unloaded media
uses bounded asynchronous preparation. No missing-media path performs synchronous I/O
on sequence evaluation or the callback, substitutes a filename/clip, or reports an
implicit successful silent voice.

Audio commands may originate from gameplay systems, animation events, timeline
playback, editor preview, asset hot reload, streaming services, scene unload, or
device lifecycle services. These producers do not write directly into the audio
callback's real-time command buffer.

Command flow:

```text
Gameplay / editor / asset / lifecycle producers
      |
      v
MPSC AudioCommandQueue
      |
      v
AudioFrontend drains, validates, normalizes, and coalesces commands
      |
      v
SPSC real-time command buffer
      |
      v
Audio callback consumes bounded prevalidated commands
```

Accepted command kinds include:

```text
CreateVoice
StartVoice
StopVoice
SetVoiceParameters
SwapMixerGraph
ReleaseResource
ScheduledCommandBatch
```

Queue-full behavior is explicit. Critical stop/release commands have reserved
capacity or a separate safe path. The frontend may merge redundant parameter
changes before they enter the real-time buffer, but it must not reorder commands
across documented lifecycle boundaries such as scene unload or device reset.

The additive `HoroEngine::AudioCommands` contract owns `Audio/AudioCommands.h`
and depends on AudioMemory for generation-scoped prepared storage identities.
Its typed value surface represents next-buffer-boundary create/start/stop,
parameter, graph-publication, logical-release, scene-unload and reset intents,
plus retained scheduled-batch references. `Audio/AudioClock.h` maps a bounded
producer-clock correlation to one exact sample frame using checked integer
arithmetic. Runtime, sample-clock, producer-clock and discontinuity generations
must all match; paused, stale, out-of-window and overflow mappings are explicit
rejections. The mapping never reads wall time or derives callback cadence from a
presentation or simulation frame.

`Audio/ScheduledAudioCommandBatch.h` prepares at most sixteen normalized commands
with one complete scope and one target. A next-buffer target carries no sample
identity; an exact-sample target carries nonzero clock and discontinuity
generations. Control retains the complete fixed batch in admitted CommandStorage
and publishes one `AudioScheduledBatchCommand` reference through the existing
FIFO. The callback therefore observes the batch as one indivisible record and
applies its children in declared array order at the named boundary. Nested batches,
mixed scopes, ambiguous targets and incomplete storage fail before publication.
These contracts do not execute voices or commit lifecycle state. Implementations
must consume them rather than inventing string opcodes or callback-side asset/ECS
lookups.

All scene-bound intents carry exact runtime/epoch/context identities. Global
audio uses an explicitly host-owned context; a reset instead has an entirely
empty scene handle and targets the current runtime epoch. Only control stages
lifecycle barriers. Structurally valid handles are not proof of actual liveness:
control validates registry generations, closed admission, prepared resources and
leases before publication. Copying an intent retains values, not resource lifetime.
The control owner retains referenced graph/clip/voice storage through consumption
and the appropriate acknowledgement; release intents never free it on the callback.

Normalization rejects malformed identities and nonfinite parameter values without
partially changing its output. It canonicalizes signed zero and subnormal parameters
to positive zero but preserves finite model units without a hidden gain/range clamp.
Only adjacent unpublished parameter snapshots with the same complete scope, voice
and parameter may coalesce. These snapshots are last-value-wins state, not individually
completed playback operations. No start/stop/release or barrier is coalescible, and
the critical reservation class never grants priority reordering. Queue implementations
must preserve accepted FIFO order and keep required work caller-owned after explicit
rejection/retry; classification alone does not provide a saturation guarantee.

`Audio/AudioCommandBuffer.h`, owned by the same AudioCommands target, implements
the final SPSC publication boundary. Its one-slot CommandStorage pool reserves and
initializes the complete fixed record array off-callback. Capacity is a power of two
from 2 to 65,536 and includes a positive critical reserve smaller than the total.
Ordinary publication stops at total-minus-reserve occupancy; critical publication
may use the full ring. This is one FIFO, not a separate priority lane. Full critical
publication returns `CriticalRetry` without accepting or forgetting the record.
Control retains that record and does not bypass it with later work. Sequence values
are nonzero, strictly increasing and never wrap; failed publication consumes no
sequence. The buffer is bound to one runtime epoch and cannot activate a new one.

Exactly one control producer owns its write cursor and last sequence; exactly one
callback consumer owns the read cursor. Publishing the record precedes release of
the write cursor, paired with the consumer's acquire. Copying the record precedes
release of the read cursor, paired with the producer's acquire before slot reuse.
The uint32 cursors intentionally wrap: outstanding distance never exceeds 65,536,
and power-of-two capacity divides the cursor range, preserving mask indexing across
rollover. Only always-lock-free cursor/bool atomics are supported. Immutable dimensions
and record spans are published before either participant starts. No callback method
allocates, frees, locks, normalizes, visits external state, or invokes caller code.

Control closes publication after its final accepted record. The consumer can then
drain all accepted records in order; closed-and-empty is observable but does not prove
backend detachment, completed command execution, or external-resource quiescence.
The host must join producers and detach/join the callback before moving or destroying
the owner. Ring-slot reuse does not authorize releasing a graph/clip/voice referenced
by the copied record. Registry admission and lifecycle acknowledgement stay under
the control runtime, not this transport primitive.

This introduces no existing-caller migration or compatibility layer. Hosts opt into
AudioCommands explicitly; public-header consumer tests check both narrow headers.
SPSC tests cover reserved critical FIFO barriers, exact sequence/epoch rejection,
all preparation allocation failures, moves, close/drain, zero general heap allocation
and free on hot methods, and 50,000 ordered two-thread records with repeated slot reuse.
`Audio/AudioCommandStaging.h` completes the bounded MPSC-to-SPSC path for these
buffer-boundary intents. Its separately identified CommandStorage pool and fixed
scene-gate array are allocated before publication. The ingress budget includes
record/pool metadata and scene gates; the callback reservation is independently
bounded. Fixed owner/mutex overhead is excluded. One mutex protects ingress FIFO,
scene admission, accepted sequence and close/reset facts. Producer and control
entry points only try-lock: `Busy` accepts nothing and leaves work caller-owned.
They do not wait for the lock, and the callback never touches it. This is not a
lock-free or starvation-free producer claim; the host owns bounded retry scheduling
and required-work reconciliation outside the callback.

Under that mutex, submissions receive non-wrapping uint64 sequences and reserve
ordinary or critical capacity. Only adjacent unpublished parameter snapshots may
replace an earlier snapshot; the result exposes both accepted and replaced
sequences. The control pump publishes at most its explicit record budget and
stops on the first blocked SPSC publication. It retains that record and its
successors rather than letting a later critical command leap over an earlier
accepted operation. Saturation is pressure, not implicit acceptance or disposal.
The producer/control caller retains `Busy`, `OrdinaryFull` and `CriticalRetry`
inputs for retry or explicit lifecycle reconciliation. After the host stages all
required critical work, `Close` rejects new ingress and the pump closes SPSC only
after every accepted ingress record is published.

Control explicitly registers each admitted scene or host-owned context slot and
generation. A queued or not-yet-acknowledged context cannot be replaced. Producers
cannot submit reset/unload payloads through ordinary ingress. `StageSceneUnload`
closes ordinary scene admission before attempting the reserved barrier; a full
critical queue leaves that admission closed, with the barrier still a required
control retry. Stop/release may finish entering before the barrier is queued,
but no further old-context work enters after it. Exact epoch/context/sequence
acknowledgement and control's independent execution/resource-lifetime proof are
required before that slot admits a strictly newer generation. Checking that the
barrier was published prevents premature acknowledgement, but is not itself proof
that the callback executed it.

`StageReset` similarly closes ordinary epoch admission during a critical retry.
Once the final reset barrier is queued, all ingress closes, so old work cannot
cross it into a new epoch. Existing accepted work drains before that barrier.
The host reconciles any unaccepted work and constructs a new epoch owner only
after quiescence; this transport neither changes device state nor commits reset.
All producers must be joined and the callback detached/joined before moving or
destroying staging. Destruction additionally requires accepted work to have been
drained or explicitly reconciled by the control runtime.

End-to-end tests saturate both queues with stop/release/unload/reset work, check
late producer rejection and acknowledged generation reuse, verify adjacent-only
coalescing and retained output backpressure, inject every preparation allocation
failure, and move/close/drain owners. Four concurrent producers, one control pump
and one callback consumer transfer 12,000 records in accepted sequence order.
Allocation/free probes cover admission, pump and callback operations. These tests
exercise the staging contract; actual voice/clip/graph liveness, execution outcomes,
device lifecycle and clock-mapped scheduling remain with their owning subsystems.

Scheduled commands carry an audio timeline timestamp or buffer-boundary target.
Commands in the same `ScheduledCommandBatch` with the same timestamp are applied
atomically at the same buffer boundary. Timeline systems and animation-event
systems use this to start multiple voices in sync without adding a separate
real-time API.

The concrete AUD-001.7 contract resolves producer timestamps on control through
an immutable `AudioClockCorrelationSnapshot`. Correlations have an inclusive
validity interval and exact runtime, sample-clock, producer-clock and discontinuity
generations. Conversion truncates sub-frame fractions toward the correlation
anchor; it never rounds work into a later boundary. A paused correlation cannot
schedule even its anchor timestamp. Resume, reset, seek or replacement must publish
a new generation/revision before scheduling resumes.

For footsteps, [ADR-091](../../adr/091-footstep-and-locomotion-event-ownership.md)
requires an application-owned post-commit adapter to join the Animation occurrence
with the exact Character surface snapshot first. Audio receives an immutable,
deduplicated cue intent with committed tick/correlation identity; it does not query
Character/Physics, infer cadence or change simulation when admission fails.

For destruction, [ADR-147](../../adr/147-destruction-event-and-cosmetic-consumer-ownership.md)
requires an application-owned dispatcher to map a committed DFR fact through a captured
cooked binding generation before Audio admission. Audio receives bounded cue/media
intent with the destruction occurrence plus destination-layer identity. It alone owns
media readiness, source-to-sample scheduling, voice/mixer/device state and callback
observations. Missing media, virtualization, queue pressure or device loss cannot alter
the committed destruction revision, and the Audio callback never invokes DFR/gameplay.

The audio thread publishes bounded completion and device events through a
lock-free or wait-free queue consumed by the owning main-thread service.

## Streaming

[AUD-002](https://github.com/HoroCore/horo-engine/issues/536) delivers the
qualified resident/streaming baseline by M4 — Beta. Streaming is part of 1.0, not
an optional Post-1.0 extension.

Streaming uses worker or I/O jobs to fill preallocated ring buffers. The audio
callback consumes available frames without waiting.

Underrun behavior:

- output silence for missing frames
- increment an underrun metric
- emit one rate-limited diagnostic outside the real-time callback
- continue or stop the stream according to policy

Streaming buffers have per-stream and global memory budgets.

Music and long ambience default to streaming. Short SFX and UI sounds default to
decoded resident clips. Per-platform cook settings may override compression,
sample rate, channel layout, and streaming thresholds.

## Editor Tooling

[AUD-009](https://github.com/HoroCore/horo-engine/issues/615) delivers these
workflows for M5 — 1.0. Middleware/procedural/capture-specific authoring surfaces
follow their Post-1.0 capability boundaries and are not implied by the core tools.

HoroEditor provides audio authoring tools:

- audio source inspector and preview playback
- listener selection and validation
- mixer/bus panel with runtime gain, mute, pause, routing controls and
  editor-only solo/meters/preview overrides
- stream/cook settings preview
- active voice and bus meter view
- asset import diagnostics for sample rate, channels, loops, and loudness
- scene validation for missing listeners, missing clips, invalid buses, and
  unsupported spatial settings
- Project Settings > Audio > Focus Behavior and per-platform profile UI

Editor preview uses the same audio frontend contracts as runtime playback. It
does not bypass real-time thread rules.

## Device Lifecycle

[AUD-007](https://github.com/HoroCore/horo-engine/issues/594) delivers the
qualified desktop backend/device baseline for M5 — 1.0.
[ADR-067](../../adr/067-platform-audio-backend-strategy.md) is the single
normative owner of output-backend roles, the supported 1.0 platform matrix,
compile-time composition, startup selection, common parity, and qualification.
WASAPI on Windows 11 x86_64, Core Audio on macOS 14+ arm64/x86_64, and PipeWire on
Linux x86_64 (Ubuntu 24.04 LTS reference) are equal-peer native production
backends. SDL3Audio is an explicit portability/reference peer and NullAudio is the
headless/test peer. SDL3Audio is not the base or silent fallback for a native
backend merely because the host also uses SDL3 for windowing.

The implemented SDL3 reference adapter is a separate optional `HoroAudioSdl3`
target. SDL headers and linkage remain private. Construction is inert; explicit
control operations initialize/probe SDL, publish an owned catalog with a stable
default-role identity, open one logical device, and install preallocated
planar-to-interleaved conversion only during `Start`. The SDL audio thread invokes
the retained Horo render port and publishes bounded callback facts. Quiescence and
stream destruction prove callback detachment before `Close`. The adapter reports
native hotplug as unsupported until the explicit switching/recovery transaction owns
loss, default-role migration, and retained delivery end to end.

The application composition root links and registers only admitted backend
targets, then resolves one stable backend ID before device construction. Backend
identity remains fixed for the AudioRuntime lifetime; selecting another peer
requires host-owned runtime replacement. A startup fallback list must be explicit
and fully preflighted, and interactive products never fall back to NullAudio.

ADR-062 owns the parent runtime state machine and callback-epoch handshake. The
states below are subordinate backend/device facts committed only by the audio
control runtime; the callback and backend cannot select product fallback or write
runtime state.

Device states:

```text
Closed -> Opening -> Ready
Ready -> Reconfiguring -> Ready
Ready | Reconfiguring -> Lost -> Opening | Closed
Ready | Reconfiguring | Lost -> Quiescing -> Closed
```

Device loss does not invalidate frontend asset identity. The runtime may
recreate device-owned resources and resume voices according to explicit policy.

Sample-rate, channel-layout, and buffer-size changes are committed at a safe
boundary.

The implemented `AudioApi` discovery slice exposes `AudioDeviceSnapshot` and
`ResolveAudioDevice`. A complete bounded snapshot owns its endpoint labels and
independent console, multimedia and communications defaults. IDs retain the
existing runtime/slot/generation contract; the adapter privately maps native
identities and must preserve IDs across unchanged enumerations. Snapshot revisions
increase on endpoint/default changes, and resolution returns the exact revision
for control to revalidate before an asynchronous open commits. Explicit stale or
missing selections never fall back to a default or the first enumerated endpoint.
Null endpoints are explicitly headless and make no physical-hardware claim.

This is an additive public contract with no existing callers to migrate. The
generated `AudioApi` public-header consumer and headless discovery tests cover the
new boundary. `AudioDeviceFormatRequest` preserves the preferred complete
rate/layout tuple, at most eight explicit alternatives and a bounded callback
period interval. `AudioNegotiatedDeviceFormat` reports effective planar and native
signal/PCM values separately. Admission rejects stale discovery/device identity,
unlisted tuple combinations, undeclared preference deviations, unapproved native
rate conversion and incomplete or semantically incorrect channel permutations.
Accepted structural data still requires prepared adapter resources, fresh format
revision, optional-mode admission and the matching callback-ready handshake.

`AudioBackendProbe` separately reports build presence, host support, current API/
service availability and typed optional-feature evidence. Unknown, Unsupported,
Unavailable and Available remain distinct; neither linking a backend nor a valid
probe activates audio. Null reports native/physical features Unsupported rather
than claiming hardware support. No available feature is published from an
unavailable or unprobed backend.

`AudioDeviceEpoch` carries runtime/device identity, format revision and callback
epoch together. Timing reports and callback facts reject stale tuples and foreign
monotonic clock domains. `AudioDurationObservation` preserves Unknown,
Unsupported, Unavailable, Estimated, Reported and Measured separately; missing
knowledge carries no numeric zero. Numeric evidence includes a source and named
monotonic timestamp, while measured means additionally require a positive sample
count/window. A report cannot contain observations from after its capture time.
Physical hardware/end-to-end measurements require loopback provenance; callback
measurements use callback or deterministic clocks. Null reports physical latency
Unsupported. Latency components are not implicitly additive.

`AudioCallbackEvent` is a trivially copyable bounded record with epoch, sample
cursor, monotonic timestamp and a typed Ready, Quiesced, Underrun or Fault payload.
Ready/Quiesced/first-Fault delivery must be retained under saturation; underrun
telemetry is separately classified. Fact validation does not commit runtime state
or prove native callback detachment. The transport must implement that retention
obligation before using the event contract in a real callback.

Enumeration execution, callback buffer execution/transport and asynchronous device
operations remain subsequent device-contract work. These value validators do not
prove availability, activation, actual sample conversion or hardware qualification,
and do not replace host-owned preflight or lifecycle commits. The additive headers
are owned by AudioApi and covered by standalone consumers and headless tests;
there are no existing callers to migrate.

## Input Capture And Speech Boundary

[AUD-012](https://github.com/HoroCore/horo-engine/issues/644) delivery is
Post-1.0 and is not required for 1.0 qualification. ADR-070 defines its boundary at
M0 so later integrations cannot move permissions, privacy, NET packets, speech, or
editor-agent intent into Audio.

[ADR-070](../../adr/070-capture-and-voice-io-ownership.md) is the single normative
owner of Audio, Platform, Security, NET, Speech and Editor capture boundaries.
Platform/private backends own native input and permission adapters; Security/host
policy owns purpose, consent, retention and privacy; Audio owns device-neutral
capture sessions, timestamped PCM, bounded buffers, monitoring and recording
candidates; NET owns packet/session/encryption/jitter/moderation policy. Speech and
Editor own transcripts and explicit agent submission.

Audio owns input-device discovery, permission-aware capture lifecycle,
timestamped PCM format, bounded capture buffers, monitoring, and recording
publication. Capture and output may share a platform device service, but they
have separate stream generations, queues, permission states, and failure paths.

Capture producers write into preallocated or policy-bounded buffers and never
block the real-time callback. Consumers receive owner-backed timestamped spans
or bounded stream reads; they do not retain native device handles. Device change,
permission revocation, focus loss, overrun, and shutdown terminate or replace a
capture generation explicitly.

Audio does not own speech recognition, language inference, model providers,
agent intent, or network voice transport. A provider-neutral speech service may
consume an admitted capture stream and publish partial/final transcript results.
The Editor AI system decides whether a transcript enters an agent request. A
transcript is not an input-action edge and never constitutes approval for an
editor mutation.

Raw microphone data is privacy-sensitive. It is excluded from ordinary logs,
conversation history, diagnostic bundles, and telemetry by default. Recording
or retaining it requires a separate explicit user action and storage policy.

Recorded candidates enter AST staging and atomic publication; the capture callback
never writes project files. Network voice consumes capture through a bounded PCM
adapter and returns validated remote PCM as an ordinary Audio source. Audio never
sees sockets, packets, credentials or peer authority, and a slow NET/Speech/
recording consumer cannot block capture or output.

## Threading Rules

The real-time callback cannot:

- allocate or free general heap memory
- acquire contended mutexes
- access files or network
- call ordinary logging
- query GUI, ECS, or configuration services
- wait for jobs
- execute user-provided unbounded callbacks

It also cannot open, reconfigure, recover, or close the device; commit runtime,
scene-context, focus, suspend, reset, fatal, or shutdown transitions; invoke a
producer; or reclaim callback-visible memory. It may adopt/quiesce a published
epoch at a buffer boundary, render preallocated silence, update allocation-free
counters, and emit bounded acknowledgements/fault/device records. The control
owner consumes those records and commits every lifecycle transition.

It consumes prevalidated state and writes only to real-time-owned memory and
bounded event queues.

When a middleware backend replaces the native device and mixer, Horo's
threading rules apply to `IAudioMiddlewareBackend::Submit()` and any Horo-side
processing. The middleware's own real-time thread is governed by its own
contract, which the integration must document and must not violate Horo's
ownership or observability invariants.

### Bounded Memory Ownership

`HoroEngine::AudioMemory` owns `Horo/Audio/AudioMemory.h` and depends only on
`AudioApi`. Its scratch arena and fixed block pools are preparation-time owners,
not a device, mixer, voice allocator policy, graph executor or queue protocol.
Hosts link this target explicitly; existing `AudioApi` consumers do not gain an
implicit memory implementation dependency. This is an additive public contract;
there are no existing callers to migrate or compatibility implementations to keep.

Pools reserve aligned payload and all slot metadata before publication, within an
explicit byte budget. Typed purposes separate voice, graph, command and event
storage reservations; each has a unique host-assigned pool identity within its
runtime generation. They expose raw bytes, not implicit object constructors or
destructors. General diagnostics and allocation failure reporting occur during
preparation, never during callback admission. Exhaustion returns a fixed-size
status with no heap fallback. Acquiring a block zeroes its aligned stride and is
bounded by the admitted block size; it is not a constant-time claim independent
of that size. Hosts must budget this clearing work for their callback envelope.

Runtime, pool, one-based slot and non-wrapping generation identify a block.
Retiring it stops new handle resolution but preserves existing borrows. Only the
control owner may reclaim retired storage after proving completion through its
last-use epoch and ending object lifetimes. Reclamation scans at most the explicit
visit budget in round-robin order; it neither destroys heap storage nor acts as a
barrier. Generation exhaustion permanently removes a slot from reuse. A completion
watermark is monotonic and cannot authorize a stale retirement. Exhausting the
epoch space requires a new runtime generation, not wraparound.

All operations require exclusive ownership. Runtime integration must establish a
quiescent handoff before control accesses callback-owned pools and hand ownership
back before callback processing resumes. These primitives add no atomics, locks,
ambient registration or implicit cross-thread access. Failure to prove quiescence
retains the owner and its storage under ADR-062; a failed shutdown never justifies
freeing callback-visible memory. Destruction and move assignment remain off-callback.

Scratch borrows are valid only within one strictly increasing runtime-owned epoch.
Reset requires all old borrows and child work to have ended; allocation zeroes the
requested rounded stride and cannot grow the reservation. Both primitives expose
reserved/occupied/peak bytes and saturating failed-allocation counts. Pool reservation
includes slot metadata; occupied and peak counts include payload padding and retired
blocks until reclamation. Scratch reservation is payload storage. Fixed owner-object
overhead is excluded, and reserved storage is eagerly allocated rather than a
separate virtual-memory reservation. No metric reset erases lifetime pressure evidence.

Dedicated headless tests instrument ordinary and aligned C++ allocations, validate
exhaustion, failed preparation rollback, wrong identities, stale epochs/handles,
bounded deferred reuse, zeroing, moves and alignment. Actual voice/graph and command/
event queue composition must consume these owners without introducing callback
allocation; those subsystem behaviors are not implemented by raw storage pools.

## Null Backend

The [AUD-001.10](https://github.com/HoroCore/horo-engine/issues/534) Null
backend is an M1 — Engine Prototype delivery and remains a required 1.0 test/
headless peer. It validates resources, commands, handles, clock generations and
lifecycle without opening a device, and advances the deterministic sample clock
under the same frontend/render contract. It does not claim hardware latency,
hotplug, device quality or interactive fallback.

`HoroAudioNull` is a build-tree-only concrete peer of the internal Audio backend
contract. Host composition constructs it explicitly; creating the descriptor does
not register a service, select a backend, inspect ambient state or perform hardware
probing. Tests drive admitted control operations with `AdvanceControl` and exact
negotiated callback blocks with `AdvanceCallback`. The latter uses preallocated,
64-byte-aligned planar storage and integer remainder accounting, so identical step
sequences produce identical lifecycle events, render phases and sample-clock
correlations without wall-clock input.

The Null peer retains the same render port used by interactive backends. Command
scheduling, voice progression and mixer validation therefore remain owned by the
backend-neutral render core rather than being reimplemented as Null-only policy.
Synthetic device-loss and interruption facts are explicit, bounded test inputs;
they never imply hardware discovery, automatic recovery or fallback selection.

## Middleware And Extension Boundary

[ADR-069](../../adr/069-audio-extension-capability-and-abi.md) is the single
normative owner of Audio extension capability IDs, the generic ExtensionHost
handoff, the separate Audio real-time ABI, registration, trust, ownership, and
unload. EXT/PKG remains authoritative for verified package identity, dependencies,
trust, module bootstrap, enablement, and lifecycle. Audio consumes one exact leased
activation candidate and never scans or loads packages itself.

Callback-executed third-party DSP/spatial code must implement its versioned family-
specific Audio RT C ABI with exact ADR-063 block/layout semantics, fixed memory and
work limits, separate prepare/process/destroy entry points, bounded fault reporting,
and graph-generation owner leases. A generic extension callback, `void*`, runtime
system, or application capability cannot be reinterpreted as Audio RT code. Such
code is trusted in-process native code, not sandboxed; restart is the default for
disable/update unless every graph, callback, job, tail, queue, state, and module
lease is proven drained.

[ADR-072](../../adr/072-audio-middleware-integration-model.md) is the single
normative owner of event-bridge versus backend-replacement semantics, normalized
frontend frames, stable event/parameter bindings, final device/mixer ownership,
native coexistence, global budgets, transactional banks, profiling and release
gates. Middleware contributes one closed `audio.middleware.event_bridge` or
`audio.middleware.backend` capability through ADR-069; it does not introduce a
generic third integration shape. The contract is fixed at M0, while middleware
adapter/bank/tooling implementation and product delivery remain AUD-016 Post-1.0.

Core provides:

- AudioSource / AudioListener
- AudioClip asset identity
- 2D and basic 3D playback
- mixer buses and categories
- basic DSP effects
- distance attenuation
- optional per-source doppler
- voice priority and concurrency policy
- streaming for music and ambience
- audio profiler metrics
- audio import and cook integration
- decoder plugin registry for import/cook
- DSP node and spatializer extension slots

The lists below describe ownership and supported extension seams, not the 1.0
delivery checklist. The roadmap table above is authoritative for product scope.
In particular, adaptive music systems, capture, procedural graphs, advanced
propagation and middleware implementations remain Post-1.0 even though their
contracts are documented now.

Packages or extensions may provide:

- Wwise integration
- FMOD integration
- adaptive music systems
- procedural audio graphs
- advanced spatial audio
- occlusion, diffraction, and propagation
- convolution reverb
- dialogue and lip-sync systems
- text-to-speech or speech recognition

Middleware integration has two supported shapes:

```text
Backend replacement:
  Horo AudioFrontendFrame -> middleware adapter -> vendor mixer/device
  optional Horo submix ------^ (only with admitted HoroSubmixInput)

Event middleware:
  Horo AudioFrontend -> event proxy -> vendor event engine -> bounded stems
                                                        -> Horo mixer/device
```

The application selects one exact adapter/model before runtime construction and
that identity is fixed until ordered teardown. Event bridge keeps Horo as the only
Master/device owner; the vendor does not open an independent endpoint. Backend
replacement makes the adapter the only final mixer/device owner. Event bridge and
replacement, two final Masters, or two independent devices cannot coexist in one
runtime.

### Middleware Backend Contract

Middleware integrations use explicit extension points rather than replacing
scene/audio ownership with ad hoc plugin calls.

```cpp
class IAudioMiddlewareBackend {
public:
    virtual Result<void> Initialize(const AudioBackendConfig& config) = 0;
    virtual Result<void> Submit(const AudioFrontendFrame& frame) = 0;
    virtual AudioMiddlewareStats QueryStats() const = 0;
    virtual void Shutdown() = 0;
};

class IAudioMiddlewareEventBridge {
public:
    virtual Result<AudioMiddlewareEpochToken>
    PrepareOutputEpoch(const AudioMiddlewareOutputConfig& config) = 0;

    virtual Result<void>
    ActivateOutputEpoch(AudioMiddlewareEpochToken preparedEpoch) = 0;

    virtual Result<void>
    PostEvent(AudioEventInstanceId instance,
              StableEventId event,
              const AudioEmitterContext& emitter,
              const AudioCommandTiming& timing) = 0;

    virtual Result<void> StopEvent(AudioEventInstanceId instance,
                                   AudioFadeDescriptor fade) = 0;
};

class IAudioParameterBridge {
public:
    virtual Result<void> SetParameter(AudioParameterTarget target,
                                      StableParameterId id,
                                      AudioParameterValue value,
                                      AudioCommandTiming timing) = 0;
};
```

These classes illustrate Horo's internal semantic adapter shape; they are not a
third-party C++ ABI. Packaged middleware negotiates ADR-069/ADR-072 versioned C
tables and is wrapped by a private Horo adapter target. STL, exceptions, vendor SDK
types and allocator ownership do not cross that binary seam.

Backend replacement may bypass Horo's native `MixerGraph` and `VoiceRegistry`,
but it must still consume normalized frontend frames, apply the effective Horo
budget/profile and publish bounded observations/stats. Native Horo source/bus/DSP/
spatial semantics are available only when the adapter declares exact mapping or
`HoroSubmixInput`; required absence fails activation.
Event middleware uses `AudioSourceKind::MiddlewareEvent` and routes stable event
IDs, emitter transforms, and parameters through the same command staging path as
native clip playback. It reserves proxy and conservative event-cost tokens before
posting, correlates private vendor playing handles to `AudioEventInstanceId`, and
routes declared vendor stems into Horo buses.

Parameter bridge IDs are stable authoring/runtime IDs. They are registered in
the audio parameter registry during project cook or package build so that
middleware-specific RTPC/FMOD-style parameters are resolved to numeric IDs
before entering the real-time command buffer. Runtime code never maps strings
to parameter IDs inside the audio callback.

```cpp
struct AudioParameterRegistry {
    StableParameterId Register(StableParameterName name);
    std::optional<StableParameterId> Find(StableParameterName name) const;
};
```

Middleware event IDs follow the same rule. Event names are registered in the
audio event registry at cook/build time and resolved to stable numeric event IDs
before runtime. The bridge receives `StableEventId`, not raw strings.

```cpp
struct AudioEventRegistry {
    StableEventId Register(StableEventName name);
    std::optional<StableEventId> Find(StableEventName name) const;
};
```

Event string-to-ID lookup is forbidden on the audio thread. Missing event IDs
are reported through metrics and diagnostics, not through runtime logs.

The cooked middleware binding manifest is the private translation boundary. It
maps stable Horo event/parameter IDs to exact adapter, SDK, bank and vendor identity
bytes with typed schemas and conservative cost. Vendor strings, GUIDs, hashes,
playing IDs and handles never enter ordinary gameplay APIs, scene components,
frontend frames, save data or metric dimensions.

Adapter, complete binding manifest and all required banks preflight and publish as
one generation. Missing/mismatched content, target, capability, budget, trust or
license evidence rolls back the candidate; a partial event set never activates.
Old banks/code remain leased until events, voices, tails, callbacks, queues and
profiler views retire.

All adapters publish a normalized bounded stats snapshot for voices/events,
command lag/queue pressure, render load, underruns, bank memory and faults, with
unknown/unsupported distinct from zero. Vendor-specific profiler data stays in an
explicit integration panel/capture and is labeled with provenance.

The middleware backend participates in the device lifecycle. `Initialize` and
`Shutdown` are invoked from the same state machine that manages native device
transitions. Device loss, reopen, and reconfigure events are reported by the
middleware backend so that Horo's `AudioRuntime` remains in a typed state even
when the middleware owns the device.

## Platform Focus And Suspension Policy

Audio focus behavior is explicit per host/profile:

```text
On focus lost:
  - continue audio
  - pause all buses
  - pause gameplay buses but keep music
  - mute output
```

Editor preview, play-in-editor, and packaged games may use different defaults,
but each policy is typed configuration. Focus loss must not silently leave the
audio runtime in an undocumented state.

Focus policy is distinct from host suspension and device loss. Host/application
policy submits the requested action; audio control orders and commits it. During
explicit host suspension the required bounded audio lifecycle pump remains on the
owner-thread command/end-frame boundaries while ordinary variable update is
skipped. The callback follows only the committed continue/hold/silence/quiesce
policy and never infers suspension from missing render frames. The remaining pump
drains lifecycle-critical acknowledgements/device/fault records and can publish
reserved critical work; ordinary scene/game command preparation stays suspended.

This policy is configured in Project Settings > Audio > Focus Behavior, with
per-platform profiles for editor preview and packaged games. The same page
defaults mute-on-minimize on most desktop hosts, pause-gameplay-buses for
mobile focus loss, and continue-everything for dedicated audio preview windows.

## Metrics

[AUD-010](https://github.com/HoroCore/horo-engine/issues/626) delivers the
bounded metrics, profiler correlation and qualification baseline for M5 — 1.0.
Feature-specific Post-1.0 metrics extend this catalogue only when that feature is
included; their architecture definitions are not evidence that the feature ships.

Audio exposes:

- callback duration and budget utilization
- underrun count
- command/event queue depth and drops
- active, virtualized, and rejected voice counts
- stream buffer fill
- decoded and resident audio bytes
- device sample rate and buffer size
- per-bus peak/RMS levels where supported
- mixer CPU time
- effect CPU time
- rejected voice count by reason
- spatializer fallback count and reason
- occlusion/zone staleness age and deadline misses
- parameter lookup failures (missing stable ID)
- event lookup failures (missing stable event ID)
- variation container selection telemetry (optional)

No ordinary log formatting occurs on the callback thread.

## Testing

The following is the cumulative contract catalogue. The 1.0 qualification gate
requires the AUD-001 through AUD-011 items that are implemented by that checkpoint.
Capture, procedural, advanced-propagation, adaptive-music and middleware items
become required only for products/packages that include those Post-1.0
capabilities. Documenting their tests now is not a 1.0 support claim.

Required applicable tests cover:

- voice and resource handle generations
- runtime/device/callback-epoch legal transitions and stale acknowledgement rejection
- startup cancellation/failure unwind at every acquired stage and callback-ready timeout
- mixer routing and gain behavior
- constrained-DAG root, parent, send/return, route, and feedback validation
- deterministic stable-ID topological and accumulation order across serialization,
  registration, and job-completion order
- graph-build failure, stale revision, atomic swap, last-good retention, and
  generation-safe DSP/provider retirement
- voice capacity policies
- real-time queue saturation
- streaming underrun recovery
- device loss and reopen
- WASAPI/Core Audio/PipeWire/SDL3Audio/NullAudio common backend-contract parity
  plus native per-platform hardware qualification
- native-default and explicit startup backend resolution with no registration-
  order selection or implicit SDL3Audio/NullAudio fallback
- requested, effective, API-reported, and measured format/buffer/latency separation
- explicit checked sample-rate presence and invalid/overflow rejection for every
  block and published render plan, with no hidden default-rate substitution
- layout-only conversion preserves sample rate and resampling preserves semantic
  channel roles; each operation requires its own admitted plan
- scene unload with active voices
- scene-context unload barriers under saturation, late producer completion, and replacement
- null backend deterministic clock
- callback path allocation and lock checks
- audio asset format validation
- planar block alignment/stride/capacity/silence and borrowed-lifetime validation
- exact speaker preset and native/file role-map fixtures without index reinterpretation
- ACN/SN3D Ambisonic order/count and FuMa/N3D conversion/rejection fixtures
- denormal, non-finite, internal headroom, final clamp, and integer conversion policy
- 2D vs 3D source behavior
- distance attenuation policy
- listener selection and missing-listener diagnostics
- default bus/category routing
- editor preview command path
- mixer mute/solo/volume controls
- middleware extension boundary does not bypass real-time thread rules
- third-party callback contributors negotiate the versioned ADR-069 Audio RT C ABI,
  with public-header checks rejecting vendor/native types and C++ ABI leakage
- command staging from MPSC queue to SPSC real-time buffer
- parameter automation and fade ramp behavior
- audio snapshot/ducking transitions
- gapless loop metadata preservation
- platform focus/suspension policy
- suspend/resume discontinuity, device recovery, failed reopen, and no silent Null fallback
- callback/control fatal failure, terminal reconciliation, and detachment-gated reclamation
- repeated shutdown after active, suspended, recovering, failed, and partial-start states
- decoder plugin registration and cook-time selection
- DSP node prepare/process real-time contract
- spatializer registration and fallback behavior
- exact/ordered/automatic spatial-provider resolution independent of registration
  order, including required-capability preflight failure with no stereo fallback
- optional spatial fallback observability, runtime provider fault recovery,
  device/profile/content re-preflight, and generation-safe provider retirement
- occlusion provider one-frame-late ramp behavior
- scheduled batch atomicity for timeline and animation events
- middleware event and parameter bridge validation
- locale-aware clip resolution happens before audio runtime
- middleware device lifecycle state propagation
- stable parameter ID authoring-time registry resolution
- stable event ID authoring-time registry resolution
- variation container resolves to concrete clip at voice creation
- sidechain duck node configuration and gain reduction
- occlusion provider deadline miss produces stale-value fallback and metric
- spatializer fallback emits observable metric
- parameter/event lookup failures emit observable metric
- capture permission denial, revocation, input-device replacement, overrun,
  cancellation, and shutdown
- allocation/lock checks on the capture callback path
- speech consumers cannot obtain native device handles or convert transcripts
  into implicit editor approval

## Editor UI Wireframes

Audio editor surfaces must include an ASCII wireframe or equivalent design note
in this architecture document or in a feature-specific UI plan before
implementation begins. This applies to audio modals, pages, panels, tabs,
inspectors, and tool windows.

The wireframe is not decorative. It defines the expected layout, grouping,
alignment, primary actions, secondary actions, scroll regions, and empty/error
states before code is written.

Required wireframe coverage:

| Surface | UI Placement | Access Pattern |
|---|---|---|
| Audio Source component | Inspector | Select object with AudioSourceComponent |
| Audio Listener / Camera integration | Inspector | Select camera or listener object |
| Audio Mixer / Bus panel | Persistent dockable panel/tab | Menu: Window > Audio > Mixer |
| Audio asset import/cook settings | Inspector + Project Settings | Select AudioClip asset or Project Settings > Audio > Import |
| Audio preview | Inline inspector panel | Select clip/asset, click Preview |
| Active Voices / Profiler | Profiler tab or dedicated Audio Profiler panel | Menu: Window > Audio > Profiler |
| Missing clip/listener/bus diagnostics | Diagnostics/Validation panel + inline inspector warnings | Scene save/validation or build |
| Project Settings > Audio > Focus Behavior | Project Settings page | Edit > Project Settings > Audio |
| Per-platform audio profile | Project Settings page | Edit > Project Settings > Audio > Platforms |

Modal usage for audio is restricted to:

- import conflict resolution
- missing dependency repair wizard
- destructive mixer/bus reset confirmation
- delete bus with active references confirmation
- unsaved mixer changes prompt

Persistent audio tools (mixer, profiler, bus meters) should not be modals.
They must remain visible while the user edits the scene so audio state can be
compared against scene state.

Example mixer panel wireframe:

```text
+--------------------------------------------------------------+
| Audio Mixer                                           [Reset] |
+----------------------+---------------------------------------+
| Buses                | Selected Bus: SFX                     |
|                      |---------------------------------------|
| > Master             | Volume     [-----------o-----]  -6 dB |
|   Music              | [ ] Mute   [ ] Solo                   |
|   SFX                | Route      Master v                   |
|   UI                 | Effects                               |
|   Voice              |   + Gain                              |
|   Ambient            |   + Low Pass                          |
|                      |                                       |
+----------------------+---------------------------------------+
|Output: 48 kHz / 512 frames | Voices: 24 active, 3 virtualized|
+--------------------------------------------------------------+
```

Example audio source inspector wireframe:

```text
+--------------------------------------------------------------+
| Audio Source                                                 |
+--------------------------------------------------------------+
| Clip        [ explosion_01.ogg                         ... ] |
| Bus         [ SFX                                      v   ] |
| Mode        (x) One Shot  ( ) Loop  ( ) Stream               |
| Spatial     ( ) 2D        (x) 3D                             |
| Volume      [-----------o-----] 0.80                         |
| Pitch       [------o----------] 1.00                         |
| Priority    [ Normal                                   v   ] |
|                                                              |
| Attenuation                                                  |
|   Min Distance [ 1.0 ]   Max Distance [ 40.0 ]               |
|   Rolloff      [ Linear                              v   ]   |
|                                                              |
| [Preview] [Stop]                              [Apply]        |
+--------------------------------------------------------------+
```

Implementation should not introduce a modal/page/tab whose layout expectation
has not been captured at architecture-note or feature-plan level. If the
implementation intentionally deviates from the agreed wireframe, the design note
or feature plan must be updated in the same change.

## Usage Scenarios

| Scenario | UI action | Runtime path | Result / note |
|---|---|---|---|
| 1. Asset import | Project panel → Assets/Audio/SFX → Import Asset → `explosion.wav` | Import dialog → AssetImporter validates WAV → AudioCooker builds platform formats → AudioClip asset + registry | The asset appears in the browser; the inspector shows duration, sample rate, and loop settings |
| 2. Add Audio Source | Hierarchy → Player → Add Component → Audio → Audio Source → sound (clip or variation container)/bus/mode/spatial/preview | AudioSourceComponent serializes into the scene → SceneRuntime extracts the transform → sound resolves → CreateVoice → MPSC → SPSC → callback → SFX bus → mixer | Preview uses the normal command queue rather than a separate bypass |
| 3. Configure a mixer bus | Window → Audio → Mixer → set SFX bus volume to -6 dB, mute, or editor-only solo | AudioFrontend coalesces redundant commands → SwapMixerGraph / SetVoiceParameters → apply at the buffer boundary | Mute affects the runtime SFX bus; solo is not serialized into the scene |
| 4. Post-1.0 middleware event | Inspector → Audio Source Kind: Middleware Event → StableEventId from registry (authoring name `enemy_footstep`) | AudioFrontend → VoiceRegistry proxy slot/cost reservation → IAudioMiddlewareEventBridge → AudioEventInstanceId | Native and middleware voices share the same voice budget and policy |
| 5. Trigger a snapshot | Game code: `AudioMixer.ApplySnapshot("pause_menu")` | AudioFrontend snapshot command → Mixer ramps Music↓, UI↑, SFX↓ | Ducking applies without clicks or pops |
| 6. Animation event | Animation editor → frame 42 → Event type: Play Audio Clip → `footstep_run` | Animation becomes a command producer → CreateVoice + StartVoice with timestamp | Sound remains synchronized without polling Animation from the audio thread |
| 7. Timeline-synchronized audio | Timeline → align explosion/dialogue/music cues to one timestamp | Timeline → ScheduledCommandBatch → SPSC → callback applies atomically | Sounds start with sample-accurate synchronization |
| 8. Mobile focus loss | User switches to another application | Host focus event → Project Settings > Audio > Focus Behavior authors `audio-focus-policy.json` → AudioRuntime reads policy → gameplay buses pause while music continues | Game audio does not leak; playback resumes according to policy on return |
| 9. Audio Profiler | Window → Audio → Profiler | Callback publishes bounded metrics → observability queue → panel reads; callback does not log or allocate | Real-time cost remains visible without disrupting the audio thread |
| 10. Localized dialogue | Project locale is tr-TR; scene uses the `dialogue_greeting` clip | Asset pipeline resolves `dialogue_greeting` → `dialogue_greeting_tr-TR` → runtime receives the resolved AssetId | Runtime does not interpret locale; it sees only the final AssetId |

## Related Documents

- [Audio Mixer UI Reference](./audio-mixer.html): bus routing, DSP chains, meters, and middleware bridge status panel.

- [Asset Pipeline](./asset-pipeline.md)
- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Scene Runtime](./scene-runtime.md)
- [Game UI And HUD](./game-ui-and-hud.md)
- [Configuration System](../foundation/configuration-system.md)
- [Accessibility Architecture](./accessibility-architecture.md): Semantic caption cues independent of playback; fixed-size audio-only metadata
- [ADR-015: Accessibility Ownership, Typed Transport and Non-Gating Policy](../../adr/015-accessibility-ownership-typed-transport-and-non-gating-policy.md)
- [Observability Metrics And Profiling](../observability/observability-performance.md)
- [XR Architecture](./vr-ar-architecture.md)
- [Editor AI Agent Architecture](../editor/editor-ai-agent-architecture.md)

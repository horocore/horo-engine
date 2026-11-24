# ADR-070: Capture and Voice I/O Ownership

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Input-device capture, permissions, timestamped PCM transport, monitoring, recording, network voice, speech, privacy, and editor boundaries
- **Issue**: [AUD-012.1](https://github.com/HoroCore/horo-engine/issues/645)
- **Jira**: [HORO-645](https://horo-engine.atlassian.net/browse/HORO-645)
- **Parent**: [AUD-012](https://github.com/HoroCore/horo-engine/issues/644)
- **Related**: [ADR-062](062-audio-runtime-ownership-and-update-order.md), [ADR-063](063-audio-sample-format-and-channel-layout.md), [ADR-064](064-audio-asset-and-cook-boundary.md), [ADR-067](067-platform-audio-backend-strategy.md)
- **Normative documents**: [Audio Architecture](../architecture/runtime/audio-architecture.md), [Platform Abstraction](../architecture/foundation/platform-abstraction.md), [Application Security](../architecture/security/application-security.md), [Networking Architecture](../architecture/runtime/networking-architecture.md)

## Context

Microphone capture is useful for recording, monitoring, network voice, speech
services, and editor tools, but those uses have different authorities. Audio must
provide one reusable backend-neutral capture/playback seam without absorbing OS
permission UI, privacy policy, packet transport, peer/session policy, moderation,
speech recognition, transcripts, or editor-agent approval.

Capture also has callback and lifetime risks distinct from output. Permission may
be denied or revoked, input devices may disappear, consumers may stall, and raw
voice is sensitive. A shared device service does not justify sharing stream state,
queues, generations, or failure policy with output.

## Decision

### 1. Authorities are explicit

| Concern | Authority |
|---|---|
| Native input enumeration/open/callback/route notifications and OS permission adapter | Platform/private Audio backend |
| Whether capture is allowed, consent provenance, retention/export/diagnostic policy | Security plus host/application product policy |
| Capture session state, device-neutral format, timestamps, bounded buffers, monitoring and recording candidate | Audio control/runtime |
| Durable recorded asset identity, staging, import/cook and atomic publication | AST under ADR-064 |
| Packet framing, transport, encryption, peer/session identity, jitter/network policy, QoS, moderation and remote consent | NET and product services |
| Speech recognition, language/model/provider policy and transcripts | Speech/application service |
| Capture/record UI, destination choice, preview and agent submission | Editor/application surfaces |

Audio never sends packets, authenticates peers, selects moderation rules, prompts
with native UI, chooses transcript language/model, treats speech as an input action,
or writes arbitrary recording paths. NET and speech never retain native input
handles or call the capture callback.

### 2. Capture and output are separate generations

`AudioCaptureSessionId` is generation checked and owned by Audio control. Each
session pins one input device generation, permission decision revision, immutable
effective format, buffer policy, purpose, consumer set, and owner token. Capture
and output may share a platform device service, but use separate streams, callback
epochs, queues, clocks, faults, cancellation, and teardown.

The capture state machine is:

```text
Created -> AwaitingPermission -> Opening -> Capturing
Capturing -> Pausing -> Paused -> Capturing
Opening | Capturing | Paused -> Reconfiguring -> Capturing | Paused
Opening | Capturing | Paused | Reconfiguring -> Lost | Revoked | Failed
any nonterminal state -> Stopping -> Stopped
```

Only Audio control commits these states. Platform permission/device callbacks emit
bounded facts; capture callbacks publish frames/faults only. Repeated stop is
idempotent, and no state leaves `Stopped`.

### 3. Permission and purpose precede device open

A request declares a stable purpose such as editor recording, local monitoring,
network voice, or speech input; foreground/background policy; requested device and
format limits; retention/export intent; and owner/cancellation token. Security/host
policy validates the purpose and Platform performs any OS request on its required
thread. Audio opens no device until a current `Granted` result matches the request.

`NotDetermined`, `Requesting`, `Granted`, `Denied`, `Restricted`, and `Revoked` are
distinct typed results. Audio does not display permission UI or turn denial into an
empty successful stream. Revocation closes admission, stops the native stream,
invalidates the generation, notifies consumers, and applies retention policy.

Permission for one purpose does not authorize another. Editor preview consent does
not grant packaged-game network voice, and microphone permission does not grant
recording retention, upload, telemetry, or speech-provider network access.

The editor connection test computes a live peak level in the capture callback.
The test publishes activity and scalar level only. It does not queue PCM for a
consumer, route microphone audio to a playback device, or authorize recording.

### 4. The reusable transport is bounded timestamped PCM

The private backend reports native input format and timestamps. Audio validates and
converts outside any output callback into a Horo-owned `CapturedAudioBlock` with:

- capture session/device/clock generation and monotonically ordered sequence;
- checked start timestamp and frame count;
- explicit sample rate, representation and semantic channel layout;
- owner-backed immutable PCM span and bounded discontinuity/fault flags.

ADR-063 is the processing format when a consumer requires mixer/DSP-compatible
blocks; native/interleaved capture may have a separate typed adapter format before
conversion. Channel count alone is never layout identity.

Preallocated rings or policy-bounded pools separate the native callback from
consumers. The callback never blocks, allocates, logs, waits for output/network/
Speech/Editor, or calls consumers. Overrun policy is explicit: drop oldest or
newest complete block, insert a discontinuity marker, increment bounded counters,
and notify control. It never overwrites a leased block or claims continuity.
Consumers can read the latest discarded block's sample position and timestamp
even if no later block reaches the queue. This metadata read does not copy PCM
or wait for the producer.

Consumers obtain leases or bounded copied reads with independent cursors and
backpressure. A slow NET/Speech/recording consumer cannot block capture or output;
its own policy drops/cancels/fails with visible gap evidence.

### 5. Monitoring is an explicit local Audio route

Monitoring is disabled by default and requires an explicit admitted request,
permission, gain, destination bus, latency/buffer and feedback-safety policy. Audio
prepares a capture-to-output source outside both callbacks and publishes it through
normal graph/voice generation boundaries. Input callback memory is never borrowed
directly by the output callback.

Monitoring does not imply recording, network transmission, speech processing, or
retention. Device combinations that risk acoustic feedback are warned/rejected by
typed product policy; Audio does not silently enable echo cancellation or modify
the microphone signal.

The monitor route is scoped to one capture generation, a pinned playback
endpoint and callback epoch, and a mixer bus owned by that playback runtime.
The host must verify private listening for that exact route. SDL rejects the
automatically migrating default playback endpoint. Monitoring PCM is available
only through its gain-limited reader, which discards samples outside the queue
age budget. Playback quiescence retires monitoring and any replacement requires
fresh admission. An opt-in echo reference is eligible only from the program
tap before microphone monitor mixing, in the same output epoch and clock
domain. An echo-canceller owns its alignment and processing separately.

### 6. Recording publishes through AST

Editor/game code submits start/stop/cancel through an application recording use
case. Audio captures owned timestamped blocks and finalizes a bounded recording
candidate with format, duration, gaps, device-safe metadata, consent/purpose and
integrity facts. It does not choose project paths or publish files.

AST receives the candidate through a trusted typed contribution, stages it,
creates stable asset identity/sidecar/cooked outputs, and atomically publishes or
rolls back. Cancellation, permission revocation, device loss, disk/budget failure,
or malformed/gapped policy failure preserves the last good project state. Raw
temporary data follows explicit retention and secure cleanup policy.

The baseline recording path accumulates finite PCM in bounded private memory.
The Audio owner detaches capture before transferring the unread tail on a
normal stop. The application discards an incomplete or revoked take, validates
the complete WAVE through the built-in source importer, and publishes it through
the normal authored asset transaction.

### 7. NET owns voice packets and remote policy

Network voice consumes captured leases/copies through a narrow application adapter
and returns decoded remote PCM through a symmetric bounded playback-source seam.
NET owns codec/packet negotiation at the session boundary, sequence numbers,
packetization, transport reliability, encryption/keys, authentication, peer/channel
routing, jitter/reorder/loss concealment policy, bitrate adaptation, mute/block,
moderation, abuse reporting and network backpressure.

Audio may provide reusable media codec/resample/DSP primitives with typed contracts,
but invoking them does not transfer network policy. Audio sees no sockets, packet
headers, credentials or remote peer authority. Remote PCM becomes an ordinary
generation-scoped Audio source after NET validates session/peer/content policy.
Capture may continue locally when transport fails only if the purpose policy says
so; Audio does not reconnect or change bitrate.

### 8. Speech and editor-agent boundaries remain outside Audio

A speech service may consume an admitted capture stream and publish typed partial/
final transcript results. It owns model/provider/language, network, consent,
redaction, retention and cancellation policy. Audio owns neither transcript text
nor conversational history.

The Editor decides whether an explicit user action submits a transcript to an
agent. A transcript is not an input-action edge and never constitutes approval for
a mutation. Capture start/stop UI reflects authoritative permission/session state
but does not own the device, buffers or recording transaction.

### 9. Privacy is data-flow policy, not a log preference

Raw microphone/remote voice, derived features, transcripts and device identifiers
are privacy-sensitive. They are excluded from ordinary logs, telemetry, crash/
diagnostic bundles, conversation history and caches by default. Diagnostics expose
safe session generations, states, formats, durations, counters and stable reason
codes, with device identity redacted/pseudonymized.

Retention, recording/export, upload, speech processing and diagnostic attachment
each require explicit purpose/policy authorization. Buffers are bounded and cleared
on lease retirement; shutdown/revocation stops admission, cancels/joins consumers,
stops native callbacks, retires leases, applies cleanup, then releases devices.

### 10. Migration and verification

Audio Architecture projects this ownership model. Platform, Security and Networking
documents retain their native, consent/privacy and packet/session authorities.
AUD-012.2 and later children implement devices, buffers, recording, monitoring,
privacy and NET boundaries without creating a second capture API.

Required coverage includes permission grant/deny/restrict/revoke and purpose
separation; open/start partial failure; device/default change; timestamp wrap/
discontinuity; format/layout conversion; long capture; overrun with leased blocks;
slow independent consumers; monitor feedback/latency and output reset; recording
cancel/loss/AST rollback; NET transport failure/jitter/remote PCM handoff with no
sockets in Audio; Speech cancellation and no transcript approval; privacy/redaction,
cleanup, late callbacks, repeated stop and shutdown.

## Consequences

Recording, monitoring, network voice and speech can reuse one safe capture/PCM seam
without moving packet, consent, transcript or editor policy into Audio. Capture and
output failures remain isolated and raw voice has explicit privacy ownership.

The cost is separate generation/state machines, permission-purpose tokens, bounded
multi-consumer transport, explicit adapters and retention policy. Those boundaries
are required for real-time and privacy correctness.

## Rejected Alternatives

### Put VoIP transport inside Audio

Rejected because sockets, encryption, peers, jitter, moderation and QoS are NET/
product policy. Audio supplies capture and playback media seams.

### Let every consumer open the microphone

Rejected because permission, device state, buffer budgets and privacy would have
competing authorities. Audio owns capture sessions.

### Share one callback/ring for capture and output

Rejected because their clocks, formats, generations, backpressure and failure paths
differ; one stalled consumer must not break output.

### Treat denied permission as a silent empty stream

Rejected because callers could report successful recording/voice input. Denial and
revocation are typed lifecycle outcomes.

### Write recordings directly from the capture callback

Rejected because file I/O blocks and bypasses AST identity, validation, staging,
atomic publication and rollback.

### Send transcripts directly to an editor agent

Rejected because speech output is data, not user intent or mutation approval.

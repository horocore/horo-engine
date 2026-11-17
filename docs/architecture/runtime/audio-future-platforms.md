# Future Audio Platform Paths

AUD-007.12 documents compatibility paths for Android, Web and XR. It adds no
mobile/browser/XR implementation or desktop 1.0 support commitment. The common
ownership, generation, RT and requested/actual contracts remain those of
[Audio Architecture](audio-architecture.md), ADR-062 and the Horo backend protocol.

| Future target | Backend and packaging path | Capabilities and activation boundaries |
|---|---|---|
| Android | An Android-owned Oboe/AAudio adapter behind Horo's device protocol; native libraries and ABI/API admission belong to Android packaging | Explicit output usage/performance/sharing modes, negotiated burst/format, route changes and disconnects. Application/activity backgrounding and focus/interruption arrive as typed host facts. Output does not request microphone permission; capture remains the permission-aware AUD-012 seam. No assumption that low-latency/exclusive mode is available. |
| Web | A Web-owned WebAudio/AudioWorklet adapter with bounded prepared PCM/control transport; processor/module/WASM assets and cross-origin requirements belong to browser packaging | User activation may be required before AudioContext resume. Suspended/blocked activation is observable pending/failure state, not silent success. Capability-check AudioWorklet, output selection, shared-memory/isolation and sample rate. A missing SharedArrayBuffer path requires an explicitly admitted bounded alternative; it cannot allocate or post unbounded messages per render quantum. Browser/device/end-to-end latency remain distinct observations. |
| XR desktop or standalone | The existing XR owner supplies a typed recommended endpoint/route capability to application composition; the appropriate desktop or Android audio adapter still owns output | OpenXR native paths/handles never enter Audio API. A recommended endpoint is a selection candidate, not a new hardware clock or permission to replace the selected backend. Required spatial/object output needs explicit provider capability admission. Missing recommendations, session loss, headset standby and route changes use the same host/device transaction. |

Each future peer must preserve these obligations before implementation can be
qualified:

The current device contract has a closed desktop `AudioBackendKind` set and the
native composition helper admits WASAPI, Core Audio and PipeWire only. Android
and Web therefore need an explicit versioned identity/capability migration before
implementation. They cannot borrow SDL3/Null or another peer's identity. That
migration must update exhaustive visitors, selection/packaging admission and
consumer coverage together; this document does not add those peers to the 1.0
binary contract.

- Resolve composition and any authorized fallback before runtime activation.
  Native API types, IDs, permission objects and service handles remain inside the
  platform adapter. Required unavailable output/spatial capability fails explicitly.
- Publish a complete immutable requested/effective format and period plan, with
  prepared conversion and bounded memory. AAudio burst size, browser render
  quantum and an XR display frame are different timing domains; none is an audio
  sample-clock authority by itself.
- Keep callbacks free of heap allocation/free, blocking, logging, discovery,
  configuration, ECS and ordinary host calls. Retain processors, JS/WASM modules,
  native callbacks, epoch storage and code until native unregister/stop plus
  matching quiescence proof. Worklet termination or app suspension alone is not
  proof that queued work cannot access retained storage.
- Treat focus loss, lifecycle backgrounding, OS interruption, permission denial,
  device disconnection and suspend/resume as distinct typed inputs. Control owns
  continue/pause/mute/reopen policy and new clock/discontinuity generations. No
  unbounded wall-time catch-up or unnoticed restart/fallback is allowed.
- Keep game and preview outputs independent. Browser output-selection support,
  Android simultaneous streams and headset clock drift require their own
  capability, resource and hardware evidence. Mirroring or aggregates cannot
  assume shared clocks; unsupported compositions fail explicitly.

Required evidence is per OS/browser/device and packaging cell: native build and
private dependency boundaries, physical/default/explicit endpoint behavior,
user-activation/permission denial, background/suspend/session-loss recovery,
format/rate change, RT allocation/deadline checks and callback/module retirement.
No such platform cell is qualified by this document. XR implementation, device
qualification and CI remain with the XR workstream; this audio compatibility path
must not create a second XR runtime or endpoint implementation.

Platform API references: the official [Oboe guide](https://github.com/google/oboe/blob/main/docs/FullGuide.md)
and [disconnect lifecycle notes](https://github.com/google/oboe/wiki/TechNote_Disconnect)
describe Android negotiation/recovery; the [Web Audio specification](https://www.w3.org/TR/webaudio/)
defines context activation, control/render threads and AudioWorklet processing.

# Audio Device Delivery And Qualification

This is the implementation and evidence ledger for AUD-007. The ownership,
callback and state authorities remain in [Audio Architecture](audio-architecture.md)
and ADR-062. [ADR-067](../../adr/067-platform-audio-backend-strategy.md) still
carries **Proposed** status; referencing its equal-peer direction does not record
an acceptance decision or grant a platform support claim.

## Delivery boundaries

`HoroAudioOutputPolicy` exposes non-installed control contracts. Buffer profiles
request 5 ms, 20 ms or 40 ms at the canonical sample rate, rounded up and clamped
to the explicit admitted frame bounds. These are preferences; native negotiation
may reject them. `EstimateSoftwareLatency` adds only caller-declared disjoint DSP,
resampler and decoded-queue frame contributions with checked arithmetic. The
result has Estimated/Adapter provenance. Device, stream and end-to-end latency
stay in `AudioDeviceTimingReport`; overlapping native components must not be
added to that estimate.

`PrepareOutputDiagnostics` publishes preserved intent, admitted actual format,
fresh epoch timing and the new software estimate together. Reconfiguration starts
with the new candidate's observations, including Unknown values; it cannot inherit
old hardware measurements. `UpdateOutputTiming` rejects stale epochs, backwards
capture time, cross-domain evidence and invalid provenance without mutation.
WASAPI's API-reported maximum stream delay is stored as adapter latency with
overlapping-component semantics, never labeled as isolated physical hardware or
measured end-to-end latency.

`AudioDeviceSwitch` orders quiescence, stop/detach, close, fresh host discovery,
open/admission, playback preparation, start, exact-epoch Ready and activation. Its
operation identity and
terminal record are retained until acknowledgement. A timeout, wrong completion,
unproven native stop or failed activation cannot claim successful migration or
release callback state. Backend progress and event distribution remain host-owned.
The host continues pumping the adapter while the coordinator polls its result.

Successful switch completion now retains `activationAnchor`, the first validated
replacement `AudioCallbackReady` event with its exact epoch, Horo sample cursor and
monotonic timestamp. The coordinator stores that fixed-size fact instead of a
boolean; a duplicate cannot replace the original anchor. It accepts the fact even
if native Start completed before its retained completion was polled. Failed or
cancelled completions carry no activation anchor, and acknowledgement clears the
old record before reuse. Regression sources check stale/foreign-clock rejection,
duplicate rejection, exact retained values and cancellation without a false anchor;
they remain uncompiled/unexecuted.

This record supplies actual callback evidence for the host's pending clock
handoff. The host must still publish a strictly fresh sample-clock generation,
discontinuity revision and admitted producer correlation before resuming producer
admission. Neither the future Pump deadline nor an old clock snapshot can replace
the Ready timestamp. The anchor is not a native packet-submission, hardware-clock
or queued-consumption measurement. Production frontend/clock publication remains
required; retaining the fact does not complete that wiring.

At `AwaitDiscovery`, the old connection is Closed and the host explicitly begins
Enumerate on the same backend, then pumps it to completion. `SupplyDiscovery`
accepts only that backend's still-retained catalog completion with an operation
sequence newer than the acknowledged Close. It resolves the preserved default or
explicit selection, reserves the new device epoch and acknowledges that receipt.
An earlier catalog, unrelated response or unfinished operation is not consumed.
This permits a restarted service to publish new device identities after the old
connection retires. There is no replacement epoch before discovery admission,
no enumeration-order selection and no substitution for a missing explicit device.
Discovery failure/cancellation/timeout retains uncertain native resources and
produces the same single transaction terminal; it cannot claim successful migration.

The host closes producer admission and performs its prepared fade before beginning
a switch. At `AwaitPlayback`, the old native reader is detached and the new
candidate's format and timing are validated. AUD-003 owns voice cursor/stream/scene
behavior. Its host adapter must prepare the requested Preserve or Restart policy
and supply a retained render port, including fade-in, before the coordinator starts
the replacement. Preserve currently accepts an unchanged effective format; a rate
or layout change requires explicit Restart. Its admission accepts the previous
actual format from either the preferred tuple or an explicitly allowed complete
alternative. A 48 kHz preference with a still-admitted actual 44.1 kHz output does
not require rewriting the caller's preference. Rate and layout components from
different alternatives cannot be combined. The actual opened candidate must still
match that old effective tuple before Preserve preparation is admitted. Regression
sources cover preserved/rejected native-mechanism candidates and mixed-tuple
rejection, but remain uncompiled/unexecuted. The coordinator does not claim that
recording a policy value alone preserves playback. Aggregate application wiring,
voice receipts, clock discontinuity publication and audible fade qualification
remain required integration evidence.

`AudioFrontendComposition` now exposes an internal completed-block playback hold
boundary as a prerequisite for that handoff. `RequestPlaybackHold` rejects while
accepted transport remains pending and closes further transport admission on
success. The actual render callback executes one final real voice/mixer block,
fades it to positive zero and publishes a non-recycled request identity, exact
epoch, actual source cursor and final processed output-frame boundary. Held
callbacks retain source/resampler/mixer state and write silence; they do not
consume the stream ring. Their native output clock may continue advancing.
`PollPlaybackHold` reads only a matching release-published callback record; control
pumping without a callback cannot acknowledge it. `ReleasePlaybackHold` resumes the
same active epoch with a first-block fade-in. A new hold admitted before any
resumed callback retains the same source boundary under a new identity.

Hold records do not prove native detach, allow epoch rebinding, or implement loss
recovery when callbacks cease. Close still requires the normal actual retirement
proofs, including when a hold is pending. Real frontend/Null regression sources
cover fades, cursor freeze versus advancing output clock, preserved transport
receipts, stale/foreign request rejection, no-callback non-completion, successive
holds, resumed cursor progress and callback allocation/free counts. These sources
are uncompiled/unexecuted. Production switch orchestration, changed-format Restart,
clock discontinuity publication and platform/audibility qualification remain open;
this prerequisite is not the completed device-switch host integration.

The hold acknowledgement is a **render-core** boundary. The native adapter can
retain part of that canonical block while serving smaller native packets. On a
normal Rendering-to-Quiescing transition it now finishes that bounded retained
block before invoking the core's Quiescing acknowledgement. Previously the phase
change discarded the retained PCM and could truncate the frontend fade. Other
phase transitions and callback faults still replace stale scratch immediately;
device loss still requires proven native detachment before synchronous final
quiescence and cannot promise delivery of a retained tail.

A new shared-native regression source splits a 16-frame fade into a 5-frame
prefix and 11-frame tail. It requires exact tail samples, no early core quiescence,
silence on the next packet, one acknowledgement and no observed C++ allocation or
free during the tail transfer. It runs against each peer's deterministic native
primitive fixture, not an OS endpoint; the source remains uncompiled/unexecuted.
Neither the hold cursor, adapter scratch consumption nor core acknowledgement
proves native API submission, queued-device consumption or audibility. Production
handoff still needs executed real frontend/native composition coverage, native
submission and device-progress reconciliation, and explicit loss/discontinuity policy before
claiming completed fade or exact playback preservation. The fixed-period Null
hold fixtures do not establish those boundaries. No adapter-submission receipt or
completed production drain integration is claimed by this fix.

`NativeAudioFrontendHandoffTests.cpp` additionally composes the actual resident
voice, mixer, frontend and shared native adapter with a synchronous packet driver.
Its hold record reaches core frame 48/source frame 32 while the fixture has
transferred only 37 frames. Closing must preserve the 11 remaining fade frames,
wait for the next silent Quiescing packet, then acknowledge actual fixture
Stop/Close before frontend release. The fixture records no native API submission
or device-consumption proof. This source is included in `HoroAudioEditorPreviewTests`
and the platform audio CI build/test selection, but has not been compiled or run.

The switch coordinator also accepts explicit cancellation of activation. This
retires the whole transaction rather than resuming the old output: old quiescence,
Stop and Close still complete; an acquired replacement closes without starting,
and a started/priming replacement goes through its own Quiesce/Stop/Close without
CommitRendering. A pending Open or Start is reconciled before cleanup, including
its native failure and resource disposition. At AwaitDiscovery the host still
supplies its actual completed enumeration receipt so neither its slot nor any
new native discovery prefix is abandoned. A discovery failure becomes the cause
of cancellation after successful Close; failed cleanup or expiration retains
ownership. Only actual Closed completion permits a Closed cancellation terminal.
That single terminal stays stable until acknowledgement; cancellation after any
terminal reports AlreadyTerminal. A cancelled transaction never accepts playback
preparation or Ready as authorization to activate.

New source regressions use real Null lifecycle operations before discovery,
through pending/completed Open, before/during Start, before/after Ready and during
discovery cancellation. They require actual Closed, zero replacement Rendering,
exact receipt consumption and stable terminal evidence. An expired cancellation
and a composed native-mechanism fixture whose Stop fails must retain the reader
and must not claim Close. These tests have not been compiled or executed. The
frontend/application still needs to own this coordinator through shutdown and
bind the real hold, new-format playback and clock handoff; cancellation support
alone does not complete that production wiring.

The existing command transport and processing owners bind an exact callback
epoch; retaining that old scope across a new device callback would admit stale
commands. `HoroAudioPlaybackEpochContract` adds a narrow, non-installed detached
adoption authority. Its declarations expose only forward-declared Horo types and
fixed status values and a forward-declared Result for transport preparation.
Command, voice and mixer implementations retain their private state
layouts in their own targets; no broad source include or reverse dependency is
introduced. Existing installed headers grant only this internal authority friend
access; their public construction/control signatures and ownership registry stay
unchanged. Commands, VoiceRender and Mixer public-header consumer builds remain required.

After actual old native Stop/join, accepted-command reconciliation and completed
publication reclamation, the host may adopt the same-format active mixer and voice
into a strictly newer epoch. Actual PCM, source cursor/fraction, converter history,
stream port, spatial state, gain, mixer storage and DSP state remain owned. Only
scope/graph epoch identity and epoch-local command/publication counters change.
Slot generations and graph publication generations are not recycled. Detached
adoption ends old graph-identity borrows; metadata remains immutable while
published. The host must retire the old transport and prepare a new transport
with fresh storage identities; old commands never cross the reset boundary.
`PrepareTransport` now checks the actual old staging owner is closed and drained,
matches its old owner/epoch and still-open context generation, then allocates a
separate staging owner for a strictly newer epoch. Both supplied storage identities
must differ from both old lane identities; cross-lane reuse is rejected. Only the
same active context is registered on the new owner. Records, scene gates and
sequence counters are not copied; the first admitted new command starts at one.
Budget/allocation failure leaves the retired old owner unchanged. Closed/drained
transport still proves neither command execution nor native Stop/join; the host
must establish and reconcile those boundaries before calling this operation.

The Commands target owns this definition and consumes the declaration-only epoch
contract privately. Its installed staging header adds only a forward declaration
and narrow friendship, with no construction/control signature, layout, public
dependency or header-ownership change. Command tests opt into the non-installed
contract explicitly; the generated Commands consumer still sees only its declared
public headers. Regression sources cover old admission/backlog, scope/context and
cross-lane alias rejection, exact new registration/sequence and budget failure.
The actual PCM/voice/mixer adoption fixture now uses this factory rather than
constructing and registering an unchecked replacement queue itself.

Frontend operation receipts now use a separate monotonically increasing identity
for the entire frontend lifetime. Epoch-local command sequences can restart on a
new staging owner without reusing an external accepted-operation identity. Each
bounded receipt stores an atomic internal command mapping published before its
external identity; callback completion matches only pending mappings. Atomic
mapping reads are necessary while control recycles unrelated terminal slots.
Retained terminal results keep their original external identities until consumed.
Exhaustion rejects before command admission, and admission failures consume no
external identity. The installed API signatures and result layout are unchanged;
callers continue correlating the returned admission sequence with acceptedSequence,
and must not interpret that sequence as an internal command-buffer position.
The editor preview/document callers already use this correlation without inspecting
queue positions. Regression sources cover startup-record independence, applied and
rejected results, failed admission and slot reuse. Actual queue replacement and
counter-exhaustion execution remain unverified; the production switch is not wired.

Pending publications, a changed runtime/context, closed/unprepared owners or
automation bound to the old epoch reject without mutation. Automation requires
explicit new preparation rather than relabeling an old sample-clock binding.

New regression sources use actual resident PCM and mixer execution before/after
adoption, preserve cursor/voice/signal, reject old-scope commands, and admit a
new-epoch queue beginning at sequence one. The synchronous fixture never attaches
a native callback and finishes each processing call before adoption. Other sources
cover pending/stale/foreign mixer scopes and unchanged sealed-automation rejection.
These C++ sources remain uncompiled/unexecuted. Native Stop proof, producer/barrier
orchestration, production use of new queue preparation, format-changing Restart
and clock correlation still require production host integration; this authority does not manufacture
those proofs or alter backend ownership.

`HoroAudioNativeRuntime` implements backend-neutral lifecycle and float/planar
adaptation. WASAPI, Core Audio and PipeWire own separate native drivers behind
that Horo-only internal boundary. None uses SDL3 or another concrete peer as a
base. All factories are inert; the process host selects and owns the concrete
peer. The editor process now captures `--audio-preview-backend` (`sdl3`, `wasapi`,
`coreaudio`, `pipewire` or `null`) independently of renderer and game-output choice.
Absent selection preserves the SDL3 default; repeated, missing or unknown values
fail validation. Renderer/project restart preserves the exact typed selection.
The application links installed native targets explicitly and captures a selected
factory, without probing or switching peers. A host opting into a driver must
link its explicit target and
pump `NativeAudioBackend::AdvanceControl` on the declared control thread.

Unavailable native selections return ProviderUnavailable; they do not become SDL3
or Null. SDL3 preserves its pre-existing deferred unavailable-provider error when
audio was deliberately omitted from a GUI build, so the optional preview does not
prevent that editor from starting. PipeWire selection initializes one root-owned
library lease and shares it across output generations. Factory-selection tests
construct distinct owners without connecting to an audio service. The native CI
lane now explicitly builds/tests the preview composition alongside native factories.

The Windows SDL GUI retains its STA. For an explicitly selected WASAPI preview,
the process composes `EditorAudioPreviewControlHost`: one owned control thread runs
the entire frontend/backend protocol, including real Begin/Poll/Acknowledge and
destruction. The driver establishes MTA on that owner; its render worker stays in
the same MTA instead of making per-packet calls through the GUI apartment. Other
selected peers retain their existing declared control owner. This is a host
affinity decision, not another backend selection or fallback.

The bridge admits one copied request, captures diagnostic/operation context, and
publishes generation-matched value snapshots. It holds no scene, document or UI
borrow. UI Pump/Snapshot try-lock the exchange and never wait for native work;
native rendering never acquires that mutex. Stop uses an independent atomic
generation, so a busy exchange or a previous generation cannot lose or misapply
revocation. The owner checks revocation after preparation and before startup
pumping. Shutdown closes admission and wakes even an idle owner, retires the inner
host on that owner, then joins the actual thread. A five-second wait timeout
terminates without freeing the unproven ownership island. Captured context remains
bound through native teardown and is replaced between request generations.

Gate-based real-Null regression sources cover UI calls while preparation is
blocked, copied source values, no Rendering after preparation revocation, actual
active/idle shutdown, retained causes, old Stop versus a later generation, and
diagnostic-context isolation/retirement. These sources and the Windows MTA/GUI
path remain unexecuted; native playback and platform support are not qualified.

Rejected inspector-preview preparation now owns its selected peer until an exact
Close completion is acknowledged, including when enumeration failed with retained
resources or was cancelled. The bounded two-second cleanup pump advances control
only; it never drives a headless render clock. An unproven close keeps the process
fatal ownership policy instead of unwinding native resources. Successful
composition transfers this responsibility to the frontend. The preview's default
role remains in its Open request so a native driver can revalidate that selection.
Preview now opens before playback preparation, requesting 48 kHz stereo with
44.1 kHz stereo explicitly allowed and a 16–4096-frame period range (256 preferred).
The actual rate and callback period size every voice/converter/mixer reservation.
Unsupported layouts or unoffered formats fail before callback attachment; preference
values remain unchanged. Composition polls the exact retained Open receipt and
acknowledges it only after detached publications succeed. An adopted open candidate
requires Close/Pump even when frontend Start was never requested. Start attaches
that candidate instead of opening twice; ordinary Pump cannot activate it implicitly.
The original inert-output composition remains available for hosts preparing the
preferred format before acquisition. Native product selection and OS execution
evidence remain incomplete.
The new rejection/retirement tests use the real Null protocol with injected
discovery outcomes and pending control progress; they are not native OS evidence.
Additional real-Null render tests select an admitted 44.1 kHz rate and/or a 512-frame
period, retain the original preferences, open once and exercise actual PCM output.
Null and SDL3 now accept Close while already closed, including discovery-only
prefixes; SDL3 does not pass an empty device handle to native close. This corrects
their mismatch with the common repeated-Close contract. These new tests are not
recorded as passed until the required native/focused builds execute.

Native callback state is fully prepared before Start. One serial native producer
borrows immutable epoch, format, scratch and render port. Lock-free phase and
first-fact mailboxes preserve Ready, Quiesced and the first fault independently
of event drain speed. Native packet sizes use a preallocated planar FIFO; channel
mapping is a complete semantic permutation, with no implicit downmix/resampling.
Physical output clamps only at the final float device boundary. Quiesced/faulted
cores stop receiving ordinary render calls. Native stop proof precedes scratch,
port or code reclamation. On device loss, a stopped native reader may be followed
by one synchronous final core quiescence call on control; control retains the
core and supplies the exact old epoch through that detached boundary.

`NativeAudioFacts` retains the admitted epoch's first loss in control-owned storage
before Close releases its native source, alongside one coalesced catalog
invalidation. Empty/tiny event destinations cannot erase a required loss. A new
Open, Start or readiness commit cannot proceed while native loss remains
unaccepted, including a loss still latched in the native source. An explicitly
requested Quiesce can retire a lost native reader before the host drains the loss
fact; the original event remains queued for the host instead of blocking shutdown.

WASAPI captures final notification/worker facts after successful unregister and
before releasing its notification object; registration state is explicit so retry
does not unregister twice. This follows the [MMDevice client lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-unregisterendpointnotificationcallback),
which leaves the final client reference with the caller. Core Audio captures after
listener removal/serial-queue drainage, and PipeWire after owned loop retirement.
Closed drivers clear the live epoch, preserving copied event identity separately;
a later probe failure cannot masquerade as loss of an old active output. Native
unregister/queue/worker behavior still requires each platform's execution evidence.

WASAPI period admission now intersects requested/native bounds with the native
fundamental-period lattice before rounding the preference. For an admitted range
of 128–500 frames with a 128-frame quantum, a 500-frame preference selects 384
instead of rounding to 512 and rejecting a valid device. Existing upward rounding
is retained when it fits. The non-installed helper uses wide arithmetic and rejects
invalid bounds or intervals containing no legal multiple. The driver passes that
exact selected period to the native initializer, and negotiation still reports the
deviation from the original preference. This follows the documented
[shared-stream period constraints](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream).
Regression sources cover the upper-bound case, lower native bounds, empty/invalid
intersections and 32-bit arithmetic boundaries. These sources remain uncompiled;
this arithmetic fix does not qualify native period negotiation on hardware.

Native retirement distinguishes Pending from Complete. A successful poll that is
still Pending does not publish Stopped/Closed, detach a render port, release a
rejected candidate's resources or authorize final loss quiescence. The common
operation remains pending under its original absolute deadline. Deadline failure
retains the native ownership island; it does not cancel a native join in flight.

| Peer | Native ownership and current boundary | Remaining qualification |
|---|---|---|
| WASAPI | MTA control apartment; MMDevice private endpoint registry; event-driven shared float output; IAudioClient3 legal period negotiation; worker-local render-interface acquisition/packet pairing/release; bounded worker join; notification facts | Native build and hardware lanes; default switch/invalidation; endpoint/name churn; USB/Bluetooth; rate/layout; suspend; long-running cadence/underrun/latency |
| Core Audio | HAL device/UID registry; AudioUnit explicit mono/stereo float client; private property-listener serial queue; callback admission, reader retirement and queue drainage | macOS SDK/build and arm64/x86_64 lanes; native callback-detach guarantee review; multichannel maps; actual latency provenance; route changes; aggregates/Bluetooth; suspend |
| PipeWire | Explicit process library lease; private service/registry/metadata/stream; observed float negotiation; target serial; bounded callback; private loop stop/join | Linux SDK/build and daemon lanes; reconnect/default metadata; RT data-thread scheduling and detach proof; graph/device latency; physical endpoint classification and device matrix |

PipeWire's baseline intentionally omits `PW_STREAM_FLAG_RT_PROCESS`: processing
runs on the same private loop whose stop/join proves callback retirement. It is
not yet a qualified low-latency RT data-thread implementation. Discovery describes
server sink nodes as Virtual rather than inferring a physical transport from
names. The process host initializes one `PipeWireAudioLibrary` explicitly and
shares its lease with its independent drivers; the final lease releases native
library state after driver retirement. Native library dependencies, COM, Apple
frameworks, PipeWire and SPA remain private to their concrete target.

PipeWire loop locking and stop/join run on one owned retirement pthread. Control
uses nonblocking `pthread_tryjoin_np`; only actual worker exit after native loop
stop admits detachment. No worker is detached, killed or replaced while it owns
the loop, and discovery/activation reject work during retirement. The driver,
callback context and library lease survive a pending or failed join. This native
GNU/Linux dependency is private to the PipeWire target. Its use follows the
[PipeWire stop/lock contract](https://docs.pipewire.org/page_thread_loop.html)
and the [Linux nonblocking join contract](https://man7.org/linux/man-pages/man3/pthread_tryjoin_np.3.html).
Linux compilation and fault-injected native worker qualification remain required.

Closing a PipeWire connection no longer marks the published device catalog as
removed. Reopen revalidates the selected serial and current default against fresh
discovery. Private registry keys include the native server-instance cookie so a
serial reused by another server instance cannot alias an old Horo device ID.
Duplicate default names remain unavailable instead of choosing enumeration order.
The cookie's role is defined by the [PipeWire core information contract](https://docs.pipewire.org/structpw__core__info.html).
Reconnect, server restart and duplicate-name execution evidence still require a
Linux daemon fixture; the common fake driver does not qualify these native paths.

## Independent outputs and preview

`AdmitOutputComposition` accepts one game and one editor-preview output only when
each has a distinct runtime owner and a valid device epoch. Each owns its mixer,
sample clock, adapter buffers, queues, decode/DSP budgets and shutdown transaction.
Matching device rates, timestamp clock-domain numbers or physical endpoints do
not imply a common hardware clock. Host admission must account for the sum of
both runtimes' memory, decode and callback budgets and verify both native opens.
This structural admission is not a backend simultaneous-output capability probe.

Mirrored and aggregate output return `OperationUnsupported`. The host must
explicitly fail an unsupported second open; it cannot silently copy the game
stream, collapse preview routing, choose another backend or substitute Null.
A future synchronized output path requires explicit drift observation/correction,
resampler work budgets, timestamp mapping and teardown semantics before admission.

## Offline output

`HoroAudioOffline` owns a synchronous tooling driver, separate from Null lifecycle
simulation and physical backend identity. `RenderOfflineAudio` produces complete
interleaved float PCM using exact integer sample-frame time and a caller-prepared
offline render port. It bounds output bytes, splits at the content/tail boundary,
handles a partial final block, rejects nonfinite output and returns no partial
export on failure. There is no hardware access, pacing, file I/O or asynchronous
retention. Finite mixer headroom is retained; no physical-device clipping is
applied to exported PCM. Output is bounded by 512 MiB; cancellation is checked
between process calls and completes with no partial PCM publication. Quiescence
uses the actual rendered cursor when an export stops early.

The caller prepares an exclusive offline core, matching content/tail plan,
automation and retained media. The port stops introducing source input at the
declared content boundary and drains exactly the admitted mixer-tail budget.
The driver does not infer tail completion by detecting silence.
`HoroApplicationAudioExport` now supplies a non-installed application file sink
over that driver and the host's no-follow filesystem authority. It encodes complete
float32 WAVEFORMATEXTENSIBLE with an explicit speaker permutation, retains finite
headroom, and uses private creation plus tracked atomic replacement under an
actual nonblocking export writer lease. Cancellation, cleanup failure and
post-commit durability failure retain separate creation/removal/commit evidence.
Both PCM and encoded storage are bounded; no hardware or timeline discovery is
introduced. See [offline file publication](../../guides/audio-offline-export.md).

New application regression sources use a scoped native authority and real native
fault adapters for semantic channel order, byte-identical partitions, complete
tails, Unicode paths, no early rendering on admission/lease rejection, existing
prepared-file preservation, cancellation and irreversible publication. They are
included in `HoroAudioOfflineTests` but remain uncompiled/unexecuted.
The cinematic export host still owns timeline-to-audio schedule preparation and
must invoke this operation; no production CIN/CLI/editor invocation is introduced
here. `OfflineMixerRenderTests.cpp` composes the
shipping graph runtime, sample automation and core low-pass filter, including its
complete declared tail budget, and compares block partitions and an analytical
signal sample. Production export invocation and compiled codec/file qualification
remain required.

## Acceptance evidence ledger

The 2026-11-11 acceptance audit read parent #594 and all thirteen child bodies
from GitHub and their matching HORO Jira work items. GitHub issues #595, #596, #597
and #603 are closed; Jira has HORO-595 Done and HORO-596/HORO-597/HORO-603 In Review.
The parent and other nine children remain open/To Do. These statuses are audit
context, not evidence that integrated native behavior passed. ADR-067 remains
Proposed. No issue state is changed by this ledger.

The only committed child delivery in this worktree is the future-platform
document; the branch has not been pushed and has no pull request. Native and
control changes remain local, uncompiled source. Local source progress therefore
cannot satisfy the parent's hardware smoke, simulated recovery, Bluetooth/default
device and shutdown validation as an integrated capability.

The tests listed here are source coverage, **not recorded pass results**. Record
actual commands/results from the final worktree before changing an evidence cell.
The first Windows configure attempt failed in compiler detection with MSVC C1902
(program database manager mismatch); no test ran. A local embedded-debug-info
configuration is prepared for the next authorized heavy-validation slot.
The static CI grouping and native-lane regression command
`python -m pytest -q --tb=short tests/python/test_audio_native_ci.py tests/python/test_ci_workflow_grouping.py`
last passed all 43 tests in 0.09 seconds using the existing project virtual
environment. That recorded run predates the qualification-guide addition; its
inputs are the CI/workflow contracts. It starts no native build and proves CI
target selection only.

| Work item | Existing or added implementation | Acceptance still requiring evidence |
|---|---|---|
| #595 / HORO-595 [AUD-007.1] | Existing strategy decision; no reimplementation | ADR status remains Proposed; acceptance status must not be invented |
| #596 / HORO-596 [AUD-007.2] | Existing discovery/negotiation/timing/backend protocol and contract suites | Native peer execution and hardware parity are separate evidence |
| #597 / HORO-597 [AUD-007.3] | Existing SDL3 reference adapter and tests | Reference results do not qualify native peers |
| #598 / HORO-598 [AUD-007.4] | WASAPI driver plus shared native mechanism | Native compile/smoke and all Windows endpoint/recovery cases |
| #599 / HORO-599 [AUD-007.5] | HAL/AudioUnit driver source plus shared mechanism | Apple compile/runtime and callback/property lifetime qualification |
| #600 / HORO-600 [AUD-007.6] | PipeWire driver source plus shared mechanism | Linux compile/runtime, reconnect and genuine RT data-thread path |
| #601 / HORO-601 [AUD-007.7] | Ordered switch coordinator; Null-backed switch and native-loss fixtures | Production host/playback/clock/fade integration; hardware switch recovery |
| #602 / HORO-602 [AUD-007.8] | Requested buffer profiles; provenance-safe software estimate; fresh diagnostics generations and stale-update rejection | Native observation/export wiring, end-to-end evidence and reconfiguration integration |
| #603 / HORO-603 [AUD-007.9] | Existing typed focus/suspend policy and tests | Native suspension/device recovery integration |
| #604 / HORO-604 [AUD-007.10] | Deterministic bounded PCM driver; real mixer/automation/filter-tail fixture; application WAVE publication with tracked native commit and file fault regression sources | Compiled regression pass evidence, production cinematic export invocation/schedule binding and codec/file qualification |
| #605 / HORO-605 [AUD-007.11] | Qualification protocol, result matrix and blank record; CI Release enables and links the host-native peer and runs inert factory/header-boundary checks | Native CI execution evidence and all hardware cells remain unqualified |
| #693 / HORO-693 [AUD-007.12] | [Future platform paths](audio-future-platforms.md) | Future platform-specific implementation and qualification stay Post-1.0 |
| #694 / HORO-694 [AUD-007.13] | Independent-owner routing admission; explicit mirror/aggregate rejection | Host budget/routing integration and simultaneous native-device lanes |

Formal blocked-by relationships remain in Jira/GitHub. Qualification #605 requires
the outcomes of #598/#599/#600/#601/#602, and #694 requires #601/#602. Source progress does not
remove these relationships or close a work item. AUD-003 voice/scene lifecycle and
XR endpoint implementation remain owned by their separate workstreams.

## Hardware qualification protocol

The [result matrix and recording guide](../../guides/audio-device-qualification.md)
publishes each native host/device-class cell and a blank evidence record. Every
cell is currently unqualified; the template itself records no executed result.

Each matrix cell records OS/build, architecture, backend/API/library version,
privacy-safe device class, requested versus effective format and periods,
workload, observation timestamps/windows, result and diagnostic evidence. Record
unsupported/missing evidence explicitly. Null, virtual sinks and fixture drivers
cannot qualify speakers, Bluetooth or physical latency.

Run each supported native peer separately from SDL3 on the same device classes:
speakers, wired headphones, USB and Bluetooth. For every available class verify:

1. Default and explicit enumeration/open; stable IDs across unchanged discovery,
   role defaults, missing devices, bounded catalogs and long/non-ASCII names.
2. Requested/effective sample rate, semantic layout, Horo block, native period,
   queue/device latency and Unknown/Reported/Estimated/Measured provenance.
3. Ready/activation, sustained finite PCM, callback allocation/deadline evidence,
   underruns and observation windows. No absolute latency guarantee follows from
   a profile request or an API latency value.
4. Default change, unplug/replug, endpoint invalidation, Bluetooth route/rate
   change, service restart and explicit switch, with one terminal result and the
   configured preserve/restart behavior.
5. Focus/minimize, suspend/resume and interruption through the host policy and
   device lifecycle, including producer admission and clock discontinuities.
6. Failure/cancellation at each acquisition stage, stale/late facts, pending
   completion retention, deadline expiration, repeated shutdown and verified
   native worker/listener detachment before releasing state or code.

WASAPI/Windows, Core Audio/macOS arm64, Core Audio/macOS x86_64 and PipeWire/Linux
are all **unqualified** until their build/link/package and physical-device runs
are recorded. Hardware tests are opt-in; build success and shared contract
fixtures alone cannot promote a cell to supported.

## Internal migration

All added contracts are non-installed build-tree interfaces with narrow include
roots. No `include/Horo/` header ownership changes are required. Hosts opting in
link `HoroAudioWasapi`, `HoroAudioCoreAudio` or `HoroAudioPipeWire`, and retain the
corresponding driver/runtime/code ownership island until Close. The new CMake
options reject unsupported host platforms; they do not select a runtime backend.
No existing host receives an implicit native, SDL3 or Null fallback.

`AudioFrontendOutput` adds an optional `openOperation` receipt at the end of its
non-installed aggregate. Existing four-field inert-output callers retain their
closed-backend path. The editor preview now passes a real retained Open operation,
prepares for its actual format and transfers it through the same composition
authority. No caller-supplied `Opened` copy or boolean proves acquisition. Failure
leaves the operation and owners with the caller, including a possibly published
detached generation that must be reconciled during cleanup. Public frontend
signatures and header ownership stay unchanged; documentation clarifies explicit
retirement for adopted open candidates and Start's acquisition/attachment modes.
Regression sources cover receipt consumption, no implicit activation, one explicit
Start and Close-before-Start. Negative admission cases include pending, foreign,
stale and wrong-kind receipts, invalid actual format, a foreign timing clock domain,
and failed acknowledgement after detached publication. Rejection retains caller
owners and the exact offered receipt. Executed test and full platform evidence
remain required before claiming complete integration.

Priming may quiesce before Ready. Frontend Close never waits for readiness or
commits Rendering to authorize cleanup. Null and SDL3 admit that transition like
the native common runtime. The frontend observes every fact in an already-drained
batch while retaining its first failure, and never activates from Ready while
closing. New direct-peer and frontend tests cover no Ready, queued Ready and device
loss before Ready, without ordinary render execution. These are unexecuted
regression sources until the focused builds and CTest run.

`IAudioComponentPreview::Start` now explicitly documents admission versus completed
preparation. Its snapshot adds trailing `AudioComponentPreviewPreparation`, default
None for existing synchronous providers. The control bridge publishes Pending
before a real frontend exists and clears it on actual result publication or
preparation cancellation. Pending requires no output snapshot; it cannot invent a
voice or device epoch. `EditorWorkspaceController` retains its existing copied
workspace/revision fences, and the inspector projects Pending through the existing
localized opening status. Existing five-field snapshot aggregate callers keep None.
Bridge and inspector-render regression sources cover the added state; public
EditorServices consumer builds and manual GUI interaction are still required.

Preview hosts now borrow the process composition's `AudioRuntimeOwnerIssuer` from
the non-installed output-policy target. Its control-only atomic counter prevents
identity reuse across concurrent owners and replacement host instances; it does
not publish resources or correlate device sample clocks. The graphical startup
owns this authority once and retains it in startup options through all dependent
control-host shutdown. Direct and dedicated-owner preview constructors now require
that explicit borrow; application and workspace-test compositions migrate together.
Future game-output composition must borrow this same authority rather than create
another counter. No installed signature or header ownership changes here.
Regression sources reserve another output family's identity, run two actual Null
frontends with unequal callback progress, retire one without stopping the other,
replace a host without identity reuse, and issue unique identities on two control
threads. These sources remain unexecuted; two preview lanes do not qualify a game
host, hardware drift, mirroring or aggregation.

The internal `NativeAudioDriver::Stop`/`Close` return type changed from a void
success to explicit Pending/Complete progress because native retirement must not
block control or treat a merely admitted stop as detach proof. All three concrete
drivers, the shared native lifecycle and its deterministic primitive fixture move
together. WASAPI and Core Audio currently return Complete after their synchronous
native proofs; PipeWire polls its owned worker. No installed header or existing
`AudioBackend` caller changes, and no parallel compatibility protocol is retained.

The new internal `HasUndrainedRequiredFacts` query reads native-source/mailbox
backpressure without OS work; all three drivers and the primitive fixture implement
it. It prevents epoch-state reuse from deleting or relabeling an older loss before
the host accepts it. Only Horo-owned fact values cross this interface, and native
callbacks never mutate the control-owned mailbox.

Native producers publish presence and the first typed loss cause together through
`NativeAudioLossLatch`. Later notifications cannot overwrite that cause. Control
copies the fact before resetting or releasing the detached source. The shared
lifecycle rechecks native loss when an admitted Start executes and before readiness
activation; admission alone does not authorize attaching a port after device loss.
PipeWire scopes core errors to `PW_ID_CORE`; optional metadata/object errors do not
prove output loss. Broken core connections report service loss, permission errors
report revoked access, and unclassified core errors remain backend failure. These
native paths still require platform execution evidence.

An expired native control operation does no native work and preserves an already
proved callback detach. Its failure reports the driver's current disposition for
detached/open-only state; an attached reader still requires Retained. Expiration
never proves Close or overrides a driver's retained cleanup prefix. Regression
sources cover an unexecuted Start, an unexecuted Close after successful Stop, and
an unexecuted Quiesce while a reader remains attached, followed by explicit cleanup.
These production-clock tests have not yet been compiled or executed.

Native candidate admission and device-switch admission additionally require the
timing snapshot's monotonic domain to match the configured control/deadline domain.
Generic timing validity alone only establishes a nonzero coherent domain; it
cannot prove this host's clock identity. A foreign-domain native candidate is
rejected and its acquired prefix explicitly closed, before Start or playback
adoption. The native-candidate fixture covers this rejection alongside foreign
format revisions; these regression sources have not yet run.

The uncommitted switch contract now separates `DeviceSwitchTarget` intent and
reserved revisions from the resolved `Open`. Its earlier request-carried catalog
could not discover a restarted PipeWire service before the failed old connection
closed. `SupplyDiscovery` and optional completion epoch replace that assumption;
both existing Null transaction tests and the composed native-mechanism fixture
move together. Production host callers do not yet exist. The native fixture
changes the default ID only after Close and exercises receipt admission through
actual `AudioBackend` operations; it is not Linux daemon/hardware qualification.

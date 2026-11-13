# Voice Playback Qualification

This evidence ledger covers [AUD-003](https://github.com/horocore/horo-engine/issues/550),
including its scene-unload policy and runtime qualification children. It does not
declare the initiative complete. Audio Architecture remains normative; ADR-062,
ADR-063 and ADR-067 still have `Proposed` headers. This document neither changes
their status nor grants implementation approval for additional device policy.

## Lifecycle invariant and regression

An ordered scene-unload or runtime-reset barrier cancels nonterminal voices.
Already terminal voices preserve their original Stopped, Finished, Cancelled or
Failed outcome. They must not reject the barrier by attempting another terminal
transition. The executor validates scope, generation and FIFO sequence before
accepting the barrier. Replaying its sequence remains invalid.

Callback cancellation does not release resident PCM, pending publication storage,
stream render ports or worker resources. `EndBlock` acknowledges the mixer's last
borrow; `CompleteShutdown(false)` still cannot reclaim the ownership island.
Native detachment proof precedes release and stream retirement on control.

Detached voice retirement retains one immutable `AudioVoiceSnapshot` before the
registry slot is released. `AudioVoiceRenderRuntime::TerminalSnapshot()` returns
that value only after successful `CompleteShutdown`; unproved detachment exposes
no final outcome. The frontend copies the value into
`AudioFrontendSnapshot::terminalVoice` before destroying its render owner. Closed
frontend snapshots therefore preserve the exact terminal identity and reason even
after the registry rejects the old handle. Existing terminal states remain intact;
otherwise detached shutdown commits Cancelled once. Neither getter reads active
callback registry state or publishes a terminal result merely because output is
silent. These are retained values, separate from transport-operation receipts.

The voice-preparation failure sweep also covers the fixed coefficient bank's
ownership transfers. MSVC Debug's `std::vector` move constructor allocates an
iterator proxy inside a `noexcept` boundary; an allocation failure there terminates
before the playback factory can translate it. The immutable coefficient bank now
uses an owned fixed array, whose move cannot allocate a container proxy. Ordinary
preparation allocations remain covered by the existing factory failure path.
The published parent measures the complete successful preparation allocation
count, injects failure at every observed position and checks the following
successful position. Every failed preparation must leave all registry slots
reusable. This qualification retains that measured sweep rather than a fixed
failure-count cutoff.

Persistent ambience belongs to a separately host-owned context under ADR-062;
it is not an exception allowing scene-owned voices to survive scene unload.
`AudioRepeatedPlaybackConfig::hostContext` is an explicit host-admitted process
context. When supplied, `KeepAliveInHostContext` allocates the voice and its
returned command scope in that context from initial admission. The producer lane
retains its source scene identity. Source unload closes that lane but leaves
already admitted playback and pending starts under the host's ownership. Host
retirement cancels them and closes further persistent admission. No live callback
resource is reparented, and no scene unload barrier is bypassed. The host still
validates source/context admission and registers the host context with its
transport before accepting producer work.
The present reset path cancels the old voice identity. Resume/restart across a
new device epoch must use the explicit migration transaction owned by AUD-007.7;
the executor does not silently resume, restart, fall back to Null, or transfer old
commands to a replacement device.

## Executable coverage

On 2026-11-11, GitHub's native sub-issue list still contained all ten AUD-003
children. The first eight were closed and their implementation commits were
verified as ancestors of this worktree's base; they are not reimplemented here.
Jira parent identities all resolve to HORO-550.

| GitHub / Jira | Domain ticket | Jira state | Existing implementation commit |
|---|---|---|---|
| #551 / HORO-551 | AUD-003.1 | In Review | `129c16392760` |
| #552 / HORO-552 | AUD-003.2 | In Review | `b1f2fca1002f` |
| #553 / HORO-553 | AUD-003.3 | In Review | `f0273a83313e` |
| #554 / HORO-554 | AUD-003.4 | In Review | `bd4b2e4f597c` |
| #555 / HORO-555 | AUD-003.5 | In Progress | `726449e898d4` |
| #556 / HORO-556 | AUD-003.6 | Done | `11874fa92c63` |
| #557 / HORO-557 | AUD-003.7 | In Review | `54d4d4ad9d09` |
| #558 / HORO-558 | AUD-003.8 | In Review | `835611169b81` |
| #559 / HORO-559 | AUD-003.9 | To Do | Current lifecycle regression work; device migration blocked |
| #560 / HORO-560 | AUD-003.10 | To Do | Current bounded qualification work; full matrix remains open |

| Contract | Production path and regression suite |
|---|---|
| Scene source defaults and sound references | AudioApi; AudioPlaybackRequestTests, AudioSoundReferenceTests |
| One terminal outcome, stale generation isolation | AudioVoiceStateMachineTests; full registries at 1, 64, 256 and 1024 slots, four reuse cycles |
| Deterministic capacity decisions | AudioVoiceAdmissionTests; all eight policies at 1, 64, 256 and 1024 physical slots, repeated identical inputs, plus existing rank/bucket/cooldown tests |
| Cursor and loop phase through virtualization | AudioVoiceVirtualizationTests, AudioVoiceRenderStreamTests; partitioned resident advancement and real stream worker realization |
| Resident playback at configured ceilings | AudioVoiceVirtualizationTests; 1, 64, 256 and 1024 actual prepared PCM owners, alternating physical/virtual execution, loop phase, pause/resume and cancellation with no callback allocation/free |
| Retrigger and variation | AudioRepeatedPlaybackTests |
| Copied routing/spatial state | AudioVoiceRenderRuntimeTests; real PCM, mixer, retained publications and completed-block acknowledgement |
| Unload/reset with active and terminal voices | AudioVoiceRenderRuntimeTests; Ready, Scheduled, Playing, Paused, Virtual, Stopping and all four terminal outcomes |
| Stream cancellation during pending realization | AudioVoiceRenderStreamTests; unload/reset, already stopped/cancelled voices, cursor hold, silent output, retained retirement pins |
| Pending schedules, callbacks and reset reconciliation | AudioLifecycleReconcilerTests, AudioCommandStagingTests, ScheduledAudioCommandBatchTests |
| Actual Null callback dispatch | AudioVoiceRenderNullTests and NullAudioBackendTests; deterministic backend, not physical hardware evidence |
| Synthetic device loss with pending controls | AudioFrontendTests; resident and real-service stream paths close admission, cancel unexecuted controls, retain render-port pins until Null detachment and release decoder/package ownership exactly once |
| Persistent resident ambience and schedules | AudioRepeatedPlaybackTests; explicit host opt-in, source/host retirement, host-scoped pending starts, real looping PCM, pause/resume and reset cancellation |

The configured regression ceilings are not a claim of qualification at the identity
limit of 1,048,576 slots. Unit policy tests, synchronous render tests and worker
stream tests do not establish long-running hardware stress or every cross-product
of source type, policy, device format and operating system.

## External acceptance gaps

- [HORO-601 / AUD-007.7](https://horo-engine.atlassian.net/browse/HORO-601)
  remains To Do: device migration, stop/resume/restart policy, fade and new
  rate/layout clock correlation are owned by that transaction. Its prerequisites
  HORO-532 and HORO-596 are In Review. This blocks full HORO-559 device-loss
  acceptance and consequently full HORO-560 qualification.
- Real WASAPI unplug/default-device replacement, headphones/Bluetooth, CoreAudio
  route changes and PipeWire restart have not been run by this worktree.
- HORO-533 is In Review while GitHub #533 is closed. Jira links its scene
  prerequisite to HORO-22; GitHub's native dependency endpoint identifies #20. Preserve
  this metadata discrepancy; do not infer dependency completion from issue numbers.

## Validation record

Public migration: `AudioRepeatedPlaybackConfig` adds one trailing optional
`hostContext`, defaulting to empty. Existing aggregate callers retain their
scene-stop behavior and continue to reject KeepAlive requests without host
opt-in. Callers adopting persistence must supply an admitted host context and
dispatch returned command scopes, rather than reconstructing the source scope.
`AudioRepeatedPlayback.h` remains solely owned by HoroAudioPlayback in the public
header registry; its dedicated public consumer covers aggregate compatibility,
the default and typed host-context assignment. Rebuild native consumers for this
configuration-layout change; serialized scene data is unchanged. No target
dependency is added.

The render getter is additive; the trailing optional `terminalVoice` frontend
snapshot field requires recompiling native Frontend consumers. VoiceRender and
Frontend retain their existing public-header ownership and dependencies. Their
public consumers cover the optional Horo snapshot type and noexcept render
getter. No native handle or callback pointer is exposed.

Initial base: `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`, verified against actual
`origin` branch advertisement on 2026-11-11. The three Audio commits were later
rebased without content changes onto the unpublished local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08` (C11 admission fixture correction).
`git range-diff` confirmed all three patches were preserved. Delivery branch:
`feat/HORO-559_voice_lifecycle_qualification`.

On 2026-11-13, the delivery branch was rebased onto published
`origin/codex/editor-improvements` at
`3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`, verified by fetch and the actual remote
advertisement. The parent's equivalent fixed-array resampler ownership and
measured allocation-failure sweep were preserved during conflict resolution.
The topic retains deterministic initialization of invalid evaluator selection
and its lifecycle/persistence/qualification changes.

Build/test and quality results must be recorded after execution. Required targets
are HoroAudioApiTests, HoroAudioPlaybackTests, HoroAudioVoiceRenderTests,
HoroAudioCommandTests, HoroAudioNullTests, HoroAudioEditorPreviewTests and
HoroAudioDspTests, plus HoroAudioPlaybackPublicHeaderConsumer. Reconfigure after
the header-consumer and ownership-registry changes. Run
native MSVC Debug, serial builds and CTest with the shared
`scripts/dev.py heavy_build_lock`; source inspection alone does not satisfy this
validation.

Current validation requires a bounded batch admitted by the physical shared lock. No updated
native test or full quality-preflight pass is claimed. The first historical
five-target build predates the final source edits, and its CTest filter matched
zero tests. Formatting and diff checks do not replace runtime qualification.
Before configure/build/test or analyzer compilation, verify the actual open
Windows lock handle against the canonical shared heavy-build lock, not merely the
cache identity. Acquire nonblocking; a busy lock defers the batch without leaving
a waiter. The 2026-11-13 policy removes the former named-grant scheduling gate.
Use one build worker and `CTEST_PARALLEL_LEVEL=1`.

The first 2026-11-13 bounded native batch configured successfully, including
public-header inventory and dependency-direction checks, then failed at the new
persistence test: its lvalue `Result::Value()` is const and cannot call mutable
`Bind`. The test now moves the owner out of the result. CTest did not run in that
batch; a fresh incremental build/test is required. Review also strengthened the
barrier tests to inspect the first borrowed PCM output before a second Render
overwrites scratch storage.

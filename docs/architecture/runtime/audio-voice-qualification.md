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

The voice-preparation failure sweep also covers the fixed coefficient bank's
ownership transfers. MSVC Debug's `std::vector` move constructor allocates an
iterator proxy inside a `noexcept` boundary; an allocation failure there terminates
before the playback factory can translate it. The immutable coefficient bank now
uses an owned fixed array, whose move cannot allocate a container proxy. Ordinary
preparation allocations remain covered by the existing factory failure path.
The sweep probes 64 allocation positions and still requires all registry slots
to remain reusable after failed preparation.

Persistent ambience belongs to a separately host-owned context under ADR-062;
it is not an exception allowing scene-owned voices to survive scene unload.
`KeepAliveInHostContext` is currently an authored/request policy, not an
implemented transfer operation: `AudioRepeatedPlayback` explicitly rejects that
binding. There is no production host-context promotion transaction in this
worktree. Persistent scene ambience therefore remains an acceptance gap; changing
the executor to ignore an unload barrier would violate ownership rather than
implement that policy.
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
| Retrigger and variation | AudioRepeatedPlaybackTests |
| Copied routing/spatial state | AudioVoiceRenderRuntimeTests; real PCM, mixer, retained publications and completed-block acknowledgement |
| Unload/reset with active and terminal voices | AudioVoiceRenderRuntimeTests; Ready, Scheduled, Playing, Paused, Virtual, Stopping and all four terminal outcomes |
| Stream cancellation during pending realization | AudioVoiceRenderStreamTests; unload/reset, already stopped/cancelled voices, cursor hold, silent output, retained retirement pins |
| Pending schedules, callbacks and reset reconciliation | AudioLifecycleReconcilerTests, AudioCommandStagingTests, ScheduledAudioCommandBatchTests |
| Actual Null callback dispatch | AudioVoiceRenderNullTests and NullAudioBackendTests; deterministic backend, not physical hardware evidence |
| Synthetic device loss with pending controls | AudioFrontendTests; production frontend closes admission, cancels unexecuted controls and releases the registry only after Null detachment |

The tested configured ceilings are not a claim of qualification at the identity
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

Worktree base: `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`, verified against actual
`origin` branch advertisement on 2026-11-11. Delivery branch:
`feat/HORO-559_voice_lifecycle_qualification`.

Build/test and quality results must be recorded after execution. Required targets
are HoroAudioApiTests, HoroAudioPlaybackTests, HoroAudioVoiceRenderTests,
HoroAudioCommandTests, HoroAudioNullTests, HoroAudioEditorPreviewTests and
HoroAudioDspTests. Run
native MSVC Debug, serial builds and CTest with the shared
`scripts/dev.py heavy_build_lock`; source inspection alone does not satisfy this
validation.

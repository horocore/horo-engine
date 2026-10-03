# Resident repeated playback

`AudioRepeatedPlayback` in `HoroAudioPlayback` consumes the existing
`AudioPlaybackRequest` and `AudioVariationAssetSchema` contracts, owns a canonical
`AudioVoiceStateMachine`, prepares `AudioVoicePlayback`, and emits normalized
`ScheduledAudioCommandBatch` values. It does not register a service or start a
device. Application composition supplies admitted scene/emitter/owner identities,
an explicit timeline and conversion budgets, and resolved resident PCM.

## Ownership and command path

Create reserves bounded lane, voice, bucket and projection storage. Bind copies
the policy and validated schema during control preparation. A lane identifies the
exact sound, scene generation, emitter generation, playback-owner generation and
nonzero incarnation. A duplicate identity is rejected even if playback defaults
differ. RetireScene closes its lanes permanently until a timeline reset; it
cancels pending/live voices but retains terminal snapshots until Release.

Submit validates the request's bound sound/context and ordered producer sequence,
then evaluates source-local instance limits and the authored scoped group against
the canonical registry. A group has one authored rule across all its buckets.
Ready/pending starts reserve capacity; paused/virtual counting follows the existing
group evaluator. Restart excludes only the exact voice reservation it updates,
and still observes cooldown and all other group members. No admission decision
steals another voice or invents physical virtualization.

A new start chooses the clip and adjustments on a private candidate replay state,
validates finite gain and pitch in `(0, 8]`, prepares and copies its PCM/resampler,
reserves a real Ready slot, and normalizes its command. Only then does it commit
the candidate state and successful target-frame cooldown. Errors leave them
unchanged. Ignore and cancellation before submission emit no command and consume
no variation/cooldown state. Cancellation after successful reservation retains
the committed choice and cooldown, even if the start never executes. This is a
recorded successful admission, not a rollback of an earlier sequence.

Restart retains the exact live voice's selected PCM, gain and pitch and draws
no randomness. It uses a held-sample fade, resets the source cursor/history and
resumes paused playback. A terminal voice cannot reopen; a subsequent Submit
creates a new generation. A pending restart cannot be replaced by another one.
Each command carries its producer operation sequence so a previously consumed
restart cannot execute a newer pending operation on the same handle.

The host retains and publishes each normalized batch through the ordinary
command storage/staging/SPSC path. At its boundary it calls Apply for the typed
child command. Exact-sample commands require the exact target frame and current
timeline. Early commands remain pending for retry; late and stale targets return
a static error. Buffer targets use the processing boundary supplied by the host.
The host owns clock mapping, batch storage acknowledgement, and context admission.
Latest identical Submit replay returns the original choice with disposition Replay
and no new commands, including after its target frame has elapsed. Replay still
requires the current timeline and non-retrograde control time; only new admissions
require a future target. Older or conflicting sequence reuse fails. A delayed restart
whose voice has naturally finished fails at application without reopening it.

Render uses production resident PCM/resampler processing with linear gain and
exactly-once terminal evidence, including natural EOF tail drain. Direct typed
pause/resume/seek/loop/stop/cancel controls use Apply with operationSequence zero;
they cannot forge a policy start/restart. Release is detached control work after
terminal evidence has been reconciled. Reset requires a newer callback epoch and
timeline token, cancels retained voices, invalidates all lanes, clears replay and
cooldown history, and retains terminal records for explicit Release.

No method runs concurrently with another. Bind, Submit, cancellation, reset,
Release and destruction require detached control ownership. Apply and Render
are allocation-free processing operations. A host using an asynchronous native
callback must provide a quiescent ownership-transfer boundary; this owner is not
a synchronization mechanism.

## Replay version 1

The seed and ordered request/admission/cancellation/clock sequence, authored
policy and resolved media revisions are replay inputs. `AudioPlaybackReplayVersion`
is 1. This version fixes:

- SplitMix64 unsigned arithmetic and constants;
- unbiased unsigned rejection mapping, at most 128 attempts per draw (exhaustion
  rejects the candidate rather than using a biased fallback);
- canonical clip-identity ordering for Random, Shuffle and WeightedRandom;
  RoundRobin preserves authored order;
- Random and weighted selection exclude the last clip when alternatives exist;
- Fisher-Yates bags visit each clip once, with an unbiased alternative first
  entry if a new bag would repeat the previous last clip;
- weighted tickets are `ceil(weight / maximumWeight * 2^32)`, so every positive
  weight retains at least one ticket, and the bounded sum fits uint64;
- selection draws precede one pitch and one gain draw for every new variation
  admission, including singleton and constant ranges; bound-one selection draws
  consume no word;
- adjustment draws use the upper 24 bits on the inclusive grid
  `numerator / (2^24 - 1)`, with binary64 range arithmetic then binary32 rounding.

Selection and delta replay is exact under IEEE round-to-nearest binary32/64.
Linear pitch `basePitch * exp2(semitones / 12)` and gain
`baseGain * pow(10, dB / 20)` have declared relative tolerance `2e-6` across
libm implementations. Outputs outside the existing finite gain/pitch contract
are rejected rather than clamped. Shuffle state has a fixed 1024-entry ceiling;
registration validates malformed, duplicate, empty and oversized schemas through
the existing schema validator. Resolver sets must be complete, bounded and unique.

## Migration and current adapter limits

The new public header belongs only to `HoroAudioPlayback`. That target now
declares `HoroAudioCommands` publicly because the API emits its batch/command
contracts. Existing direct `AudioVoicePlayback` callers retain unity gain by
default. `AudioVoiceControl::Restart` is appended and `operationSequence` defaults
to zero, preserving existing aggregate control callers. Consumers must update
exhaustive control switches if they interpret the enum themselves. No persisted
asset schema or scene format changes.

The current implementation handles resolved resident clips/variations and 2D
unassigned-bus playback. Streams, middleware, spatial providers, bus mixing,
physical stealing/virtualization and keep-alive scene policy return Unsupported.
The native SDL3 adapter does not automatically compose this owner. Tests exercise
its production Apply/Render path with actual Null callbacks and the SPSC buffer;
these limits do not establish native device or application-host integration.

# Audio assets and streaming validation

This guide maps AUD-002 acceptance to executable evidence. The normative ownership
and migration contract remains in [Audio Architecture](../architecture/runtime/audio-architecture.md)
and [ADR-064](../adr/064-audio-asset-and-cook-boundary.md); ADR-064's recorded status
is unchanged.

## Acceptance coverage

The matrix covers all thirteen children, including the merged ones. A source/test
mapping is not a current-run pass or an issue-state transition.

| Ticket | Concrete source and regression evidence |
|---|---|
| HORO-537 / #537 / AUD-002.1 | ADR-064 and the Audio/Asset Pipeline architecture assign generic orchestration/publication to AST and media semantics to Audio. `VerifyAudioDependencyPolicy.cmake` enforces target direction. The ADR's Proposed status is preserved rather than silently promoted. |
| HORO-538 / #538 / AUD-002.2 | `AudioAssetSchema.h/.cpp` owns versioned clip/stream/variation values with finite bounded format, loop, marker, seek and variation validation. `AudioAssetSchemaTests` checks versions, limits, half-open ranges, identity and invalid numeric metadata. Persisted values contain stable IDs rather than decoder/voice handles. |
| HORO-539 / #539 / AUD-002.3 | `AudioSoundReference.h/.cpp` represents Clip, Variation, Stream, Music and Extension using typed IDs and contribution/version identity. `AudioSoundReferenceTests` exercises all core/extension kinds, mismatched target rejection, definitions and playback defaults without vendor enums. |
| HORO-540 / #540 / AUD-002.4 | `AudioMediaFormatRegistry` separately validates container, codec, payload kind and PCM facts in an immutable bounded catalog. `AudioMediaFormatRegistryTests` rejects unknown combinations, invalid/duplicate identities and excess metadata and proves copied ownership. |
| HORO-541 / #541 / AUD-002.5 | `AudioSourceImporter`, `AudioSourceProbeDetail.inl` and `AudioSourceAnalysis` perform bounded source reads/decode and deterministic WAV/Ogg analysis. `AudioSourceImporterTests` checks the pinned Vorbis fixture, decoded order/loops, hostile sizes, waveform/LUFS/true peak and source failures. New unsupported/duplicate sampler and truncated Vorbis regressions require the final run. |
| HORO-542 / #542 / AUD-002.6 | `AudioCookProfile` fingerprints exact effective policy, target and toolchain; `AudioCooker` emits deterministic payload/manifests. `AudioCookerTests` checks identical bytes, overrides, invalidation and ordinary AST cache/publication. Schema v3 needs a fresh run and normal recook. |
| HORO-543 / #543 / AUD-002.7 | `AudioStreamDecoder` admits explicit worker output/scratch bounds, progressive decode, exact seek, cancellation and typed provider failures. `AudioStreamDecoderTests` checks lifecycle, progress, unsupported seek, provider exceptions and cancellation; the new finite-PCM guard commits no failed cursor. Callback execution uses prepared ring media instead of this decoder. |
| HORO-544 / #544 / AUD-002.8 | `AudioStreamingService` owns ordinary host jobs, priority/lookahead and bounded ring publication. `AudioStreamingServiceTests`, `AudioStreamingCancellationTests` and `AudioStreamingControlTests` exercise ordered ring reuse, final publication, silence/stop policy, cancellation/join, leases and budget release. The descriptor-copy allocation fix needs the final run. |
| HORO-545 / #545 / AUD-002.9 | Cook schema v3 preserves importer loop regions. Inspection bounds loop counts, IDs and half-open ranges. `AudioCookerTests` verifies deterministic loop round trips and unsupported nonzero preroll/crossfade. `AudioStreamingControlTests` compares exact stereo reference samples through repeated worker loops, ring wraps, late seek and terminal silence. Cursor and sample error tolerance is zero for these PCM fixtures. The authored ramp's boundary jump is preserved exactly rather than smoothed implicitly. |
| HORO-546 / #546 / AUD-002.10 | `AudioSourceAnalysis` produces waveform pyramid, LUFS/true peak/RMS/normalization and byte estimates. `AudioCookInspector::InspectCookedAudio` returns embedded validated analysis from cooked bytes without a source-reader argument or decode call. `AudioSourceImporterTests` validates measurements and `AudioCookerTests` validates analysis round trips. This is the reusable inspection contract, not an observed editor UI interaction. |
| HORO-547 / #547 / AUD-002.11 | `AudioAssetSchemaTests` covers duration/rate/layout, loop/seek and non-finite metadata. `AudioStreamDecoderTests` covers provider errors/exceptions/protocol/cancellation and non-finite PCM rejection without cursor commit. `AudioCookerTests` rejects every proper prefix of its valid loop-bearing source and artifact, hostile loop fields, corrupt chunks/digests and hash-forged non-finite PCM. Invalid cooks return no output candidate; invalid decode blocks never publish ring samples. |
| HORO-548 / #548 / AUD-002.12 | `AudioStreamingControlTests` covers incompatible, cancelled, invalid and prefetched replacements with an active predecessor. `AudioCookerTests` exercises immutable package-to-filesystem generation replacement, including corrupt replacement bytes. `AudioVoiceRenderStreamTests` keeps a real active voice on its retained old generation during successful/failed candidate preparation and proves early retirement is rejected until detached voice shutdown. |
| HORO-549 / #549 / AUD-002.13 | `AudioStreamingControlTests` checks exact per-stream/global byte reservations, independent invalid/budget counters, fill capacity, decode time, scheduler rejection and coalesced rate-limited underrun deltas. Existing stop-with-silence, bounded fills, allocation and cancellation tests remain in the same suite. |

The parent deterministic-cook/cache/reimport evidence is in `AudioCookerTests`:
identical inputs yield identical bytes/manifests; profile/toolchain changes invalidate
identity; generic AST staging/cache/publication handles fresh, cached and failed
generations. Filesystem and archive tests both consume cooked PCM without authoring
source fallback. `AudioSourceImporterTests` covers bounded WAV/Ogg reads and the
known pinned Ogg/Vorbis reference.

## Ownership and observability

Replacement follows the retain-existing-voices policy. A new stream gets an
independent generation, source/code lease and full budget reservation. Commit opens
new-port admission after validation/prefetch; it never edits a live port or reclaims
the old payload. New ports start at frame zero. Voice control remains responsible
for an explicitly selected fade/restart and eventual callback detachment. Candidate
cancellation and failure preserve the old stream. No extra registry, publication
cache or scheduler is introduced.

Stage and commit both reconcile completed predecessor work and require that exact
predecessor to remain active. Stopping, cancellation or worker failure during
candidate preparation closes replacement publication, even if the old handle is
still retained for safe callback retirement. The late-commit regression verifies
that the unpublished candidate remains ineligible for a render port.

Starvation causes are current control observations, not retrospective attribution.
The callback publishes only bounded counters and PCM consumption; control reads
snapshots and rate-limited reports. Scheduler rejection counts rejected work, not
lost PCM. Rate-limited reports coalesce rather than drop observations; counters
saturate at exhaustion. Byte charges cover ring, decode/scratch and worst-case
package storage rather than allocator RSS.

Existing OBS integration is implemented in `AudioMetrics`,
`AudioMetricTelemetry.cpp` and `AudioMetricExtraction.cpp`: host control can project
the stream fill ratio through `ObserveGauge(StreamFillRatio, ...)` into the existing
single-series `audio.stream.buffer_fill` instrument. The generic extraction queue
tracks drops/coalescing/rate limiting separately. Stream diagnostics retain their
typed detailed API facts; they do not add asset IDs as unbounded metric labels or
introduce an AudioApi-to-Metrics reverse dependency.

## Targeted validation

Configure a separate Debug/Ninja directory with testing enabled and optional
device/GUI backends disabled. Hold the repository-wide `scripts/dev.py`
`heavy_build_lock` for configure, build, tests and any analysis build. Use one
compiler job and `CTEST_PARALLEL_LEVEL=1`.

Build `HoroAudioApiTests`, `HoroAudioImportTests`, `HoroAudioCookTests`,
`HoroAudioVoiceRenderTests`, `HoroAudioApiPublicHeaderConsumer` and
`HoroAudioCookPublicHeaderConsumer`. Run those executable suites plus
`HoroAudioDependencyPolicyTest` and `HoroAudioCallbackLockPolicyTest`.
After final edits run the full quality preflight against the actual target branch
with the exact worktree compile database; record IDE, secrets and Codacy separately.

These headless tests do not establish native-device, Linux/macOS or hardware
qualification. A local clean report does not replace hosted quality gates. Exact
commands, run results and report paths belong in the delivery validation summary.

## Recorded evidence and remaining gates

The initial Windows MSVC Debug checkpoint preceding the final delivery commits
built the four executable suites and AudioApi/AudioCook public-header consumers.
Its CTest run selected 209 tests: AudioApi 165 passed / 1 failed; Import 4 passed;
Cook 12 passed; VoiceRender 24 passed / 1 failed; two dependency/callback policy
tests passed. Total: **207 passed, 2 failed**. That run does not validate later
descriptor-copy, malformed-source or late/saturated underrun reporting edits.

The two failures were diagnosed from their actual process stacks:

- Stream admission's by-value descriptor moved an MSVC Debug vector under the
  allocation probe before the typed failure boundary. The local fix borrows the
  descriptor and copies owned storage inside that boundary; its new failure-site
  sweep still requires execution.
- The existing resident voice allocation sweep aborted in the resampler coefficient-vector move. On 13 November the published parent `3df476ff430a2ad28cd62a8a0c1780ae0fea71bc` was verified and adopted. Its `22cec166a` integration already uses `unique_ptr<float[]>`; the former missing-prerequisite observation against parent `5d0aff9d6` is historical. Fresh broad voice execution is still required and its test remains enabled.

All thirteen children still require final integrated evidence at the delivered
head. The four open GitHub children (#545, #547, #548, #549) have local source/test
deliveries, not completed acceptance clearance. Closed GitHub states or differing
Jira review states do not replace these gates.

No heavy worker is queued. Under the 13 November policy a named chat grant is no longer required. The next nonblocking physically admitted bounded batch must cover an
incremental out-of-source Debug rebuild, the owning public-header consumers, the
targeted media suites and relevant voice regressions, followed by a fresh full
`scripts/analyze.py check --base origin/codex/editor-improvements` using this worktree's
compile database. Reconfigure for the updated parent before compiling. Use a
nonblocking canonical lock acquisition and verify the actual open Windows lock
handle before starting configure/build/test/analysis. An occupied, inaccessible or
different physical lock defers the work without a private fallback.

Local doctor found the portable IDE profile ready for a local check; Codacy cloud
configuration drift remained incomplete because authentication failed. The final
Sonar IDE, secrets and Codacy provider checks have not yet run on the final edits.
Any trust/authentication or platform limitation must stay recorded separately;
no hosted, native-device or cross-platform clearance is implied.

On 13 Novemberthe restored worktree was rebased onto the verified published parent. Its
first bounded rebuild admission attempt measured CPU30 28.20% and free RAM 14.71%, below
the required 25% RAM threshold; no configure/build/test child started. A subsequent
documentation write exposed a full C: volume (zero free bytes). The guide was restored
from its committed blob and only this task's regenerable old test PDBs were removed;
original test logs/receipts and source commits were retained. These are resource-admission
and recovery observations, not final-head build/test or analyzer passes. The final full
Sonar IDE/secrets/Codacy preflight remains unrun until physical lock and resource
admission permit it. Any draft delivery must retain that explicit gap.

A second build admission attempt measured CPU30 15.10% and free RAM 34.28%;
resource checks passed, but nonblocking physical lock acquisition was busy. No
configure, compiler or test child started. Draft publication is permitted by the
13 November user instruction while external evidence remains incomplete. The
final provider checks are not claimed as run or passed by these admission receipts.

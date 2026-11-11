# Audio assets and streaming validation

This guide maps AUD-002 acceptance to executable evidence. The normative ownership
and migration contract remains in [Audio Architecture](../architecture/runtime/audio-architecture.md)
and [ADR-064](../adr/064-audio-asset-and-cook-boundary.md); ADR-064's recorded status
is unchanged.

## Acceptance coverage

| Ticket | Concrete source and regression evidence |
|---|---|
| HORO-545 / #545 / AUD-002.9 | Cook schema v3 preserves importer loop regions. Inspection bounds loop counts, IDs and half-open ranges. `AudioCookerTests` verifies deterministic loop round trips and unsupported nonzero preroll/crossfade. `AudioStreamingControlTests` compares exact stereo reference samples through repeated worker loops, ring wraps, late seek and terminal silence. Cursor and sample error tolerance is zero for these PCM fixtures. The authored ramp's boundary jump is preserved exactly rather than smoothed implicitly. |
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

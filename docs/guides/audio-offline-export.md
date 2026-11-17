# Offline Audio File Publication

`HoroApplicationAudioExport` is a non-installed application operation over
`HoroAudioOffline` and a host-injected `DurableFileSystem`. It renders a fresh,
exclusive prepared core to complete PCM, encodes a float32 WAVE file and publishes
it with the filesystem's tracked atomic replacement. It selects no device, opens
no hardware and does not use Null lifecycle simulation.

The host prepares the actual voice/mixer/automation/media owners, explicit content
and mixer-tail budget, dedicated offline epoch and deterministic clock domain.
Those owners and their code remain alive through the synchronous call and their
subsequent detached teardown. No concurrently active device core may be borrowed.
The operation consumes the prepared schedule; it does not derive a cinematic
schedule, map sequence time to samples or infer tail completion from silence.

Compose a no-follow publication authority limited to a host-selected derived
output namespace, such as `CreateNativePublicationFileSystem`. Retain that grant
and protect its directory ownership through the call. The destination directory
and its private `.horo-audio-export-staging` child must already exist. The request
names an absolute normalized destination and a fresh prepared-file leaf directly
inside that staging child. A dedicated nonblocking writer lease under the staging
directory serializes exports before rendering; its exact live path protection is
validated, and it stays held through failure cleanup, including exception handling.
The operation does not fall back to direct
native I/O or create arbitrary destination directories.

All currently modeled speaker roles have explicit WAVE speaker bits. PCM channels
are permuted into ascending WAVE mask order without mixing, resampling, clipping
or changing finite headroom. This matters for Horo's side/back channel order.
WAVEFORMATEXTENSIBLE carries the float subtype, valid-bit count and speaker mask;
the `fact` chunk counts content plus tail frames. Integer fields and binary32 data
are encoded explicitly little-endian. Discrete and Ambisonic layouts reject
before rendering rather than losing their semantics. Another container preserving
those semantics remains a separate capability.

Both PCM storage and encoded storage have finite admission bounds of at most
512 MiB each; prepared core storage is owned separately by the caller. The encoded
limit includes the complete 80-byte header. Cancellation is checked between core
blocks, bounded encoding batches and before commit. Synchronous filesystem calls
cannot be interrupted. A successful irreversible commit wins cancellation.

Keep `OfflineAudioWaveExportReceipt` alive across success, errors and exceptions.
It records one-shot admission before entering the core, private-file creation,
successful pre-commit removal and irreversible
publication separately. A pre-existing prepared file is never truncated or removed.
Write/cancellation/pre-commit failures clean only invocation-created private
storage; cleanup failure preserves both errors and leaves ownership explicit.
A post-commit durability error leaves `publication.WasCommitted()` true: the
complete destination may already be replaced, and reporting rollback is wrong.
An admitted receipt is not reset or reused for another export, including after a
render failure that created no private file. Retrying requires a new exclusive
prepared core and a fresh receipt. Rejection before admission leaves the core
untouched.

Regression sources in `HoroAudioOfflineTests` use a scoped native authority and
real native filesystem fault adapters. They cover Unicode/space paths, complete
7.1 and 7.1.4 semantics and headroom, byte-identical block partitions, exact tail
length, invalid paths/bounds, occupied writer leases, existing prepared files,
render/nonfinite faults, cancellation, cleanup and post-commit durability failure.
These C++ sources have not yet been compiled or executed for this delivery.

This operation supplies the application file sink. It is not yet invoked by a
production cinematic export, CLI command or editor workflow. Actual timeline
preparation/invocation, compiled regression results and file/codec qualification
remain required; the current source is not a completed export-host acceptance.

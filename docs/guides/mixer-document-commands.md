# Mixer asset document commands

`HoroEngine::EditorServices` owns `Horo/Editor/MixerAssetDocument.h`. The header
adds one explicit public dependency on `HoroEngine::AudioApi`; Audio never depends
on Editor. Consumers link EditorServices and use staged target headers. Existing
source schema and runtime compiler callers do not change.

The workspace/application host issues an `Asset` `DocumentIdentity` and opens a
document from detached `MixerAssetSchema` data. It owns source I/O, atomic durable
publication, workspace placement and preview/compiler requests. The document is
usable headlessly and does not create devices, select backends, retain scene
memory, or register ambient services. Dedicated mixer controls and preview are
separate AUD-009 children; this change provides their shared command authority.

`Open` borrows limits by const reference only for the call, then copies the
validated policy into the session. It avoids an unnecessary large input copy
without retaining caller storage. Existing invocation syntax, including default
and temporary limits, is unchanged; consumers rebuild against the updated header.
The regression suite changes caller limits after open and proves the retained
session ceilings remain enforced.

All methods run on the document owner thread. Mutations capture `Revision()` and
submit typed commands in one bounded transaction. Bus insertion includes its
primary route. Removing a leaf bus removes its own primary route but rejects any
remaining send or child reference. Remove those references or reparent the children
explicitly in the same transaction. Bus property edits preserve ID, role and
insert chain; effect moves preserve effect identity. Route edits use stable route
IDs. Names and positions never resolve references.

Only the final private candidate is schema-validated, allowing an atomic primary
route replacement without exposing its temporarily incomplete tree. Failed,
stale, oversized, cancelled-before-submission or no-op transactions change no
source revision, dirty state or history. Staging launches no background work, so
there is no detached completion/cancellation lifetime. Allocation failures leave
the authoritative state unchanged and propagate to the host. Every successful
commit, undo, redo or reload advances the monotonic revision exactly once.

History retains before/after semantic bus and route deltas, including exact
effect values/order and source positions. It does not retain complete graph
snapshots per command. The item limit is 64; retained logical payload is bounded
to 8 MiB (allocator/container overhead is additional). Oldest steps are evicted
without altering current state. New edits clear redo only after validation and
history allocation succeed. Undo/redo stage and validate before publication.

`Snapshot()` produces an owned immutable capture of the validated source, session,
revision and semantic state ID. The host passes that capture to existing source
persistence or Audio graph preparation; derived work must compare the capture's
revision to the current document before submitting a runtime swap. No mutable
graph is exposed. Audio still owns compiler/catalog/profile validation, buffer
boundary adoption, acknowledgement and retirement under ADR-065.

Call `MarkSaved(capture)` only after the host's atomic durable publication has
succeeded. A save of an older captured state leaves newer edits dirty. Undoing
back to the saved state clears dirty state while advancing the revision. The
method does not write files or imply that compilation saved authoring data.

Supported older source schemas pass through `MigrateMixerAssetSchema` without
modifying the input. Migrated data remains dirty until acknowledged as durably
saved. Invalid or unsupported source versions cannot open/reload a document.
Reload requires a clean source or explicit discard; it clears history and fences
all pre-reload source captures. Failed reload leaves every prior state intact.
Close requires a clean source or explicit discard, releases history and authoring
storage, and is idempotent. Moved-from instances are closed. Host instance IDs
must remain unique across open sessions, as required by DocumentIdentityRegistry.

`HoroMixerDocumentTests` exercises stable bus/route/effect identities, reference
rejection, graph cycles and missing endpoints, transactional rollback, chain
order, source-array positions, dirty/saved state, bounded history, stale requests,
detached snapshots, migration, reload barriers and close/move ownership. It is
included in Windows checks and the Sonar editor coverage suite. The generated
EditorServices consumer compiles the public header at its owning boundary.

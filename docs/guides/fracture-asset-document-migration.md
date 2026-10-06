# Fracture asset document model

`HoroEngine::FractureDocument` owns the public
`Horo/Editor/FractureAssetDocument.h` contract. Link that target instead of
publishing repository-wide include paths. Its sole direct public dependency is
`HoroEngine::DestructionApi`; it is usable without editor GUI, Physics, Render,
platform backends or a display. The public-header consumer exercises this boundary.

This implements the DFR-005.2 authoring model under ADR-148. No previous persisted
fracture document format is replaced. Import/generator controllers, UI panels,
Assets publication orchestration and preview sessions remain the distinct owners
described by ADR-145/148; this model never selects or starts those services.

## Opening and editing

The editor host assigns a fresh nonzero `FractureDocumentSession` for each open
and admits exactly one writable document per canonical fracture asset. Decode
accepted source bytes, then open with the durable Assets source revision. All
mutation, save acknowledgement and close calls run on that single editor owner;
workers may retain immutable snapshots, never the mutable document.

Transactions carry the exact session and current monotonic document revision,
host-selected edit permission and cancellation token. Operations address stable
chunk IDs, ordered endpoint pairs and material slots. Settings, graph, hierarchy,
materials and damage policies are staged together and validated before one
no-fail source/history publication. Invalid final references, hierarchy cycles,
unsupported capabilities, nonfinite values and required chunks without anchor
reachability reject the entire transaction. Intermediate values in a compound
transaction need not form a valid source. Empty or semantically unchanged edits
create no history, revision or redo-branch change.

Undo/redo store exact typed before/after values. Explicit import/generation
acceptance uses `ReplaceFractureSource`; the document copies the checkpoint to
prevent later mutation through caller aliases. Undo/redo never reruns generation.
State identity tracks the saved content independently of monotonic revision and
history eviction. New edits discard redo only after successful publication.

Hard envelopes are 1,024 chunks/sites, 4,096 contacts, 256 materials and 256
operations per transaction; the chosen tier may impose stricter chunk/depth
limits. Default history retains at most 128 entries and 4 MiB of semantic
representation, including retained checkpoint sections. The configurable byte
ceiling is 16 MiB. A transaction whose complete history does not fit fails before
publication; older history eviction never changes source or saved state. These
are semantic storage budgets, not an allocator-level resident-memory guarantee.
Validation and copying are bounded editor/tooling work, never a frame-hot path.

## Source publication and reopen

`CaptureSave` returns owned immutable bytes and the expected durable source
revision without clearing dirty state. Assets must atomically compare that
revision and durably publish exactly those bytes, then the editor owner calls
`AcknowledgeSave` with the receipt revision. The host is responsible for verifying
the durable receipt; this document does not perform filesystem I/O or fabricate
publication success. Later edits remain dirty when an earlier capture completes.
Conflicting receipts and tickets from another session cannot clear dirty state.
An exact duplicate accepted receipt is idempotent. Recovery capture has the same
encoding but must not be acknowledged as an ordinary source save.

Source schema 1 begins with ASCII `HFRS` and a little-endian uint32 version.
Integer fields use their explicit widths, enum/boolean fields use one byte,
floating fields use little-endian IEEE binary32/binary64 bits, asset IDs are
16 raw bytes and dependency digests are 32 bytes. Collections have uint32 counts
and canonical stable-key order. Readers bound counts and minimum remaining wire
bytes before allocation and reject unknown schema versions, truncation, trailing
data, malformed booleans, nonfinite floats and noncanonical negative zero.
Writers normalize zero. Source stores exact mesh/material dependencies,
recipe/version/seed/toolchain intent, explicit Voronoi sites, UV settings,
hierarchy/connectivity/anchor annotations and damage policy. It excludes session,
revision/history/dirty state, generated meshes, cooked/native products, runtime
health/body motion and broken sets. Reopen creates a fresh clean session from
decoded durable bytes; save bytes and leased snapshots remain readable after
close, while late edits and acknowledgements are rejected.

## Validation

`HoroFractureDocumentTests` covers transactional rollback, exact semantic history,
immutable leases/checkpoints, save conflicts and reopen, stale revision/session,
permission/cancellation/close fences, history eviction/exhaustion, portable
round-trip and malformed/version-skewed/bounded source input.
`HoroFractureDocumentPublicHeaderConsumer` verifies the narrow consumer target.
Both join the focused Windows CI composition; model cases join Sonar coverage.

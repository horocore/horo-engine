# Runtime UI accessibility order and announcement delivery

`HoroEngine::RuntimeUi` owns these contracts; existing public headers retain their
single target ownership and staged include boundary. No dependency or native
accessibility adapter is introduced.

Publisher budgets and announcement cursors now pass by const reference to avoid
copying enlarged owner evidence. Ordinary call syntax remains compatible; clients
taking method pointers must update their signatures and recompile.

## Order and focus migration

Interactive publication calls `UiAccessibilityExtractor::Extract` with the actual
`UiFocusGraph` for the presented player/layer. The graph and semantic descriptor
must match instance, canvas, document, document/tree revisions and last-presented
interaction revision. An extractor binds to that graph audience on its first
successful graph publication. A different audience needs its own extractor.

`ReadingOrder()` contains active Visible/Offscreen nodes in retained-tree preorder,
including noninteractive labels and disabled controls. `FocusOrder()` contains
eligible targets in the graph's authored sequential participation order. Explicit
navigation links still control directional movement. Semantic candidate focus
flags are replaced by graph evidence. Eligible graph targets missing from semantics,
disabled or inactive are rejected before publishing any immutable slot.

`FocusState()` owns the exact audience, focus target, top modal generation and
inclusive modal root. The current modal covers outside semantic nodes and removes
them from reading/focus participation. Older snapshots retain their own order,
text and focus evidence through graph reload/shutdown. Extraction without a graph
remains available for noninteractive semantic consumers; it has no focus-order or
audience evidence. Mixing it with graph publication cancels prior scoped speech.

`UiFocusSnapshot` now includes `modalRoot`. Code manually constructing an active
modal snapshot must supply the matching root target; obtain snapshots from the
graph to preserve its invariants. `UiFocusGraph::Order` preflights output capacity
and leaves the destination unchanged on failure.

## Delivery migration

Consumers that previously spoke `Changes()` announcement previews must switch to
`Announcements()` and resolve text with `AnnouncementText(cursor)`. Previews are
ephemeral and may disappear during delta overflow. The retained queue is the sole
delivery authority and is independent of full-snapshot resynchronization.

Each accepted occurrence gets a strictly increasing nonzero sequence within an
exact instance/canvas/delivery generation. The host supplies a nonreused
`deliveryGeneration` and increments it when recreating a publisher for the same
instance/canvas. Occurrence IDs are stable producer identities, not text hashes.
Retries of the same ID/node are suppressed for the revision window and while the
record remains unacknowledged, even beyond the window. A new status transition
uses a new occurrence ID; status and validation speech are never coalesced.
Validation text and provenance must match the node's published typed error.

Process records in sequence order. Speak Pending records under their polite or
assertive policy, process Cancelled records without speech, then cumulatively
`Acknowledge` the last processed cursor. Repeating an acknowledgment is idempotent.
Foreign owners/generations, zero and never-issued future sequences fail without
changing the queue. Acknowledgment releases a prefix and compacts owned text;
all borrowed views are call-duration and must be reread after successful mutation.

Document/tree revision replacement, audience replacement or a different modal
activation cancels pending records with `OwnerChanged`. Hidden, disabled or
inactive/removed nodes cancel with `NodeUnavailable`. Retirement closes admission
and marks pending records `Retired`; cancellation records retain sequence evidence
until acknowledged and cannot be replayed. A newer semantic/interaction revision
alone does not discard otherwise eligible accepted occurrences.

Count, text and sequence exhaustion reject the entire publication before changing
baseline, deltas, accepted records or deduplication history. Producers retain and
retry rejected occurrences after the consumer acknowledges capacity. Cancellation
does not silently evict records; canceled speech bytes are reclaimed on prefix
acknowledgment or retirement. Queue creation reserves fixed bounds; successful
extraction/publication, cancellation, acknowledgment and retirement allocate no
new storage. All mutation is serialized on the Runtime UI owner thread. Future
platform adapters must marshal copied typed records to their required OS thread.

## Regression evidence

`HoroRuntimeUiAccessibilityTests` covers distinct reading/focus order from actual
tree/graph paths, focus movement and modal generation changes, retained snapshots,
stale audiences/revisions, transactional output capacity, rapid occurrences,
deduplication across revisions, byte/count exhaustion and retry, acknowledgment,
validation provenance, reload/inactive/retirement cancellation and allocation
probes. `HoroRuntimeUiAccessibilityPublicHeaderConsumer` verifies the staged
public surface. Native adapters and GPU presentation are outside this contract.

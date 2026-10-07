# World Streaming Eviction Policy Integration

WST-003.11 adds `StreamingEvictionPolicy.h` to `HoroWorldStreaming` without changing
existing residency, source pin, scheduler or resource-lease APIs. Existing consumers
need no migration. Host/authority integrations adopt the new pure selection call
instead of locally sorting eviction victims; it owns selection only.

The authority supplies a complete bounded snapshot with exact owner, epoch and
current generation for every cell. It advances `StreamingEvictionSnapshotRevision`
without wrapping or reuse whenever residency, pins, leases, priority or use changes.
Capture rows and context at one StreamingAuthorityRole safe point. Output storage
must cover the entire input ceiling even when every row is pinned. Failure preserves
all output; success writes only the returned selected prefix.

Immediately before submitting a selected victim to canonical retirement, compare
its owner, snapshot publication and generation to the current authority. A changed
pin, lease or successor invalidates the proposal and requires fresh selection.
Source, gameplay and provider pins are explicit owner-scoped demands released by
that owner or its teardown; they are never cleared by this policy. Outstanding
leases do not forbid logical retirement, but `RequiresLeaseDrain()` exposes why
physical reclamation must wait. Even a zero-lease victim requires Scene/provider
retirement acknowledgement before releasing reservations or cache charges.

Cancellation, failure and shutdown continue through the existing operation ledger
and retirement DAG. A new partition or policy publication does not discard old
charges, leases or retirement acknowledgements. Selection performs no I/O, backend
selection, allocation, registration or residency mutation.

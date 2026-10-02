# World Streaming feature budget reservations

Use `StreamingFeatureBudgetReservations` at the host's StreamingAuthorityRole
when provider work requires global and feature capacity. Its owning target is
HoroWorldStreaming; public consumers link HoroEngine::WorldStreaming.

Build an immutable StreamingBudgetPolicy with complete known resource axes.
Supply exactly four complete slices for Terrain, Foliage, Navigation and Physics
to StreamingFeatureBudgetPolicy. The coordinator derives General by subtracting
those slices from global hard limits; slices never add allowances to the global cap.
Soft-target and service-window evaluation remain the existing budget-model responsibility.

Create the coordinator with the host's existing runtime composition configuration
and borrowed services. It owns that composition's canonical scheduler and one
SharedAssetResidencyLedger. Use its const child projections for diagnostics;
do not compose parallel mutable copies of those ledgers for the same work.
Borrowed feature/cache services must outlive both operation and cache retirement.

Submit a complete five-feature peak plan, positive scheduler units and the host's
current canonical cell-attempt fence to TryAdmit. Keep the returned exact reservation.
Refresh Context after each successful mutation. Failed requests change neither ledger
nor revision. Grow reserves additional complete costs before any allocation.
Worker/provider completions route to the authority; there is no new executor,
clock, backend selection, cache pointer, allocator or native-resource owner.

RealizeShared hands an exact operation's reserved portion to a cache-owned charge.
For a new allocation, peakPortion equals the complete measured residentCost.
Remaining scratch/upload peaks are retained. For an already charged cache allocation,
peakPortion may be zero or an explicitly reserved componentwise subset of residentCost;
that duplicate peak is no longer required. The original feature keeps the single
physical charge while all consumers receive independent fenced leases.
An all-zero complete peak is appropriate only for known cached-only work, never
for missing metadata. Providers must still reserve every separate new allocation.

Advance routes lifecycle commands to the existing canonical scheduler. Cancel, Fail,
Replace and Shutdown retain peaks through AcknowledgeRetirement. Release after terminal
completion removes only unrealized portions; it does not destroy or uncharge cache data.
ReleaseShared ends exact consumer retention after readers/native dependents retire.
BeginRetireShared closes new leases; AcknowledgeSharedRetired returns the realized
budget only after the cache owner confirms actual allocation retirement.

BeginShutdown closes both child admission seams and remains Draining while either
operations or cache charges survive. Service replacement is permitted only after
both have drained. Policy or partition replacement creates a new owner lifetime;
keep old authorities and borrowed services alive until their exact tokens drain.
Moved-from coordinators support destruction only. Successful commands use preallocated
bounded metadata; typed error diagnostics use the existing Foundation allocation model.

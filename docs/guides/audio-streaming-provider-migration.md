# Audio streaming provider context migration

HORO-544 / #544 / [AUD-002.8] replaces the unchecked `void*` state in
`AudioStreamDecoderProvider` and `AudioStreamPackageSource` with Foundation's
existing `BorrowedCallbackContext`. This is a deliberate C++ source-contract
change: an incorrectly paired callback and object must fail before dereference,
not reinterpret foreign storage. No compatibility callback or competing opaque
contract is retained.

Provider authors must wrap their mutable private state explicitly with
`BorrowedCallbackContext{statePointer}`. Decode, seek, release and open now take
`const BorrowedCallbackContext&` instead of `void*`. Resolve the exact registered
private type using `context.Get<State>()`. A null result returns a typed decoder
or stream error before I/O/state access; noexcept release must leave foreign
storage untouched. The checked reference allocates nothing and owns nothing.
Creation and resolution belong to the same compiled provider, not separately
rebuilt native module/extension ABI identities.

The in-tree cooked PCM adapter, decoder fixture, streaming fixture and service
construction/lease tests are migrated together. The Audio Api/Cook public-header
consumers compile the new Foundation dependency through the existing narrow
target boundaries; no public header is added or reassigned.

Ownership does not change: a rejected decoder takes no state and never releases
it; a successful decoder assumes exactly one release after serialized worker
completion. Moving a decoder transfers that responsibility. Cancellation never
releases live state. The source object/code lease outlives all fills and decoder
releases, including bounded-join failure and shutdown retry. Existing provider
errors and exception containment remain intact; no decode or source callback
ever runs on the real-time render lane.

Regression coverage includes empty-context admission, foreign-type decode/seek
rejection without state access, harmless foreign-type release, unchanged typed
errors, standard/non-standard exceptions, allocation-free successful decode,
exactly-once release, cancellation, lease retention and cooked PCM reproduction.

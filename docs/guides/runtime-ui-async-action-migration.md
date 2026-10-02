# Runtime UI asynchronous action ownership

HORO-752 adds `UiAsyncActions.h` to the existing `HoroEngine::RuntimeUi` public
header owner. No new dependency edge or backend is introduced. Existing
synchronous `UiActionHandler` callers retain their typed dispatch contract.

Typed route request/result validation and router execution now have separate
implementation files. Router and screen state each remain private to one
implementation owner. Route factory/navigation guards and asynchronous query keys
are borrowed by const reference to avoid repeatedly copying large correlation
carriers. Existing call expressions remain valid; downstream binaries and any
explicit function-pointer declarations must be rebuilt against these headers.
The screen-stack regression suite and RuntimeUi public-header consumer cover
the affected calls. Mutation methods remain non-const: const ownership handles
do not grant permission to mutate a router, operation or route lifecycle.
Private state access pairs mutable and const read-only views, including record
lookup. Cancellation observers retain const records. Dispatch independently pins
storage lifetime across callbacks; a state view does not provide ownership or
extend the lifetime of a borrowed router.

## Provider and route composition

An asynchronous provider implements `UiAsyncActionHandler::Start`. The router
reserves bounded pending state **before** invoking that scheduler. The scheduler
copies any required command values and retains the move-only
`UiAsyncActionProducer` in its own operation coordinator, outside controls. Its
execution jobs remain owned by the provider's structured `TaskGroup`; the UI
lease is neither a job executor nor permission to detach work.

Workers receive only the lease's `UiAsyncActionCancellation` observer. They
produce immutable values through the provider's existing bounded owner-thread
handoff. At `ApplyQueuedOwnerThreadCommands`, the provider invokes
`PublishProgress`, `Complete`, or `Fail` on the retained lease. These calls,
lease destruction and every store/router/control/stack mutation are owner-thread
operations. Only cancellation observation and token lifetime may cross threads.
Never capture a router, widget, tree, control or stack pointer in a worker.

The screen owner transfers its router to `UiScreenStack::AttachActions` after
successful route activation. The exact route instance then owns the router.
Borrow `Actions(route)` only for one call; route mutation invalidates that borrow.
Pop/back/clear cancel with `OwnerRetired`, replace cancels with `Superseded`, and
stack shutdown cancels with `Shutdown`. Covering a route preserves its operation.
Rejected or cancelled navigation preserves the current router and pending state.
Dispatch pins its own storage across a synchronous scheduler/handler call, so a
handler closing its route cannot release storage still in use by dispatch.

Non-route UI owners retain their router in their own instance lifetime and call
`BeginRetirement(Reload)` before publishing a reload replacement.
`Shutdown`, destruction and owner replacement are safety nets. The first
terminal transition wins; subsequent cancellation cannot rewrite successful or
failed results. The provider still accounts for and drains its jobs before its
dependencies disappear. Cancelling UI observation never falsely asserts that
worker execution or an authoritative domain commit was rolled back.

## Projection and retention

After owner-command handoff and before input/default actions in VariableUpdate,
call `UiControlStateMachine::ObserveAsyncActions` with the exact router store.
It copies the latest source-matching state; it neither polls jobs nor invokes
provider callbacks. Pending state clears transient activation and supplies
`Busy` effective availability. A terminal projection restores separately
configured availability, preserving a control disabled during the operation.
Progress, terminal payload and the original typed failure are available through
`AsyncAction`. Presentation consumers can map this copied busy state to existing
style tokens and bounded progress presentation without owning the operation.

Operation queries, cancellation and release use `UiAsyncActionKey`, which includes
the exact source/revision context, request and operation. An operation sequence
alone cannot authorize a query against another router/canvas/tree. Reload must
create a new owner/revision context, as required by the existing router contract.
Each admitted request sequence is consumed once within that router generation.

One source may have only one pending action. Operation capacity equals the
router's configured finite command capacity. Busy or capacity refusal preserves
the queue head. Terminal state remains until explicit `Release(key)`. Releasing
retention does not invalidate a completion lease or cancellation observer: those
references continue to pin their old slot, and admission reports backpressure
until all references retire. Slot reuse never resets a cancellation signal still
visible to a worker. Dropping an unfinished producer cancels once.

Progress is indeterminate or in [0, 1000] permille and monotonic within a numbered
phase; advancing the phase permits a reset. Failure publication retains a
provider-prepared immutable Foundation `Error`, preserving typed identity,
diagnostics and causes. At most eight chain nodes, sixteen diagnostics per node
and 4096 combined text bytes are admitted. Invalid failure input leaves the
operation pending and returns a typed error to the provider.

Creation preallocates every slot. Successful admission, state projection,
progress, completion and cancellation perform bounded scans/copies without
allocation, job polling, blocking or I/O. Expected rejection errors use the
existing Foundation diagnostic construction path. Providers prepare error
storage and submit jobs outside layout/extraction/render callbacks.

## Validation

`HoroRuntimeUiActionsTests` covers real router/control/route composition, progress,
typed failures, queue pressure, exactly-once terminals, token-pinned reuse,
reload and self-closing routes. It also runs cancellation through a provider-owned
Foundation task group and checks successful frame operations with the allocation
probe. `HoroRuntimeUiFeedbackPublicHeaderConsumer` compiles the additive surface
using only the owning RuntimeUi target.

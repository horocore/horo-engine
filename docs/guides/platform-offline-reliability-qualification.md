# Platform offline reliability qualification

This evidence ledger covers HORO-1873 #1917 [PLS-007.5], HORO-1877 #1921
[PLS-007.6], HORO-1879 #1923 [PLS-007.8] and HORO-1878 #1922 [PLS-007.9].
It does not certify the parent capability or private providers.

The live issue audit for the persistent all-children goal found this inventory:

| Alias | GitHub / Jira | Tracker state | Remaining evidence |
| --- | --- | --- | --- |
| PLS-007.1 | #1892 / HORO-1848 | Closed / Done | Preserve ownership decision; integrated runtime validation remains a parent obligation |
| PLS-007.2 | #1920 / HORO-1876 | Closed / To Do | Storage implementation exists; durable owner/recovery integration is not proven |
| PLS-007.3 | #1919 / HORO-1875 | Closed / To Do | Deterministic identity/in-flight ledger exists; durable replay integration is not proven |
| PLS-007.4 | #1918 / HORO-1874 | Closed / To Do | Ordering/coalescing/expiry exists; final retry/partial-expiry regressions are pending |
| PLS-007.5 | #1917 / HORO-1873 | Open / To Do | Durable scheduling, qualified replay, rate policy and account fencing |
| PLS-007.6 | #1921 / HORO-1877 | Open / To Do | Owner recovery, migration, storage/auth faults and bounded shutdown |
| PLS-007.7 | #1924 / HORO-1880 | Closed / To Do | Existing descriptors/snapshots require integrated privacy/bounds verification |
| PLS-007.8 | #1923 / HORO-1879 | Open / To Do | Real owner composition, GUI/profile integration and final validation |
| PLS-007.9 | #1922 / HORO-1878 | Open / To Do | Full cross-service/provider/profile and crash/resource-bound qualification |

The parent #1873 / HORO-1829 is open. No issue state was changed by this audit.
The tracker discrepancies are retained, not converted into acceptance evidence.

The requirements are owned by
[Platform Services Architecture](../architecture/runtime/platform-services-architecture.md#offline-and-degraded-behavior).
ADR-136 remains **Proposed**; this change does not change its status.

## 13 November recovery and validation restart

The original worktree had been removed by managed cleanup. Its 39 changed source
files were recovered from the retained Codex snapshot `f42fe53fbdfa1e66bd889981cc0581277ba984c8`
(parent `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`) and compared with the
snapshot contents before integration. The delivery branch now uses the verified
published base `3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`. The admission merge
preserves both that base's allocation-failure commit guard and the prohibition on
coalescing into an attempted aggregate.

The current user coordination policy removes the old named-grant gate. Heavy
verification still requires nonblocking acquisition of the canonical OS lock,
verification of its actual Windows handle, a single worker and CPU/memory guards.
`heavy_build_lock(repo, blocking=False)` adds bounded admission to the existing
helper; the default remains blocking for existing callers. The regression source
checks busy rejection and successful acquisition after release without a waiter.

The first configure admission attempt returned busy before starting a child. This
is not a configure, test or quality result. Native, GUI and full Sonar IDE, CLI
secrets and Codacy validation remain pending on the recovered final inputs.
Publication is a review checkpoint, not evidence that the parent acceptance or its
open children are complete. The Jira parent `HORO-1829` and `[PLS-007]` GitHub #1873
and replay child `HORO-1873` / `[PLS-007.5]` #1917 were read and verified again.

## Public contract migration

`HoroEngine::PlatformServices` continues to own the offline policy/storage headers
and now owns `PlatformDiagnostics.h` in `HoroPublicHeaderOwnership.cmake`.
`PlatformOfflineQueueConfig` adds finite retry limits; operation snapshots add the
attempt count and next eligible monotonic time. Existing source aggregate
initializers may omit these appended fields and use the finite defaults. Binary
consumers must rebuild. No proprietary SDK or native account type is added.

The storage publication signature deliberately changes from
`Publish(partition, records)` to `Publish(partition, records, expectedRevision)`.
The old lock covered only atomic replacement: two owners could read the same old
state and serially overwrite each other's durable work. Mutating callers must now
obtain `LoadSnapshot(partition)`, apply their mutation to that snapshot's records,
and retain its opaque revision through `Publish`. `Load` remains a read-only
convenience and provides no publication authority. There is no unconditional
compatibility overload.

Under the existing publication lock, storage compares the complete bounded document
digest and presence against the expected revision. Missing/foreign/wrong-partition/
expired tokens and changed documents fail without replacement. Adapter copies share
their lifetime marker, while a newly created adapter must read its own token. Tokens
hold weak provenance and never extend filesystem or adapter lifetime. The one marker
allocation occurs when creating the storage adapter, outside frame-hot work.

`platform.offline.storage_changed` is not retryable with an unchanged request:
reload, preserve unrelated intent and revalidate before attempting publication again.
After unknown atomic replacement outcome, inspect the actual document rather than
resubmit an old snapshot. Corrupt/quarantined documents do not issue fresh revisions;
an earlier valid revision cannot overwrite later corrupt evidence. The version-1
on-disk format is unchanged; revisions are process-local and never persisted/logged.
All in-repository storage publication callers are in the storage regression suite
and have been migrated; the owning staged public consumer remains mandatory.

Offline policy and storage also share one canonical descriptor for each of
`InvalidConfiguration`, `CapacityExceeded` and `IdentityConflict`. The policy
symbols become constant references to the existing storage descriptors, preserving
their domain/code identities while removing conflicting metadata for the same
registered code. This is a binary contract change; consumers must rebuild. Source
callers continue to pass descriptors to `MakeError` without changes.
`PlatformOfflineErrorDomain()` contributes all 17 distinct offline codes as inert
metadata. Host composition includes that contribution explicitly, and its GUI
module metadata now declares the same Platform Services dependency as CMake.
Descriptor construction performs no service registration or lifecycle action.

The queue remains an **in-memory policy core**. `RetryUncommitted` requires semantic
owner evidence that the attempt had no remote effect. It cannot resolve ambiguous
commit, create provider requests, reauthorize an account or claim durable acceptance.
Persistent owners must commit transitions before provider submit or durable outcome
publication. Attempts and deadlines must be persisted by that owner; copying the
diagnostic snapshot is not a persistence protocol.

Once dispatch begins, later admission cannot coalesce into that aggregate. Partial
receipt expiry does not rewrite its attempted payload. If the remaining live receipts
would require a different reduction, redispatch fails closed while independent lanes
remain usable and each receipt reaches its original expiry. Expiry events remain
explicit; no attempt count or original expiry resets on suspend/resume or duplicate
admission. Ordinary expiry/compaction never deletes `Reconciling` work.
Jitter has millisecond resolution even for the first one-second retry delay;
provider delay hints are clamped before conversion so oversized hints cannot overflow.

`PlatformOfflineQueueStorage::Quarantine` writes a durable fence beside the original
partition document. It preserves the original bytes, rejects subsequent `Load` and
`Publish` for that partition, and does not fence another partition. Startup must not
interpret a fenced or corrupt document as an empty queue. This is whole-partition
containment: the existing format has one document checksum and cannot authenticate
individual lanes after document corruption. It is not a row-salvage or migration API.
Explicit recovery of a fence and schema migration remain separate owner work.

Storage error normalization preserves the typed code/domain/severity but excludes
protected partition paths and unrestricted native message, diagnostic and cause
contents. The owner retains its explicit root for authorized recovery; broad errors
must not become a private-locator export channel. Corrupt-load and injected lock,
capacity-query and write failures have regression sources for this boundary. Their
final execution is pending.

The existing progression and queue public headers contained the same `detail`
function-template definition. The queue-local helper now has its own name so both
contracts can be included together. `PlatformOfflinePublicContract.cpp` exercises
the combined public surface using only the owning target's usage requirements.

## Diagnostics composition

`PlatformDiagnostics` borrows the host composition and read-only policy queue on
their owner lane. It projects aggregate receipt states and finite capacities. It
does not copy account, subject, intent, definition, presence content, credentials or
private binding locators into diagnostics. Metric history remains Observability-owned.

An optional host command must checkpoint suspension before returning success. The
adapter rejects shipping/headless/server/unsupported products, explicit Null, closed
composition/queue, wrong lanes and stale provider/queue generations before invoking
that command. It offers no force-replay, retry-delay override, queue deletion, account
selection or consent override.

The localized Platform dock pane borrows `PanelContext::platformDiagnostics` and
releases it on detach. The field is appended and defaults to null, preserving existing
source aggregate callers. The `HoroGui` implementation declares a private dependency
on Platform Services; the public panel context uses a forward declaration. Hosts with
no composed source show unavailable diagnostics. **No live durable queue capability
is composed into the editor by this change.** The panel does not manufacture one.

## Regression evidence inventory

| Area | Source coverage | Execution status |
| --- | --- | --- |
| Retry count, positive delay, capped provider hints, jitter, clock rollback, cancellation/suspension and ambiguity | `PlatformOfflineQueueLifecycleTests.cpp` | Final execution pending |
| Attempted-envelope immutability under later admission and partial expiry | `PlatformOfflineQueueLifecycleTests.cpp` | Final execution pending |
| Quarantine persistence across adapter recreation, corrupt-byte preservation, independent partition and invalid input | `PlatformOfflineQueueStorageTests.cpp` | Final execution pending |
| Stale-owner lost update, fresh merged retry, copied-adapter provenance, token partition/adapter/lifetime scope, absence/empty/deletion distinction, corrupt-document preservation | `PlatformOfflineQueueStorageTests.cpp` | Final execution pending |
| Canonical offline descriptors, complete registry contribution and host resolution | `PlatformServiceErrorsTests.cpp` and `HostModuleCompositionTests.cpp` | Final execution pending |
| Exported metric names, service/outcome-only dimension allowlists, finite series budgets and runtime restart rebinding | `PlatformServicesMetricsTests.cpp` | Final execution pending |
| Null and product-profile developer denial, stale generation, wrong lane, closed host | `PlatformServicesCompositionTests.cpp` | Interim composition executable passed; final edits require rerun |
| Combined queue/idempotency/storage/diagnostics public headers | `PlatformOfflinePublicContract.cpp` and generated header consumers | Final combined consumer pending |
| Detached/read-only/enabled pane at narrow and wide widths | `GlobalDockPanelRenderTests.cpp` | GUI build/render pending |
| English/Turkish keys | `LocalizationServiceTests.cpp` | 39 new keys inspected for aligned nonempty translations; executable pending |

The lightweight JSON comparison parsed both catalogs and verified that all 39
added keys match, contain text and have no duplicates. Both catalogs already
contain a duplicate `workspace.global_dock.network.column.connection` key in
`HEAD`; its occurrence count is unchanged. This comparison does not replace the
localization executable or visual verification. `git diff --check` also passed.

Targeted CTest selection must match discovered cases such as
`^HoroPlatformRequestTests::`, not only the executable name. An earlier exact-name
filter ran the composition executable alone; it provides no queue-test result.

All heavy validation must use the repository-wide lock and a single build/test
worker. A historical sandbox redirected the same logical lock into a different
physical cache root. Subsequent validation verifies the live open-file lock handle
against the coordinator's physical reference before starting a child process; a
logical Git/cache identity alone is insufficient.

## Outstanding production acceptance

All four issues remain incomplete until their full acceptance evidence exists:

- **PLS-007.5:** transactional durable admission/attempt/terminal/checkpoint owner,
  exact progression-envelope/provider integration, finite fair priority scheduler,
  provider-wide throttle coordination, and same-binding reauthorization. The
  identity owner is preparing a binding-evidence seam; there is no substitute based
  on the current account or live handle bytes.
- **PLS-007.6:** canonical recovery into that owner, invalid-record/lane isolation
  where framing is trustworthy, schema migration, secure-store failure, publication
  ambiguity reconciliation, bounded flush/shutdown and callback/credential lease
  retirement. Whole-partition quarantine alone does not prove these obligations.
- **PLS-007.8:** editor composition of the real owner-backed source/control, final
  GUI/public-consumer build, normal/hover/active/disabled/focus checks, narrow/long
  localized text checks and product-profile integration. The current projection
  supplies receipt counts, capacities, product/selection and service availability;
  owner-backed bytes by service/state, oldest bounded age, next eligible delay,
  normalized last failure, pending requests and Observability-owned rate/latency
  evidence are not yet projected. The optional presentation seam alone is insufficient.
- **PLS-007.9:** crash injection before/after durable admission, provider commit,
  terminal commit and compaction; cross-service/provider/profile and concurrent Save
  cloud-journal qualification; measured memory/work/storage/shutdown bounds; current
  dependent work and available native adapters. Private SDK/device/account and
  Linux/macOS evidence are unavailable in this worktree and are not certified here.

Final Sonar IDE, CLI secrets and Codacy preflight must run after final source edits
against the actual delivery base. No local/cloud quality result is claimed yet.

## Dependency audit

The initial base is `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`.
The delivery worktree was rebased onto the unpublished local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`, which adds the provider-admission
fixture's C11/MSVC atomics configuration. Preservation checks found 29 task files
byte-identical; only `tests/CMakeLists.txt` changed through the parent, and its
task-specific additions/deletions were unchanged. No identity/provider implementation
or uncommitted parent work was imported by that rebase. Reconfigure and final
validation are still pending; the parent's reported successful build/admission test
does not validate this worktree's offline changes.

`git merge-base --is-ancestor` verified `57a8ad03`, `7ce22fbe`, `30ef4646`,
`04c1b477`, `c68c5acd`, `591b428b`, `4794a994`, `b1d43764` and `aeaab851`
against that base. These changes are not reapplied; ancestry is not a substitute
for the production evidence listed above.

The original progression audit found only the void
`IProgressionProjectionGate::Validate` and no typed durable qualification handoff.
The owner's subsequent board record and source now show an **uncommitted, unexecuted**
`PlatformProgressionReplay.h` draft: `PlatformProgressionReplayRequirements`, immutable
`PlatformProgressionQualifiedReplayIntent`, and `IPlatformProgressionReplayQualifier`
with `Qualify`/`Revalidate`. The tuple preserves provider/generation/policy and actual
provider-qualification, effective-semantics, project-policy, stable-ID registry,
definition-registry and mapping-generation fingerprints. No default real provider
qualifier is supplied. The application preparation path does not admit volatile or
durable work and does not authorize a fallback.

This source draft changes the next integration step, not production acceptance.
Wait for its verified commit and the actual provider owner's effective semantics;
persist the complete original envelope, then requalify that exact decoded envelope
through the queue owner's trusted qualifier on recovery and before dispatch. Generic
service availability or a C++ handoff value cannot supply the missing evidence.
Durable admission remains unavailable until those contracts and their actual
integration are verified; no volatile fallback is permitted.

The identity owner published source commit `b7a4587c7` containing
`PlatformSessionBindingIssuer`, immutable binding evidence and six regressions.
Source inspection confirms that `ValidatePlatformSessionBindingAccess` checks current
provider availability, exact subject/session/access, expected binding/provider and
issuer lifetime. Same-binding reauthentication needs fresh broker evidence. The
protected partition's canonical data remains with its owner; a new broker lifetime
must re-resolve its expected binding from the trusted store, not deserialize live
evidence or derive it from the current account.

The identity owner's evidence commit `9b01ed358` records Windows MSVC 19.44 Debug
compilation and 83 focused request/identity/session/frontend/friends/presence cases,
including all six binding cases, with zero failures. Its generated harness reports
three passing entries for the focused request group, composition and public friends
consumer; both staged header consumers built. A frontend cancellation provenance
guard-bypass RED failed as expected and the restored GREEN passed. These are reported
sibling-worktree results, not an execution of the offline integration in this worktree.

Native lifecycle baseline execution stopped before testing because the C fixture's
compile command omitted C11 (`C1189`). Native authentication/provider qualification,
the full request suite and final quality checks remain incomplete. No push or shared
base update is implied by the evidence commit.

The dependency is not integrated or qualified for offline replay in this worktree.
Queue consumers must receive validated evidence rather than the issuer capability,
and must not reproduce or infer identity authority. Integrate the dependency through
the coordinated base update, then test sign-out, account switch, issuer revocation
and same-binding restart with the actual queue owner.

After the identity branch's own rebase onto `5d0aff9d6`, the owner reported these
replacement identities: binding `09067bc34`, friends `761173593`, lifecycle
`70bba8aa2`, offline recovery `5e7babc79`, evidence `61759df2e`, and documentation
HEAD `f4bc46fe2`. Its seven-patch range comparison was reported unchanged; earlier
83-case results remain evidence from the previous base. Rebased native execution,
reconfiguration and quality are not yet reported. These local identities have not
been fetched/pushed or integrated into this worktree.

# Cloud Provider Contract Qualification

This record tracks PLS-005 as a capability group. Public-engine qualification and
private-provider certification are separate evidence. Neither scripted results nor
the synchronous atomic test model establish a native provider's durable CAS or
mutation-ID deduplication guarantee.

## Delivery and acceptance map

| Work item | Implementation and regression evidence | Remaining qualification |
| --- | --- | --- |
| PLS-005.1, #1890, HORO-1846 | ADR-134 and the normative platform-services architecture | Existing merged architecture; no reimplementation |
| PLS-005.2, #1908, HORO-1864 | `PlatformCloudObjects`, frontend list/read admission, ordered bounded page and complete-read validators | Existing merged implementation; Jira/GitHub state differs |
| PLS-005.3, #1905, HORO-1861 | Conditional write/delete, exact intent replay validation, quota and atomic provider model tests | Existing merged implementation; Jira remains In Review |
| PLS-005.4, #1907, HORO-1863 | Bounded operation-owned staging, explicit partial/absence/revision/cleanup errors, preserved frontend cloud errors, cursor-loop rejection; transfer tests in `PlatformCloudObjectsTests` | Native callback/module leases, staging overhead and atomic SDK evidence remain provider-package obligations |
| PLS-005.5, #1911, HORO-1867 | Explicit fail-closed Null cloud entry points; scripted list/read/write/delete/quota; current-session validation, finite transfer slots and redacted typed failures | Test execution and external production providers must be recorded separately |
| PLS-005.6, #1912, HORO-1868 | `PlatformCloudProviderConformanceTests` exercises both available public providers through `ICloudService`; existing atomic-model tests cover revision races and exact replay | Blocked on #1887/HORO-1843 and #1916/HORO-1872 plus actual private-provider/cross-platform evidence |

The delivery branch starts at `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`.
Open-ticket identities were checked against GitHub and Jira on 11 November 2026.
No Jira status or GitHub issue state is changed by this delivery.

## Ownership and migration

`CloudBlobStagingBuffer` is an additive helper in the existing
`Horo/PlatformServices/PlatformCloudObjects.h` header. Its owner remains
`HoroPlatformServices` in `cmake/HoroPublicHeaderOwnership.cmake`; no public target
dependency or include root changes. `CloudProviderPublicContract.cpp` extends the
owning public-header consumer with direct Null overload and move-only staging checks.

Adapters admit an exact length with `Begin`, copy borrowed chunks with `Append`,
and obtain complete bytes once through `Finish`. An oversized chunk permanently
closes the buffer; a short transfer returns `PartialTransfer`; a bad digest returns
`IntegrityMismatch`. Moving closes the old buffer. No partial span, local path or
publication authority is exposed. Adapters still validate captured subject, session,
key and revision through `ValidateCloudBlobReadCompletion` before request success.
The adapter owns the buffer and provider module lease until native callbacks retire.

Frontend list/read admission now preserves the original typed cloud validation error
instead of flattening it to `FrontendErrors::InvalidRequest`. Callers previously
matching that generic error must match the cloud code, including `PayloadTooLarge`.
Available read-only cloud capability snapshots must also declare page/object bounds
within the cloud hard ceilings, even when mutation support is absent. Invalid bounds
fail composition rather than leaving an apparently available but unusable service.
Short complete-read evidence now returns `PartialTransfer` rather than `InvalidPage`.
A page repeating the submitted cursor is invalid; longer cursor cycles require the
bounded paging owner to track its own continuation history. Null's new overloads
return the same `platform.provider.null` as its existing reads and mutations.
Empty brace calls to the concrete overloaded read API must specify `CloudReadRequest`
or `CloudBlobReadRequest`, as demonstrated by the public consumer and unit tests.

The test mock additionally requires an explicitly installed `SetCloudSubject` for
opaque operations. A replacement must advance the session generation; sign-out
cannot resurrect an old generation. Completion validates the current subject even
for a scripted failure. No raw account identifier is used. Cloud failure scripts
accept declared cloud descriptors or a normalized provider category; arbitrary
provider message text is discarded before retention.

The mock advertises and enforces 8 concurrent cloud transfers and 64 KiB complete
payloads, within production hard ceilings. Request/response storage, schedules,
inspection histories and diagnostics retain their existing finite capacities.
Timeout/cancellation does not release a transfer slot while simulated native or
duplicate callbacks remain. One response payload is shared by its scheduled events.
The mock schedules no automatic retries; callers may script exact-intent retries
inside their own policy. It has no durable storage and cannot qualify real remote
deduplication. The synchronous atomic model remains explicitly a test model.

No cloud mutation enters `PlatformOfflineQueue`. `CloudSaveCoordinator` under
ADR-115 remains the sole durable intent owner. Timeout, cancellation, cleanup-pending
or late provider evidence never acknowledges an unknown remote outcome as success.

## Execution evidence

Windows MSVC Debug configure and the `HoroPlatformRequestTests` build completed on
11 November 2026 under `scripts.dev.heavy_build_lock`, keyed by the shared Git common
directory. The pre-build 30-second CPU sample was 6.6%, available memory 40.34%.
Build parallelism was 1 and `CTEST_PARALLEL_LEVEL=1`.

The initial full target run reported 194/196 passed, with two failures. The mock
oversize test still expected the old generic diagnostic; its fixture is migrated to
assert typed `PayloadTooLarge`, zero provider calls and zero request records.
The unchanged presence-allocation test entered a Microsoft CRT abort dialog during
an injected allocation failure. A captured native stack locates a debug-iterator
proxy allocation in `std::string`'s noexcept move, while constructing the by-value
`PlatformOfflineIntent` argument before `PlatformOfflineQueue::Admit` is entered.
The owning hung test was stopped after stack capture; it is a failed verification,
not a passed or skipped test. No allocation guard or test has been disabled.
This run predates the final staging/error edits and does not validate those inputs.

Subsequent CTest runs use `--timeout 300 -j1 --output-on-failure` under the same
shared lock. Heavy reruns wait for the main Windows validation block to finish.
Final build, public-consumer, cloud-suite and full-target results are pending.
The analysis helper now honors `CMAKE_BUILD_PARALLEL_LEVEL` when preparing compiler
contexts, retaining its previous default when unset. Qualification sets it to `1`;
both Ninja object and Make target command paths have lightweight regression coverage.

The initial `scripts/analyze.py doctor --offline` found Sonar CLI 1.10.0,
Codacy Analysis CLI 0.25.0 and VS Code 1.145.3; CMake/Ninja were absent from that
uninitialized shell PATH. This is incomplete prerequisite evidence. The final
worktree-aware Sonar IDE, CLI secrets and Codacy check must use the initialized
native toolchain and exact-worktree compile database; no clean quality claim is made.
An initialized-shell doctor subsequently found CMake 4.3.1 and Ninja 1.13.2;
Codacy cloud-drift comparison remained incomplete because its disposable mirror
could not be indexed. A later read-only Git dry-run could enumerate that mirror;
the transient indexing failure is not a clean drift check.

## External evidence and blockers

The private SDK packages, provider accounts, native atomic commit/dedupe evidence,
Linux/macOS execution and platform certification have not been supplied or run here.
They are unavailable evidence, never a successful unsupported test of a real SDK.

#1887/HORO-1843 owns provider package lifecycle qualification and still depends on
#1826/HORO-1782 (CFG-001.6). #1916/HORO-1872 owns real sign-in/sign-out/account-switch
invalidation. Both external work items remain To Do in the checked Jira state.
This work does not copy their credential-provider or lifecycle implementation.
Local mock fences are regression fixtures, not proof of those integrated lifecycles.
PLS-005.6 and the parent must remain unqualified until that evidence is available.

The PLS-006 owner reports an in-progress cancellation-provenance fix on
`feat/HORO-1872_platform_identity_lifecycle`, based on `f51e0c0`. It will bind
frontend-issued handles to a weak issuance authority so equal numeric request IDs
from different backend stores cannot cancel each other's requests. Handles returned
by a frontend must be cancelled through that same frontend; direct backend/store
handles remain with their issuing owner. The weak token is not a provider/request
lifetime lease, and the request-store/native C ABI is unchanged in the reported plan.
This worktree's scripted cancellation fixture already calls its issuing backend.
Frontend cloud mutation coverage also retains and moves each returned handle, then
cancels through the issuing frontend. That compatibility regression is pending native
execution. The external fix has no reported test/quality/push evidence and is not
present in this worktree's base; it does not discharge #1916's qualification blocker.

ADR-134's header currently says **Proposed**, while the normative platform-services
architecture references it as the ratified baseline. This delivery follows the
normative contract and records the inconsistency without changing ADR status.

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

The delivery branch originally started at `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`
and was rebased onto local parent `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`.
All nine delivery commits retained identical patches in `git range-diff`. The parent
adds only the C11 admission-fixture configuration fix; the main checkout's other
uncommitted fixes, including the CRT allocation-fault correction, are not included.
The parent is not yet published to origin; no fetch or push was used for this rebase.
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

The parent acceptance audit also identified missing Save/transport compatibility
fixtures. `SaveCloudRevisionMetadataTests.cpp` now exercises actual public Save
metadata and transport request-store types together in
`HoroRuntimeSaveCloudRevisionMetadataTests`. It explicitly copies a coordinator-owned
mutation identity and exact opaque revision into a transport intent. Successful,
cancelled, timed-out and quota-failed requests cannot rewrite the paired local
generation/archive/confirmed-mutation metadata; late evidence cannot overwrite a
terminal result. The fixture also rejects Save metadata keys/revisions that fit
Save's wider metadata bound but exceed transport bounds, preserving the source
evidence rather than truncating it. Cloud mutation types remain structurally
unrepresentable in `PlatformOfflineOperation`.

Only this test target gains a private link to Platform Services and its narrow test
support include directory. Production Runtime/Platform Services dependencies and
header ownership are unchanged. This is an executable compatibility fixture, not a
durable coordinator implementation or private-provider qualification. Its native
execution remains pending the shared heavy-work slot.

The cloud conformance suite also exercises an older adapter that implements only
the legacy object-ID read method. All five new opaque operations return the exact
`UnsupportedCapability` error without invoking that older route. This compatibility
fixture proves the default unsupported boundary without claiming an actual private
SDK adapter is installed or qualified; its native execution is still pending.

## Execution evidence

Windows MSVC Debug configure and the `HoroPlatformRequestTests` build completed on
11 November 2026. The helper invoked `scripts.dev.heavy_build_lock` and reported the
shared Git-common-directory cache identity, but did not inspect its actual lock
handle. That initial run is not evidence of physical canonical-lock exclusivity:
later coordination found that sandbox virtualization can redirect an apparently
identical cache path. The pre-build 30-second CPU sample was 6.6%, available memory
40.34%. Build parallelism was 1 and `CTEST_PARALLEL_LEVEL=1`.

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

The local verification wrappers now require `psutil.Process().open_files()` to show
the exact physical canonical `heavy-build.lock` handle after acquisition and before
any heavy operation. A redirected/private path or inspection error stops the job;
there is no fallback. This guard is prepared but has not yet been exercised by a
new heavy run. Subsequent CTest runs use `--timeout 300 -j1 --output-on-failure`.
Heavy reruns wait for the main Windows validation block or an explicit bounded slot.
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
The later full-capability audit found an existing portable VS Code source profile.
Running `scripts/analyze.py doctor --offline --code-user-data <existing-profile>`
with the initialized tool PATH reported `ready_for_local_check` in report
`1794391219402139100-3d726185.json`. No profile/security setting was changed.
This prerequisite result does not verify connected-mode authentication, Workspace
Trust, C++ sensor execution, secrets, Codacy findings or cloud configuration drift.

The renewed acceptance audit confirmed all six native GitHub children and the Jira
identities in the delivery table. Architecture/metadata/mutation children are merged
but the metadata and mutation Jira states still differ from GitHub. Transport,
scripted providers and qualification remain open. A persistent active goal now
tracks every remaining child acceptance item, including the two newly identified
compatibility fixtures and final native/quality execution. Clean local commits do
not establish group completion. No heavy helper or background queue was started
during this audit; the main validation owner must grant the next bounded slot.

## External evidence and blockers

The source audit also confirms an application/profile integration gap:
`save-game-and-persistence.md` explicitly records no live `CloudSaveCoordinator`
transfer/retry implementation in this tree. The existing revision sidecar and the
new Save/transport compatibility fixtures cannot prove a production journal,
reconnect reconciliation or durable local publication after a remote outcome.
Those remain with the SAV-006 coordinator owner; this transport delivery must not
create a replacement durable queue or claim end-to-end synchronization success.

The private SDK packages, provider accounts, native atomic commit/dedupe evidence,
Linux/macOS execution and platform certification have not been supplied or run here.
They are unavailable evidence, never a successful unsupported test of a real SDK.

#1887/HORO-1843 owns provider package lifecycle qualification and still depends on
#1826/HORO-1782 (CFG-001.6). #1916/HORO-1872 owns real sign-in/sign-out/account-switch
invalidation. Both external work items remain To Do in the checked Jira state.
This work does not copy their credential-provider or lifecycle implementation.
Local mock fences are regression fixtures, not proof of those integrated lifecycles.
PLS-005.6 and the parent must remain unqualified until that evidence is available.

The PLS-006 owner reports a cancellation-provenance fix on
`feat/HORO-1872_platform_identity_lifecycle`, based on `f51e0c0`. It binds
frontend-issued handles to a weak issuance authority so equal numeric request IDs
from different backend stores cannot cancel each other's requests. Handles returned
by a frontend must be cancelled through that same frontend; direct backend/store
handles remain with their issuing owner. The weak token is not a provider/request
lifetime lease, and the request-store/native C ABI is unchanged in the reported plan.
This worktree's scripted cancellation fixture already calls its issuing backend.
Frontend cloud mutation coverage also retains and moves each returned handle, then
cancels through the issuing frontend. That compatibility regression is pending native
execution. External focused test evidence is recorded below; final quality/push
evidence is pending and the fix is not present in this worktree's base. It does not
discharge #1916's full qualification blocker.

The same owner also reports a candidate `PlatformSessionBinding.h` seam with
`ProviderAccountBindingId`, `PlatformSessionBindingEvidence` and a move-only
host/private-broker-owned `PlatformSessionBindingIssuer`. That header is absent
from this worktree. Its proposed access-validation route receives current provider
capabilities, the published session, captured evidence, an explicit expected binding
and the service. Cloud integration must obtain that expected binding from a trusted
host association; a subject handle, cloud object key or ordinary consumer cannot
manufacture the association. The issuer remains outside ordinary consumers, and its
close/destruction revokes weak captured evidence without extending provider lifetime.

This proposed seam is neither a mutable session owner nor credential storage or a
cloud journal. The coordinator retains its existing protected durable partition.
Authenticated stable-binding resolution remains a private-broker precondition and
provider qualification gate. No seam implementation is copied or consumed here.
The owner reports `4e718b2` as a frontend const-view compile correction only. The
candidate seam is recorded in local commit `b7a4587c7`; both its public header
and implementation are present in that Git object. That commit is not integrated
here. After issuer restart,
the trusted mapping owner must resolve the expected binding again for the existing
durable partition. A local source SHA does not discharge the qualification gate.

The external owner subsequently recorded bounded Windows evidence in
`9b01ed358`, covering binding source `b7a4587c7` and friends-recovery source
`f67fb91f5`. Its evidence document reports successful MSVC 19.44 Debug builds,
83 focused identity/session/frontend/friends/presence/request/completion-queue
test cases with zero failures, three focused CTest entries passed, and successful
staged-header consumer builds. The selected group includes six binding cases and
foreign-issuer, reauthentication and weak-lifetime checks. A frontend guard-bypass
RED reproduced the numeric-ID collision, and restoring the fixed source passed
GREEN. These are external-worktree results, not execution of this cloud branch or
the complete request suite.

Native lifecycle RED/GREEN did not reach test execution: the C fixture stopped
with MSVC C1189 because its target did not select C11. Native authentication and
qualification, the full request suite and final Sonar IDE/secrets/Codacy evidence
remain open. The owner reports exact temporary-source restoration and canonical
lock release; its bounded slot was returned to the main validation work. There
is no reported push or provider certification, and no source is imported here.
The later main-parent C11 fix `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08` is now
the base of this cloud branch. The main owner reports its Full11 Windows ALL build
and provider-admission test passed. This removes the known C11 source prerequisite;
it does not rerun or qualify the external identity lifecycle or this cloud branch.
The identity owner has also rebased onto that parent: binding `b7a4587c7` maps to
`09067bc34`, friends recovery `f67fb91f5` to `5e7babc79`, and evidence `9b01ed358`
to `61759df2e`, with reported branch head `f4bc46fe2`. Its seven-patch range diff
was unchanged. The 83-case success remains evidence from the earlier base; no
rebased native/reconfigure/quality execution or push is reported. These references
track external evidence only and do not import that branch into cloud delivery.

ADR-134's header currently says **Proposed**, while the normative platform-services
architecture references it as the ratified baseline. This delivery follows the
normative contract and records the inconsistency without changing ADR status.

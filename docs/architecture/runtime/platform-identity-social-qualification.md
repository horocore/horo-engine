# Platform identity and social qualification

This record separates Horo contract evidence from native provider qualification.
It does not change ADR-135's Proposed status or close an issue based on source
inspection alone.

## Delivery scope

| Work | Identity | Evidence owner |
| --- | --- | --- |
| Existing architecture decision | HORO-1847 / #1891 / PLS-006.1 | Normative architecture and ADR-135 |
| Existing opaque user/session model | HORO-1865 / #1909 / PLS-006.2 | `PlatformUserSession` |
| Existing engine-thread session notifications | HORO-1866 / #1910 / PLS-006.3 | `PlatformSessionObserver` |
| Sign-in/out and account-switch invalidation | HORO-1872 / #1916 / PLS-006.4 | Platform Services lifecycle and composition |
| Existing presence mapping/coalescing | HORO-1871 / #1915 / PLS-006.5 | `PlatformPresenceCoordinator` |
| Read-only friends, consent and restrictions | HORO-1869 / #1913 / PLS-006.6 | Friends publication/cache coordinator |
| Integrated qualification | HORO-1870 / #1914 / PLS-006.7 | Public contracts plus private provider evidence |

The already merged PLS-006.1, .2, .3 and .5 implementations remain the architecture,
session model, observer and presence sources of truth. They are not reimplemented
by this delivery. Public test fixtures use synthetic Horo-only values, not real
accounts or native identifiers.

## Acceptance audit and remaining work

The 2026-11-11 audit read the parent and all seven GitHub issues, their current
Jira work items and local Git ancestry. An issue's closed state, an old test pass
or a clean worktree does not establish integrated completion. The parent
HORO-1828 / #1872 remains open; its old-session commit, denial-vs-empty-success
and privacy criteria require the combined evidence below.

| Child | Current tracker evidence | Implemented evidence | Remaining acceptance evidence |
| --- | --- | --- | --- |
| PLS-006.1 / HORO-1847 / #1891 | GitHub closed; Jira Done | Identity/privacy/consent architecture commit `0a8f4a203` is an ancestor; ADR-135 still says Proposed | Preserve its identity boundaries in the integrated lifecycle/provider tests; do not silently change ADR status or reimplement the decision |
| PLS-006.2 / HORO-1865 / #1909 | GitHub closed; Jira In Review | Merged #2706 (`b48b7f578`), opaque handles, immutable snapshots, stale access and transactional replacement tests | Rerun current-tree consumers and regression scope; provider-native privacy evidence belongs to qualification |
| PLS-006.3 / HORO-1866 / #1910 | GitHub closed; Jira To Do | Merged #3015 (`b1d437646`), deferred engine-thread dispatch, bounded revisions, reentry and subscription lifetime tests | Current-tree integrated dispatch/shutdown validation; tracker reconciliation is not performed by this delivery |
| PLS-006.4 / HORO-1872 / #1916 | GitHub open; Jira To Do | Local stale-session terminal fence, once-only native cancellation, frontend provenance and protected binding handoff; frontend RED/GREEN and binding cases passed on the prior base | Native RED/GREEN after the C11 parent fix; verified integration of provider Close fixes; full relevant regressions and final quality check |
| PLS-006.5 / HORO-1871 / #1915 | GitHub closed; Jira To Do | Merged #3016 (`62217cafa`), registry-only mapping, bounded coalescing/rate gate, stale completion and sign-out invalidation tests | Current-tree integrated regression and private native Set/Clear/restriction conformance; preserve existing implementation |
| PLS-006.6 / HORO-1869 / #1913 | GitHub open; Jira To Do | Local bounded approved pages, distinct social handles, all denied access states, malformed-data rejection, cache invalidation, cancellation and offline recovery coverage | Rebased-tree tests/consumers and final quality check; real provider consent/child/region evidence remains external |
| PLS-006.7 / HORO-1870 / #1914 | GitHub open; Jira To Do | Public matrix, synthetic rapid-switch/offline/delayed-result tests and explicit execution records below | Converged provider/configuration dependencies, current-tree native/full regression, separate Sonar IDE/secrets/Codacy evidence and actual supported-platform/native qualification |

All four cited architecture/model/observer/presence commits were independently
checked with `git merge-base --is-ancestor <commit> HEAD`; each returned zero.
Existing implementation is retained rather than recreated to reconcile trackers.
Jira's recorded dependency edges remain distinct from code availability: for
example PLS-006.7 still records the model, observer, lifecycle, presence, friends
and provider qualification as prerequisites. No Jira transition, GitHub checkbox
or issue relationship was changed during this audit.

The audit checkpoint is local branch
`feat/HORO-1872_platform_identity_lifecycle` at `f4bc46fe2`, with eight local commits
above parent `5d0aff9d6` and zero dirty files before this audit update. A remote
head query returned no branch and GitHub's exact-head PR search returned no PR.
Local checkpoints therefore are not a pushed or reviewed delivery. Subsequent
audit-only updates do not replace executable validation.

The remaining work is tracked by an active persistent PLS-006 goal covering all
seven children, not just the next checkpoint. The next requested bounded heavy
slot is native baseline RED with CTest timeout 300, exact source restoration,
fixed native admission/lifecycle build and test, then current-tree focused
identity/composition/public-consumer verification. The full request suite also
needs the separately owned Windows allocation-failure prerequisite; the earlier
selected subset is not a replacement for that suite. A separate final
`scripts/analyze.py check --base codex/editor-improvements` scope must run after
final edits with current compilation context and separate IDE/secrets/Codacy
results. No heavy job is queued while the main CI owner holds priority.

## Public evidence matrix

| Invariant | Executable coverage |
| --- | --- |
| Failed detached session candidates preserve the previous immutable snapshot; legal transitions advance generation/access revision | `PlatformUserSessionTests.cpp` in `HoroPlatformRequestTests` |
| Deferred owner-thread observers, stale revision filtering, reentry and subscription/close lifetime | `PlatformSessionObserverTests.cpp` in `HoroPlatformRequestTests` |
| Old native work terminates on session invalidation without waiting for a callback; native cancellation is issued once even on rejection; delayed old completion cannot change new work | `PlatformProviderSessionLifecycleTests.cpp` in `HoroPlatformProviderAdmissionTests` |
| Native partial-start rollback and BUSY shutdown retain callback/code ownership | `PlatformProviderLifecycleTests.cpp` in `HoroPlatformProviderAdmissionTests` |
| Retained old success stays immutable but cannot authorize late or already queued observer delivery after replacement; each callback is fenced independently; moved native handles retain provenance and foreign store handles fail stale | `Native session observers*` cases in `PlatformProviderSessionLifecycleTests.cpp` (new source, execution pending) |
| Composition replacement closes retained old frontends before new factory activation; failed replacement cannot reopen old admission | `HoroPlatformServicesCompositionTests` |
| A retired frontend's handle cannot cancel a new account's request even when numeric ID/generation collide; frontend moves preserve valid cancellation | `PlatformServicesFrontendTests.cpp` in `HoroPlatformRequestTests` |
| Same protected binding requires fresh evidence after reauthentication; another account/environment cannot retarget an old partition and restrictions/capability absence remain failures | `PlatformSessionBindingTests.cpp` in `HoroPlatformRequestTests` |
| A foreign issuer cannot forge association from an expected binding; moving preserves provenance and issuer close/destruction invalidates weak evidence | `PlatformSessionBindingTests.cpp` in `HoroPlatformRequestTests` |
| Presence registry validation, coalescing, rate limits and session/access token invalidation | `PlatformPresenceCoordinatorTests.cpp` in `HoroPlatformRequestTests` |
| Friends capability absence, explicit Null and all denied access states are distinct failures | `PlatformFriendsCoordinatorTests.cpp` and `PlatformServicesFrontendTests.cpp` in `HoroPlatformRequestTests` |
| Friends pages reject mismatched offset/scope, oversized/malformed UTF-8, NUL, duplicate handles and invalid continuation before any partial publication | `PlatformFriendsCoordinatorTests.cpp` in `HoroPlatformRequestTests` |
| Friends cache expiry, invalid session updates, consent/restriction replacement, cancellation, failure, timeout and shutdown preserve publication authority | `PlatformFriendsCoordinatorTests.cpp` in `HoroPlatformRequestTests` |
| Scripted delayed friends results cannot cross access/session publication fences | `[qualification]` case in `PlatformFriendsCoordinatorTests.cpp` |
| An offline refresh preserves its typed provider cause, clears retained presentation and recovers only under the current account after rapid switches | `[qualification]` case in `PlatformFriendsCoordinatorTests.cpp` |
| Social handles cannot become local subject capabilities or ordinary stream/log values; staged public header remains consumable | `HoroPlatformFriendsPublicConsumer` |

## Execution status

### 2026-11-13 resumed delivery

The ten delivery patches were rebased without conflicts onto published
`origin/codex/editor-improvements` at `3df476ff4`; range-diff marked each equivalent.
This base contains both C11 admission-fixture support and the Windows offline-queue
allocation-ownership fix. Current regression execution remains required.

The user authorized normal branch push and truthful draft review while acceptance
remains open. The current protocol permits one atomic nonblocking acquisition of
the canonical physical build lock with actual-handle, CPU/RAM and one-worker guards;
old named-grant HOLD text is historical. The first observer RED attempt did not
launch an engine child: CPU30 was 34.44%, but free RAM was 10.33%, below 25%.
The local helper now preserves the canonical cleanup guard without a probe/release/
blocking-reacquire race. Subsequent full validation attempts and provider outcomes
must be reported separately; resource deferral is not an analyzer or native pass.

Windows CI previously omitted Platform Services qualification from its focused
closure. Six request/native/composition/social/header targets and the direct CTest
labels are now registered. The source-level grouping regression suite passed
41/41; native configure/build/runtime validation is still pending.

Fresh doctor found installed tools and a usable local IDE profile, but Codacy cloud
configuration comparison failed authentication. Doctor is not C++ sensor execution,
a secrets scan or a Codacy code analysis. Final scope must use the actual published
PR target, `--base origin/codex/editor-improvements`, with final worktree inputs.
The previous 83-case result predates the changed handle and remains historical.
Provider/credential/private-native/OS/consent/certification qualification stays open.

### Earlier 2026-11-11 evidence

The tests above are coverage locations, not a claim that they have passed on every
host. Targeted Windows Debug configuration with MSVC 19.44 completed after the
helper verified its actual canonical shared-lock file handle. Earlier sandbox
configuration stopped with `C1902`; that attempt supplies no compiler/test success.

The first frontend RED build stopped before test execution: `BindDispatch` tried
to mutate a handle through `Result::Value()`'s const lvalue view. The source fix
moves the handle out through rvalue `Value()` before binding authority. The bounded
retry compiled this fix and the new binding/friends tests successfully.

Frontend RED then failed at the expected assertion with only its cancellation
provenance guard bypassed: an old frontend handle was able to cancel a new account's
request with reused numeric identity. The JUnit report recorded that failure.
Restoring the exact fixed source and rebuilding produced these Windows Debug results:

- `HoroPlatformRequestTests`, `HoroPlatformServicesCompositionTests`,
  `HoroPlatformFriendsPublicConsumer`, `HoroPlatformServicesPublicHeaderConsumer`
  and `HoroPlatformServicesExtensionPublicHeaderConsumer` built successfully with
  `cmake --build build/skeleton --target <targets> --parallel 1`.
- A generated CTest harness ran the request executable with the explicit filter
  `[friends],[frontend],[session],[presence],[request],[completion-queue]`:
  all 83 test cases passed, including the handle regression, six binding cases
  and offline friends recovery. This is a selected subset, not the full request suite.
- The same harness ran composition and friends public-consumer executables:
  all three CTest entries passed with one worker and a 300-second timeout.

The native lifecycle RED baseline build stopped before test execution in
`PlatformProviderOperationsFixture.c`: MSVC reported C1189, `C atomics require C11
or later`. The `HoroPlatformProviderAdmissionTests` target's C compile command did
not select C11 on the previous `f51e0c0f4` base. Both temporary baseline sources were restored
byte-for-byte; the shared lock was released and independently reacquired without
starting another job. Native RED/GREEN, the full request suite and final managed
Sonar IDE, CLI secrets and Codacy checks remain incomplete. No native test pass is
inferred from compilation of dependencies or the successful focused group.

The delivery is now rebased onto local parent `5d0aff9d6`, which selects C11,
disables C extensions and enables native MSVC C atomics for that fixture target.
`git range-diff` confirmed all seven delivery patches remained equivalent; the
resulting source tree differs from the pre-rebase checkpoint only by this parent
CMake fix. The execution results above precede that integration. Native retry,
reconfiguration and final validation still require a new shared heavy-build slot;
no test result on the rebased tree is inferred from patch equivalence.

The all-child acceptance audit found a further old-scope observer gap: the native
host delegated `OnComplete` directly to its retained request store. A retained
success could therefore be subscribed after replacement, or a second old callback
could run after the first callback advanced the account. The local source fix now
captures immutable weak host/revision evidence at admission, checks late observer
admission and checks each deferred delivery without invoking user code under a
lock. Three new regression cases cover retained/queued success, reentrant account
replacement and move/foreign-store provenance. Existing invalidation/timeout tests
now assert withdrawn old-scope observer delivery while preserving exact terminal
results and native lease/cancellation assertions, as required by ADR-135.
This source/test/migration change has not yet compiled or executed; the previous
83-case run does not validate the changed handle header. The next bounded native
RED/GREEN scope must include both terminal invalidation and observer withdrawal,
followed by the affected current-tree consumers and final quality scope.

`clang-format` and `git diff --check` have run on the changed sources. The portable
VS Code profile with installed CMake/Ninja passes the local preflight prerequisite
doctor. Doctor's Codacy cloud-configuration comparison is incomplete because cloud
authentication could not be verified. Prerequisite readiness is not C++ sensor
execution, a clean analyzer result, or a hosted quality gate.

A bounded Lizard 1.23.0 measurement completed for the friends coordinator, native
lifecycle dispatch, frontend and binding implementation sources. Their maximum
per-function CCN estimates were 14, 14, 11 and 10 respectively. These are advisory source metrics,
not complexity thresholds, complete-file rule analysis or executable test results.

## External evidence still required

PLS-006.7 depends on open provider qualification #1887 / HORO-1843. Its open
configuration prerequisite #1826 / HORO-1782 belongs to the configuration workstream.
This delivery does not duplicate either external implementation.

Provider lifecycle conformance also owns two source-reviewed shutdown gaps:
repeated BUSY `Close` must not resend native cancellation, and the first close
attempt must revoke request observer delivery even if native admission/ingress
closure or drain fails. Native leases and callback context must remain retained
until successful drain. The provider workstream has prepared separate fixes and
regressions; their integration and execution evidence are still pending. Existing
BUSY lifetime tests alone do not establish these stronger delivery guarantees.
The local provider-workstream candidate `c77b2c33d` contains both Close fixes and
separate package lifecycle fixtures/regressions. Its relevant Host source diff
was inspected during this audit; the commit has not been integrated into this
delivery and that inspection supplies no executed conformance result.

Private providers must still demonstrate native account-change and forced-logout
callbacks, protected stable-subject/profile binding, real OS/provider versus
product-consent intersection, child/parental/region restriction projection,
credential revocation and late native callback retirement. Certification and
Linux/macOS/native SDK results must be recorded from those actual compositions;
public Null/Mock/C ABI fixtures do not prove them. Durable progression, offline
queue and save/cloud ambiguity remain owned by their respective coordinators and
must never be retargeted to a newly current account.

No issue status, hosted rule configuration or acceptance checkbox is changed by
this record. PLS-006.7 and the parent remain unqualified until the required external
matrix and local execution evidence are available.

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

## Public evidence matrix

| Invariant | Executable coverage |
| --- | --- |
| Failed detached session candidates preserve the previous immutable snapshot; legal transitions advance generation/access revision | `PlatformUserSessionTests.cpp` in `HoroPlatformRequestTests` |
| Deferred owner-thread observers, stale revision filtering, reentry and subscription/close lifetime | `PlatformSessionObserverTests.cpp` in `HoroPlatformRequestTests` |
| Old native work terminates on session invalidation without waiting for a callback; native cancellation is issued once even on rejection; delayed old completion cannot change new work | `PlatformProviderLifecycleTests.cpp` in `HoroPlatformProviderAdmissionTests` |
| Native partial-start rollback and BUSY shutdown retain callback/code ownership | `PlatformProviderLifecycleTests.cpp` in `HoroPlatformProviderAdmissionTests` |
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
not select C11 on the current base. Both temporary baseline sources were restored
byte-for-byte; the shared lock was released and independently reacquired without
starting another job. Native RED/GREEN, the full request suite and final managed
Sonar IDE, CLI secrets and Codacy checks remain incomplete. No native test pass is
inferred from compilation of dependencies or the successful focused group.

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

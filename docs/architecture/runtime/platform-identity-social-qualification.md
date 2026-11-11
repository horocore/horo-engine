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
| Presence registry validation, coalescing, rate limits and session/access token invalidation | `PlatformPresenceCoordinatorTests.cpp` in `HoroPlatformRequestTests` |
| Friends capability absence, explicit Null and all denied access states are distinct failures | `PlatformFriendsCoordinatorTests.cpp` and `PlatformServicesFrontendTests.cpp` in `HoroPlatformRequestTests` |
| Friends pages reject mismatched offset/scope, oversized/malformed UTF-8, NUL, duplicate handles and invalid continuation before any partial publication | `PlatformFriendsCoordinatorTests.cpp` in `HoroPlatformRequestTests` |
| Friends cache expiry, invalid session updates, consent/restriction replacement, cancellation, failure, timeout and shutdown preserve publication authority | `PlatformFriendsCoordinatorTests.cpp` in `HoroPlatformRequestTests` |
| Scripted delayed friends results cannot cross access/session publication fences | `[qualification]` case in `PlatformFriendsCoordinatorTests.cpp` |
| Social handles cannot become local subject capabilities or ordinary stream/log values; staged public header remains consumable | `HoroPlatformFriendsPublicConsumer` |

## Execution status

The tests above are coverage locations, not a claim that they have passed on every
host. The current delivery's targeted configure/build/CTest and final managed
Sonar IDE, CLI secrets and Codacy check are pending the repository-wide heavy-build
slot. Initial sandbox configure stopped in the MSVC compiler sanity test with
`C1902`; the subsequent non-sandbox attempt deferred because the shared lock was
busy. No C++ regression result is inferred from either attempt.

`clang-format` and `git diff --check` have run on the changed sources. The portable
VS Code profile with installed CMake/Ninja passes the local preflight prerequisite
doctor. Doctor's Codacy cloud-configuration comparison is incomplete because cloud
authentication could not be verified. Prerequisite readiness is not C++ sensor
execution, a clean analyzer result, or a hosted quality gate.

A bounded Lizard 1.23.0 measurement completed for the friends coordinator, native
lifecycle dispatch and frontend implementation sources. Their maximum per-function
CCN estimates were 14, 14 and 11 respectively. These are advisory source metrics,
not complexity thresholds, complete-file rule analysis or executable test results.

## External evidence still required

PLS-006.7 depends on open provider qualification #1887 / HORO-1843. Its open
configuration prerequisite #1826 / HORO-1782 belongs to the configuration workstream.
This delivery does not duplicate either external implementation.

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

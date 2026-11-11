# Platform provider package qualification

This is the integrated acceptance ledger for [PLS-002](https://github.com/HoroCore/horo-engine/issues/1868),
including all six child outcomes. Issue closure is not execution evidence.
ADR-130 and ADR-131 currently say **Proposed**; the linked architecture documents
remain the normative implementation contracts.

## Ownership and acceptance map

| Child | Implemented boundary and regression source | Remaining evidence |
| --- | --- | --- |
| PLS-002.1 / HORO-1838 / #1882 | Public provider C ABI; private SDK ownership; explicit host composition. `ExtensionAbi.h`, `PlatformServicesBackend.h`, ADR-131 and the platform-services architecture define the boundary. | Private SDK packages and vendor qualification are external. A public fixture does not establish certification. |
| PLS-002.2 / HORO-1839 / #1883 | Typed service capability/limit/version validation and explicit unsupported/Null results. `PlatformServicesBackendTests.cpp` and `PlatformServicesCompositionTests.cpp`. | Each real provider must qualify every advertised service; the reference package below advertises achievements only. |
| PLS-002.3 / HORO-1840 / #1884 | Extension claim copying, exact identity/version admission, capability/factory revocation and executable leases. `PlatformProviderAdmissionTests.cpp`. Package permissions are additionally intersected with the exact installed package approval before host publication. | Native safety depends on a cooperating provider's retire/drain contract; refusal retains the code and requires explicit recovery. |
| PLS-002.4 / HORO-1841 / #1885 | Exact selection; ordered services/session/ingress startup; partial initialization rollback; cancellation/drain/shutdown. `PlatformProviderLifecycleTests.cpp` and the installed package tests below. | The current operations ABI admits only achievement unlock in the lifecycle host. It does not qualify other native service adapters. Credential injection is still an integration dependency. |
| PLS-002.5 / HORO-1842 / #1886 | Explicit Null/absence and product provenance, profile capabilities and transition retirement. `PlatformServicesCompositionTests.cpp`. | Signed shipping/server evidence, entitlements and real restricted products require their own release qualification. |
| PLS-002.6 / HORO-1843 / #1887 | Verified archive installation, explicit native and role-aware activation, hostile claims, requests, replacement, revocation, rollback and uninstall. `PackagePlatformProviderLifecycleTests.cpp`. | Windows execution is pending the shared build slot. Linux/macOS execution and real vendor packages are not available in this worktree. |

## Reference package coverage

The merged prerequisite boundaries were also reviewed at the starting base:

| Prerequisite | Relevant implementation and regression |
| --- | --- |
| #1874 / HORO-1830 | `PlatformRequest.cpp`, `PlatformServicesFrontend.cpp`; request/frontend tests in `HoroPlatformRequestTests`. |
| #162 / HORO-162 | Inert `ModuleDescriptor` validation and deterministic graph construction; `ModuleDescriptorTests.cpp` and `ModuleDescriptorGraphTests.cpp`. |
| #101 / HORO-101 | `ModuleHostLifecycle.cpp` closes callback admission before reverse-order drain and deactivation; `ModuleHostLifecycleTests.cpp`. |
| #1822 / HORO-1778 | Foundation immutable configuration snapshots and explicit reload boundaries; `ConfigurationTests.cpp`, plus platform project configuration tests. |
| #126 / HORO-126 | Exact verified module handoff, dependency receipts and explicit retirement owner; `PackageHybridLifecycleTests.cpp`. |

Their delivery commits are ancestors of the starting base. The table records
source review and test ownership; it does not label an unexecuted regression as
passed. HORO-1782 / #1826 remains the open credential integration prerequisite.

`HoroPlatformPackageFixture` is a public C11 test module with no proprietary SDK.
Tests place its bytes and exact extension descriptor in a verified archive/file
inventory, publish a real `PackageInstallService` record, and activate through
`PackageLifecycleService`. Paths include spaces and non-ASCII characters.
The test artifact gate and trust authority are fixtures; they do not prove a
real publisher signature, platform account, private SDK or certification result.

`HoroPlatformProviderPackageTests` checks:

- installation alone performs no native load or provider publication;
- both `Activate` and `ActivateModules` reach the explicit publication callback;
- provider and headless-tooling roles load once under their selected native owners,
  without replaying a sibling's provider registration;
- denied/stale package trust, mixed provider/importer claims and absent publication
  owners fail before native loading;
- malformed operation tables, incompatible ABI negotiation, wrong host profiles
  and unapproved permissions publish no usable provider;
- a broader process permission policy cannot bypass exact package approval;
- unavailable SDK initialization and failed partial candidate creation balance
  native create/destroy, including a BUSY retirement retry;
- replacing package version 1 with version 2 closes old factory admission while
  outstanding native work retains the old image, callback sink and verified files;
- old close/drain retry terminalizes admitted requests and releases the old image
  only after native work retires;
- BUSY close retries do not duplicate native cancellation, including a rejected
  cancellation; late subscriptions lose delivery authority on the first close
  attempt even when native admission/ingress closure fails or draining stays BUSY;
- cancellation or install replacement after native publication rolls back the
  staged provider and preserves the old active composition;
- a 1 → 2 → 1 package rollback followed by uninstall revokes every prior realm
  and releases native resources and staged files.

The application supplies one registry and retains its monotonically increasing
provider generations. The overlap fixture versions expose semantic contracts 1
and 2, so staging keeps the old exact binding usable until publication retires it.
The generic registry's duplicate capability/version rule remains unchanged.
A same-contract update is rejected while the old publication remains active;
a separate test disables/drains it before reuse and requires a fresh generation.
The fixture owns independent heap candidates and is not a process-singleton SDK.
Its overlap test does not authorize concurrent initialization of singleton SDKs;
those require old SDK shutdown before new initialization, and a failed cutover
must leave the frontend explicitly unavailable.
Provider/importer mixed contribution batches remain rejected by the normative
provider-only ABI profile; the tests do not broaden that profile.

## Required validation

Build and run these targets with GUI, interactive renderer and proprietary SDKs
absent:

```text
HoroPlatformProviderPackageTests
HoroPlatformProviderAdmissionTests
HoroPlatformRequestTests
HoroPlatformServicesCompositionTests
HoroPackageLifecycleTests
HoroPackageLifecyclePublicHeaderConsumer
HoroPackageHostSurfacePublicHeaderConsumer
HoroFoundationModuleDescriptorTests
HoroFoundationModuleHostCompositionTests
HoroFoundationConfigurationTests
```

The new package tests and application composition consumer are included in the
Windows CI target group. Linux and macOS native CI discover the package tests
through the ordinary headless test registry. Inclusion is not a passed run.
`tests/python/test_ci_workflow_grouping.py` passed locally: 39 tests.
Native configure/build/tests and final local quality preflight remain pending;
this ledger must not be interpreted as completed parent acceptance.

## Session delivery, credential and external qualification gaps

PLS-006 owns account/session replacement and old-scope observer withdrawal. At
this worktree's current base, the native lifecycle host's `OnComplete` delegates
directly to the request store. An immutable terminal result does not authorize
delivery into a replacement account scope. PLS-006 checkpoint
`471caf6c76860844cc13c48d2ce876086e55fb58` contains the admission-revision and
weak-state delivery guard in `OnComplete`, private handles and two `Submit`
metadata assignments. Source review confirms these Host hunks do not overlap the
PLS-002 `Close` change. Its native/frontend handle groundwork is
`70bba8aa2406853b970df3da99da6417033b3355`; the associated move-before-binding fix is
`98e62aaa164b5af7df2f340c8be7f83cacfb231d`. These checkpoints and their execution
evidence are not integrated here; current observer regressions and final quality
are still pending at the source owner. The PLS-002
failed-Close subscription regression verifies shutdown withdrawal only. It does
not prove account-switch withdrawal, including a session change between two
observer deliveries. Keep this integration gate open without mutating historical
terminal results or invoking user callbacks under provider locks.

HORO-1782 / #1826 owns credential provider references. The existing version-2
operations table's `initializeServices(candidate, requiredMask, outMask)` accepts
no typed credential-reference acquisition context or revocable credential lease.
The installed package test module therefore holds no credential and cannot prove
credential acquisition/revocation across update or rollback. Integration must bind
the CFG owner's exact typed handoff without transporting secrets/native handles
through the public provider ABI. This dependency remains open; it is not replaced
by a mock credential test.

The full Windows/Linux/macOS matrix, private SDK runtime availability, signed
shipping manifests, vendor accounts, hardware and certification are separate
evidence obligations. Capability deviations must report the exact unsupported
service/profile and retain the common frontend error, cancellation and ownership
contract. Never weaken that contract to mark a matrix cell passed.

# OpenXR native session composition

`HORO_BUILD_XR_OPENXR` is opt-in and defaults to `OFF`. Non-XR builds do not
populate the SDK or construct a native session. The option installs only the
reviewed SDK license notices; it does not discover, select or bundle a runtime.

The private `HoroXROpenXR` target implements the instance/system/session
transaction using official native entry points. `HoroXROpenXRHostInterface` is
an explicit **non-installed** interface for application composition and selected
native adapters. Its include path is limited to `runtime/xr/openxr/host`.
Neither target changes `HoroXRApi` or `HoroXRRuntime` public headers or exposes
native types through their usage requirements. Host consumers deliberately link
both the implementation and internal interface; ordinary XR consumers link only
the existing backend-neutral targets.

## Host obligations

The host supplies:

- A shared lease on the exact Platform library admitted by loader preflight.
  The native owner retains it through all native handles and dispatch use.
  A null or incompatible library is rejected, without ambient loader lookup.
- An already selected graphics adapter implementing `IOpenXRGraphicsBinding`.
  `Validate` checks current device/platform ownership. `Prepare` calls that
  API's OpenXR graphics-requirements function for the candidate instance and
  system, validates the actual selected device and supplies one stable binding.
  Missing/headless bindings, mismatched native structure types and unrelated
  chain nodes fail explicitly. No method chooses or falls back to a renderer.
- A composition fence checking the **current** loader attempt, installation,
  runtime/system incarnation, capability revision, product policy and selected
  renderer/device. Comparing a snapshot only to itself is insufficient.
  It runs before object creation, between preparation boundaries, at publication,
  and before downstream native borrowing.

All activation, borrowing and teardown operations are serialized on the host's
declared control thread. Host graphics and fence objects outlive the native
owner. Replacement uses a separate owner and graphics port; it cannot mutate
the existing session or its device borrow.

## Transaction and retirement

Discovery has fixed bounds: 32 API layers, 128 unique extensions across the
runtime and explicitly enabled layers, and 16 enabled extensions including the
selected graphics API extension. Overflow fails rather than truncating. Required
missing extensions fail; optional missing extensions remain observably disabled
through `HasExtension`. Shipping products cannot enable development layers.

The owner secures bootstrap instance-retirement dispatch before allocation.
It then prepares the instance, candidate-scoped dispatch, system, graphics
binding and session. Only a completely prepared, freshly fenced transaction
publishes its Horo session identity. Every failed boundary rolls back acquired
candidate resources in reverse order. Native errors preserve the operation and
numeric native result, without copying device names or paths. Official native
function pointers are recovered with equal-size representation copies, checked
by `std::bit_cast`; the supported loader ABI must use matching object/function
pointer representations. Dispatch and negotiated name storage have separate
private owners, retained by the same immovable session.

Host preparation runs inside a no-throw invocation boundary that returns only a
fixed exception category. Typed error allocation and reverse-order retirement
remain outside that boundary. Standard and non-standard host exceptions retain
the existing failure messages and rollback behavior; this does not mark the
allocating `Create` API itself `noexcept`.

`Borrow` returns a private non-owning native tuple only after exact Horo
identity and current composition/device admission. It cannot escape the
serialized native operation or survive teardown/revocation. Subsequent
spaces/actions/swapchains remain the native backend's responsibility.

The host closes frame/input admission, drains dependent native users and retires
Renderer GPU/external-image references **before** `Close`. Close invalidates the
published identity first, destroys session, releases the graphics borrow, then
destroys instance. A native destruction failure returns an actionable error and
retains the corresponding objects and loader for explicit retry. The host must
complete retirement before destroying the owner; the destructor enforces this
invariant rather than unloading code beneath surviving native handles.

Native preparation does not itself publish XRRuntime `Ready`. The existing
host lifecycle must also prepare required spaces/swapchains, Renderer targets
and Input/UI adapters before committing a frontend generation. This change
does not implement predicted frames, action polling or physical-device
qualification.

## Dependency provenance and verification

The authoritative baseline has no OpenXR dependency in its CMake/vendor
configuration. The optional headers-only dependency is Khronos OpenXR-SDK
`release-1.1.63`, pinned to commit
`f2448a8797c85814aa892efc1ab8707900fbcc78`. `LICENSE` is Apache-2.0,
canonical LF SHA-256
`cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`.
The dependency does not build or link the SDK's loader; the host owns the
verified runtime loader lease. SDK native headers are used only at the private
native boundary.

`HoroXROpenXRTests` drives the production transaction through an injected native
dispatch table. Its failures cover acquisition rollback, dispatch absence,
capacity/malformed negotiation, stale admission, graphics failure, replacement
and retryable teardown. `HoroXROpenXRHostInterfaceConsumer` independently checks
the narrow host header boundary. Deterministic dispatch is not physical-device
evidence; device/runtime/OS/renderer tuple qualification remains `NotRun` until
performed on suitable hardware.

## Explicit CI qualification

The existing Linux/GCC, macOS/Clang and Windows/MSVC CI matrix explicitly
configures `ci-xr-openxr`, builds the native lifecycle suite, XRApi/XRRuntime
suites and three boundary consumer targets, then runs the selected executable
regressions serially. No-tests is an error and native qualification failures
remain blocking. JUnit reports are archived with the existing platform reports.
The Sonar preset independently opts in so new native production code has
coverage evidence rather than being omitted from its build.

Ordinary Debug presets retain default-OFF behavior. Each platform also
configures `ci-xr-disabled`: native implementation/interface/SDK targets and
current FetchContent population must be absent. Cached SDK source files from
the ON configure may remain; their presence is not native activation.
The qualification presets disable unrelated concrete rendering, Physics,
Navigation and networking backends. These fake-dispatch lifecycle tests never
claim headset, compositor or physical graphics-device qualification.

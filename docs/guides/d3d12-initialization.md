# D3D12 device initialization

HORO-331 realizes the startup-only portion of [ADR-032](../adr/032-d3d12-baseline-and-agility-sdk-policy.md).
`HoroRenderD3D12` owns its private initializer, DXGI factory/adapters, D3D12
device and requested queues. It has no public installed headers. Its only engine
dependency is `RenderApi`; platform-native types stay in the Windows translation
unit. Other renderer targets and their public contracts are unchanged.

This target does not register an interactive renderer, publish presentation or
resource capabilities, or select a backend. It is a composition seam for the
remaining D3D12 tickets. No existing host changes backend selection because the
target exists.

The host constructs `D3D12Initialization` on its render-capable thread after COM
initialization. `ID3D12HostAdmission::VerifyRuntime` must verify the executable's
fixed Agility activation exports, SDK 619 / package 1.619.5 contract and trusted
payload before any DXGI/D3D12 acquisition. Delivery owns those exports, the pinned
dependency and packaging; a successful device call cannot substitute for trusted
payload verification. `AdmitDriver` applies the host's restrictive versioned
qualification policy to the machine-local adapter LUID and actual driver version.
No permissive production policy is supplied here.

Initialization checks native Windows 11 x64 and COM, enables explicitly requested
debug layers before any device creation, and enumerates at most the requested
limit (1–64), rejecting overflow. Discovery uses DXGI's unspecified/default GPU
preference. It retains this order when qualifying hardware. Explicit adapter
requests match exactly or fail. Software requires explicit test admission; remote
adapters are rejected. Feature-level or Shader Model rejection during automatic
qualification may advance to another hardware adapter. Native acquisition errors,
driver-policy rejection and exact-selection failure stop the attempt. No backend
or software fallback occurs.

Device creation requires feature level 12_0; Shader Model 6.0 is queried separately.
The direct queue is mandatory; independent compute and copy queues are created only
when requested. Readiness means these startup objects exist, and does not establish
format, resource, output or interactive parity admission.

All calls are synchronous and startup-only. Enumeration and candidate attempts are
finite. Cancellation is observed between stages and queue calls; a native driver
call cannot be interrupted by this token. Wrong-thread calls return typed failures.
The host must destroy the session on its construction thread. Failed/cancelled
attempts release partial state and may be retried. Explicit idle, initializing,
ready and stopped states reject reentrant initialization/shutdown from host
admission callbacks. Shutdown is idempotent and
permanently closes admission. Queues are released before the device, retained
adapters and factory. No command submission escapes this session, so teardown has
no GPU work to drain and introduces no GPU wait. Later submission integration must
establish retirement/fence ownership before exposing native borrowing.

Debug-layer enablement is a process-wide D3D12 setting and cannot be undone by
resource rollback. The host must choose it before any D3D12 device in the process
exists; switching diagnostics requires a fresh process. Requested unavailable
layers fail explicitly. Probe hosts must use the same host release/admission
contract, create an isolated session, then shut it down without retaining objects.

`HoroD3D12InitializationTests` exercises startup policy using a native-free runtime,
including failure injection, partial rollback/retry, cancellation, owner-thread
rejection, discovery bounds, explicit selection, software admission and optional
queues. Non-Windows builds additionally verify the production unsupported-host
result. Windows compilation and device/debug-layer execution require Windows SDK
and a qualified Windows 11 x64 device; the portable tests do not establish those
native results.

The focused Windows CI capability suite includes this test target so the native
translation unit is compiled and the policy tests run under MSVC. This provides
native compilation evidence; it does not create a device or verify a packaged
Agility payload on hosted runners.

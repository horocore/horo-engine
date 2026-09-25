# Platform Abstraction Architecture

## Purpose

This document defines the boundary between portable engine code and operating
system services such as windows, event pumping, filesystem integration,
processes, native dialogs, clocks, credentials, crash handling, and dynamic
libraries.

Platform abstraction normalizes capabilities and ownership. It does not hide
meaningful operating system differences or force every host to create a window.

## Core Decisions

- Portable modules depend on narrow platform interfaces, not OS headers.
- Platform implementations are selected by the process composition root.
- Headless hosts omit presentation and other optional capabilities; baseline
  platform services remain available through real, deterministic, or restricted
  implementations.
- Paths use structured path types and explicit encoding rules.
- Process execution never relies on shell command concatenation.
- Window and native UI operations declare main-thread affinity.
- Capability absence is represented explicitly and can be queried.

## Capability Model

```cpp
struct PlatformServices {
    FileSystem& files;
    Clock& clock;
    ProcessService& processes;
    UserDirectories& directories;
    CredentialStore* credentials;
    NativeDialogs* dialogs;
    CrashService* crash;
};
```

`FileSystem`, `Clock`, `ProcessService`, and `UserDirectories` form the baseline
service set for supported engine hosts. Headless and test hosts provide suitable
headless, deterministic, or restricted implementations rather than making this
baseline nullable. A restricted implementation returns a typed `Unsupported`
error for an operation forbidden by host or platform policy.

Scenario-specific capabilities use nullable construction-time pointers, as in
the example, or typed capability queries. `CredentialStore`, `NativeDialogs`,
and `CrashService` are optional because availability depends on host role,
platform integration, and product policy. An operation requiring an unavailable
capability returns a typed error.

The graphical editor composes a `NativeDialogs` file and folder picker in its
application host and lends it to the asset-import modal. The picker returns
structured native paths; an empty file list or absent folder means the user
cancelled. The modal holds an input-router `NativeDialog` token for the entire
synchronous picker call. Headless modal callers omit this optional picker.

The platform layer includes:

- Windows, macOS, Linux, and Android implementations
- deterministic or in-memory test implementations
- headless implementations where appropriate

Android application/activity lifecycle, replaceable native surfaces, runtime
permissions, scoped storage, deployment, and resource-pressure specialization
follow [Android Platform Host](./android-platform-host.md). Portable callers use
the same capability model; they do not branch on Android APIs or lifecycle
callbacks.

[ADR-171](../../adr/171-android-host-target-and-ownership.md) fixes the initial
Android composition and ownership boundary: a private GameActivity-based host
owns lifecycle serialization, PlatformAndroid owns admitted native-window and
Android-service lifetimes, and native types remain outside public Horo headers.

## Filesystem And Paths

The engine uses structured path values:

- `ProjectPath`: normalized path relative to an open project
- `AbsolutePath`: validated absolute OS path
- `VirtualPath`: path inside an archive or package
- `UserDataPath`: resolved application support location

Rules:

- project files store portable forward-slash relative paths
- host adapters convert native input into structured paths
- normalization does not silently resolve outside an allowed root
- case sensitivity follows the storage contract, not the current machine alone
- symlink traversal is explicit for security-sensitive operations
- path comparisons are tested on Windows, macOS, and Linux semantics

Filesystem writes that protect user data use temporary same-filesystem writes
and atomic replacement when supported.

The implemented PRJ-001A baseline exposes `DurableFileSystem` and the move-only
`ExclusiveFileLock`. Native Windows, macOS, and Linux adapters provide durable
write/copy/remove, same-filesystem atomic replacement, parent-directory
synchronization according to host policy, capacity inspection, and immediate
OS-held exclusive locking. Lock-file text is diagnostic only; ownership lives
in the native handle and is released by RAII or process termination.

Operations that create a lock file or destination file also create missing
parent directories. This is part of the `DurableFileSystem` contract rather
than a native-adapter convenience, allowing bounded clients such as the package
cache to publish a fresh directory hierarchy through the durable abstraction.
Existing alternative adapters must adopt this behavior before serving those
clients; the native implementation and fresh-root package-cache regressions are
the compatibility reference.

## User Directories

The platform service resolves logical directories:

```text
config
state
cache
logs
crash
temporary
```

Callers never build these paths from `$HOME`, `%APPDATA%`, or platform-specific
string literals. Locations follow platform conventions and the storage policies
in the owning architecture documents.

Application state roots are opened for a validated `ProductStorageId`; product
display name, executable path and current working directory are not storage identity.
Platform Abstraction returns a root/container capability only. Under
[ADR-113](../../adr/113-local-storage-user-profile-and-slot-ownership.md), the save
storage adapter alone maps environment, local-user/game-profile or server owner and
logical slot IDs beneath that capability. UI, gameplay and cloud backends never
receive the resulting host path.

## Window And Event Pump

Window creation is optional. A window owns:

- native window lifetime
- event integration
- logical and framebuffer size
- DPI/content scale
- focus and minimize state
- graphics-surface compatibility

The host owns event-pump ordering. Input consumes a normalized event snapshot
after platform polling. Renderer resize occurs from committed framebuffer-size
state, not directly inside arbitrary native callbacks.

Native callbacks enqueue or update platform-owned state; they do not mutate
editor documents or application services.

On mobile hosts, a native window is a replaceable capability rather than a
process-lifetime assumption. Activity resume does not guarantee that a window
already exists, and pause or surface destruction does not necessarily terminate
the process. Every native-window snapshot therefore carries a generation;
renderer presentation rejects stale generations and owns graphics-resource
retirement.

## Time

The platform exposes:

- monotonic clock for durations, scheduling, and frame timing
- wall clock for records and user-facing timestamps
- high-resolution profiler timestamps

Runtime simulation never uses wall-clock jumps. Tests can inject deterministic
clocks.

## Process Execution

```cpp
struct ProcessRequest {
    AbsolutePath executable;
    std::vector<std::string> arguments;
    EnvironmentOverlay environment;
    std::optional<AbsolutePath> workingDirectory;
    ProcessIoPolicy io;
    Duration timeout;
};
```

`EnvironmentOverlay` has explicit base semantics:

```cpp
enum class EnvironmentBasePolicy {
    InheritParent,
    AllowlistedParent,
    Empty
};

struct EnvironmentOverlay {
    EnvironmentBasePolicy base;
    std::vector<std::string> inheritedNames;
    std::vector<EnvironmentAssignment> set;
    std::vector<std::string> unset;
};
```

Every process request selects a base policy; there is no implicit inheritance
default. `set` adds or replaces values after the base is formed, and `unset`
removes names after inheritance and assignment. `inheritedNames` is used only
with `AllowlistedParent`. Full parent inheritance is reserved for trusted local
tools that require it. Security-sensitive and release operations use an
allowlist or an empty base, and credentials are never inherited or injected
implicitly.

Arguments remain separate from the executable and are passed through native
process APIs. Shell execution requires a dedicated, security-reviewed operation
and is not the default.

The service supports:

- bounded stdout and stderr streaming
- cancellation and timeout
- exit status and termination reason
- redacted diagnostic representation
- process-tree termination where the OS supports it

Process cancellation results are normalized by the job system so that UI and
operation observers see stable codes. Graceful cancellation before work completes
maps to `job.cancelled`; timeout maps to `job.cancelled` with cause
`platform.process.timeout`; forced termination maps to
`platform.process.terminated`.

## Native Dialogs

File pickers, credential prompts, and other native dialogs temporarily enter
`NativeDialog` interaction scope. In an interactive Horo host, the GUI thread is
the process main thread; native dialogs are invoked only from that thread.
Headless hosts have no GUI thread or native-dialog capability. A future host
with a dedicated GUI thread must expose that affinity as a distinct contract
rather than treating `GUI thread` and `main thread` as interchangeable names.

Dialog results are normalized paths or typed cancellation. Native dialogs do not
perform project mutations directly.

## Credentials

Credential storage is a platform capability implementing the contract in
[Release Security](../release/release-security.md). Callers work with opaque credential
references and short-lived secure values.

Absence of a platform credential store is explicit. Falling back to plaintext
persistence is forbidden.

The backend-neutral credential lifecycle and secure byte owner live in
`HoroSecurity`; Platform implementations provide only explicitly composed
native capabilities. Cryptographic entropy uses `BCryptGenRandom` on Windows,
`SecRandomCopyBytes` on Apple platforms, and `getrandom` on Linux. A native
entropy error clears the destination and fails; standard-library PRNGs are not a
recovery path.

## Dynamic Libraries

If dynamic modules are supported, the platform layer owns loading and symbol
lookup primitives. ABI compatibility, extension package policy, and module lifecycle are
defined by the [Extension System](../extensions/plugin-system.md) and
[Gameplay Module Boundary](../extensions/gameplay-module-boundary.md). Raw
dynamic-library handles do not escape into ordinary engine modules.

The optional first-party OpenXR backend follows
[ADR-158](../../adr/158-openxr-loader-backend-packaging-and-host-composition.md).
Platform may resolve a product-approved bundled or platform-provided loader artifact
and lend bounded open/symbol/close plus native-host initialization capabilities.
XROpenXR alone owns loader API calls and dispatch. Platform does not scan runtime
manifests, select an XR runtime, create OpenXR objects or treat a successfully opened
library as product support.

## Crash And Emergency Services

The crash service installs the smallest safe platform handlers required to:

- preserve a crash marker or dump
- record process and build identity
- flush preallocated emergency logging state where safe
- avoid allocations and ordinary locks in signal-sensitive contexts

Where the platform and product support it, an out-of-process crash collector is
preferred for dump capture and durable metadata. The in-process handler performs
only the signal-safe notification or marker write needed to wake that collector.
When no collector is available, the handler falls back to a minimal preallocated
tombstone; it does not upload, symbolize, format a complex report, or attempt
general recovery in the compromised process. Abrupt process or operating-system
termination may prevent either path from completing, so the contract guarantees
best-effort evidence rather than a dump for every failure.

Crash upload, consent, retention, and symbol handling are separate application
and release policies.

[ADR-048](../../adr/048-gpu-crash-and-device-loss-diagnostic-bundles.md) may add a
minimal preallocated renderer incident slot containing only scalar session,
renderer/device generation and last-frame/submission context. The Platform handler
may seal that slot and notify the existing collector; it never calls renderer
objects, performs native GPU queries or assembles a bundle. Out-of-process or
next-launch code owns rich association and preserves best-effort/partial coverage.

## Threading

[ADR-070](../../adr/070-capture-and-voice-io-ownership.md) assigns Platform the
native microphone/input-device and OS permission adapter only. Platform reports
typed permission/device facts on required owner threads and keeps native handles
private; it does not own Audio capture sessions, consent/retention policy,
recording assets, network voice, speech, or editor workflows.

Capabilities declare affinity:

| Capability | Required affinity |
|---|---|
| Window and event pump | Interactive main thread |
| Native dialogs | Interactive main thread |
| Graphics surface creation | Render-capable thread |
| Filesystem and subprocess I/O | Any thread through owned synchronization |
| Credential UI | Interactive main thread |
| Monotonic clock | Any thread |

Android activity and permission callbacks arrive through the platform adapter
and are committed on the application owner thread. Native surface handoff still
obeys the renderer's render-capable-thread contract.

The platform layer does not dispatch arbitrary callbacks while holding native
or internal locks.

## Testing

Required tests cover:

- path normalization and root escape prevention
- Unicode and platform-specific path behavior
- atomic write and interrupted-write recovery
- subprocess arguments without shell interpretation
- subprocess environment inheritance, allowlisting, override, and removal
- sensitive variables do not reach restricted child processes
- bounded process output, cancellation, and timeout
- logical versus framebuffer resize behavior
- deterministic clock injection
- missing optional capabilities
- native dialog interaction-scope transitions
- crash marker creation through test-safe hooks
- collector notification and in-process tombstone fallback where supported

## Related Documents

- [System Design](./system-design.md)
- [Configuration System](./configuration-system.md)
- [Concurrency And Job System](./concurrency-and-jobs.md)
- [Input Architecture](../runtime/input-architecture.md)
- [Android Platform Host](./android-platform-host.md)
- [ADR-113](../../adr/113-local-storage-user-profile-and-slot-ownership.md): product
  state roots and save namespace/path ownership.
- [XR Architecture](../runtime/vr-ar-architecture.md)
- [Release Security](../release/release-security.md)
- [Extension System](../extensions/plugin-system.md)
- [Gameplay Module Boundary](../extensions/gameplay-module-boundary.md)

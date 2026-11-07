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

### Desktop file-manager integration

`HoroPlatformDesktop` owns `Horo/Platform/FileManager.h` independently of the
baseline `HoroPlatform` target. The graphical composition links this optional
desktop adapter through `HoroGui`; headless builds retain the contract with a
typed `file_manager.unavailable` implementation and no desktop dependencies.
Call `RevealInFileManager` on the main thread after an explicit user action.
Directories open themselves. Files are selected in Explorer/Finder; the Linux
adapter opens the containing directory through the existing SDL desktop URI
handler. Dispatch acceptance does not prove external-application completion.

Common validation requires an existing absolute native regular file/directory,
rejects embedded NULs and a symbolic link at the target, and does not reinterpret
the path through the process locale or resolve it against the current directory.
Project-root containment remains the caller's responsibility. Windows uses wide
Shell APIs with scoped COM/PIDL ownership. macOS uses filesystem URLs and
`NSWorkspace`. Linux percent-encodes path bytes into a local file URI; no platform
builds shell command strings from a path. Native UI dependencies remain private
to `HoroPlatformDesktop`. The editor logs typed native failures before presenting
localized failure copy.

This replaces the editor-menu-owned macOS reveal implementation and non-Apple
stubs. Existing editor callers retain their private bool bridge; new portable
callers link `HoroEngine::PlatformDesktop` and consume its typed result. Public
header consumer coverage and filesystem/URI regression tests cover this boundary.
Cross-platform native desktop behavior still requires tests on each OS.
`HoroFileManagerTests` never launches desktop applications. For an explicit native
check, build the opt-in `HoroFileManagerSmoke` target in a GUI build and invoke it
with an existing absolute directory/file, including spaces and non-ASCII names.
This helper is excluded from the default build and unattended CTest runs.

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

Project reads retain native directory identities through descendant traversal and
content capture. `IProjectReadFiles::Text` and `Files` retain their interactive
1 MiB ceiling. Load-time source payloads use the separate `SourceText` operation
and `ProjectSourceReadLimits`, bounded to 16 MiB and 30 seconds. The caller's
original authority, cancellation and absolute deadline still apply, including
between native reads; links, reparses and multiple hardlinks remain rejected.
Restricted readers refuse source capture by default. Source capture grants no
write authority and does not widen code-query admission.

Filesystem writes that protect user data use temporary same-filesystem writes
and atomic replacement when supported.

The implemented PRJ-001A baseline exposes `DurableFileSystem` and the move-only
`ExclusiveFileLock`. Native Windows, macOS, and Linux adapters provide durable
write/copy/remove, same-filesystem atomic replacement, parent-directory
synchronization according to host policy, capacity inspection, and immediate
OS-held exclusive locking. Lock-file text is diagnostic only; ownership lives
in the native handle and is released by RAII or process termination.

The `ExclusiveFileLock` value wrapper and its public observation symbols belong
to Foundation. Its incomplete state is implemented by Platform through the
single staged, non-installed `HoroPublicationLockInternal` header. This keeps
Assets publication Foundation-only: it observes the borrowed lease without
linking a native adapter or depending on static archive order. Native acquisition,
canonical path verification, process exclusion and resource release remain in
Platform. The internal state adds one virtual observation and a virtual destructor
on this tooling path, with no extra allocation and no frame work. The host keeps
the native implementation resident until every lease has been released.

This moves implementation symbols between the existing static libraries; hosts
must relink both libraries. Public declarations, value layout and source callers
are unchanged. The Foundation-only consumer checks empty/moved leases without
Platform, while native lock tests retain exact-path and lifetime coverage.
The native adapter canonicalizes host-owned parent directories before creating
missing parents and acquiring the lock, including macOS `/var` aliases. Alias
and canonical spellings share one exclusion key. The lock file itself must
remain a regular single-link file; symlinks and reparse points are rejected.
`ExclusiveFileLock::ProtectsPath` continues to require the exact canonical
lock-file path selected by the publication authority.

Operations that create a lock file or destination file also create missing
parent directories. This is part of the `DurableFileSystem` contract rather
than a native-adapter convenience, allowing bounded clients such as the package
cache to publish a fresh directory hierarchy through the durable abstraction.
Existing alternative adapters must adopt this behavior before serving those
clients; the native implementation and fresh-root package-cache regressions are
the compatibility reference.

Private update-package transfer also uses a native durable append primitive.
It requires an existing protected parent directory, creates a new file only at
offset zero, and otherwise appends only to a regular single-link file with the
exact expected length. The file is flushed before checkpoint publication;
checkpoint recovery rejects any interrupted length mismatch.

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

Windows subprocesses inherit only the explicitly listed standard-stream handles.
Callers that previously relied on ambient inheritable handles must pass a typed
capability through a dedicated process contract; they must not set process-wide
inheritance flags to transfer a release or installation lease.

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

## Cook publication directory authority migration

`DurableFileSystem::CreateDirectoryPath` is an explicit pure virtual capability. Its
`Single` policy creates only the final component and reports an already-existing
directory without claiming exclusive creation; `Parents` permits missing-parent
creation. Both retain typed failures. Directory durability remains an explicit
`SyncDirectory` responsibility at the transaction boundary.

Cook publication previously called native `std::filesystem` directory creation
outside the host-injected filesystem. A restricted or root-bound writer therefore
could not govern all publication mutations. Generation-parent, staging-parent,
operation and private-generation creation now use the same filesystem authority
as file writes and replacement. Reusing an exclusive operation/generation directory
still fails; rejected creation preserves the last committed selector and original
failure identity. No native fallback bypasses a denied capability.

This deliberately changes the C++ filesystem contract: every implementation must
provide directory creation, and binary consumers rebuild. All existing native-backed
test adapters delegate to their established native owner; in-memory adapters that
never supported directory publication reject it explicitly. No persistent format
changes or parallel legacy creation path are introduced. This migration supplies
the required authority hook; it does not itself replace the native writer's
host-owned-parent precondition or claim complete protection against concurrent
hostile namespace replacement. Root-bound native publication remains separate work.

## Root-bound immutable file storage

`ImmutableFileStorage` is a Foundation-owned, load/tooling-time capability for
bounded relative file reads and immutable entry publication. The host explicitly
opens an existing authorized parent and fixes a derived-data namespace with
`CreateNativeImmutableFileStorage`; the namespace may be absent and the factory
performs no output creation. Per-call names cannot replace or escape that fixed
namespace. Callers cannot select another native backend
or fall back to ambient filesystem operations when an access is refused.

The retained root and each subsequently opened parent are native directory
identities. Replacing the root's former pathname does not redirect a capability to
the replacement. Portable relative UTF-8 names are validated before traversal;
symbolic links, Windows reparse points, special objects and multiply linked files
are refused. Reads have independent cursors, finite caller-reducible byte bounds
and mutation metadata checks around the complete transfer. A missing component is
a miss; an unsafe or unreadable component is an error.

Writes prepare complete bytes in an exclusively created private operation
directory, flush the file and publish with a native no-replace operation. POSIX
preparation directories admit only the owning user's access; Windows preparation
uses the host process's security policy. The host must protect private preparation
content from other code running with the same principal's filesystem authority.
Parent directory identities stay retained throughout preparation and publication.
Identical existing bytes are idempotent; different bytes are a typed conflict.
POSIX publication synchronizes the source and destination directory identities.
Windows follows the existing native filesystem policy: file contents are flushed,
but this implementation does not claim a supported directory-flush primitive.
An unsupported atomic no-replace primitive fails instead of degrading to an
overwriting rename. A post-commit durability error can leave a complete cache
entry; it does not represent a generation-selector commit or rollback receipt.

Shared handle/read primitives use the staged, non-installed
`HoroRootedFileNativeInternal` interface and remain owned by
`HoroPlatformProjectReadFiles`. The separate non-installed
`HoroRootedStorageNativeInternal` target owns storage preparation, synchronization,
and Windows handle-relative rename primitives shared by the immutable and mutable
publication backends. Mutation is not added to the project-read capability, and
neither backend reopens an admitted descendant through its former absolute path.
The two new public headers have exactly one owner each
in the public-header registry. Consumers depend on the Foundation contract;
application composition chooses the concrete Platform factory. The inert error
domain contribution must be registered explicitly by a host before presenting
these errors.

This additive C++ capability does not change cache filenames, key encodings or
persisted formats. Migrating `AssetCookCache` and the cook selector, inventory,
recovery and publication operations to complete host-owned I/O authority is still
required. This capability alone does not complete HORO-1253 or grant project trust,
authored-source writes, cache eviction or generation activation.

## Bounded publication inspection and reads

`DurableFileSystem` exposes `ReadFileBounded`, `InspectEntry`, and
`ListDirectory` so publication owners can use one injected authority for reads,
inventory and mutations. Unsupported adapters reject these operations by default;
there is no native fallback. The native publication test adapter explicitly
forwards all three capabilities to its owned native implementation.

Reads admit a finite positive bound up to 1 GiB, open a no-follow regular
single-link leaf, transfer in 4 KiB steps with cancellation checks, and compare
handle-derived identity/mutation metadata before returning complete owned bytes.
Windows read handles refuse concurrent write sharing while retaining read/delete
sharing for atomic replacement.
Metadata fencing detects observed changes; it does not create an atomic content
snapshot or replace envelope/hash verification and publication writer ownership.
Inventory enumerates only immediate children, preserves no-follow classifications,
and fails at its finite entry bound rather than returning a prefix. It does not
promise an immutable directory snapshot. Hosts register the existing inert
`ImmutableFileStorageErrorDomain` contribution to present these typed failures.

This adds virtual operations and plain metadata types to the existing Foundation
header; implementations and consumers must rebuild. Existing write-only adapters
remain write-only through explicit unsupported results. Native methods retain the
existing host-protected-parent precondition; they do not grant project trust or
provide a root-identity capability. The next migration must route cook selector,
manifest, artifact and recovery inventory access through these operations and
compose the root-bound publication implementation. Parent HORO-1253 remains open.

## Shared native project I/O composition

`HoroPlatformProjectIo` provides `CreateNativeProjectIoOwners` for hosts that need
source capture, immutable cache entries and durable publication for the same
project. The factory admits one existing directory once, then derives all three
owners through independent retained directory handles. It never calls the three
pathname factories in sequence. Replacing the root spelling between or after
construction cannot redirect one of the owners to another project directory.
Construction creates no output. Each returned owner retains an independent
lifetime; callers drain work before releasing the corresponding capability.

The source identity/generation is explicit. Cache and publication namespaces are
host-selected, lexically disjoint derived-data namespaces; hosts must also keep
them distinct under the storage's case and Unicode alias rules. No namespace is
an authored-source write grant. The shared factory rejects invalid roots,
identities, zero generations, portable-path violations and lexical namespace
overlap before I/O. Existing isolated pathname factories remain available for
hosts that require only one capability.

Native handles cross only the staged, non-installed
`HoroRootedProjectOwnersInternal` builder contract. Each existing concrete target
still owns its implementation. The composition peer links those concrete targets
privately and exposes only Foundation and project-read API types. Its new public
header is owned solely by `HoroPlatformProjectIo` and has consumer coverage.
Consumers must relink the affected native targets; persisted formats do not
change.

The navigation CLI and its MCP binding now compose this owner set and inject the
publication owner into the existing bake service. They no longer perform an
ambient output-path check followed by an unrestricted durable writer. Source
capture still decodes `.horo/project.json` through the retained read authority and
matches the host's inspected compatibility identity/version/contract before bake
work is admitted. Generation, revocation, cancellation, deadlines, publication
writer exclusion, source-freshness fences and MCP mutation approval remain owned
by their existing application and host boundaries. The platform bundle itself
grants no trust or permission to execute package code.

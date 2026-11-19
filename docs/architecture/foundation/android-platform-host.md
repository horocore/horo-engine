# Android Platform Host Architecture

## Purpose

This document specializes the general
[Platform Abstraction](./platform-abstraction.md) contract for Android hosts.
It defines application/activity lifecycle, surface delivery, permissions,
storage, packaging, deployment, diagnostics, and device-resource policy shared
by ordinary Android applications and standalone Android-class XR targets.

Android support is a platform capability. XR, game runtime, editor tooling, and
renderer systems do not call Android APIs directly.

## Decision Authority And Initial Baseline

[ADR-171](../../adr/171-android-host-target-and-ownership.md) owns the Android
entry, target and native-resource ownership decision. The initial interactive
host uses pinned AndroidX GameActivity integration, API 29 as its minimum,
`arm64-v8a` as its production ABI, and Android Vulkan 1.1 plus the Android
Baseline profile. `x86_64` is a development/emulator ABI until separately
distribution-qualified. OpenGL ES, `armeabi-v7a`, and `x86` are outside the
initial product scope.

Every product profile still freezes its actual compile/target API level and
distribution constraints. Store-policy changes may advance those values; they
do not silently alter the engine floor, ownership boundaries, or another
distribution profile.

## Composition

```text
Android product application (GameActivity)
  +-- private AndroidHost composition root
  +-- Horo application/runtime composition
  +-- private Android platform adapter
  |     activity/process lifecycle
  |     native window generation
  |     input, sensors, permissions
  |     storage and user directories
  |     device/resource snapshots
  +-- Android Vulkan renderer or explicit RenderNull test adapter
  +-- optional OpenXR backend
```

The private AndroidHost process composition root selects the application profile, renderer, and
optional XR backend. The Android host does not discover and activate arbitrary
renderers after a native surface has already been bound to another backend.

For an XR-enabled product, packaging records one verified OpenXR backend and loader
artifact per shipped ABI. The application passes Android application/activity
initialization inputs through a private host capability to XROpenXR. Android Platform
does not call OpenXR, interpret runtime manifests, select a runtime or own loader
dispatch. A missing ABI artifact, incompatible loader, unavailable runtime and
unsupported XR system are different typed preflight outcomes.

## Target Contract

Every shipped Android profile declares:

- minimum and target API level;
- supported CPU ABIs;
- renderer/backend requirements;
- application entry and packaging model;
- required/optional permissions and hardware features;
- runtime asset and native-library layout;
- supported lifecycle/recreation policy;
- device qualification matrix.

An unsupported API, ABI, renderer, device feature, or native dependency fails
preflight with a typed reason. Runtime code does not probe build/toolchain policy.

## Lifecycle

Implementation status on 10 September 2026: the PLT-001.2 portable baseline
provides a bounded callback-observation queue, owner-thread state controller,
distinct process/Activity/window/presentation generations, and a target-private
adapter seam. It deliberately contains no GameActivity, JNI, `ANativeWindow`,
Vulkan, Gradle, SDK, or device-support claim; concrete native glue and physical
qualification remain separate target work.

Android lifecycle callbacks are translated into Horo runtime transitions on the
application owner thread:

```text
process create
  -> activity create/start/resume
  -> native window available
  -> running
  -> pause / window lost
  -> stop
  -> resume / replacement window
  -> destroy
```

Callbacks can be duplicated, delayed, or interleaved with native-window and
permission events. The platform adapter validates transitions and publishes
generation-safe snapshots. It never mutates editor documents, scene runtime, or
renderer resources from Java/native callbacks.

Backgrounding is not shutdown. Producers pause or neutralize according to their
runtime contracts while durable application state remains owned by the
application. `ActivityPaused` enters suspension; a later `ActivityStarted`
observation makes the Activity visible but does not resume application work until
`ActivityResumed` commits. Final destruction cancels owned work and releases
services in reverse dependency order.

## Native Window And Presentation

An Android native window is a replaceable, generation-scoped presentation
capability. The platform adapter owns callback/native-reference lifetime; the
selected renderer backend owns graphics surfaces, swapchains, framebuffers, and
GPU synchronization.

Surface loss or replacement:

1. prevents new presentation work against the old generation;
2. lets queued GPU references retire safely;
3. destroys backend resources without normal-frame device-idle waits;
4. imports the replacement window through the same typed contract;
5. resumes only after framebuffer extent and presentation capability commit.

Headless Android test compositions provide an explicit no-presentation profile.

### PLT-001.3 implementation and qualification

The private window owner, the distinct Vulkan 1.1 development presentation target,
and the GameActivity composition are being integrated under HORO-2198. The
[window/presentation delivery guide](../../guides/android-window-presentation.md)
records the effective maintenance-feature requirements, source/ownership boundary,
and current evidence. Portable host tests, native NDK compilation, package assembly
and physical-device qualification remain distinct outcomes.

## Input, Sensors, Focus, And Permissions

Android touch, keyboard, gamepad, and admitted sensor events enter the existing
Horo Input pipeline. Physical/native codes remain backend details. Focus loss,
activity pause, device removal, and permission revocation neutralize affected
actions before the next published input frame.

Permission state is not a boolean. Requests expose `NotRequested`,
`Requesting`, `Granted`, `Denied`, `DeniedPermanently`, and `Revoked` where the
platform can distinguish them. Sensitive sensors and capture services stop
publication immediately after loss of permission.

XR actions, poses, and headset tracking are owned by the XR backend. The Android
input adapter must not create a competing controller/tracking path.

### PLT-001.4 Input Composition

`HoroEngine::AndroidInputIntegration` owns the portable, owner-thread
`AndroidInputSession` adapter. It projects keyboard, incarnation-safe touch and
at most four canonical gamepads into the existing `RawInputCollector`. Physical
D-pad keys and normalized hat events retain independent fixed per-device held
evidence; either source keeps the canonical direction held until both release.
Focus loss and device retirement clear that evidence. The same
collector implementation is compiled into the normal Input target and the NDK
package qualification host; this does not create another input stack. Its source
split preserves the `HoroInput` public header owner and ordinary consumers.

Canonical Input snapshots additionally carry fixed-capacity accelerometer,
gyroscope and exact step-counter evidence. Native sensor types and handles stay
private. Unsupported, disabled, permission-required, suspended, awaiting-sample,
native-failure and available states are distinct; unavailable samples always
zero values/counter/timestamp. Physical collection is bounded, while device
connection/disconnection and initial resource preparation are explicit lifecycle
operations outside the routine projection allocation guarantee.

The private `GameActivityInput` bridge consumes the pinned GameActivity's actual
key/motion buffers and actual NDK sensor queue. It maps physical key/button/axis
codes, enables joystick collection, burns touch incarnations, consumes at most
512 native input events and 64 sensor events per frame, and fails closed on input
or callback overflow. The default volume/camera key filter is retained. Motion
event timestamps in pinned GameActivity 3.0.5 are milliseconds; key timestamps
are nanoseconds. Both use Android uptime; `CLOCK_MONOTONIC` focus/device-removal
cutoffs discard pre-resume/pre-removal input after converting units, and boottime
sensor cutoffs prevent queued pre-grant/pre-resume samples from returning.

`HoroInputActivity` is the private Java producer for Activity Recognition
permission and device removal/reconfiguration observations. Its synchronized
peer gate clears the native peer and drains callbacks before native storage
retires. A bounded SPSC handoff publishes observations at the Input owner phase.
Permission denial, permanent denial, revocation, requesting and settings return
remain explicit. Step-counter access requires both explicit host admission and
`ACTIVITY_RECOGNITION`; ordinary collection never prompts. Accelerometer and
gyroscope require explicit host admission. Focus/pause/device loss neutralizes
controls and sensor samples before the next committed Input frame. A granted-to-
non-granted permission transition independently neutralizes held controls and all
prior sensor samples, even when no focus callback arrives. Native input/sensor
time fences reject pre-revocation buffers before fresh observations resume.

The existing Android qualification application now composes this actual bridge
with explicit motion-sensor admission and step-counter disabled by default. Its
manifest declares optional Activity Recognition access so an explicit host/user
request can be exercised; permission declaration alone grants no sensor access.
The profile/manifest verifier continues to reject undeclared permissions and
required hardware features. It selects no renderer or gameplay runtime. Host
regressions, NDK compilation/package assembly, and physical-device qualification
are separate evidence. Authoring these sources does not itself prove any of
those checks passed, and a built APK does not qualify physical sensor/focus or
permission behavior.

## Storage And Assets

Android adapters resolve logical Horo locations to app-private storage, cache,
packaged assets, or explicitly user-granted document sources. Content URIs are
opaque platform resources; they are not stored as portable project paths.

Required behavior includes:

- scoped-storage and grant lifetime;
- packaged read-only asset access;
- durable app-private writes and recovery where supported;
- cache eviction and capacity reporting;
- Unicode names and bounded stream access;
- permission revocation during an operation;
- no traversal outside an admitted root/source.

Project and package formats remain platform-neutral. Android is an adapter over
those formats, not a second asset model.

### PLT-001.5 adapter and host migration

`HoroEngine::Platform` owns the additive `AndroidStorage` public contract and
its staged standalone header consumer. No existing filesystem/path API changes.
Hosts compose existing, disjoint `Context.getFilesDir()` and `getCacheDir()`
roots through the target-private `AndroidStorageAdapter`. Roots are canonicalized
once (including native temporary-directory aliases); symlink roots and every
symlink/hardlink in a consumer path or transaction sidecar are rejected. The
host protects these app-owned namespaces from concurrent external replacement.
This is blocking load/tooling work, never an activity callback or frame-hot path.

Android hosts retain their Java AssetManager reference, bind the corresponding
real `AAssetManager`, and destroy the service before releasing that owner.
Packaged reads use `AASSET_MODE_STREAMING` and `AAsset_read`, so compressed assets
do not require filesystem paths or an uncompressed descriptor. Native Android
builds link the existing Platform target privately to `android`; this adds no
GameActivity, SDK selection, ABI packaging or renderer composition.

For a user-selected document the Android host verifies the exact read grant,
then calls `ContentResolver.openFileDescriptor(uri, "r")` and transfers ownership
with `ParcelFileDescriptor.detachFd()` into private `AdoptDocument`. The host
keeps the URI and its Activity/persisted permission registry. Persisted lifetime
is admitted only after `takePersistableUriPermission` succeeds; an offered grant
flag alone is not persistence evidence. Java `SecurityException`, revoked grants,
missing files and provider failures map through `AccessFailure` without exposing
personal URI text. The engine sees only a service-owned opaque document handle.
It does not obtain a portable project path or an atomic-write capability.

Both actual regular descriptors and provider pipes are bounded, single-use read
capabilities. Reads return no partial result, poll pipes at most every 100 ms,
time out after 30 seconds, and close consumed descriptors on every result.
Revocation and activity retirement may run on host callback threads; atomic grant
state protects publication while descriptor ownership stays on the storage owner
thread. The host drains callbacks before destroying the service and retires old
Activity grants before admitting replacement grants. Persisted grants survive
activity retirement but remain revocable. Admission has a fixed 256-document
process budget; the host must destroy/recompose storage at a quiescent boundary
after exhaustion rather than grow an unbounded personal-source registry.

App-private/cache publication holds the destination's native writer lock, removes
abandoned same-directory staging data, checks capacity, writes durably, then uses
`AtomicReplaceTracked`. The caller retains the receipt across errors: a committed
replacement with failed directory durability must never be reported as rollback.
Interrupted staging remains recoverable on the next locked publication. Cache
publication has these file-operation guarantees but cache eviction still makes it
unsuitable as durable user-state authority. Packaged/document publication fails
explicitly; Android document providers are not assumed to support atomic replace.

Host regressions exercise real POSIX files/pipes and native durable transactions,
with injected pressure/interruption/postcommit faults. They do not qualify Java
grant acquisition, packaged reads on a physical APK, provider disconnect, or an
Android device tuple. Those require a composed Android host/NDK/device run.

## Toolchain, Packaging, And Deployment

Android builds use explicit CMake/toolchain presets and declared SDK/NDK
requirements. Preflight validates tool versions, API/ABI compatibility, native
dependencies, signing profile availability, and runtime asset inputs.

Package assembly is deterministic for identical inputs and records provenance,
ABI contents, native libraries, runtime assets, permissions, and manifest
features. Machine-specific SDK paths and credentials never enter committed
metadata.

OpenXR-enabled packages additionally record the Horo backend contract, loader artifact
identity/digest/provenance and loader source policy. They do not package a vendor runtime
or development API layer as an implicit dependency. Non-XR Android profiles contain no
OpenXR loader/backend artifact or runtime probe.

The private OpenXR native owner accepts the Android host's Java VM, a global
reference to the process application Context and a global reference to the
selected Activity. The host manages JNI attachment and reference destruction.
It retains the Context through loader unload and the Activity through native
instance retirement. Activity replacement requires a new native owner after the
old instance has retired.

The platform-private `AcquireGameActivityJniApplication` operation reads the
active GameActivity on its native control thread and acquires separate JNI
global reference owners for the application Context and Activity. The GameActivity
input bridge shares the same scoped thread attachment. Application composition
can retain the Context independently if the loader remains loaded after an
Activity exits. Final reference release obtains a JNI environment on the
releasing thread. The current Input and Vulkan profiles request no XR
application references during startup.

The native owner resolves `xrInitializeLoaderKHR` from the verified package
loader and passes the application Context before enumerating layers or
extensions. The selected Android product must require
`XR_KHR_android_create_instance`. Only after that policy is enabled does the
owner chain the exact Activity into instance creation. A missing host lease,
unavailable loader entry point or failed initialization prevents native
discovery.

The signed `qualification-openxr-development` APK compiles the same Android
loader initialization and Activity instance ABI used by `HoroXROpenXR`. Its
GameActivity queues one cold diagnostic on resume and consumes it after pending
commands. Pause, stop and destruction cancel a queued diagnostic. The installed
base APK's exact stored ABI image is checked against the compiled lock digest
and opened by descriptor and offset. The diagnostic reports the HMD system
query and runtime version only after successful instance retirement. Native
session and graphics activation require separate product composition and device
qualification.

If native instance destruction fails, the Activity keeps the exact instance,
JNI references and loader for retry on its control lane. The recorded
diagnostic stays failed even if a later retry succeeds. Host exit polls the
retained owner within a 30-second retry window. If retirement remains uncertain,
the process terminates with the failed ownership still retained.

Developer deployment selects a device explicitly. Install, launch, log capture,
termination, timeout, and disconnect use argv-based process APIs and bounded
output. Multiple connected devices never cause implicit target selection.

For the qualification packages, `python scripts/project.py android-deploy`
takes the existing signed package output directory, declared profile, Android
SDK and a specific ADB serial. It verifies the staged APK, the activity
component and the installed byte digest before launching. It selects the
device's current ADB transport and Android user explicitly for install and launch. A
replacement connection requires a new invocation. Missing,
unauthorized, disconnected or incompatible targets return portable failure
codes and no automatic device retry or downgrade.

Deployment can require the expected host and connection explicitly. The
report separates the requested mode from the selected ADB inventory's USB,
emulator or TCP/IP classification. Ambiguous inventory cannot satisfy an
explicit connection request. The signed APK, its embedded provenance digest
and source revision, the renderer declaration and packaged ABIs identify the
artifact. The provenance includes the producer's Java dependency inventory
and the checked-in dependency lock digest.

The OpenXR deployment diagnostic requires explicit host, connection, device
and `--runtime openxr` selection. The command accepts a single closed marker
from the verified cold process, validates its runtime result and ABI, and stops
that exact diagnostic process before reporting `diagnostic_complete`. The
observation records only the diagnostic stage, native result and numeric
runtime version. The report keeps physical XR qualification separate.

The command emits one JSON record. Process evidence is restricted to the
matching package UID, PID and kernel start ticks. The log query accepts only
entries timestamped after the device clock sample immediately before launch and
through the sample after capture. The host's monotonic interval bounds both
samples. Unavailable timestamps or a detected clock discontinuity yield a
partial result. The process identity is rechecked before accepting the capture.
Only closed, profile-specific facts leave the tool. The input-only package
records entry after the GameActivity callback and input bridge are ready.
The Vulkan qualification package must report both lifecycle and renderer
counters before the host marks the deployment complete.
The host waits for the applicable evidence or a crash marker within the same
deadline. Cancellation, unverifiable installation, missing evidence and
crashes retain a partial result. Before
replacing an APK, the command checks whether its package already has a process.
After an attributed launch, failure gives cleanup its own five-second deadline.
Cleanup checks the selected transport, installed digest and process identity
before requesting a stop for the exact package and user. The JSON reports
stopped, already_exited or unconfirmed alongside the deployment error.
The first Activity attests to a cold launch with a single-use random marker
and the process's kernel start ticks. Ordinary launch and recovery after an
interrupted start both require one matching record from the candidate UID and PID
inside the device clock window. The host rechecks the live process incarnation
after reading the receipt and before stopping it. An older APK that emits only
the nonce needs a rebuild before it can produce attributable launch evidence.
The receipt is never included in the deployment report. Warm or ambiguous
responses and disconnected transports can leave cleanup unconfirmed.
A successful operation leaves the app running. The renderer declaration
comes from the package. Renderer counters describe
observed acceptance, with GPU and physical presentation completion outside
this diagnostic contract.

Pass `--diagnostic-bundle` with a new absolute `.zip` path in an existing
ordinary directory to retain the same portable report, including partial
outcomes. The ZIP contains `android/deployment.json` and a digest manifest
with the report's exact byte count and SHA-256. Publication uses the same
filesystem and refuses to replace an existing destination. The filesystem
must support hard links. The report and archive exclude raw logcat output,
device serials, UIDs, PIDs, launch receipts and installed paths. An archive
write failure leaves the recorded deployment outcome intact and returns a
portable `bundleError` alongside it. A successful ordinary deployment keeps
its app running even if the later archive write fails.

## Diagnostics And Crash Evidence

Android diagnostics may collect:

- application/package/build identity;
- OS, API level, ABI, device class, and renderer identity;
- lifecycle and native-window generations;
- permission/capability states;
- thermal, memory-pressure, battery, refresh, and frame-timing summaries;
- process-scoped Horo/platform logs and crash markers.

They exclude credentials, unrelated device logs, raw camera/audio, continuous
tracking, and personal storage paths unless a separate explicit consent policy
admits them. Disconnect or crash during capture produces partial evidence with
an honest completion state.

## Resource And Performance Policy

Thermal, memory, battery, display-mode, and background restrictions are
immutable snapshots with monotonic revisions. Runtime and Renderer decide how
to adapt through their own typed policies; Platform does not silently mutate
authored quality settings.

Frame-hot publication is bounded and allocation-conscious. Native callbacks do
not perform blocking I/O, formatting, or engine-wide locking. Long-running
pressure tests report the device, build, renderer, refresh mode, workload, and
before/after metrics.

## Standalone XR Dependency

Standalone Android-class headset qualification requires both an admitted
Android host tuple and an admitted XR tuple:

```text
Android OS/API/ABI/device/renderer evidence
                    +
headset/runtime/connection/profile/extensions evidence
                    =
qualified standalone XR tuple
```

PC Link, wireless streaming, or another tethered mode is separate evidence and
does not qualify the standalone Android path.

## Validation

Required coverage includes:

- cold launch, background/foreground, duplicate callbacks, activity recreation,
  configuration change, native-window loss/replacement, and shutdown;
- permission grant, denial, permanent denial, revocation, and settings return;
- touch/gamepad neutralization and sensor capability loss;
- app-private, packaged, cache, and user-granted storage behavior;
- missing SDK/NDK, unsupported API/ABI, package inspection, install/launch,
  multiple devices, disconnect, timeout, and log redaction;
- memory pressure, thermal throttling, refresh changes, low battery, and recovery;
- at least one reproducible physical-device tuple for each declared ABI/renderer
  path.

Deterministic host fakes cover lifecycle ordering and failures, but they do not
replace physical-device qualification.

## Related Documents

- [System Design](./system-design.md)
- [Platform Abstraction](./platform-abstraction.md)
- [Runtime Lifecycle](../runtime/runtime-lifecycle.md)
- [Rendering Architecture](../runtime/rendering-architecture.md)
- [Input Architecture](../runtime/input-architecture.md)
- [Asset Pipeline](../runtime/asset-pipeline.md)
- [XR Architecture](../runtime/vr-ar-architecture.md)
- [Application Security](../security/application-security.md)

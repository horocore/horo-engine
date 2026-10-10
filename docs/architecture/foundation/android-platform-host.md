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

Developer deployment selects a device explicitly. Install, launch, log capture,
termination, timeout, and disconnect use argv-based process APIs and bounded
output. Multiple connected devices never cause implicit target selection.

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

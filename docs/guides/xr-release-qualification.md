# XR 1.0 release qualification

Use the [XR device smoke procedure](xr-device-qualification.md) for each
declared release tuple. The release matrix must include a standalone Reference
tuple and a PCVR Reference tuple. PCVR may be tethered or wireless streaming.
Declare both separately when offering both modes. Qualify every additional
support claim against its own package and physical result.

## Reproduce the examples

Start with the [XR simulator sample](../../examples/xr-simulator/README.md).
Use its normal Play path, the focus and session-loss scenario, a fresh session
reset, and Stop Play. The simulator evidence covers synthetic Input and owner
lifecycle. Use the focused examples below to observe capability boundaries.
Run them from a build configured with `BUILD_TESTING=ON`.

| Example | Run | Observe |
| --- | --- | --- |
| Controller input | `HoroXRActionBindingTests "[unit][xr][input]"` | Submit a scalar sample through the existing Input cutoff. Replace the session and verify stale provenance is refused. |
| Hand tracking | `HoroXRApiTests "[xr][hands]"` | Inspect the bounded joint snapshot and exact gesture policy. Inject tracking loss and verify that the next snapshot has no stale joints. |
| World-space UI | `HoroRuntimeUiCanvasSpaceTests "[runtime_ui][canvas]"` | Resolve a logical world canvas. The fixture requires external projection before pointing at it. |
| Comfort | `HoroXRCharacterLocomotionTests "[xr][character][locomotion]"` | Exercise movement and recenter ownership, reject unsupported input, and check that shutdown withdraws comfort hints. |

These examples exercise deterministic or simulator paths. For physical smoke,
repeat the corresponding flows on the selected device with the release package.
Use the setup view or package probe to confirm the chosen runtime, renderer and
permissions. If a requested capability is unavailable, retain its typed error
and follow the admitted fallback for that product. For missing display data,
check the runtime's enumeration support and record an explicit limitation. If
tracking stops, verify neutral input before recovering or replacing the session.
Keep exported evidence redacted and scoped to the permission and consent that
were active during the run.

## Reproduce OpenXR backend checks

On Linux, configure the build with testing, Editor, Vulkan, and the OpenXR
backend enabled. Build the native application fixture and terminal launcher.

```sh
cmake --build build --target HoroRenderVulkanTests horo-engine
build/tests/HoroRenderVulkanTests "[xr][application]"
ctest --test-dir build -R '^HoroXRPackageLaunchContract$' --output-on-failure
```

Use the configured build directory in all three commands. The installed
application fixture exercises the native dispatch path with a scripted
runtime. It covers session creation, frame admission, event backlog, loss,
replacement, and teardown. The terminal test covers package ownership,
receipt validation, and process retirement. Retain these results as
deterministic fixture evidence. Record unavailable runtime or headset
combinations as `NotRun` in the device matrix.

## Assemble the matrix

Prepare a version 1 JSON matrix with these fields.

| Field | Contents |
| --- | --- |
| `schemaVersion` | Integer `1`. |
| `sourceRevision` | Exact lowercase Git object ID of the release source, with 40 or 64 hex digits. |
| `reviews` | An object with `foundation`, `backend`, `input`, `renderer` and `interaction` keys. Each review has `owner`, `status`, `sourceRevision`, `evidenceLevel` and `artifactSha256`. The owner accepts results against that source revision, including relevant normal, unsupported, replacement and shutdown behavior. Keep deterministic, simulator, runtime and physical levels labeled in the review. |
| `tuples` | An array of rows with a unique `id`, planning `category`, and named `owner`. Each row also has an explicit `releaseClaim` boolean, a `declaration`, a `requiredOptionalChecks` array and a `recordSha256`. |

Each claimed row has a complete smoke `declaration` with `tuple` and
`testPlan`. Use `requiredOptionalChecks` to require `haptics` and
`optionalMR` when the product claims them. When requiring `optionalMR`,
add the exact `mixedRealityCapabilities` list to the declaration.
Each claimed capability must have its OpenXR route in the tuple's admitted
extensions. Passthrough requires `XR_FB_passthrough`. Native anchors require
`XR_MSFT_spatial_anchor`, and persistence also requires
`XR_MSFT_spatial_anchor_persistence`. Plane tracking and plane-backed hit
testing require `XR_EXT_future`, `XR_EXT_spatial_entity` and
`XR_EXT_spatial_plane_tracking`. Light estimation requires
`XR_ANDROID_light_estimation`.
An unrun claim has a null
`recordSha256` and blocks the gate. A Conditional or Future planning row
uses `releaseClaim: false`. Its declaration and record may be null while
the device owner investigates.

Every claimed smoke record must include `provenance.sourceRevision` from the
tested package's build provenance. It must match the matrix's exact source
revision. Older records without this field remain readable by the device
validator. To qualify them for a release, recover the revision from the original
build provenance or repeat the device test with a traceable package.

For each passing display check, store a separate runtime display capture as
JSON. Use `schemaVersion: 1`, the smoke record's exact `tuple` and `provenance`,
and the observed `refreshMilliHertz` and ordered `viewExtents` arrays. Reference
its digest in both `display.evidenceSha256` and the `displayEnumeration` check.
The release checker compares the values and rejects captures from another run
or connection mode. If an older record points to a generic artifact, transcribe
the original runtime capture into this format or repeat the display check.

Each claimed row also has `renderTimingLimits` with `minimumDurationNs`,
`minimumFrameCount`, `maximumPeakFrameIntervalNs`, `maximumMissedDeadlines`,
and `maximumSwapchainFailures`. Use the limits from the declared test plan.
The `renderTiming` check references a separate JSON capture with `schemaVersion: 1`,
the smoke record's exact `tuple` and `provenance`, and these integer measurements:

| Field | Measurement |
| --- | --- |
| `targetRefreshMilliHertz` | Selected rate from the run's display capture. |
| `durationNs` | Measured elapsed time of the sample window. |
| `frameCount` | Number of frames measured in that window. |
| `peakFrameIntervalNs` | Longest frame interval observed. |
| `missedDeadlines` | Number of measured frames that missed the deadline. |
| `swapchainFailures` | Number of measured frames with a swapchain failure. |

The release gate checks the measurement bounds and every declared limit.
For older records, use the original run's timing measurements to create the
capture and update the check digest, or rerun the timing check with the declared
limits. Planning rows need no timing limits until they are claimed.

When a release requires `optionalMR`, store a redacted physical-device result
as JSON with `schemaVersion: 1`, `evidenceLevel: "PhysicalDeviceQualification"`,
and the smoke record's exact `tuple` and `provenance`. Add a `capabilities`
object keyed by the names in the declaration. Allowed names are
`passthrough`, `spatialAnchors`, `spatialAnchorPersistence`, `scenePlanes`,
`environmentHitTesting`, and `lightEstimation`. Each entry contains boolean
`operation`, `consentDenial`, `revocation`, `fallback`, and `shutdown`
results. Live anchors also require `relocalization`. Persistent anchors
also require `restore` and `staleLocalization`. Record the passing results
as `true` and reference the JSON digest in `checks.optionalMR.evidenceSha256`.
Record failed or unavailable device work in the smoke check status. Keep raw
images, geometry, gaze, anchor tokens, and diagnostic payloads out of this record.
The release owner verifies the underlying observations before approving a claim.

The packaged PCVR route requires Windows or Linux with Vulkan and a
successful Desktop OpenXR preflight receipt. Use Windows or Linux for
`hostPlatform` and keep the operating system version in `hostVersion`.
Keep other PCVR combinations in planning rows until the package supports
their host and renderer. Store the exact product stdout from
`horo-engine --probe-xr-package ... --connection tethered` or
`--connection wireless-streaming` and set the matrix row's
`desktopPreflightSha256` to its SHA-256 when the package receipt
is a separate file. In that case, set `desktopComponentSha256` to the verified
installed OpenXR component digest from the package provenance. The gate compares
this digest with the component reported by the preflight. If the tuple already
refers to the preflight receipt as its `packageReceiptSha256`, both fields can
be omitted. Set
`desktopPreflightSha256` to null to record an unrun preflight. The gate
reports such claims under their assigned owner. The receipt must match the
same executable, runtime and HMD, runtime version, host, renderer, and declared
connection mode as the physical evidence. Generate a separate receipt for each
connection mode. Version 3 appends a bounded `display=` field containing a
refresh rate count, ascending rates in millihertz, a view count, and ordered
width and height pairs. A zero rate count means enumeration was unavailable.
The XR 1.0 matrix requires exactly two ordered views in the physical display
capture and in a version 3 Desktop receipt. Version 2 receipts remain usable
for setup evidence because they carry no view measurements. The single-device
checker can also validate four-view measurements for post-1.0 work.
Each release claim requires the remaining physical checks for its declared
connection mode.

For a standalone Android claim, an optional `androidPreflight` object can
attach the exact JSON stdout from a successful `project.py android-deploy
--runtime openxr` invocation. It contains `receiptSha256` for those stdout
bytes, `apkSha256` for the independently selected diagnostic APK,
`deviceBuildSha256` for the Android build fingerprint recorded independently
for the physical run, the diagnostic `profile`, and `adbConnection` (`usb`
or `tcpip`). Store the diagnostic APK and receipt in the same evidence
directory as the physical record. The checker verifies the source revision,
package and device build, connection, runtime version and process retirement.
When deployment uses `--expect-runtime-version`, the checker verifies both the
selected version and the observed version against the physical run's tuple.
When the cold OpenXR instance can enumerate the primary stereo recommendations,
the receipt includes both recommended widths and heights in runtime order.
Those recommendations provide setup context. The physical run must still
capture session display modes, refresh rates and view extents for its own tuple.
Android deployment also checks that the foreground user is consistent before
installation, launch, and evidence publication. A user switch invalidates the
run. The receipt omits the Android user number.
The OpenXR deployer checks the Android kernel boot timestamp before installation,
launch, cleanup and receipt publication. A changed timestamp invalidates the
run even when the adb target and system build are unchanged. Cleanup stops
only a process attributed to the original boot. The receipt omits the timestamp.
Cleanup rechecks the selected ADB transport immediately before stopping the
attributed process. A transport mismatch prevents the stop request and the
deployment receipt stays partial.
The physical smoke record must contain `provenance.androidDeviceBuildSha256`
from the observed Android system build. It must match the row's
`deviceBuildSha256` and the discovery receipt. Legacy smoke records remain
usable for claims without an attached Android preflight.
The checker accepts only the typed fields emitted by the redacted deployment schema.
Unknown or invalid fields are rejected without printing their contents.
The cold-discovery APK has its own digest. The smoke record still identifies
the exact product binary, headset, physical checks and package receipt.

Store the exact tested executable, package receipt, physical smoke record,
every referenced check observation, and each subsystem review as files in a
local evidence directory. Name each file after the lowercase SHA-256 of its
contents. Preserve the artifact provenance and original format with the run.
The CLI reads JSON inputs up to 64 KiB and verifies evidence artifacts up to
512 MiB.

```sh
python scripts/qualify_xr_release.py xr-release-matrix.json --evidence-dir xr-evidence
```

The command reports missing, stale and failed reviews and tuple results with
their owners. Passing the command verifies the matrix and referenced bytes.
The release qualification owner reviews the captured observations, grants
approval, and records known limitations before publishing a support claim.

## Apple Vision Pro interoperability decision

The Apple Vision Pro target stays in **Future investigation** under
[XRA-007.26](https://github.com/horocore/horo-engine/issues/2285). The
investigation reviewed vendor documentation, the OpenXR conformance registry,
and Horo's platform targets on 2026-11-26. It found a conformant host runtime
for streaming, a separate Apple-native development path, and no qualifying
Horo device result for either path.

| Path | Evidence and Horo decision |
| --- | --- |
| Native Horo app on visionOS | Apple documents a custom Metal renderer driven through Compositor Services, with ARKit providing tracking. Horo's [Metal baseline](../adr/030-metal-platform-and-feature-baseline.md) covers a different platform scope. A visionOS port needs its own signed host, package, lifecycle, tracking and renderer integration. Status: unsupported in the current Horo product. [Apple's custom renderer guide](https://developer.apple.com/documentation/CompositorServices/drawing-fully-immersive-content-using-metal). |
| Native OpenXR runtime on visionOS | The Khronos registry reviewed for this decision lists the Vision Pro streaming entry on a Linux x86-64 host with Vulkan. It lists no on-device Vision Pro OpenXR tuple. The ratified Metal graphics binding specifies how a runtime exchanges Metal resources. An OpenXR loader and that extension alone cannot establish runtime availability. Status: unsupported in the current Horo product. [Khronos conformance registry](https://www.khronos.org/conformance/adopters/conformant-products/openxr), [Metal binding specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XR_KHR_metal_enable). |
| Host OpenXR over CloudXR | Khronos lists the NVIDIA CloudXR Runtime streaming-to-Vision-Pro tuple for OpenXR 1.0 and 1.1 on Linux x86-64/Vulkan. NVIDIA documents a visionOS client and Apple documents a foveated client path. The Horo application would run on the host. The headset would receive frames and supply admitted input over the stream. Status: first interoperability candidate, pending Horo qualification. [NVIDIA's CloudXR runtime guide](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_runtime/getting_started.html), [Apple's foveated streaming guide](https://developer.apple.com/documentation/foveatedstreaming/streaming-a-cloudxr-application-to-apple-vision-pro-with-foveation). |
| Compatibility apps and community streaming | A compatible iPad or iPhone app runs in a window. ALVR describes a separate PC/SteamVR streaming path for Vision Pro. Neither supplies a Horo qualification record. Any ALVR test requires its own host runtime, driver and client tuple and cannot inherit the CloudXR result. Status: outside the current Horo release scope. [Apple's compatibility guide](https://developer.apple.com/documentation/visionos/determining-whether-to-bring-your-app-to-visionos), [ALVR project](https://github.com/alvr-org/ALVR). |

Keep planning rows free of unobserved runtime and device values. This row
registers the streaming investigation without creating a release claim.

```json
{
  "id": "vision-pro-cloudxr",
  "category": "Future",
  "owner": "xr-platform",
  "releaseClaim": false,
  "declaration": null,
  "requiredOptionalChecks": [],
  "recordSha256": null
}
```

For a CloudXR follow-up, start with the documented Linux x86-64/Vulkan
conformance entry. Record the exact headset model and visionOS build, client
and host runtime versions, host OS, loader, renderer and driver, network
connection mode, interaction profile, admitted extensions, signed Horo
package, and test-plan revision. Use the [device smoke procedure](xr-device-qualification.md)
to record runtime-enumerated views and refresh rates, tracking and input
neutralization, measured stream and render timing, comfort, and disconnect
and recovery. Exercise optional capabilities only when advertised and
admitted. Qualify each alternate host or client path independently.

Reopen the native OpenXR decision when a reproducible on-device conformant
runtime and graphics binding are available. Evaluate a native Apple-framework
port as a separately scoped backend against Horo's
[XR contracts](../architecture/runtime/vr-ar-architecture.md), including
session lifecycle, per-view rendering, tracking and input, permission
withdrawal, and signed packaging. Reject a release claim when the proposed
path lacks its required runtime, cannot meet those contracts, or lacks the
physical evidence for its exact tuple.

## Open qualification work

| Evidence | Status | Owner |
| --- | --- | --- |
| 1.0 standalone reference declaration and matching physical run | NotRun | Release qualification and standalone platform owners |
| 1.0 PCVR reference declaration and matching physical run | NotRun | Release qualification and desktop OpenXR owners |
| Other advertised modes and device tuples | NotRun | Device qualification owners in [XRA-007](https://github.com/horocore/horo-engine/issues/2166) |
| Integrated release package smoke, timing and recovery | NotRun | Release package, renderer and OpenXR backend owners |

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
`optionalMR` when the product claims them. An unrun claim has a null
`recordSha256` and blocks the gate. A Conditional or Future planning row
uses `releaseClaim: false`. Its declaration and record may be null while
the device owner investigates.

Every claimed smoke record must include `provenance.sourceRevision` from the
tested package's build provenance. It must match the matrix's exact source
revision. Older records without this field remain readable by the device
validator. To qualify them for a release, recover the revision from the original
build provenance or repeat the device test with a traceable package.

The packaged PCVR route requires Windows or Linux with Vulkan and a
successful Desktop OpenXR preflight receipt. Use Windows or Linux for
`hostPlatform` and keep the operating system version in `hostVersion`.
Keep other PCVR combinations in planning rows until the package supports
their host and renderer. Store the exact product stdout from
`horo-engine --probe-xr-package ... --connection desktop-local` and set the
matrix row's `desktopPreflightSha256` to its SHA-256 when the package receipt
is a separate file. In that case, set `desktopComponentSha256` to the verified
installed OpenXR component digest from the package provenance. The gate compares
this digest with the component reported by the preflight. If the tuple already
refers to the preflight receipt as its `packageReceiptSha256`, both fields can
be omitted. Set
`desktopPreflightSha256` to null to record an unrun preflight. The gate
reports such claims under their assigned owner. The receipt must match the
same executable, runtime and HMD, runtime version, host, and renderer as
the physical evidence. The release claim still requires the underlying
physical qualification checks for the declared connection mode.

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

## Open qualification work

| Evidence | Status | Owner |
| --- | --- | --- |
| 1.0 standalone reference declaration and matching physical run | NotRun | Release qualification and standalone platform owners |
| 1.0 PCVR reference declaration and matching physical run | NotRun | Release qualification and desktop OpenXR owners |
| Other advertised modes and device tuples | NotRun | Device qualification owners in [XRA-007](https://github.com/horocore/horo-engine/issues/2166) |
| Integrated release package smoke, timing and recovery | NotRun | Release package, renderer and OpenXR backend owners |

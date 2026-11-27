# XR device smoke tests and compatibility records

[XRA-007.7](https://github.com/horocore/horo-engine/issues/2173) defines the shared
record and procedure for XR qualification. [ADR-163](../adr/163-xr-tooling-diagnostics-privacy-and-qualification-ownership.md)
assigns release approval to the release qualification owner.

## Declare the tuple

Use one record per exact product binary, package receipt, headset model, runtime
and version, loader and backend version, operating system, renderer and driver,
device class, connection mode, current interaction profile and admitted OpenXR
extensions. Record admitted extensions in sorted order from host evidence.
Repeat the procedure for each tethered, wireless-streaming or standalone mode
the product declares. A runtime, driver, extension, test plan or binary change
requires a new exact declaration and matching evidence.

XR Setup shows the rates and ordered view sizes measured by its session test.
The session test can inform setup and troubleshooting. Record physical display
measurements for the exact product run in the release display capture.

Use the existing package inspection and launch receipt to populate package and
loader provenance. Hash that receipt into `packageReceiptSha256` and the exact
tested binary into `buildSha256`. Package provenance and loader discovery can
support the setup check. Use the physical headset for every required smoke check.
See [ADR-158](../adr/158-openxr-loader-backend-packaging-and-host-composition.md)
for loader provenance and packaging rules.

| Category | Use in planning |
| --- | --- |
| Reference | A declared reference tuple targeted by the release plan. |
| Compatibility | An additional tuple targeted for compatibility checks. |
| Enterprise | A tuple with a deployment or enterprise runtime requirement. |
| Conditional | An investigation that depends on an external capability or provider. |
| Future | A tuple being tracked for a later product decision. |

## Run the checks

Start from the declared package on a fresh session. Record the test-plan revision
and a run ID. Keep evidence under that run ID with a content digest for each
observed check. Use approved and redacted captures. Report stable issue keys or
reason codes for unsuccessful or limited checks. Omit serial numbers, personal
identifiers, raw poses, biometric signals and physical room geometry from the
record. Exporting evidence follows the product's consent and retention rules.

For standalone Android, record `provenance.androidDeviceBuildSha256` when the
release plan pairs this run with a cold-discovery receipt. Capture the active
system build during the physical run. Use the same SHA-256 encoding as the
deployment probe, hashing the ASCII `ro.build.fingerprint` value without its
trailing newline. Retain only the digest in the smoke record.

| Check | Record the observed behavior |
| --- | --- |
| `setup` | Launch with the selected package and XR configuration. Confirm the active system, renderer, permissions and admitted extensions. |
| `input` | Exercise grip, aim, select and supported analog controls under the active interaction profile. Disconnect a controller and verify neutralization before reconnecting. |
| `displayEnumeration` | Capture runtime-enumerated refresh rates in integer millihertz and recommended width and height for each ordered view in the selected runtime view configuration. Preserve asymmetric extents. An unavailable refresh enumeration must be reported as a limitation. |
| `renderTiming` | Run a repeatable scene at an enumerated setting. Record the timing window, frame interval, missed frames or deadlines and swapchain behavior against the test plan's thresholds. |
| `comfort` | Exercise recenter, the chosen movement and turning modes, and the boundary and reduced-motion behavior declared by the product. Record the active settings. |
| `haptics` | Verify pulse delivery and cancellation on focus loss or controller removal if the product declares haptics. |
| `optionalMR` | Verify only the declared MR capabilities with consent, revocation and the planned fallbacks. |
| `lifecycle` | Observe ready, focused, hidden or unfocused, resumed and clean shutdown. Confirm provider and swapchain ownership is released. |
| `recovery` | Exercise the declared loss or runtime replacement procedure. Verify stale input is neutralized and a failed restart or the new session is reported. |

`horo.xr.accepted_projection_interval` reports host elapsed seconds between
consecutive OpenXR projection acceptances. An empty frame, a native failure,
or compositor reset ends the interval sequence. The metric uses no device or
session labels, and measures host submission cadence. Physical display timing
and missed deadlines still require the observations specified by the test plan.

The record keeps runtime evidence at the level actually observed.
`StaticContract`, `DeterministicFake` and `SimulatorReplay` may document
repeatable checks. `RuntimeCompatibility` records a real runtime and host
session. `PhysicalDeviceQualification` records the physical smoke procedure.

Use `Passed` for completed passing checks, `PassedWithLimitations` for an
observed limited outcome, `Failed` for an observed failure, `Blocked` for
a run prevented by a prerequisite and `NotRun` for a pending run. A missing
headset is `Blocked` or `NotRun` depending on whether the run was attempted.
A `PassedWithLimitations` record requires at least one passing observation
and a documented limitation. A failed check requires a `Failed` record,
including failures in optional checks that were exercised.
If a runtime cannot enumerate refresh rates, leave the refresh list empty and
record the reason on the display check. Use its recommended width and height
values if the view enumeration succeeded.

## Record and check eligibility

Version 1 is a UTF-8 JSON record validated by the repository script. Each
record includes every check. Unobserved tuple values can be `null` until a
physical run is recorded. The example is a planned standalone run.

```json
{
  "schemaVersion": 1,
  "category": "Reference",
  "evidenceLevel": "PhysicalDeviceQualification",
  "status": "NotRun",
  "tuple": {
    "buildSha256": null,
    "packageReceiptSha256": null,
    "headsetModel": null,
    "runtime": null,
    "runtimeVersion": null,
    "loaderVersion": null,
    "backendVersion": null,
    "hostPlatform": null,
    "hostVersion": null,
    "renderer": null,
    "driverClass": null,
    "driverVersion": null,
    "deviceClass": null,
    "connectionMode": "Standalone",
    "interactionProfile": null,
    "admittedExtensions": []
  },
  "provenance": {"testPlan": "xr-smoke-v1", "runId": null},
  "checks": {
    "setup": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "input": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "displayEnumeration": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "renderTiming": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "comfort": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "haptics": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "optionalMR": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "lifecycle": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"},
    "recovery": {"status": "NotRun", "evidenceSha256": null, "issue": "awaiting-device"}
  },
  "display": null
}
```

A completed display observation has `evidenceSha256`, the same capture
digest used by the display check, `refreshMilliHertz`, a sorted distinct array
of rates, and `viewExtents`, an ordered array of objects with integer `width`
and `height`. A passing check has a digest and a null `issue`. A limited or
failed observation has a digest and a stable `issue`.

```sh
python scripts/qualify_xr_device.py smoke.json
python scripts/qualify_xr_device.py smoke.json --expect release-tuple.json --require haptics
python scripts/qualify_xr_device.py smoke.json --expect release-tuple.json --display-capture display.json
python scripts/qualify_xr_device.py smoke.json --expect release-tuple.json --desktop-preflight desktop-preflight.txt
```

The release declaration is an object with a complete `tuple` and exact
`testPlan` string, prepared independently by the release owner. The
second invocation requires a passing physical record and passing checks
for setup, input, display enumeration, timing, comfort, lifecycle and recovery.
Pass `--display-capture` to verify the actual display capture locally. The
checker matches its digest, exact tuple and run identity, enumerated refresh
rates, and ordered view extents to the smoke record. The release matrix
performs this capture check for every claimed tuple.
To verify frame timing before assembling the release matrix, pass both the
timing capture and a JSON file containing the release plan's `renderTimingLimits`
object. Include the display capture so the checker can verify the timing target
against the refresh rates enumerated by the same physical run.

```sh
python scripts/qualify_xr_device.py smoke.json --expect release-tuple.json \
  --display-capture display.json --render-timing-capture timing.json \
  --render-timing-limits timing-limits.json
```

Add `--require haptics` when the release claims haptics. For MR, add
`mixedRealityCapabilities` to the declaration and pass `--require optionalMR`
with `--mr-capture <capture.json>`. The
[XR release qualification guide](xr-release-qualification.md) defines the exact
redacted record. Keep any limited optional results visible in the matrix.
Pass `--physical-check-evidence-dir <directory>` with `--expect` to verify the
setup, input, comfort, lifecycle, recovery and any required haptics receipts
against their SHA-256-named observation files. The checker verifies each receipt
against the declared tuple and the same smoke run. It also rejects check
receipts used as observations and JSON observations with another tuple or run.
Display, timing and MR captures satisfy their dedicated checks only. A separate
redacted observation may support several checks when it records each outcome.
For PCVR, `--desktop-preflight` validates the exact stdout receipt from
`horo-engine --probe-xr-package ... --connection tethered` or
`--connection wireless-streaming`, matching the tuple. Save the version 2
receipt bytes directly. When the probe receipt and product package receipt
are separate files, keep the product receipt's digest as
`packageReceiptSha256`. Pass the independently recorded probe digest and
installed OpenXR component digest for the separate receipt.
The launcher rejects a preflight if any child stdout line or the stdout byte
budget was truncated. A complete receipt requires the full captured stream
so the launcher can rule out conflicting records. It does not retain or
publish the child's stderr.

```sh
python scripts/qualify_xr_device.py smoke.json --expect release-tuple.json \
  --desktop-preflight desktop-preflight.txt --desktop-preflight-sha256 "$PREFLIGHT_SHA256" \
  --desktop-component-sha256 "$COMPONENT_SHA256"
```

When the probe receipt is also the package receipt, the checker uses
`packageReceiptSha256` by default.
Use the runtime-reported labels from the probe in `runtime` and `headsetModel`,
and record `runtimeVersion` as `major.minor.patch`. The checker also confirms
the executable digest, host, renderer, and declared connection mode. Verify the
transport during the physical smoke test.

For standalone Android, the same checker can match the cold-discovery receipt
and diagnostic APK against an independently prepared Android preflight
declaration and the physical smoke run.

```sh
python scripts/qualify_xr_device.py smoke.json --expect release-tuple.json \
  --android-preflight receipt.json --android-preflight-declaration android-preflight.json \
  --android-apk diagnostic.apk
```

Use the same object shape as `androidPreflight` in the
[release matrix guide](xr-release-qualification.md#assemble-the-matrix). The
checker verifies the exact diagnostic APK bytes and the receipt's source
revision, Android system build, OpenXR runtime version, declared ADB transport,
and process retirement. Cold discovery remains setup evidence. The physical
smoke record proves the on-device observations.
The APK digest and the contents of its reported native images are checked on
the same opened file. A changed APK invalidates the receipt check.
If the Activity exits before or during capture, the deployment tool checks the
installed APK, selected Android build, foreground user, transport and boot before
including the attributed crash in the report. A crash during capture must match
the admitted process ID and start time.
Before requesting a package stop, it also rechecks the process ID, UID and start time
after validating the cleanup transport and boot. If the process exits between
those reads, the tool accepts retirement only after confirming that no matching
process remains and that the transport and boot still match.

The release owner verifies the referenced captures and package receipt before accepting the
result under [XRA-007.8](https://github.com/horocore/horo-engine/issues/2174).
Use the [release matrix and example procedures](xr-release-qualification.md)
to verify all claims together.

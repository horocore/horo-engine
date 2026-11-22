# XR Foundation Contract Qualification

[XRA-001.8](https://github.com/horocore/horo-engine/issues/2116) qualifies the
backend-neutral foundation contract. Its scope covers the product profiles,
capability evidence, owner generations, coordinate and time domains, session and
frame transitions, and loss or shutdown. Release evidence levels and ownership
follow [ADR-163](../adr/163-xr-tooling-diagnostics-privacy-and-qualification-ownership.md).

## Reproduce the foundation matrix

Configure an XR-disabled graph to verify that applications can omit the native
backend and SDK. Configure a separate native-enabled build to exercise the
backend-neutral and OpenXR dispatch contracts together. The OpenXR tests inject
deterministic dispatch and resource adapters. The commands below use Ninja and
assume the intended platform compiler is on `PATH`.

```sh
cmake -S . -B build/xr-foundation-off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DHORO_BUILD_EDITOR_GUI=OFF -DHORO_ENABLE_IMGUI_UI_TESTS=OFF -DHORO_BUILD_XR_OPENXR=OFF -DHORO_VERIFY_XR_DISABLED=ON
cmake -S . -B build/xr-foundation -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DHORO_BUILD_EDITOR_GUI=OFF -DHORO_ENABLE_IMGUI_UI_TESTS=OFF -DHORO_BUILD_XR_OPENXR=ON -DHORO_BUILD_RENDER_VULKAN=ON
cmake --build build/xr-foundation --target HoroXRApiPublicHeaderConsumer HoroXRRuntimePublicHeaderConsumer HoroXRFakeRuntimeHostCompositionConsumer HoroXRInputBindingsPublicHeaderConsumer HoroXRApiTests HoroXRRuntimeTests HoroXRActionBindingTests HoroXROpenXRTests
ctest --test-dir build/xr-foundation -R "^HoroXR(Api|Runtime|ActionBinding|OpenXR)Tests::" --timeout 20 --no-tests=error --output-on-failure
```

| Contract | Evidence level | Reproducible exercise |
|---|---|---|
| Native XR omitted and headers remain target-owned | StaticContract | XR-disabled dependency-policy configure plus the XR API, Runtime, Input Bindings, and fake host consumers |
| Backend-neutral identity, time, spaces, tracking, and view shapes | DeterministicFake | `HoroXRApiTests` checks malformed and stale generations, composition and snapshot limits, and mono, stereo, and quad data. Unsupported production view configurations reject intact. |
| `XRProjection1_0` and `XRTrackedInteraction1_0` admission | DeterministicFake | `HoroXRRuntimeTests` exercises every required feature, checks all unavailable state categories under both profiles, and verifies explicit optional fallback or disablement, finite limits, and stale plan rejection. |
| Session, frame, and tracking transitions | DeterministicFake | `HoroXRRuntimeTests` checks wrong-thread use, invalid order, partial preparation rollback, cancellation, focus or tracking loss, runtime replacement, repeated Ready transitions, cleanup debt, and shutdown. |
| Canonical Input handoff | DeterministicFake | `HoroXRActionBindingTests` exercises the Input cutoff, owner and revision fencing, neutral state on profile loss, cancellation, and declared fallback. |
| Loader and native lifecycle boundaries | DeterministicFake | `HoroXRApiTests` exercises loader absence, system and runtime availability, and policy failure. `HoroXROpenXRTests` checks native dispatch order, partial enumeration, frame/image and action cleanup, and explicit retry after staged failure. |

Keep the configuration result and CTest output with the tested revision,
compiler, and platform. A native-enabled build with injected dispatch is still
deterministic fake evidence.

The XRA-001.8 run on Windows x64 with MSVC 19.44.35229 and CMake 4.3.1
in Debug passed the XR-disabled graph check and built all four listed consumers.
All 249 headless CTests passed (60 API, 63 Runtime, 14 Input Bindings, and
112 OpenXR dispatch tests). The run used no real runtime session or device.

## Open release evidence

These rows record evidence outside this foundation run. The release qualification owner
checks every required product tuple and its current evidence before signing off.
An unrun or blocked required row leaves that tuple unqualified.
Use the [shared XR device smoke protocol](xr-device-qualification.md) to collect
evidence for each declared physical tuple.

| Evidence | Status | Owner and work |
|---|---|---|
| Real OpenXR loader/runtime/host session | NotRun | Application composition and OpenXR backend owners exercise declared runtimes and platform tuples under [XRA-002.9](https://github.com/horocore/horo-engine/issues/2126). |
| Physical headset, controller, haptics, and input across declared profiles | NotRun | XR Input and device qualification owners record physical evidence under [XRA-003.9](https://github.com/horocore/horo-engine/issues/2136) and the device entries under [XRA-007](https://github.com/horocore/horo-engine/issues/2166). |
| Renderer/driver presentation, loss recovery, and timing on physical devices | NotRun | Renderer and OpenXR backend owners qualify external-image and compositor behavior under [XRA-004.9](https://github.com/horocore/horo-engine/issues/2146). |
| Tracked interaction, Runtime UI, and comfort in product composition | NotRun | Runtime UI, interaction, and accessibility owners qualify end-to-end behavior under [XRA-005.8](https://github.com/horocore/horo-engine/issues/2155). |
| Supported desktop and standalone product tuples, packaging, and release evidence | NotRun | Product platform and release qualification owners assemble the exact build, driver, connection, profile, capability, and device evidence under [XRA-007.8](https://github.com/horocore/horo-engine/issues/2174). |

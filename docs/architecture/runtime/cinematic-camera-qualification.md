# Cinematic camera qualification

Evidence scope is the CIN-003 delivery branch based on
`f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`. GitHub and Jira were read on
2026-11-11. Tracking status is recorded without changing external work items.

| Child | GitHub / Jira | Tracking status | Implementation and acceptance evidence |
|---|---|---|---|
| CIN-003.1 | #1700 / HORO-1659 | Closed / Done | ADR-119 authority contract is present in the normative cinematic architecture. Existing context, arbitration, handoff and frame-immutability tests are retained. |
| CIN-003.2 | #1723 / HORO-1682 | Closed / In Review | Existing runtime and editor hard-cut entry points are reused. Four-field camera keys retain their original hard-cut behavior. |
| CIN-003.3 | #1724 / HORO-1683 | Open / To Do | Authored integer duration, Linear/SmoothStep curve, Camera-owned quaternion/position/lens blend, explicit host capability admission and typed incompatibility/unsupported results are implemented. Required dual-view rendering remains unavailable pending frontend compositor and budget support. |
| CIN-003.4 | #1725 / HORO-1684 | Open / To Do | Publication resolves both endpoints from one current scene borrow. Added integration coverage uses existing CIN-001.5 transform sampling and a committed scene update for a root camera and parented camera during a blend. Native presentation transition qualification is pending. |
| CIN-003.5 | #1726 / HORO-1685 | Open / To Do | Runtime/editor parity, capability/fallback, malformed input, lens compatibility, cancellation and rebase regressions are added. Execution and native matrix evidence are pending. |

The direct predecessor of CIN-003.3 is CIN-003.2. CIN-003.4 is blocked by
CIN-003.3. CIN-003.5 has no recorded direct blocked-by relationship, and its
integrated acceptance needs the blend and presentation outcomes.

## Matrix

| Scenario | Evidence target | Current result |
|---|---|---|
| Hard cut, boundaries, stop, scene replacement, invalid targets, immutable frame | HoroCameraCutRuntimeTests | Pending execution |
| Linear and SmoothStep duration, tick/seek parity, reverse seek | HoroCameraCutRuntimeTests | Pending execution |
| Host denies single-view blend, explicit hard fallback, required dual-view denial | HoroCameraCutRuntimeTests | Pending execution |
| Compatible lens interpolation and rotation, incompatible projection rejection | HoroCameraCutRuntimeTests | Pending execution |
| Anchor-relative root/child scene publication during a blend | HoroCameraCutRuntimeTests | Pending execution |
| Actual editor controller extraction versus runtime selection | HoroCameraCutEditorIntegrationTests | Pending execution |
| Public header boundary and legacy initializer compatibility | HoroCameraCutPublicHeaderConsumer | Pending execution |
| Windows OpenGL camera blend and viewport resize rendering | HoroEditorViewportOpenGLSmoke | The retained Linear/SmoothStep player advances once across resize. Readback checks the new camera silhouette and a non-empty destination image. Pending execution. |
| Windows OpenGL native surface resize/suspension | HoroOpenGLSurfaceLifecycleSmoke | Existing real SDL surface test; pending execution |
| Windows Vulkan camera blend rendering | HoroEditorVulkanPresentationSmoke | Windows registration is added for the supported SDL host. The retained player advances across presented/cancelled frames. Execution and packaged shader inputs are pending. |
| Native display change and fullscreen during cinematic playback | Native GPU smoke | Pending execution |
| Metal/macOS | Native GPU smoke | Requires a macOS host and Metal device |
| Admitted dual-view cross-fade and budget exhaustion | Frontend/native composition | Requires an implemented two-view camera compositor and budget contract; CIN-003.3 remains open for this acceptance |

## Presentation source evidence

`EditorWorkspaceController::ResolveCameraCutPreview` republishes from the current
scene before each extraction. Preview extraction bypasses the scene-definition
cache while an active camera preview exists. The OpenGL viewport validates the
pass target and allocated extent in `ValidatePassRequest`. Vulkan validates both
in `PrepareStaticMeshPass`. `EditorPresentationSurface` owns the existing surface
generation and native realization transaction. These source checks establish
the intended seams. Native resize, display and fullscreen execution remains
required for CIN-003.4 and CIN-003.5 acceptance.

## Local validation

Formatting and `git diff --check` have run. Native build/test work is deferred
while the main Windows validation owns heavy-work priority. All later builds
must hold `scripts/dev.py`'s common-repository `heavy_build_lock`, use
`--parallel 1`, and set `CTEST_PARALLEL_LEVEL=1`.
The validation helper also checks its own open lock handle with `psutil` before
starting configure, compilation, CTest or analysis. A matching Git identity
alone does not establish that the physical shared lock is held.

The offline analysis doctor reports `ready_for_local_check` after resolving the
installed CMake/Ninja tool directories, the portable VS Code settings profile
and the existing Sonar extension directory. Final Sonar IDE, CLI secrets and
Codacy checks remain pending. Doctor is prerequisite evidence and does not
constitute source analysis or connected-mode authentication evidence.
The online doctor could not compare Codacy cloud configuration because its CLI
request exited 2. Configuration-drift verification remains incomplete.

Windows reports AMD Radeon graphics and an NVIDIA GeForce RTX 5080 with driver
32.0.16.1088. Device enumeration establishes availability for attempting native
tests. The Vulkan editor fixture additionally requires packaged viewport and
shadow shaders through `HORO_EDITOR_VULKAN_ASSET_ROOT`; shader tool/catalog
verification remains the production cook owner's responsibility.

`vulkaninfo --summary` reports loader 1.4.341 and RTX 5080 driver 610.88.
Its installed layer list omits `VK_LAYER_KHRONOS_validation`, which the native
Vulkan fixture requests. It also reports a missing third-party Epic overlay
manifest. Required validation stays enabled. A device-backed camera pass still
needs the validation layer and verified packaged shader inputs before it can
satisfy the Vulkan row.

# Android window and presentation integration

Delivery identity: HORO-2198 / #2259 [PLT-001.3]. Implementation and qualification
are in progress; the evidence below does not declare an Android device supported.

## Explicit composition and effective profile

`qualification-vulkan-development` selects `vulkan-android` before native window
admission. It uses API 29 or newer and declares `arm64-v8a` and `x86_64` as
development ABIs, with `distributionQualified: false`. The existing input/package
profiles keep their explicit `renderer: none` composition.

The new sibling backend uses Vulkan 1.1 render-pass, submission and fence
operations. Its effective development profile additionally requires actual
`VK_EXT_swapchain_maintenance1` support and feature enablement, its instance
dependencies, and one graphics queue that supports the actual Android surface.
Missing support returns a typed unsupported result. This narrower profile is not
a claim that every Android Vulkan Baseline device supports this implementation.

The implemented renderer operations are primary-output clear, presentation and
their lifecycle. Resource creation, material/geometry graphs, compute and other
unimplemented operations reject explicitly; their capability bits remain absent.
The desktop Vulkan 1.3 implementation and its shader profile are independent.

## Native ownership and generation authority

The existing `AndroidLifecycleController` and its bound `AndroidLifecycleAdapter`
remain the sole Activity, window and presentation identity authority. The private
`AndroidWindowHost` retains actual `ANativeWindow` references through NDK calls.
Its derived snapshot contains committed generations, pixel extent and a facts
revision. Android types never appear in an installed public header.

There are eight bounded native occurrences and 64 leases per occurrence. Creation
uses two cold allocations; successful observations and lease operations use fixed
storage. Loss immediately prevents new acquisition of that occurrence. An old
reference survives through the renderer's lease until genuine native retirement.
Loss followed by the same pointer issues a new lifecycle generation. Duplicate
observations of the same live occurrence are idempotent.

The application owns one GameActivity callback fanout. It preserves the existing
Input bridge, commits lifecycle observations, refreshes window facts, and polls
the explicitly registered renderer. Device initialization precedes native window
and presentation creation. A late loss for one generation cannot retire its
replacement. A zero extent admits no frame.

The native-free, app-owned `PresentationAdmission` helper reconciles actual output
readiness with the exact committed window facts. It refreshes the canonical
presentation generation after replacement or resize, including a changed receipt
while readiness remains true. Presentation readiness and Activity permission are
separate: frame execution additionally requires Running and Resumed states.

## Nonblocking acquisition and retirement

Availability polling observes the native output and advances retirement. It does
not acquire a swapchain image. Frame acquisition occurs only in `BeginFrame`;
exact typed backpressure skips that attempt without claiming a presented frame.
GPU credit exhaustion does not create a new presentation lifetime.

Rendering completion and presentation retention have separate fence proofs. An
unused acquired image is returned only after its acquisition and any accepted
rendering have completed. The old swapchain, surface and window lease remain
owned until all relevant debt retires. The backend never calls device/queue idle
as a normal-frame resize, replacement or shutdown shortcut.

After destroying an old surface tuple, the readiness poll returns unavailable
with that native output genuinely absent. A later poll imports the replacement;
the application can observe the real loss boundary even when window facts did
not change. This uses no synthetic busy timer or fabricated completion.

The maintenance extension's present fences cover presentation resource lifetime,
which differs from command completion. The Vulkan specification describes both
the [present-fence lifetime contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)
and [queued present operations, including expected surface errors](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).

## Build boundaries and current evidence

`HoroPlatformAndroidWindowInternal` and
`HoroRenderVulkanAndroidHostInterface` stage only their deliberate non-installed
headers. The application helper has its own private target. The Android
qualification graph compiles the same Foundation, RenderApi and backend-registry
source inventories and dependency pins as the full engine; it substitutes no
renderer or lifecycle implementation. Public header ownership is unchanged.

The portable window owner has passed 16 host protocol cases, and the existing
lifecycle suite has passed 11 cases, all without failures or skips. These tests
use an explicit native-reference ledger and do not execute an Android GPU.
The integrated host build passed. Five application-admission and 15 Vulkan
protocol cases also passed without failures or skips; these still use explicit
fake native operations. The first genuine NDK compilation passed the new Android
source files but found the shared API portability problems described below.
Renewed pinned NDK API 29 compilation and linking passed independently for both
ARM64 and x86_64. Package assembly and actual device lifecycle runs remain
separate pending evidence. Fresh host regression qualification passed 399 cases
across window, lifecycle, application admission, Vulkan protocol, graph inspection,
frontend and material binding suites, with no failures or skips. Affected
public-header consumers built. The package suite passed 35 tests, including actual
command dispatch of the new profile. These tests do not substitute for an Android
runtime run.

The qualification shared library exports only its GameActivity JNI initialization,
native Activity creation and native loop entry points. Engine implementation
symbols remain local to this application binary; it is not an engine SDK shared
library. The linker tracks the export map as an input. ELF metadata and dependency
closure passed the existing bounded package validator for both development ABIs,
including each ABI's pinned shared C++ runtime. API 29 provenance and 16 KiB load
alignment passed. Actual same-directory CMake reconfiguration from the Vulkan
development preset to `android-debug` removed the renderer recipes and host
Vulkan definition; the explicit renderers-disabled profile remains independent.

The package command accepts `--profile qualification-vulkan-development` and the
existing serialized Android package CI matrix includes it with both declared
development ABIs. That workflow has not yet qualified this source revision.

The pinned NDK 28.2.13676358 and CMake 3.31.6 are available locally. GameActivity
3.0.5 was checked against the repository's archive digest. Existing package
assembly policy remains Linux-only; a macOS native compile does not establish a
qualified Linux package or physical Android tuple.

## Shared Render API portability migrations

### Inspection cancellation

Graph inspection now uses the existing `Horo::CancellationToken` from Foundation instead of `std::stop_token`. The pinned Android NDK r28 libc++ does not provide the latter; this changes the actual API rather than introducing a standard-library shim, experimental library flags, or a disabled inspection path.

The changed signatures are `CaptureRenderGraphInspection`, `ExportRenderGraphInspection`, and `RenderFrameScope::CaptureInspection`. Pass `Horo::CancellationSource::Token()` and request cancellation with `RequestCancellation()`. Default arguments remain `{}` and mean no cancellation. Callers that explicitly spell function-pointer/member-pointer types must replace `std::stop_token` with `Horo::CancellationToken` and rebuild; no compatibility overload remains. Existing callers using the default argument need no source change. `RenderGraphInspection.h` directly includes the owning Foundation header; public-header ownership and target dependency direction are unchanged.

Cancellation remains polling-only at the same validation, bounded-copy and JSON-append checkpoints. Failure still returns the existing typed inspection cancellation outcome, no partial snapshot/export, and preserves the prior published snapshot. Token copies/polls do not allocate; cancellation sources remain explicit tooling-owned state. No frame automatically captures inspection. Unit tests retain default/pre-cancel/feed-preservation coverage and add inherited Foundation-token cancellation through capture/export. Two standalone consumers enforce the exact new free-function and frontend member signatures. This is a narrow graph-inspection migration: unrelated uses of standard stop tokens are unchanged.


### Flat material batches

Both `RenderMaterialPass::precedingPasses` and `RenderMaterialExecutionRequest::precedingPasses` now have type `RenderMaterialBorrowedBatch<ExactPass>` instead of `std::span<const ExactPass>`.

The former self-referential standard-span member instantiated `std::span` while its element type was incomplete. That violates the standard-span complete-element requirement; Android NDK r28.2 libc++ diagnoses the problem while declaring the enclosing implicit copy constructor. Adding a default member initializer or an anonymous union did not fix the actual NDK failure and would not establish standard conformance.

The replacement is a material-specific readonly borrowed flat batch with pointer/count storage. It owns no pass, resource, native object or retirement authority, and allocates nothing. It exposes only default-empty construction, const-compatible span/fixed-array construction, data/size/empty, readonly iteration and indexing. Constructors accepting standard spans are templates whose span element is deduced at the actual completed-type call site. Standard-span interoperation uses its ordinary contiguous/sized-range constructor after the pass type is complete. No raw pointer/count constructor is provided; no generic range framework or standard-library suppression is introduced.

Enclosing structs remain aggregates and preserve designated initialization. Ordinary existing std::span assignments and fixed-array initializers remain supported. Batch copies remain trivial and preserve the exact original pointer/count borrow. Existing admission limits, nonrecursive flat-batch rules, synchronous borrow lifetime and submitted-frame lease/resource pins are unchanged. This does not make a borrowed array survive caller storage destruction.

This is a deliberate public field-type and C++ ABI/source migration. Rebuild every engine, backend, app and consumer using MaterialBinding.h together; binary compatibility with previously compiled modules is not claimed. Code spelling the old exact member type must use the new batch type or obtain a standard span after the element is complete. No existing in-repo callers required source edits: production writes standard spans and GL/Vulkan fixtures initialize fixed arrays, both supported directly.


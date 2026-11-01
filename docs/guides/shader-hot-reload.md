# Explicit development shader hot reload

The development host can replace the actual selected editor viewport shader
family while preserving its last accepted generation. The ordinary
`apps/HoroEditor/main.cpp` calls `RunEditorGuiApp` without a compiler or reload
configuration, so this feature is disabled in the default executable. There is
no command-line switch, filesystem watcher, ambient tool discovery or packaged
source-compilation fallback in this integration.

## Trusted developer composition

An application-owned developer host must first obtain its compiler adapter and
exact target route through the existing qualified toolchain composition. Use the
existing `VerifiedShaderHostCatalog` and `ExternalShaderCompilerAdapter::Create`
path with independently approved signing roots, exact installed tool identities,
executable digests and finite process deadlines. The reload factory neither
issues tool approvals nor verifies that an arbitrary injected adapter is trusted.
A project-controlled source path or descriptor cannot authorize a compiler.

The non-installed `HoroShaderHotReloadHostInternal` target exposes the narrow
`apps/common/render` seam. The editor application entry is declared in
`apps/HoroEditor/app/HoroEditorApp.h`. A developer composition that already owns
`verifiedCompiler`, `qualifiedTarget` and `qualifiedReflectionTool` calls:

```cpp
#include "HoroEditorApp.h"
#include "ShaderHotReloadHost.h"

// assetSourceRoot is an explicit absolute root containing shaders/editor/.
// The first four identities belong to this developer host's editor session.
auto reload = Horo::Application::Internal::MakeEditorViewportHotReloadConfiguration(
    assetSourceRoot, *qualifiedTarget, qualifiedReflectionTool,
    {.projectGeneration = projectGeneration,
     .configurationRevision = configurationRevision,
     .lifecycleGeneration = lifecycleGeneration,
     .sourceGeneration = sourceGeneration});
if (reload.HasError())
    return 1;

return Horo::Editor::RunEditorGuiApp(
    argc, argv, nullptr, verifiedCompiler, qualifiedTarget, nullptr, &reload.Value());
```

This is a host composition entry, not a standalone toolchain setup program. No
ready-made signed development tool installation or alternate executable is
created by this change. The caller retains the configuration until
`RunEditorGuiApp` returns. The shared compiler and qualified target also serve
the existing explicit Runtime UI paint compiler override; qualify that route
and its source inputs when using the graphical composition.

The factory uses `Development` policy and canonical metadata-only source recipes.
It requires the requested reflection tool identity to appear in the supplied
route's tool closure. `RunEditorSession` requires a non-null explicitly supplied
compiler, obtains `IShaderHotReloadTarget` from the already selected viewport,
and fails composition if that viewport has no target. It does not change renderer
selection or substitute another backend. Only the selected live target can issue
its actual device incarnation; the host captures it during composition.

## Commands and complete families

With that developer composition active, press **Ctrl+Alt+R** in the editor window
to request a reload, or **Ctrl+Alt+C** to cancel. Key repeats and events for other
windows do not issue requests. Startup logs identify the commands. Source capture
is explicit; saving a file alone does not schedule work. A second reload while a
source capture is still retained returns `Busy`; retry after it has drained.

| Selected viewport | Required roles | Canonical paths below the explicit source root |
|---|---|---|
| OpenGL or Metal | 1 lit, 2 grid, 3 shadow | `shaders/editor/viewport_lit.hlsl`, `viewport_grid.hlsl`, `viewport_shadow.hlsl` |
| Vulkan | 1 viewport | `shaders/editor/viewport.hlsl` |

Every role in the selected family must compile, validate, reflect and prepare
before one atomic adoption. The OpenGL/Metal lit recipe preserves the sixteen
light inputs, selection tint and manual 3×3 depth comparison; grid and shadow
remain distinct required consumers. Vulkan uses its existing canonical viewport
and overlay interface. The recipes provide authored binding/member names;
compiled-artifact reflection supplies real layouts and native locations.

## Capture, fences and publication

Before taking an owned recipe copy, the host validates its bounded program,
metadata, dependency and path closure. The source IO job owns that immutable
recipe and its publication fence. It preflights every declared source/dependency
size before copying compiler blueprints or source bytes, then uses the canonical
asset source reader with physical containment and remaining aggregate limits.
Traversal, symlink escape, missing declared inputs, oversized metadata and
capacity overruns fail closed. Source files and declared dependencies are captured
as owned bytes with content identities and diagnostic source mappings.

This is an immutable observed closure, not an OS-atomic multi-file filesystem
snapshot. The source occurrence identity identifies an accepted host request;
it does not fabricate a persisted source revision or prove that disk contents
have not changed since capture. Manifest revisions and content digests retain
their canonical roles. Changing a file after capture requires another explicit
reload to use its newer bytes.

The publication fence binds project, configuration, lifecycle, source occurrence
and actual device incarnation. The concrete target also rechecks its live
program generation before preparation/adoption. Cancel advances the host's source
occurrence and invalidates previous work before replacement capture. This
editor integration keeps the configured canonical engine-source recipe and its
project/configuration/lifecycle scope for the editor session. Hosts changing that
scope must cancel and drain the old host and compose a new one; they must not
reuse its fence for a new project or device.

The application service retains at most one compiler job, one latest pending
request and one completion. It compiles the complete family sequentially through
the existing `ShaderBuildService`, `JobSystem`, `OperationStore` and process
`BuildOutputStore`. Compiler diagnostics keep the existing Build Output source
navigation and operation correlation. Source-capture/admission failures are
reported by the graphical host's `editor.shader_reload` logging; they do not
invent a compiler diagnostic session. No second diagnostics store is introduced.

The GUI pumps the host at `RuntimePhase::BeginFrame`, before the native frontend
`BeginFrame` admits a frame. That poll transfers at most one captured result and
calls service `Poll`; it does no filesystem IO and waits for neither a job nor
GPU completion. It can validate and copy the bounded completed request and
prepare native resources on the graphics owner. Native preparation is cold
owner-thread work: OpenGL driver shader compilation/linking may occur there and
may stall. This feature makes no hitch-free, constant-time or measured latency
claim.

Failure, cancellation, stale scope, rejected reflection or failed native
preparation leaves the last accepted complete family live. Cancellation is
cooperative and cannot undo a generation already adopted before the command.
Successful adoption affects future recording. Previously accepted GPU work keeps
its original resource generation through the existing backend retirement
mechanism; publication introduces no normal-frame GPU-idle wait.

## Lifetimes and shutdown

The reload host borrows the compiler service, scheduler, operation/output stores
and selected native target. Those authorities and the graphics context outlive
host cancellation, draining and destruction. Source workers capture immutable
inputs and a bounded completion slot, never renderer or mutable document
references. Compiler work retains its owned application request.

Both constructor-rollback and constructed-host `EditorWorkerShutdown` guards,
and the runtime participant's shutdown, drain the reload host before the shader
build service and jobs are shut down. `Shutdown` may join work only at teardown;
ordinary `Reload`, `Cancel` and `Poll` never wait. Qualified adapters must honor
cancellation and finite external-process deadlines. No callback is detached to
outlive its borrowed services.

## Public contract and migration

`HoroEngine::RenderApi` owns the additive
`Horo/Runtime/Render/ShaderHotReloadTarget.h` target/candidate/program contracts.
`HoroEngine::ShaderHotReload` owns the additive
`Horo/Application/ShaderHotReloadService.h` application orchestration contract.
`ValidateShaderCompilationRequest` is an additive function on the existing
compiler-pipeline surface. Runtime does not depend on the application service.
The source-recipe producer and GUI composition remain non-installed host seams.

Existing callers require no new constructor argument or feature activation.
New target implementers must supply the complete-family owner-thread
`CaptureState`, `Prepare` and `Adopt` contract, including rollback and retained
old-generation lifetimes. New service consumers explicitly compose their
borrowed authorities, development policy and publication scope. Rebuild such
opt-in C++ consumers with the matching engine snapshot and use the owning target's
staged public headers; do not expose a repository-wide include root.

These additions do not change an existing exported gameplay value layout,
GameModule virtual table/context, extension C ABI table, serialized shader
package or project schema. They therefore do not themselves require a gameplay
boundary25/fingerprint or external extension ABI increment. Exact snapshot,
compiler/platform/configuration and artifact identity rules still apply; an
additive header is not a promise of cross-snapshot native binary compatibility.

## Verification scope

`HoroShaderHotReloadHostTests` covers bounded immutable capture, capacity and
path rejection, cancellation, canonical family configuration, policy rejection
and host teardown preserving the target. `HoroShaderHotReloadServiceTests`
exercises the asynchronous application service independently. Passing these
headless tests does not establish real-driver reload, editor keyboard
interaction or visual equivalence. Those require a qualified developer
composition and an actual selected backend/device run; report each separately.

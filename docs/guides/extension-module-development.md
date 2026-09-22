# Extension Module Development Guide

## Purpose

This guide shows how to design an installable Horo add-on package that extends
engine/backend capabilities and, when needed, the engine IDE without depending
on editor internals. The example is a hybrid package that contributes:

- a headless shader compile-preview capability
- a dockable editor tab
- a Settings page
- an MCP tool
- a command-palette/menu action
- editor data-bus subscriptions
- extension-owned notifications

The guide is intentionally API/ABI focused. Exact header names may evolve, but
these boundaries are the contract extension authors should design against.

## Example: Shader Tools Extension

The example package adds a backend shader compile-preview capability and a
Shader Inspector tab over that capability. The tab reads the selected asset,
queries compiler diagnostics, listens for asset/build/log changes, and requests
compile previews through the approved backend capability.

```text
com.vendor.shader-tools
  horo-package.toml
  files.manifest.json
  extensions/
    com.vendor.shader-tools.native/
      extension.json
      bin/
        macos-arm64/libhoro_shader_tools.dylib
        linux-x64/libhoro_shader_tools.so
        windows-x64/horo_shader_tools.dll
      schemas/
        compile-preview.schema.json
      resources/
        icons/shader-tools.svg
```

The package does not patch `EditorLayer`, does not subscribe directly to
`EngineDataBus` from its GUI tab, and does not write project files directly. It
contributes descriptors. The host validates them, grants narrow backend and
frontend capabilities, and owns placement, focus, persistence, scheduling,
permission checks, result storage, and teardown.

## Development Flow

1. Choose explicit module roles for GUI-only, backend-only, script-consumable or
   mixed behavior.
2. Choose the extension points.
3. Declare package identity, dependencies and the extension descriptor path in
   `horo-package.toml`.
4. Declare module ABI/entries, permissions, settings, errors, events, and UI
   contributions in the package-scoped `extension.json`.
5. Implement the native module behind the Horo extension C ABI or an approved
   isolated helper protocol.
6. Publish typed backend implementations and host-rendered UI schemas/actions for
   the declared contributions; do not register external drawing callbacks.
7. Read engine and IDE data through approved query capabilities.
8. Mutate engine/editor state only through typed commands or application use
   cases.
9. Observe changes through `EditorDataBus` or approved process-event imports.
10. Store only bounded presentation state under the contribution ID.
11. Test registration, permission denial, event handling, workspace persistence,
    and teardown.

### Declarative extension forms

Settings pages and other host-rendered surfaces compose their content with
`Horo/Extensions/EditorUiForm.h`. The extension owns the copied schema and
current projection values, while the host owns pixels, localization, keyboard
focus, accessibility, theme tokens, DPI, and action routing:

```cpp
auto builder = Horo::Extensions::EditorUiFormBuilder::Create(
    {"com.vendor.shader-tools.settings"},
    {Horo::Extensions::EditorUiTextKind::LocalizationKey,
     "shader_tools.settings.title"});
// Add EditorUiTextFieldNode, EditorUiChoiceNode, EditorUiActionNode, and
// layout/validation nodes through the builder's typed public methods.
auto form = std::move(builder).Value().Build();
```

Use `EditorUiBindingId` for fields and `EditorUiActionId` for actions. Do not
store callbacks, raw colors, ImGui objects, renderer handles, or persistence
logic in the form. `BuildEditorUiRenderSnapshot` is the shared deterministic
projection consumed by GUI and headless adapters. See the
[Extension Declarative Form Kit](../architecture/extensions/editor-ui-form-kit.md)
and the copyable `examples/extensions/gui-form-basic` fixture.

### Backend operation lifecycle

Long-running backend work uses the host `BackendOperationRegistry`. Register the
provider generation with one or more canonical typed operation contracts and the
expected result identity; use the exact `ApplicationCapabilityProviderIdentity`
selected by composition when beginning work. Keep the returned producer alive
until it publishes `Complete` or `Fail`, and pass `Cancellation()` to worker code
so parent, caller, provider, and host shutdown cancellation is observable.

The copyable operation handle is for polling snapshots and requesting caller
cancellation. Publish exact stage-local progress and bounded Foundation
`Diagnostic` values. A phase change may reset its counters, but progress within a
phase may not regress. Result bytes are optional: omitting a payload is a valid
successful completion for an operation whose contract has no bytes to publish;
when present, the payload identity must match the registered result contract and
remain within the provider's bound. Provider registration reset and host shutdown
terminalize active operations before the provider is released, so an extension
must not retain a producer as a workaround for lifecycle teardown.

Every module and every importer contribution declares an independent canonical
semantic version. C++ importer adapters must populate both
`AssetImporterContribution::moduleVersion` and
`AssetImporterContribution::version`. Existing development adapters that only
set `version` must add `moduleVersion` before registration; the host rejects an
unversioned contribution instead of publishing ambiguous provenance.

## Manifest

A minimal package manifest contribution points to the module descriptor:

```toml
[package]
id = "com.vendor.shader-tools"
version = "1.0.0"
kind = "extension"

[[contribution]]
kind = "EditorExtension"
id = "com.vendor.shader-tools.native"
root = "extensions/com.vendor.shader-tools.native/"
descriptor = "extensions/com.vendor.shader-tools.native/extension.json"
required = false
```

The package-scoped module descriptor is below. Every path inside it is relative
to the verified package root, not to the descriptor directory.

```json
{
  "schemaVersion": 1,
  "ownerPackage": {
    "id": "com.vendor.shader-tools",
    "version": "1.0.0"
  },
  "module": {
    "id": "com.vendor.shader-tools.native",
    "version": "1.0.0",
    "kind": "native-c-abi",
    "roles": ["backend-capability", "editor-presentation"],
    "abi": "horo-extension-1",
    "entries": [
      {
        "platform": "macos",
        "architecture": "arm64",
        "path": "extensions/com.vendor.shader-tools.native/bin/macos-arm64/libhoro_shader_tools.dylib"
      },
      {
        "platform": "linux",
        "architecture": "x86_64",
        "path": "extensions/com.vendor.shader-tools.native/bin/linux-x64/libhoro_shader_tools.so"
      },
      {
        "platform": "windows",
        "architecture": "x86_64",
        "path": "extensions/com.vendor.shader-tools.native/bin/windows-x64/horo_shader_tools.dll"
      }
    ]
  },

  "contributions": [
    {
      "type": "application.capability",
      "id": "com.vendor.shader-tools.compile-service",
      "module": "com.vendor.shader-tools.native",
      "capability": "shader.compile.preview",
      "threadAffinity": "worker",
      "operationStore": true,
      "permissions": ["asset.read", "shader.compile.preview"]
    },
    {
      "type": "editor.tab",
      "id": "com.vendor.shader-tools.shader-inspector",
      "module": "com.vendor.shader-tools.native",
      "label": "Shader Inspector",
      "icon": "resources/icons/shader-tools.svg",
      "fallbackPlacement": "bottom.tools",
      "openByDefault": false,
      "events": [
        "horo.editor.selection.changed",
        "horo.editor.asset.changed",
        "horo.editor.operation.changed"
      ],
      "capabilities": [
        "selection.query",
        "asset.query",
        "shader.compile.preview",
        "log.query"
      ],
      "workspaceState": {
        "schemaVersion": 1,
        "maxBytes": 8192
      }
    },
    {
      "type": "editor.settings_page",
      "id": "com.vendor.shader-tools.settings",
      "module": "com.vendor.shader-tools.native",
      "placement": "ProjectSettings/Tools",
      "settingsPrefix": "vendor.shader_tools",
      "capabilities": ["configuration.draft_page"]
    },
    {
      "type": "command",
      "id": "com.vendor.shader-tools.compile-selected",
      "module": "com.vendor.shader-tools.native",
      "label": "Compile Selected Shader",
      "placement": "CommandPalette/Assets",
      "capabilities": [
        "selection.query",
        "asset.query",
        "shader.compile.preview"
      ]
    },
    {
      "type": "mcp.tool",
      "id": "com.vendor.shader-tools.compile-preview",
      "module": "com.vendor.shader-tools.native",
      "schema": "schemas/compile-preview.schema.json",
      "capabilities": [
        "asset.query",
        "shader.compile.preview"
      ]
    }
  ],

  "settings": [
    {
      "key": "vendor.shader_tools.default_profile",
      "type": "string",
      "scope": "project",
      "default": "debug",
      "reloadPolicy": "NextOperation"
    }
  ],

  "events": [
    {
      "name": "com.vendor.shader-tools.preview.changed",
      "scope": "editor-session",
      "payload": "revision-only"
    }
  ],

  "errors": [
    {
      "domain": "com.vendor.shader-tools",
      "code": "shader.preview.unsupported_asset",
      "defaultSeverity": "error",
      "summary": "Selected asset is not a shader",
      "userActionable": true,
      "retryable": false
    }
  ],

  "permissions": [
    "project.read",
    "asset.read",
    "shader.compile.preview",
    "mcp.register_tool"
  ]
}
```

Rules:

- Treat the JSON as the serialized form of the typed v1 model in
  [ADR-055](../adr/055-extension-manifest-v1-typed-model.md). Only the fully
  cross-reference validated value may reach registration or activation.
- IDs are stable contracts. Do not rename or repurpose them after release.
- Contributions are inert descriptors. Loading the package must not mutate host
  registries through static initialization.
- Permissions are reviewed before module activation.
- Settings, errors, and events are declared before the module can use them.
- Backend capabilities, UI surfaces, CLI/MCP tools, and commands are separate
  contributions even when implemented by the same module.
- Asset importer form fields explicitly opt into user presets with
  `includeInPresets`. Leave it false for source-specific names, identifiers,
  paths, credentials, and any value that should not be replayed on another
  asset.
- An importer may attach an `IAssetPreviewProvider` to its contribution. The
  provider receives the importer-owned editor payload and returns a bounded
  RGBA8 image. It must not retain borrowed input, call ImGui, create GPU
  resources, or mutate the project. Returning a failure selects the declared
  mesh, image, audio, or generic host fallback.
- Preview providers must poll the supplied cancellation token during expensive
  decode or raster work. The host schedules calls through a bounded service,
  retains the provider until completion, validates output, and reuses successful
  results by contribution/version, request dimensions, asset type, and payload
  digest. Providers must not add their own global preview cache or worker pool.

## Module Shapes

Horo add-ons are not necessarily GUI add-ons:

| Shape | Example | Rule |
|---|---|---|
| Backend-only | asset importer, cooker, project validator, pipeline step, toolchain provider | Declares `BackendCapability` and optional `HeadlessTooling`; it must run without `HoroEditor` or ImGui. |
| GUI-only | inspector tab over existing scene/asset/log stores, command-palette shortcut, Settings page for built-in service | Declares `EditorPresentation`, owns presentation only and calls existing capabilities. |
| Script-consumable | backend service with an approved Lua 5.4 binding | Declares `BackendCapability` plus `ScriptProvider`; a typed script API binds to one compatible service export. |
| Mixed-role | shader compile service plus inspector tab and MCP tool | Declares the explicit union of its roles; GUI/MCP/command contributions remain adapters over the backend authority. |

When a feature has real behavior, put that behavior behind a backend contribution
first. The GUI then becomes optional presentation. This keeps the same add-on
usable from the editor, CLI, MCP, CI, and tests.

```text
Backend contribution
  -> typed capability / operation / provider
  -> host-owned scheduling, cancellation, progress, result storage

Frontend contribution
  -> tab, panel, Settings page, command, MCP tool
  -> calls backend capability
  -> observes revisions and queries snapshots
```

## ABI Boundary

External native editor/tool modules cross a C ABI. C++ STL types, exceptions,
RTTI-dependent ownership, allocator ownership, and C++ object deletion do not
cross this boundary.

The C ABI also does not expose ImGui contexts, draw lists, SDL events, renderer
handles, native child windows or C++ panel/component interfaces. Embedded UI is a
copied typed schema/state/action contract rendered by Horo; complex tools use
registered specialized view schemas or a separately launched application. See
[ADR-056](../adr/056-external-editor-ui-boundary.md).

Use the concrete SDK declaration in
`include/Horo/Extensions/ExtensionAbi.h`; do not reproduce private copies of
the structs. The stable entry point is:

```c
HORO_EXTENSION_EXPORT HoroExtensionStatus
horo_extension_load(const HoroExtensionHostApi* host,
                    HoroExtensionModuleApi* module);
```

ABI rules:

- Every ABI struct starts with `size` and `version` or is passed through a table
  that has them.
- The host owns host memory. The module owns module memory. Each side frees only
  memory it allocated.
- Callbacks never throw across the ABI. The module catches internal exceptions
  and returns `HoroExtensionStatus` or an operation result.
- Function tables are append-only within a major ABI version.
- Pointers received from the host are borrowed for the documented call or handle
  lifetime only.
- A successfully registered importer context is owned by the host adapter and
  released exactly once through `destroyImporter`.
- Dynamic library unload is deferred while any catalog snapshot retains an
  importer or preview callback from that module.

## Module Registration

The module load function should only publish factories for descriptors the host
already validated from the manifest.

For a complete compiling implementation, use
`examples/extensions/asset-importer-basic`. It registers an `.hraw` importer,
one preset-compatible boolean setting, a versioned editor payload, and an RGBA8
preview callback. Its current top-level `extension.json` is a legacy development
fixture; ADR-054 migration moves package identity into `horo-package.toml` and
the module descriptor below `extensions/<module-id>/` without changing the
module/contribution IDs returned through the binary boundary.

Build it with `-DHORO_BUILD_EXAMPLES=ON`. Until the package activation-candidate
path is implemented, CMake places the legacy descriptor beside the platform
library for the explicit development/test `ExtensionManager::LoadExtension`
adapter. Production activation must not use that raw-directory path.

Activation receives a module context containing only the capabilities approved by
manifest, trust, project policy, and host availability.

```cpp
struct ShaderToolsModule {
    HoroExtensionHostApiV1 host;
    HoroCapabilityTable capabilities;
    HoroSubscriptionGroup subscriptions;
};
```

The module must not keep arbitrary host pointers beyond their documented
lifetime. Long-lived access uses handles, leases, or capability interfaces whose
ownership is specified by the host.

## Backend Capability

The backend contribution implements behavior that must work without the editor
being open. For this example, `shader.compile.preview` is an application
capability that accepts typed requests, schedules worker work through the host,
and commits progress/result snapshots to the operation store.

```text
CLI / MCP / GUI command
  -> ShaderCompilePreview::Submit(request)
  -> host validates permission and asset identity
  -> host schedules worker operation
  -> extension backend compiles preview using approved file/asset handles
  -> operation store records progress, diagnostics, and final result
```

Backend capabilities must:

- validate all input from GUI, CLI, MCP, and automation callers;
- use host-owned scheduling, cancellation, diagnostics, and result storage;
- declare thread affinity and resource budget expectations;
- report stable errors from the manifest error domain;
- avoid depending on `EditorDataBus`, ImGui, selected tabs, or current widget
  focus;
- remain observable through logs, metrics, profiler zones, and operation IDs.

Backend capabilities must not:

- write project files directly when a host transaction should own the mutation;
- hold GUI pointers or editor-session references;
- publish GUI events as their source of truth;
- perform hidden process launches or network access outside declared
  permissions;
- require an editor surface to be open for correctness.

## Reading Data From The IDE

Read editor and engine state through query capabilities. Do not reach into tabs,
`EditorLayer`, renderer backend objects, or process-global services.

For the Shader Inspector tab, the normal read path is:

```text
ShaderInspectorTab
  -> SelectionQueries::CurrentAsset()
  -> AssetQueries::GetAsset(asset_id)
  -> LogQuery::ReadRange(log_revision, range)
  -> MetricsQuery::ReadLatest(group) when visible
```

A surface attaching to the editor should query current state immediately. It must
not rely on old events being replayed.

```cpp
void ShaderInspectorTab::OnAttach(EditorExtensionSurfaceContext& ctx) {
    m_ctx = &ctx;

    m_selectionChanged = ctx.editorEvents.Subscribe<SelectionChangedEvent>(
        [this](const SelectionChangedEvent&) {
            m_dirtySelection = true;
        });

    m_assetChanged = ctx.editorEvents.Subscribe<EditorAssetChangedEvent>(
        [this](const EditorAssetChangedEvent& event) {
            if (event.asset == m_currentAsset) {
                m_dirtyAsset = true;
            }
        });

    RefreshFromAuthorities();
}

void ShaderInspectorTab::RefreshFromAuthorities() {
    const auto selected = m_ctx->capabilities.Get<SelectionQueries>().CurrentAsset();
    if (!selected) {
        m_view = ShaderInspectorView::NoSelection();
        return;
    }

    const auto asset = m_ctx->capabilities.Get<AssetQueries>().GetAsset(*selected);
    m_view = BuildViewFromAsset(asset);
}
```

The concrete host attachment contract is activation-scoped. The host creates
an `ExtensionActivationLease` from the capability admission, validates the
surface descriptor and its bounded allowlists, and attaches the surface through
an `EditorSurfaceContextProvider`. The provider returns a move-only
registration; retain only the copied typed context view in the surface adapter.
Reset the registration during detach, before the matching activation admission
is revoked:

```cpp
auto registration = contextProvider.Attach(std::move(surface), admission.ActivationLease(), approvedCapabilities);
if (registration.HasError()) {
    return registration.ErrorValue();
}

const EditorSurfaceContext &surfaceContext = registration.Value().Context();
if (surfaceContext.Allows(EditorSurfaceCommandId{"shader.preview.compile"})) {
    // Resolve the host-owned typed command adapter at the host boundary.
}

registration.Value().Reset();
```

The context is an allowlist and lifecycle proof, not a service locator. A
surface must treat a revoked context as disconnected, and must not retain raw
editor, renderer, native-window, ImGui, or callback objects. Capability handles
that were not explicitly approved for this surface remain unavailable even when
the parent contribution declares them.

Important constraints:

- Event payloads are invalidation hints, not authoritative data.
- Hidden surfaces may receive notifications but should defer expensive refreshes
  until visible.
- High-volume data is queried from bounded stores. It is not copied through the
  data bus.
- Query capabilities return typed errors; UI text is derived from error codes and
  diagnostics, not log parsing.

## Sending Data Or Mutating State

Mutations go through typed commands or application use cases. A tab does not
publish command events and does not write project files directly.

Examples:

```text
Compile preview button
  -> ShaderCompilePreview::Submit(request)
  -> OperationStore records operation state
  -> EngineDataBus publishes operation revision
  -> EditorEngineEventBridge imports editor-facing operation notification
  -> ShaderInspectorTab invalidates and queries operation result
```

```cpp
void ShaderInspectorTab::OnCompilePreviewClicked() {
    const auto selected = m_ctx->capabilities.Get<SelectionQueries>().CurrentAsset();
    if (!selected) {
        ShowError("com.vendor.shader-tools", "shader.preview.no_selection");
        return;
    }

    ShaderCompilePreviewRequest request;
    request.asset = *selected;
    request.profile = m_currentProfile;

    auto result = m_ctx->capabilities
        .Get<ShaderCompilePreview>()
        .Submit(request);

    if (!result) {
        PresentError(result.error());
        return;
    }

    m_observedOperation = result->operationId;
}
```

If the extension owns transient presentation state that other extension surfaces
need to observe, it may publish its own editor-session event declared in the
manifest:

```cpp
struct ShaderPreviewChangedEvent {
    static constexpr std::string_view HoroEventTypeName =
        "com.vendor.shader-tools.preview.changed";

    ShaderPreviewRevision revision;
};
```

The event should carry a revision or small identifier, not a copied compiler log,
shader blob, or full preview snapshot. Subscribers query the extension-owned or
host-owned store for details.

## Data Bus Rules

GUI surfaces use `EditorDataBus`. They do not directly subscribe to
`EngineDataBus` because their lifetime is one editor session, while
`EngineDataBus` is process-scoped.

Approved process events are imported through `EditorEngineEventBridge`:

```text
EngineDataBus
  -> EditorEngineEventBridge allowlist
  -> EditorDataBus
  -> extension tab/panel/modal page
```

If the package needs true process-level observation, it declares a separate
process-observer contribution or event-import request. The host validates the
consumer, permission, safe payload shape, and tests before granting that
capability.

Handlers must:

- run on the editor thread unless the extension point explicitly says otherwise;
- avoid blocking I/O, subprocess execution, compilation, network calls, and asset
  scanning;
- catch internal exceptions and report typed errors;
- release RAII subscription tokens during detach;
- avoid depending on subscriber order.

## Settings Page

Settings pages bind to typed configuration descriptors declared in the manifest.
They edit a draft owned by the Settings modal and never persist directly.

```text
Settings page field edit
  -> ConfigurationDraftPage::Set(key, value)
  -> Settings modal validates page and full draft
  -> Apply commits through ConfigurationService
  -> ConfigurationService publishes committed settings notification
```

A page can validate its local fields and return diagnostics:

```cpp
ValidationResult ShaderToolsSettingsPage::Validate() const {
    ValidationResult result;
    if (!IsKnownProfile(m_profile)) {
        result.diagnostics.push_back(Diagnostic{
            .code = "shader_tools.settings.unknown_profile",
            .severity = DiagnosticSeverity::Error,
            .message = "Unknown shader profile"
        });
    }
    return result;
}
```

The owning modal controls apply/cancel/preview/dirty-state policy. The extension
page cannot bypass close confirmation or publish committed settings events.

## MCP Tool

An MCP tool contribution exposes the same application capability as the GUI, but
through the MCP transport schema. The tool handler validates JSON input, calls a
use case, and returns a structured result or structured Horo error.

```text
MCP client
  -> mcp.tool com.vendor.shader-tools.compile-preview
  -> schema validation
  -> ShaderCompilePreview::Submit(request)
  -> structured result / Error payload
```

The MCP handler must not call GUI objects or editor tabs. If it runs inside
`HoroEditor`, mutations are marshalled onto the owning editor/application thread
through the approved use case.

## Workspace State

Extension surfaces may persist bounded presentation state:

```json
{
  "schemaVersion": 1,
  "selectedProfile": "debug",
  "showGeneratedSource": false,
  "expandedSections": ["inputs", "diagnostics"]
}
```

Rules:

- State is stored under the contribution ID.
- State is presentation only. It contains no capabilities, raw handles, pointers,
  secrets, unbounded logs, or generated blobs.
- The surface owns migration of its own state schema.
- If the provider is missing, the host keeps the state opaque within size limits
  and restores it when the provider returns.

## Observability

A module should declare log categories, metric instruments, profiler zones, and
error codes. The host validates descriptors before activation.

Recommended signals for the Shader Tools example:

```text
log category: com.vendor.shader-tools
metric: shader_tools.preview.request_count
metric: shader_tools.preview.failure_count
metric: shader_tools.preview.duration_ms
profiler zone: shader_tools.preview.compile
error domain: com.vendor.shader-tools
```

Do not emit one event per log line, compiler output chunk, profiler sample, or
thumbnail. Append high-volume data to a bounded store and publish a revision.

## Teardown

On detach, disable, update, shutdown, or future live unload:

1. Stop accepting new UI interaction for the contributed surface.
2. Release `EditorDataBus` subscriptions.
3. Cancel or detach extension-owned jobs.
4. Flush or discard transient presentation state according to the host policy.
5. Unregister contributed surfaces transactionally.
6. Drain queued callbacks into module code.
7. Call module deactivate.
8. Keep the dynamic library loaded until process exit unless live unload has been
   explicitly proven safe for the API.

No callback may execute against unloaded module code or destroyed editor-session
state.

## Testing Checklist

Each extension package should have contract tests for:

- manifest schema validation and path containment;
- duplicate contribution ID rejection;
- permission denial and trust-required flows;
- unsupported ABI version rejection;
- missing required function rejection;
- backend capability runs from headless host without constructing GUI state;
- GUI, MCP, command, and CLI adapters call the same backend capability path;
- operation progress/result storage, cancellation, and diagnostics are bounded
  and queryable;
- editor tab registration and fallback placement;
- Settings page validation and apply/cancel behavior;
- MCP schema validation and structured error payloads;
- `EditorDataBus` subscriptions detach on surface destruction;
- requested process-event imports require bridge allowlist approval;
- event handlers do not perform blocking work inline;
- workspace state size limits and schema migration;
- package disable/update requiring restart unless safe live unload is proven;
- shutdown leaves no live callbacks into module code.

## Common Mistakes

- Adding a tab by editing `EditorLayer` instead of contributing `editor.tab`.
- Implementing external editor UI with raw ImGui, native child windows or a C++
  panel interface instead of the typed host-rendered UI contract.
- Reading another tab's state directly instead of querying an authority.
- Subscribing a GUI surface directly to `EngineDataBus`.
- Sending commands through the data bus.
- Publishing full logs, compiler output, or asset snapshots as event payloads.
- Holding borrowed host pointers after the callback that supplied them.
- Throwing exceptions across the extension ABI.
- Using STL containers or C++ object ownership across the C ABI.
- Persisting secrets, raw handles, or capability-bearing data in workspace state.
- Assuming package disable can unload native code at runtime.

## Related Architecture

- [Extension System](../architecture/extensions/plugin-system.md)
- [External Editor UI Boundary](../adr/056-external-editor-ui-boundary.md)
- [Editor Panel Host](../architecture/editor/editor-panel-host.md)
- [Editor Modal Host](../architecture/editor/editor-modal-host.md)
- [Editor Data Bus](../architecture/editor/editor-data-bus.md)
- [Engine Data Bus](../architecture/foundation/engine-data-bus.md)
- [Configuration System](../architecture/foundation/configuration-system.md)
- [Error And Diagnostics](../architecture/foundation/error-and-diagnostics.md)
- [Observability Architecture](../architecture/observability/observability.md)

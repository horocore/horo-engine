# Current Target And Dependency Inventory

## Purpose

This document records the production CMake target graph that exists today and
compares it with the target architecture in
[System Design](./system-design.md),
[Desired Project Trees](../desired-project-tree.md), and
[Build System](../delivery/build-system.md). It is an evidence snapshot for
planning migration work; it does not authorize the current divergences as new
architecture.

Snapshot:

- repository commit: `763066ddb35188648bed48911214600e5dfa3341`
- inventory date: 2026-08-25
- authoritative declarations inspected: root, `src/`, and `apps/`
  `CMakeLists.txt` files
- generated graphs inspected: macOS headless and combined OpenGL/Metal editor
  configurations with tests and examples disabled
- excluded from the production inventory: test, example, generated utility,
  and third-party targets

## Status Terms

| Status | Meaning |
|---|---|
| Implemented | The documented target exists with production sources and its stated primary responsibility. |
| Partial | Some responsibility exists, but the documented boundary is incomplete, combined with another target, or named differently. |
| Planned | A placeholder path or architecture contract exists, but there is no production target. |
| Absent | No production target or implementation path was found. |
| Transitional | A real target exists but is not part of the documented target set or has a known temporary boundary. |

## Current Production Targets

The current combined macOS editor composition contains 31 first-party library
targets and two executable targets. `HoroEngine::*` names below are CMake aliases;
the unqualified names are the real targets that CMake modifies and builds.

Dependency entries contain first-party direct link edges only. `Public surface`
describes semantic ownership inferred from sources and headers; the current CMake
include directories do not enforce those ownership boundaries, as documented in
[Public Header Inventory And Boundary](#public-header-inventory-and-boundary).

### Foundation, Platform, Application, And Runtime

Audio API follow-up (2026-09-04): `HoroAudioApi` (`HoroEngine::AudioApi`) is an
always-available headless target with public Foundation and Assets dependencies.
It owns `Audio/AudioIdentity.h`, `Audio/AudioErrors.h` and the additive
`Audio/AudioResamplerPlan.h`. Resampler preparation is control-thread validation;
it does not activate a device or implement a mixer. Header ownership is registered
in `HoroPublicHeaderOwnership.cmake` and exercised by
`HoroAudioApiPublicHeaderConsumer`. Existing consumers need no migration.

The subsequent `HoroAudioDsp` (`HoroEngine::AudioDsp`) target owns private
resampler coefficient preparation and scalar evaluation under
`src/audio/resampling/`. Its only direct first-party dependency is AudioApi.
Its public `Audio/AudioResampler.h` owns single-thread streaming processing;
coefficient and native SIMD internals stay private. Tests receive the exact private directory,
and no repository-wide include root is exported. The additive public boundary is
covered by `HoroAudioDspPublicHeaderConsumer`; existing AudioApi consumers need no
migration or DSP dependency. This is a DSP primitive, not an audio runtime or
device backend.

| Real target (alias) | Availability | Owner and public/private boundary | Direct first-party dependencies |
|---|---|---|---|
| `HoroFoundation` (`HoroEngine::Foundation`) | Always | Owns Foundation primitives, canonical cross-subsystem asset cook-target identity, immutable host-published error-code registry snapshots, diagnostics, logging, telemetry facade, jobs, configuration, paths, hashing, strings, and shared `Math/**` contracts. Implementation is under `src/foundation/`. | None |
| `HoroCliHost` (`HoroEngine::CliHost`) | Always | Owns inert CLI descriptors, bounded host-policy validation, immutable command registration, deterministic discovery/help, and the bounded typed pre-dispatch parser under `Cli/**`. Parsing consumes only explicit argv, resolved configuration snapshots, path-normalizer seams, and selected stdin; dispatch, presentation, and adapter execution are not yet part of this target. | Foundation (public), nlohmann_json (private) |
| `HoroOpenTelemetry` (`HoroEngine::OpenTelemetry`) | `HORO_ENABLE_OPENTELEMETRY` | Owns the optional OTLP sink and `Foundation/Telemetry/OpenTelemetrySink.h`; the concrete exporter dependencies are private. | Foundation (public) |
| `HoroPlatform` (`HoroEngine::Platform`) | Always | Owns `Platform/**` dynamic-library and process contracts plus POSIX/Windows implementations. OS headers and `dl` are private. | Foundation (public) |
| `HoroPackages` (`HoroEngine::Packages`) | Always | Owns canonical package paths, the typed file inventory, immutable archive verification, and content-addressed cache/quarantine under `Packages/**`. JSON, miniz and Unicode normalization remain private; install/trust/semantic package-manifest services are not implemented by this target yet. | Foundation (public) |
| `HoroApplication` (`HoroEngine::Application`) | Always | Owns project version, compatibility, migration planning/execution, and host observability contracts in `Application/**`, except gameplay build. Generated compatibility data is private. | Foundation (public) |
| `HoroProjectMigrations` (`HoroEngine::ProjectMigrations`) | Always | Owns generated concrete migration catalog composition. It intentionally exposes the Application migration contract rather than a separate header family. | Application (public) |
| `HoroRuntime` (`HoroEngine::Runtime`) | Always | Owns frame scheduling, runtime lifecycle, and runtime host contracts in `Runtime/FrameScheduler.h`, `Runtime/RuntimeLifecycle.h`, and `Runtime/RuntimeHost.h`. | Foundation (public) |
| `HoroRuntimeUi` (`HoroEngine::RuntimeUi`) | Always | Owns backend-neutral identities, authored/cooked document contracts, canvas modes and deterministic logical-to-screen resolution, canvas references, mutable instance lifecycle, bounded retained element trees, deferred structural transactions, incremental measure-arrange caching, immutable layout/render snapshots, per-view render extraction, dependency enumeration, and typed stale-value rejection under `Runtime/Ui/**`. Renderer realization, output/DPI/safe-area policy, platform, editor, and gameplay ownership remain outside this target. | Foundation, Assets (public) |
| `HoroAssets` (`HoroEngine::Assets`) | Always | Owns all current `Assets/**` headers. The target currently combines registry, provider, import, preview, reimport, cook, cache, and output responsibilities. | Foundation (public) |
| `HoroAnimationApi` (`HoroEngine::AnimationApi`) | Always | Owns backend-neutral persistent animation asset identities, generation-safe runtime handles, stable validation errors, inert component contracts, immutable bounded skeleton, skinning, and clip assets, exact bounded clip traversal/sampling, plus preallocated frame pose storage with deterministic hierarchy evaluation and move-only immutable leases under `Animation/**`. Renderer resources, presentation palette publication, graph evaluation, and backend integration remain outside this target. | Foundation, Assets (public) |
| `HoroInput` (`HoroEngine::Input`) | Always | Owns the backend-neutral `Runtime/Input.h` contract and runtime implementation. | Foundation (public) |
| `HoroAudioMemory` (`HoroEngine::AudioMemory`) | Always | Owns `Audio/AudioMemory.h`, bounded scratch storage and generation-safe fixed pools with explicit deferred reuse. Aligned allocation details remain target-private. | AudioApi (public) |
| `HoroAudioCommands` (`HoroEngine::AudioCommands`) | Always | Owns `Audio/AudioCommands.h`, `Audio/AudioCommandBuffer.h` and `Audio/AudioCommandStaging.h`: typed intents, normalization/coalescing, bounded MPSC/control/SPSC transport and scene/barrier admission. | AudioMemory (public) |
| `HoroInputSdl` (`HoroEngine::InputSdl`) | Editor GUI only | Owns the SDL input adapter. It has no dedicated public Horo header; its implementation path is nevertheless exported as a public include directory. | Input (public) |

### Gameplay And Extensions

| Real target (alias) | Availability | Owner and public/private boundary | Direct first-party dependencies |
|---|---|---|---|
| `HoroGameplayApi` (`HoroEngine::GameplayApi`) | Always | Owns project-facing gameplay descriptors, game-owned asset types, behavior types, native behavior, module, and error contracts under `Gameplay/**`. | Foundation (public) |
| `HoroGameplayRuntime` (`HoroEngine::GameplayRuntime`) | Always | Owns `Gameplay/BehaviorRegistry.h` and `Gameplay/BehaviorRuntime.h` and authoritative behavior execution. | GameplayApi, RuntimeScene (public) |
| `HoroGameplayModuleHost` (`HoroEngine::GameplayModuleHost`) | Always | Owns `Gameplay/GameModuleHost.h` and dynamic project-module loading. Native dynamic loading stays private. | GameplayRuntime (public), Platform (private) |
| `HoroGameplayLua` (`HoroEngine::GameplayLua`) | Always | Owns `Gameplay/LuaBehavior.h` and the concrete Lua behavior adapter. Lua and JSON dependencies are private. | GameplayRuntime (public) |
| `HoroGameplayBuild` (`HoroEngine::GameplayBuild`) | Always | Owns `Application/GameplayBuildService.h` and project gameplay build orchestration. | Foundation (public), GameplayModuleHost and Platform (private) |
| `HoroExtensions` (`HoroEngine::Extensions`) | Always | Owns all current `Extensions/**` headers and combines ABI, manifest, inventory, host, marketplace, and external asset-import adapter responsibilities. Third-party transport/archive/parser dependencies are private. | Foundation, Platform, Assets (public) |

### Scene And Rendering

| Real target (alias) | Availability | Owner and public/private boundary | Direct first-party dependencies |
|---|---|---|---|
| `HoroRuntimeScene` (`HoroEngine::RuntimeScene`) | Always | Owns runtime scene definition, scene state, saved-scene baseline admission, and scene component contracts under `Runtime/Scene/**`, excluding primitive mesh/catalog headers. | Foundation, Runtime (including Runtime/Save), RuntimeUi, GameplayApi, Assets, SceneModel (public) |
| `HoroNetworkRuntime` (`HoroEngine::NetworkRuntime`) | Always | Owns the bounded session/scene-scoped network-object mapping and immutable diagnostic snapshots under `Network/NetworkObjectMapping.h`; it performs no Scene mutation or transport work. | NetworkApi, RuntimeScene (public) |
| `HoroSceneModel` (`HoroEngine::SceneModel`) | Always | Owns `Runtime/Scene/PrimitiveCatalog.h`, `PrimitiveMesh.h`, and `PrimitiveMeshDescriptor.h`. | Foundation, RenderApi (public) |
| `HoroRenderApi` (`HoroEngine::RenderApi`) | Always | Owns backend-neutral mesh, render-scene, shader-manifest, final-artifact reflection/source-map validation, binding-layout, and backend contracts under `Runtime/Render/**`, excluding registry/frontend/module headers. Reflection adds no native dependency and existing consumers require no migration. | Foundation (public) |
| `HoroRenderBackendRegistry` (`HoroEngine::RenderBackendRegistry`) | Always | Owns `Runtime/Render/RenderBackendRegistry.h` and backend registration/selection state. | RenderApi (public) |
| `HoroRenderFrontend` (`HoroEngine::RenderFrontend`) | Always | Owns `Runtime/Render/RenderFrontend.h` and frontend submission/resource coordination. | RenderApi, RenderBackendRegistry (public) |
| `HoroRenderNull` (`HoroEngine::RenderNull`) | Always | Owns `Runtime/Render/NullBackendModule.h`; implementation is private and deterministic/headless. | RenderApi, RenderBackendRegistry (public) |
| `HoroXRApi` (`HoroEngine::XRApi`) | Always | Owns backend-neutral XR contract versions, generation-safe live identities, immutable fixed-capacity capability snapshots, coordinate/pose evidence, bounded tracking snapshots, allocation-free pose queries, admission results, and stable error descriptors under `XR/**`. It owns no OpenXR/native state and performs no runtime discovery or activation. | Foundation (public) |
| `HoroTerrainApi` (`HoroEngine::TerrainApi`) | Always | Owns deterministic stable project/dataset/tile/foliage identities, independent Terrain revisions, generation-safe runtime foliage handles, bounded catalog validation, and stable errors under `Terrain/**`. It owns no native Render/Physics/Navigation resource, world-streaming authority, runtime scheduler, or editor state. | Foundation (public) |
| `HoroRenderOpenGL` (`HoroEngine::RenderOpenGL`) | `HORO_BUILD_RENDER_OPENGL` | Owns the concrete OpenGL backend. OpenGL and GL loader types remain private; no backend-specific public Horo header exists. | RenderBackendRegistry (public) |
| `HoroRenderMetal` (`HoroEngine::RenderMetal`) | Apple and `HORO_BUILD_RENDER_METAL` | Owns the concrete Metal backend. Objective-C++, Metal, Foundation, and QuartzCore types remain private; no backend-specific public Horo header exists. | RenderBackendRegistry (public) |

### Editor And Composition Roots

| Real target (alias) | Availability | Owner and public/private boundary | Direct first-party dependencies |
|---|---|---|---|
| `HoroEditorModel` (`HoroEngine::EditorModel`) | Always | Owns scene-document, selection, and viewport model code. Its intended contract spans selected `Editor/**` headers and internal `src/editor/**` headers; all of `src/` is currently exported to consumers. | Foundation, SceneModel, RuntimeScene (public) |
| `HoroEditorViewportScene` (`HoroEngine::EditorViewportScene`) | Always | Owns backend-neutral editor viewport scene/camera/light visualization geometry. It has no isolated installed public surface and exports `src/`. | EditorModel (public) |
| `HoroEditorRenderExtraction` (`HoroEngine::EditorRenderExtraction`) | Always | Owns editor-to-render snapshot extraction, mesh cache, picking, and asset drop conversion. It has no isolated installed public surface and exports `src/`. | EditorModel, EditorViewportScene (public) |
| `HoroEditorServices` (`HoroEngine::EditorServices`) | Always | Owns current GUI-neutral project, settings, localization, input orchestration, workspace model, editor bus, modal host, notification, menu, hierarchy, and status contracts under `Editor/**`. It also exports `src/`. | Foundation, Application, Platform, EditorModel, GameplayLua, GameplayModuleHost, GameplayBuild, Input, ProjectMigrations (public); Assets (private) |
| `HoroEditorViewportOpenGL` (`HoroEngine::EditorViewportOpenGL`) | GUI and OpenGL | Owns the OpenGL ImGui/viewport/presentation bridge. Backend, SDL, GLAD, and ImGui adapter details are private, but SDL is currently a public link dependency. | EditorViewportScene, RenderOpenGL (public) |
| `HoroEditorViewportMetal` (`HoroEngine::EditorViewportMetal`) | Apple, GUI, and Metal | Owns the Metal ImGui/viewport/presentation bridge. Objective-C++ and ImGui adapter details are private, but SDL is currently a public link dependency. | EditorViewportScene, RenderMetal (public) |
| `HoroGui` (`HoroEngine::Gui`) | Editor GUI only | Owns ImGui screens, modals, panels, workspace controllers, and design-system implementation. ImGui is private, while all of `include/` and `src/` are exported as public include roots. | EditorServices, Foundation (public); EditorRenderExtraction and Extensions (private) |
| `HoroHostModuleComposition` | Always | Non-installed application-host composition contract under `apps/common/`. It describes and activates the real linked module profiles for supported hosts without exposing an SDK header. | Foundation (public) |
| `horo-engine` | Always | Terminal/headless composition root. It currently composes only Application and does not yet compose CLI command or MCP targets. | Application (private) |
| `HoroEditor` | Editor GUI only | Graphical composition root. It selects GUI, editor services/extraction, runtime, extensions, platform, input, migrations, frontend, and enabled concrete viewport backends. | Composition-only private links |

## Public Header Inventory And Boundary

There are 388 non-placeholder headers under `include/Horo/` at this snapshot:

| Public path | Header count | Semantic owner |
|---|---:|---|
| `AI/` | 5 | AiRuntime and AiNavigationAdapter |
| `Animation/` | 8 | Animation |
| `Application/` | 7 | Application, except `GameplayBuildService.h` owned by GameplayBuild |
| `Assets/` | 14 | Assets |
| `Audio/` | 20 | AudioApi and AudioRuntime |
| `Cinematic/` | 9 | CinematicModel and CinematicRuntime |
| `Cli/` | 5 | CliHost |
| `Destruction/` | 7 | DestructionApi and DestructionRuntime |
| `Editor/` | 48 | EditorModel, EditorServices, and Gui |
| `Extensions/` | 12 | Extensions |
| `Foundation/` | 34 | Foundation, except the optional OpenTelemetry sink header |
| `Gameplay/` | 17 | GameplayApi, GameplayRuntime, GameplayModuleHost, GameplayBuild, and GameplayLua |
| `Math/` | 2 | Foundation |
| `Navigation/` | 13 | NavigationApi and NavigationRuntime |
| `Network/` | 19 | NetworkApi and NetworkRuntime |
| `Packages/` | 5 | Packages |
| `PCG/` | 8 | PcgApi and PcgRuntime |
| `Physics/` | 23 | Physics |
| `Platform/` | 6 | Platform |
| `PlatformServices/` | 10 | PlatformServices |
| `Prefab/` | 4 | Prefab |
| `Release/` | 4 | ReleaseModel |
| `Runtime/` | 56 | Runtime subsystem targets, including scene, rendering, input, UI, materials, and VFX |
| `Security/` | 4 | Security |
| `Terrain/` | 4 | TerrainApi and TerrainRuntime |
| `Vfx/` | 6 | VfxApi and VfxRuntime |
| `WorldStreaming/` | 30 | WorldStreamingApi, WorldStreamingRuntime, and adapters |
| `XR/` | 8 | XRApi and XRRuntime |

The current CMake boundary is broader than this semantic ownership map:

- every first-party library target publishes the repository-wide `include/`
  root, so linking any one library makes every Horo header discoverable;
- EditorModel, EditorViewportScene, EditorRenderExtraction, EditorServices,
  InputSdl, and Gui additionally publish all or part of `src/` to dependents;
- several implementation-only adapter targets therefore appear to have a
  public C++ surface even when no stable public header belongs to them;
- CMake cannot currently reject a consumer that includes a header owned by an
  undeclared target.

Consequently, header location communicates intended ownership but target usage
requirements do not enforce it. Separating SDK/public, internal-shared, and
target-private include surfaces is follow-up work owned by ARC-001.2.

### ARC-001.2 boundary update

ARC-001.2 replaces the broad build-tree usage requirements described in this
snapshot with target-specific staged public include views. The explicit ownership
registry accounts for every `include/Horo/` header, production `src/` roots are no
longer public usage requirements, and generated per-target consumers compile each
public header through its actual owner. Existing `Horo/...` include spellings are
preserved. The migration also makes the existing `RuntimeScene -> SceneModel`
public-header dependency explicit and removes the accidental GameplayApi-to-
GameplayRuntime header dependency by keeping registration data in GameplayApi.
`SceneComponents.h` is assigned to the neutral SceneModel contract so
PrimitiveCatalog does not create a reverse dependency on RuntimeScene.
The normative contract and caller migration guidance live in
[Header Visibility And Ownership](./header-visibility-and-ownership.md).

### ARC-001.3 dependency-direction update

ARC-001.3 adds a configure-time policy for every production target's direct
first-party dependency edges. The configured graph is rejected when a production
target has no policy entry or links a first-party target outside its explicit
allowlist. Known pre-policy violations are visible temporary exceptions with an
owner and removal ticket; CMake rejects an exception once its edge becomes stale.
The normative policy and current exception inventory live in
[System Design](./system-design.md#automated-target-policy).

### ARC-001.5 host composition update

The two supported executable roots now register and activate explicit descriptor
graphs through the shared, non-installed `HoroHostModuleComposition` target. The
headless profile contains only Foundation, Application, and the CLI host. The
editor profile mirrors the real linked first-party module closure and selects
exactly one concrete renderer and viewport adapter before window or presentation
creation. Optional OpenTelemetry participation is derived from the same build
selection that controls the executable link edge. Focused host-composition tests
validate deterministic graph ordering, headless exclusion, concrete-backend
selection, and rejection before activation of impossible host profiles.

## Documented Target Status

This table covers every canonical target named by System Design. Build System
uses `SceneRuntime`, `PluginApi`, and `PluginHost` for three entries where System
Design uses `RuntimeScene`, `ExtensionApi`, and `ExtensionHost`; those naming
conflicts are recorded rather than treated as additional implementations.

| Documented target | Status | Current evidence or gap |
|---|---|---|
| `HoroEngine::Foundation` | Implemented | `HoroFoundation` |
| `HoroEngine::Platform` | Implemented | `HoroPlatform` |
| `HoroEngine::Runtime` | Implemented | `HoroRuntime` |
| `HoroEngine::RuntimeUi` | Partial | `HoroRuntimeUi` owns typed identities plus distinct authored, cooked, scene-reference, mutable-instance, bounded retained-tree, atomic structural-command, incremental measure-arrange, immutable layout, and immutable render-extraction contracts. Service composition, concrete layout algorithms, and interaction routing remain planned. |
| `HoroEngine::Assets` | Partial | Target exists, but registry, import, cook, cache, and preview are combined beyond the narrower ownership described by System Design. |
| `HoroEngine::SceneModel` | Partial | Target exists but publicly depends on RenderApi, contrary to the documented Foundation-only level. |
| `HoroEngine::RuntimeScene` | Implemented | `HoroRuntimeScene`; Build System still calls this `HoroEngine::SceneRuntime`. |
| `HoroEngine::Physics` | Partial | Foundation/Assets target owns typed identities/descriptors, immutable settings and capability preflight. Optional private pinned Jolt composition owns explicit process registration and isolated empty-world lifecycle with rollback; Null/omitted builds retain the neutral API. Public SDK isolation, lifecycle and notice installation have regression tests. Full cook-target encoding/envelope verification, body/filter admission, registries, queues and native stepping remain planned. |
| `HoroEngine::AnimationApi` | Partial | `HoroAnimationApi` owns typed persistent asset identities, stable component/runtime identities, bounded generation-safe handles, immutable skeleton/skinning/clip assets, exact wrap/reverse traversal, allocation-free clip sampling, and bounded frame pose storage with dirty propagation, partial hierarchy evaluation, lifecycle fencing, diagnostics, and immutable cross-thread leases. Graph evaluation, render-resource realization, presentation palette publication, and backend integration remain planned. |
| `HoroEngine::AI` | Partial | `HoroAI` owns Foundation-only persistent gameplay-AI identities, bounded duplicate validation, stable errors, and SceneRuntime-incarnation-safe runtime handles. Registries, activation, blackboard storage, decision execution, and scheduling remain planned. |
| `HoroEngine::VfxApi` | Partial | `HoroVfxApi` owns Foundation-only generation-safe effect-system, emitter, particle-buffer and decal identities, canonical persistence encoding, typed stale access outcomes, and inert null/deterministic harness compositions. Simulation, asset compilation, pooling, rendering extraction and native resources remain planned. |
| `HoroEngine::CinematicModel` | Partial | `HoroCinematicModel` owns Foundation-only stable generation-safe sequence, track, keyframe, and property-binding identities with canonical persistence encoding and inert test compositions. Assets, curves, schemas, parsers, and runtime playback remain planned. |
| `HoroEngine::AudioApi` | Partial | Target owns typed identities, generation-safe handles, stable errors, device discovery/default selection, format/layout metadata, negotiation admission, capability probes, epoch-scoped timing/callback facts and borrowed planar-block validation; runtime commands and processing execution remain planned. |
| `HoroAudioBackendContract` | Contract only | Narrow non-installed internal interface for asynchronous backend operations, retained completions, render ports and device events. No native adapter, callback transport or control lifecycle implementation is implied. |
| `HoroEngine::AudioRuntime` | Planned | Architecture exists; no production target. |
| `HoroEngine::AudioMemory` | Implemented | Bounded aligned scratch arenas and typed-purpose fixed pools; production voice/graph/queue composition remains separate. |
| `HoroEngine::AudioCommands` | Implemented | Buffer-boundary typed commands and bounded MPSC/control/SPSC staging with critical reserves and barriers; clock-mapped scheduling and runtime execution remain separate. |
| `HoroEngine::AudioPlatform` | Absent | No target or implementation path. |
| `HoroEngine::AudioNull` | Absent | No target or implementation path. |
| `HoroEngine::NetworkApi` | Partial | Foundation-only target owns canonical bounded IPv4/IPv6/DNS endpoints, generation-checked listener/connection/peer handles, channel identity, stable protocol/message/schema/feature/close-reason identities, authority-epoch-scoped network-object identity, bounded immutable protocol/codec/replication registries, canonical bounded message framing and replication fingerprints, exact transport delivery capability negotiation/admission, and prepared packet-buffer/queue storage primitives; concrete synchronized transports and sessions remain planned. |
| `HoroEngine::NetworkRuntime` | Partial | Bounded session/scene-scoped network-object mapping exists with generation retirement, bidirectional lookup and immutable diagnostics; transport/session coordination, interest, baselines, capture/apply and scheduling remain planned. |
| `HoroEngine::NetworkTransportNull` | Planned | Null transport peer behind `NetworkApi`; Foundation-only, no `Platform`; no production target yet. |
| `HoroEngine::NetworkTransportGNS` | Planned | ADR-097 production direct-IP GNS transport peer behind `NetworkApi`; no production target yet. |
| `HoroEngine::RenderApi` | Implemented | `HoroRenderApi` |
| `HoroEngine::RenderFrontend` | Implemented | `HoroRenderFrontend` |
| `HoroEngine::RenderModuleAbi` | Planned | Renderer module manifest/distribution contracts exist; no target. |
| `HoroEngine::RenderModuleHost` | Planned | Runtime loading architecture exists; no target. |
| `HoroEngine::RenderOpenGL` | Implemented | Optional `HoroRenderOpenGL` |
| `HoroEngine::RenderNull` | Implemented | `HoroRenderNull` |
| `HoroEngine::RenderVulkan` | Planned | Architecture and source-layout slot exist; no target. |
| `HoroEngine::RenderMetal` | Implemented | Optional Apple-only `HoroRenderMetal` |
| `HoroEngine::RenderD3D12` | Planned | Architecture exists; no target. |
| `HoroEngine::Pipeline` | Planned | Architecture and placeholder source paths exist; no target. |
| `HoroEngine::Application` | Partial | Project/migration/observability subset exists; the broader shared use-case surface is not composed. |
| `HoroEngine::ProjectMigrations` | Implemented | `HoroProjectMigrations` |
| `HoroEngine::EditorModel` | Partial | Target exists; intended public contract is mixed with exported `src/` headers. |
| `HoroEngine::EditorServices` | Partial | Target exists but aggregates many domains and publicly links concrete GameplayLua. |
| `HoroEngine::Gui` | Partial | Target exists; public/private include boundary is not enforced. |
| `HoroEngine::Mcp` | Planned | Placeholder paths and architecture exist; no target. |
| `HoroEngine::GameplayApi` | Implemented | `HoroGameplayApi` |
| `HoroEngine::ExtensionApi` | Partial | ABI/model headers exist inside combined `HoroExtensions`; no isolated target. Build System calls this `PluginApi`. |
| `HoroEngine::ExtensionHost` | Partial | Host implementation exists inside combined `HoroExtensions`; no isolated target. Build System calls this `PluginHost`. |
| `HoroEditor` | Implemented | Optional graphical composition root. |
| `horo-engine` | Partial | Executable exists but only links Application; documented CLI and MCP composition is missing. |
| `horopak` | Planned | `apps/horopak/package_tool/.gitkeep` exists; no executable target. |

Build System additionally documents `HoroEngine::RenderBackendRegistry`,
`HoroEngine::EditorRenderExtraction`, `HoroEngine::EditorViewportScene`,
`HoroEngine::EditorViewportOpenGL`, and `HoroEngine::EditorViewportMetal`; all
five are implemented. Its `HoroEngine::TestSdk`, `EditorSourceEditor`, and
`EditorGraphEditor` targets are planned and have no production target.

Current but non-canonical targets are Transitional: OpenTelemetry,
GameplayRuntime, GameplayModuleHost, GameplayLua, GameplayBuild, Input,
InputSdl, Extensions, HostModuleComposition, and the renderer registry. They represent useful real
boundaries, but the canonical target lists must either adopt them or assign
their responsibilities to another documented target.

## Desired Tree Comparison

The desired tree is explicitly aspirational. The following differences are the
ones that affect target ownership or would mislead a migration ticket.

| Desired area | Status | Current repository evidence |
|---|---|---|
| `cmake/HoroCompilerOptions.cmake`, `HoroTargets.cmake`, `HoroDependencies.cmake`, `HoroPackaging.cmake`, `HoroSDK.cmake` | Partial | Equivalent responsibilities are partly in root CMake and differently named files such as `Dependencies.cmake`; several named files are absent. |
| `include/Horo/` module contracts | Partial | 130 headers exist; target-specific public surfaces are enforced, while several documented domains remain placeholders or absent. |
| `src/foundation`, `platform`, `application`, `editor`, `runtime`, `gameplay`, `extensions` | Partial | Active implementations exist, but most production targets are declared centrally in one `src/CMakeLists.txt`. |
| Dedicated `src/asset`, `scene`, `render`, `pipeline`, `physics`, `audio`, and `network` module roots | Partial | Asset, scene, and render code currently live under `src/runtime/`; pipeline, physics, audio, and network targets are absent. |
| `src/transport/mcp` and editor MCP bridge | Planned | Placeholder interface/editor paths exist; no MCP production target. |
| `apps/HoroEditor` | Implemented | Graphical process entry and app composition exist. |
| `apps/horo-engine` | Partial | Process entry exists without the documented CLI/MCP composition. |
| `apps/horopak` | Planned | Placeholder only. |
| `tools/*` | Planned | Tool directories are placeholders only. |
| `sdk/` | Partial | Gameplay SDK package files and a self-contained versioned extension C ABI CMake package are generated from current CMake/scripts; the desired schema/template/tooling surface is incomplete. |
| `tests/` | Implemented | Broad unit/integration/UI coverage exists; there is no `HoroEngine::TestSdk` production-style support target. |
| `deprecated/` exclusion | Implemented | Root CMake does not discover or compile the deprecated tree. |

## Dependency Direction Findings

### Confirmed violations

1. **SceneModel depends upward on RenderApi.** System Design places both
   SceneModel and RenderApi at the Foundation-only contract level. The real
   `HoroSceneModel -> HoroRenderApi` public edge lets render types define the
   scene primitive model boundary.
2. **Private editor implementation headers are public usage requirements.** Six
   targets export `src/` or a broad source path. This violates the rule that
   private implementation headers do not leak through public CMake interfaces.
3. **Public header ownership is not enforceable.** Every first-party library
   publishes the full repository `include/` root, so undeclared cross-module
   includes compile even when the matching target is not linked.
4. **EditorServices publicly selects a concrete scripting adapter.** The public
   `EditorServices -> GameplayLua` edge places a concrete Lua implementation
   below every EditorServices consumer instead of selecting it at a composition
   boundary.

### Contract and implementation divergences

These require a focused decision before being classified as code defects:

- RenderNull, RenderOpenGL, and RenderMetal depend on RenderBackendRegistry,
  while the normative dependency diagram permits concrete backends to depend on
  Platform plus their owning API. The static registration model predates the
  documented RenderModuleAbi/RenderModuleHost boundary.
- `HoroExtensions` combines ExtensionApi, ExtensionHost, marketplace transport,
  platform loading, and an asset importer in one target. This prevents the
  documented API-to-host dependency direction from being represented in CMake.
- Assets combines registry/provider contracts with importer, cooker, cache, and
  preview implementations despite System Design describing those as later
  consumers or separate targets.
- The headless configuration excludes Gui, InputSdl, backend viewport bridges,
  and HoroEditor, but still builds editor model/services/extraction/viewport
  scene and Extensions as default targets. This does not match the Build System
  claim that the CLI profile excludes EditorModel and EditorServices.
- System Design, Build System, and CMake disagree on `RuntimeScene` versus
  `SceneRuntime` and `Extension*` versus `Plugin*` names. There must be one
  canonical naming source before migration scripts or target checks are added.

No reverse edge from Application to ProjectMigrations, RuntimeScene to editor,
Foundation to higher layers, or RenderApi to a concrete renderer was found.
Native OpenGL, Metal, SDL, Lua, UFBX, curl, and platform framework dependencies
remain inside their current concrete/owning targets at link time.

## Migration Inputs

This inventory supports the following focused sequence without moving
production code in this ticket:

1. Preserve the ARC-001.2 target-specific header registry when adding or moving
   public contracts; never restore repository-wide public include roots.
2. Resolve the SceneModel/RenderApi ownership direction before adding more scene
   primitives or renderer-facing mesh fields.
3. Decide whether the current non-canonical targets become canonical and align
   System Design and Build System on one target vocabulary.
4. Split ExtensionApi from ExtensionHost and isolate concrete gameplay scripting
   selection at a composition root.
5. Make headless profile membership explicit and test it from generated target
   graphs.
6. Add missing Pipeline, MCP, horopak, renderer module host/ABI, and later
   subsystem targets only through their owning roadmap tickets.

Any later migration ticket should regenerate both inspected configurations and
update this inventory in the same change when it changes a production target,
public include boundary, canonical target status, or first-party dependency
edge.

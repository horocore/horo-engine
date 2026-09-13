# Architecture

This directory defines the required architecture of Horo Engine. Documents are
normative: they describe the system to build and the rules all implementations
must follow.

## How To Read

For a first architecture review, read:

1. [System Design](./foundation/system-design.md)
2. [Architecture Glossary](./foundation/glossary.md)
3. [Error And Diagnostics](./foundation/error-and-diagnostics.md)
4. [Scene Math](./foundation/scene-math.md)
5. [Configuration System](./foundation/configuration-system.md)
6. [Concurrency And Job System](./foundation/concurrency-and-jobs.md)
7. [Runtime Lifecycle](./runtime/runtime-lifecycle.md)
8. [Scene Runtime](./runtime/scene-runtime.md)
9. the host or subsystem documents relevant to the change

`system-design.md` defines the map and dependency direction. Detailed documents
own their specific contracts; the overview does not override them.

## Directory Layout

```text
architecture/
  README.md          reading order, index, core rules, review checklist
  desired-project-tree.md target repository, game project, package, cache, and release trees
  foundation/        system-wide contracts and dependency boundaries
  runtime/           engine runtime subsystems and asset flow
  editor/            GUI, documents, workspace, panels, tabs, and modals
  extensions/        gameplay modules and external plugins
  packages/          asset packs, game libraries, hybrid packages, templates
  interfaces/        CLI and MCP host adapters
  security/          running-application trust and capability policy
  delivery/          development, build, test, quality, and CI
  observability/     logs, metrics, profiling, and diagnostics
  release/           artifacts, signing, distribution, and updates
```

Folder placement indicates the primary owner, not permission to bypass the
dependency direction in [System Design](./foundation/system-design.md).

## Foundation

- [Desired Project Trees](./desired-project-tree.md): target Horo Engine repo,
  game project, package repository, `.horopkg`, cache, and release output tree
  structures.
- [System Design](./foundation/system-design.md): hosts, modules, dependency
  direction, application boundaries, and composition rules.
- [Current Target And Dependency Inventory](./foundation/current-target-and-dependency-inventory.md):
  evidence-backed CMake targets, public-header ownership, desired-tree status,
  and current dependency-direction gaps.
- [Header Visibility And Ownership](./foundation/header-visibility-and-ownership.md):
  enforced public, internal-shared, and target-private header boundaries.
- [Internal Module Descriptor](./foundation/internal-module-descriptor.md): inert
  built-in module metadata and pre-composition graph validation.
- [Architecture Glossary](./foundation/glossary.md): canonical cross-document
  terminology.
- [Error And Diagnostics](./foundation/error-and-diagnostics.md): results,
  stable errors, diagnostics, assertions, and host translation.
- [Scene Math](./foundation/scene-math.md): coordinate, transform, projection,
  bounds, ray, and fallible-math contracts shared across runtime and editor.
- [Configuration System](./foundation/configuration-system.md): typed settings,
  precedence, immutable snapshots, dynamic reload, and secret references.
- [Project Versioning And Migration](./foundation/project-versioning-and-migration.md):
  one Horo project version, automatic project-open migration chains, release
  bundles, staging, journaled publication, recovery, and Welcome/loading UX.
- [Cross-Engine Project Interchange](./foundation/cross-engine-interchange.md):
  application-owned foreign-project conversion, adapter isolation, fidelity
  reporting, compatibility, parser trust boundaries, and transactional publication.
- [Concurrency And Job System](./foundation/concurrency-and-jobs.md): workers,
  task groups, cancellation, progress, affinity, backpressure, and shutdown.
- [Ownership And Resource Lifetime](./foundation/ownership-and-resource-lifetime.md):
  RAII, handles, leases, allocation domains, caches, and cross-thread lifetime.
- [Platform Abstraction](./foundation/platform-abstraction.md): paths, files,
  windows, events, processes, clocks, native dialogs, credentials, and crash
  services.
- [Android Platform Host](./foundation/android-platform-host.md): Android
  activity lifecycle, replaceable native surfaces, permissions, storage,
  packaging, deployment, resource pressure, and standalone-XR prerequisites.
- [Android Host, Target and Ownership](../adr/171-android-host-target-and-ownership.md):
  GameActivity composition, initial API/ABI/Vulkan baseline, native-resource
  ownership, lifecycle generations, packaging, migration, and qualification.
- [Engine Data Bus](./foundation/engine-data-bus.md): process-scoped typed
  notifications.

## Runtime

- [Runtime Lifecycle](./runtime/runtime-lifecycle.md): startup, frame phases,
  fixed-step simulation, play state, suspension, and shutdown.
- [Scene Runtime](./runtime/scene-runtime.md): runtime scene definitions, ECS
  ownership, systems, structural changes, references, and scene transitions.
- [Rendering Architecture](./runtime/rendering-architecture.md): render
  extraction, frontend/backend boundaries, render graph, GPU resources, and
  null rendering.
- [Renderer Resource Identity and Descriptors](../adr/027-renderer-resource-identity-and-descriptors.md):
  resident resource taxonomy, owner/slot/generation handles, immutable
  descriptors, validation, replacement, and deferred retirement.
- [Render Backend Parity Contract](./runtime/render-backend-parity-contract.md):
  equal lifecycle, presentation, editor integration, and verification obligations
  for interactive renderer backends.
- [Renderer Capability, Limits and Product Profiles](../adr/028-renderer-capability-limits-and-product-profiles.md):
  reported versus effective support, driver restrictions, typed format/limit
  admission, and Baseline through Ultra quality policy.
- [OpenGL Core Profile and Platform Policy](../adr/029-opengl-core-profile-and-platform-policy.md):
  desktop 4.1 Core admission, platform qualification, context negotiation,
  and explicit macOS deprecation/migration behavior.
- [Metal Platform and Feature Baseline](../adr/030-metal-platform-and-feature-baseline.md):
  macOS 14, native GPU-family admission, MSL 2.4, effective feature support,
  and deployment/qualification policy.
- [Vulkan Loader, Platform and Version Baseline](../adr/031-vulkan-loader-platform-and-version-baseline.md):
  Vulkan 1.3 core, required enabled features, system-loader ownership,
  Windows/Linux WSI qualification, and deferred portability scope.
- [D3D12 Baseline and Agility SDK Policy](../adr/032-d3d12-baseline-and-agility-sdk-policy.md):
  Windows/feature-level/Shader Model admission, pinned Agility dependencies,
  executable-owned activation, redistribution, and driver qualification.
- [Presentation and Display Ownership](../adr/033-presentation-and-display-ownership.md):
  platform windows/display facts, renderer surface lifetime, host intent,
  resolved output, transition safe points, and multi-surface boundaries.
- [GPU Memory and Residency Ownership](../adr/034-gpu-memory-and-residency-ownership.md):
  native allocator ownership, backing-capacity accounting, streaming reservations,
  bounded pressure policy, and deferred reclamation.
- [Shader Source and Intermediate Representation](../adr/035-shader-source-and-intermediate-representation.md):
  HLSL authoring, target-specific SPIR-V/DXIL routes, normalized reflection,
  compiler identity, source diagnostics, and cooked/runtime boundaries.
- [Shader Toolchain Lock And Qualification](./runtime/shader-toolchain-lock.md):
  exact production compiler artifacts, host support, invocation policy,
  deterministic package contract, and qualification evidence.
- [Renderer Distribution And Availability](./runtime/renderer-distribution-and-availability.md):
  optional renderer components, install/repair/probe states, launcher recovery,
  and selection policy.
- [Renderer Module Package Manifest](./runtime/renderer-module-package-manifest.md):
  signed component metadata, private module ABI, variants, validation, and load
  policy.
- [Material And Shader Model](./runtime/material-and-shader-model.md): standard
  PBR material model, shader variants, material instances, feature tiers, and
  pipeline cache.
- [Advanced Rendering Architecture](./runtime/advanced-rendering-architecture.md):
  lighting, shadows, global illumination, reflections, post-processing, TAA,
  upscaling, and high-end feature tiers.
- [Animation Architecture](./runtime/animation-architecture.md): skeletal
  animation, clips, animation graphs, blend trees, IK, root motion, retargeting,
  and animation events.
- [Animation Ownership, Update Order and Clock](../adr/061-animation-ownership-update-order-and-clock.md):
  pose ownership, fixed-tick advancement, pause/step/rate semantics, root-motion
  timing, and physics/render handoff.
- [VFX And Particles Architecture](./runtime/vfx-and-particles-architecture.md):
  CPU/GPU particle systems, VFX graphs, decals, and volumetric effects.
- [Character Controller Architecture](./runtime/character-controller-architecture.md):
  kinematic capsule controller, slopes, steps, moving platforms, surface
  materials, and surface events.
- [Character Controller Ownership, Implementation and Update Order](../adr/089-character-controller-ownership-implementation-and-update-order.md):
  Horo-owned bounded query solver, per-scene Character world, fixed-tick command/
  root-motion cadence, platform/orientation composition and transform publication.
- [Character Dynamic-Body Visibility, Push and Proxy Policy](../adr/090-character-dynamic-body-visibility-push-and-proxy-policy.md):
  explicit disabled/obstacle/one-way/bidirectional modes, private kinematic
  presence proxy, next-tick reaction and fail-closed capability fallback.
- [Footstep and Locomotion Event Ownership](../adr/091-footstep-and-locomotion-event-ownership.md):
  Animation-owned footstep timing, Character-owned committed surface/facts and
  bounded post-commit Audio/VFX presentation correlation.
- [Character Controller Determinism and State Composition](../adr/092-character-controller-determinism-and-state-composition.md):
  complete canonical Character state/codec, aggregate Physics/world checkpoints,
  exact hashes, diagnostic tolerances and bounded restore/resimulation history.
- [Physics Architecture](./runtime/physics-architecture.md): fixed-step world
  ownership, transform authority, collision events, queries, and determinism.
- [Canonical Physics Solver, Units and Tolerances](../adr/084-canonical-physics-solver-units-and-tolerances.md):
  pinned Jolt baseline, private native boundary, SI/right-handed Y-up conventions,
  fp32 local clusters, tolerance/scale profile, platforms, licensing, and upgrades.
- [Physics Shape Authoring, Cook and Runtime Boundary](../adr/085-physics-shape-authoring-cook-and-runtime-boundary.md):
  typed collider descriptors, deterministic target-keyed cook artifacts, immutable
  runtime shape leases, motion compatibility, limits, and replacement semantics.
- [Collision Layer, Profile and Query Channel Policy](../adr/086-collision-layer-profile-and-query-channel-policy.md):
  project-stable typed filter identities, symmetric simulation responses, complete
  reusable profiles, query intent and private generation-scoped compiled tables.
- [Scene-to-Physics Ownership and Conversion](../adr/087-scene-to-physics-ownership-and-conversion.md):
  explicit authored body/collider/constraint producers, Physics-owned scene plans,
  detached world candidates and atomic aggregate activation/rollback.
- [Physics Determinism Capability and Support Tiers](../adr/088-physics-determinism-capability-and-support-tiers.md):
  fail-closed tier negotiation, exact execution fingerprints, same-build/platform
  support target, future cross-platform groups, exclusions and evidence gates.
- [Audio Architecture](./runtime/audio-architecture.md): ADR-backed ownership,
  clocks, formats, assets, mixer, spatial, devices, tooling and explicit 1.0 versus
  Post-1.0 product boundaries.
- [Audio Runtime Ownership and Update Order](../adr/062-audio-runtime-ownership-and-update-order.md):
  process/control/callback authority, runtime and device states, scene-context
  barriers, suspend/recovery, fatal failure, and teardown.
- [Audio Sample Format and Channel Layout](../adr/063-audio-sample-format-and-channel-layout.md):
  planar binary32 processing, explicit speaker/Ambisonic order, alignment,
  silence, denormals, layout conversion, and clipping boundaries.
- [Audio Asset and Cook Boundary](../adr/064-audio-asset-and-cook-boundary.md):
  generic AST orchestration and publication, Audio-owned media semantics,
  deterministic cook identity, and runtime payload validation.
- [Mixer Topology and Constrained DAG](../adr/065-mixer-topology-and-constrained-dag.md):
  legal bus/send/return routing, deterministic processing order, feedback
  rejection, and generation-safe graph compilation and swap.
- [Spatial Provider and Required Capability](../adr/066-spatial-provider-and-required-capability.md):
  typed spatial profiles, deterministic provider preflight, explicit optional
  fallback, required-capability failure, and recovery observability.
- [Platform Audio Backend Strategy](../adr/067-platform-audio-backend-strategy.md):
  equal-peer WASAPI, Core Audio, PipeWire, SDL3, and Null roles; the supported
  1.0 matrix; compile-time composition; selection; parity; and qualification.
- [Music Transport and Cross-System Ownership](../adr/068-music-transport-and-cross-system-ownership.md):
  sample scheduling, gameplay/narrative decisions, cinematic and animation clock
  bridges, localized media, captions, and semantic save-state boundaries.
- [Audio Extension Capability and ABI](../adr/069-audio-extension-capability-and-abi.md):
  generic EXT/PKG handoff, typed Audio capability families, the stricter Audio RT
  ABI, transactional registration, trust, owner leases, and unload barriers.
- [Capture and Voice I/O Ownership](../adr/070-capture-and-voice-io-ownership.md):
  permission-aware input sessions, bounded timestamped PCM, monitoring, recording,
  NET packet-policy exclusion, speech/editor boundaries, and privacy.
- [Procedural Audio Graph Ownership](../adr/071-procedural-audio-graph-ownership.md):
  compiled sound-generator assets, deterministic typed graphs, frontend/mixer
  reuse, extension nodes, editor boundaries, runtime limits, and retirement.
- [Audio Middleware Integration Model](../adr/072-audio-middleware-integration-model.md):
  event-bridge versus backend-replacement ownership, stable frontend identities,
  native coexistence, budgets, bank activation, profiling, and distribution.
- [Input Architecture](./runtime/input-architecture.md): input snapshots, action
  maps, focus, capture, modal routing, and simulation input frames.
- [Game UI And HUD](./runtime/game-ui-and-hud.md): runtime game menus, HUDs,
  canvases, UI primitives, focus/navigation, and UI rendering.
- [Runtime UI Ownership, Scope and Update Order](../adr/073-runtime-ui-ownership-scope-and-update-order.md):
  game/player/scene/viewport scopes, lifecycle, frame phases, pause, input,
  extraction, unload, compatibility, and shutdown.
- [Runtime UI Layout Units and Measure-Arrange](../adr/074-runtime-ui-layout-units-and-measure-arrange.md):
  logical units, constraint precedence, anchors, intrinsic/flex/grid sizing,
  deterministic two-phase layout, overflow, rounding, and compatibility.
- [Runtime UI Font Asset, Family and Fallback](../adr/075-runtime-ui-font-asset-family-and-fallback.md):
  font source/face/family identity, deterministic matching and fallback, cook
  dependencies, platform discovery, missing coverage, and runtime lifetime.
- [Runtime UI Style Asset, Token and Inheritance](../adr/076-runtime-ui-style-asset-token-and-inheritance.md):
  typed runtime tokens, single-parent inheritance, element/state precedence,
  style cook/publication, accessibility overlay, and editor separation.
- [Runtime UI Animation Clock and Time Domain](../adr/077-runtime-ui-animation-clock-and-time-domain.md):
  simulation, unscaled, transition, preview, test and manual clocks; pause/rate,
  lifecycle, cancellation, reduced motion, deterministic advance, and shutdown.
- [Runtime UI Input Context and Player Routing](../adr/078-runtime-ui-input-context-and-player-routing.md):
  device/user/player/viewport separation, UI/gameplay priority, modal exclusivity,
  consumption, assignment, focus/capture, modality, and editor-play routing.
- [Runtime UI Binding Provider Schema, Identity and Lifetime](../adr/079-runtime-ui-binding-provider-schema-identity-and-lifetime.md):
  typed provider/property schemas, scoped instances, immutable read snapshots,
  owner-validated writes, registration/revocation, module unload, and compatibility.
- [Runtime UI Presentation Scope, Layer and Route](../adr/080-runtime-ui-presentation-scope-layer-and-route.md):
  orthogonal owner/audience/route/band/visibility dimensions, fixed presentation
  bands, transactional scoped stacks, loading/debug policy, input, transitions,
  and immutable rendering plans.
- [Runtime UI and Localization Ownership Boundary](../adr/081-runtime-ui-and-localization-ownership-boundary.md):
  catalog/locale/formatting authority, localized references, shaping/layout,
  translation/font/asset fallback, snapshots, change notification, and unload.
- [Runtime UI Accessibility Capability and Ownership](../adr/082-runtime-ui-accessibility-capability-and-ownership.md):
  semantic nodes, settings projection, native platform bridges, capability truth,
  editor validation, qualification evidence, lifecycle, and unsupported behavior.
- [UI Template Identity, Schema and Expansion](../adr/083-ui-template-identity-schema-and-expansion.md):
  template/local/instance identity, typed parameters and slots, insertion versus
  linked instancing, deterministic expansion, explicit rebase, detach, and cook.
- [Networking Architecture](./runtime/networking-architecture.md): optional
  handle-based transports, session/authentication runtime, bounded I/O, and
  remote security.
- [Default Real-Time Transport Backend](../adr/097-default-real-time-transport-backend.md):
  GameNetworkingSockets direct-IP baseline, private native encapsulation,
  security/traversal ownership, bounded lifecycle and optional provider policy.
- [Protocol, Session and Trust Policy](../adr/098-protocol-session-and-trust-policy.md):
  transport-to-session admission gate, compatibility, exposure-specific peer
  trust, bounded credential verification and active-session publication.
- [Replication Ownership, Authority and Compatibility](../adr/099-replication-ownership-authority-and-compatibility.md):
  explicit world roles, stable schema/FieldId identity, owner-safe capture/apply,
  compatibility, object generations and prohibited ambient replication.
- [Prediction Capability Tiers and Determinism Policy](../adr/100-prediction-capability-tiers-and-determinism-policy.md):
  non-predicted baseline, local candidates, qualified rollback provider closure,
  bounded histories/replay, correction and side-effect reconciliation.
- [Interest, Priority and Network Budget Model](../adr/101-interest-priority-and-network-budget-model.md):
  renderer-independent network profiles, immutable relevancy facts,
  per-connection ledgers, weighted fairness and bounded overload behavior.
- [Runtime Network Modes and Authority Exposure](../adr/102-runtime-network-modes-and-authority-exposure.md):
  package support versus runtime selection, standalone/client/listen/dedicated
  host plans, scoped role capabilities and generation-safe lifecycle.
- [Network Project Configuration and Build-Profile Ownership](../adr/103-network-project-configuration-and-build-profile-ownership.md):
  shared typed resolution, project/preview/release source boundaries, role-aware
  product manifests, credential-provider isolation and migration.
- [Asset Pipeline](./runtime/asset-pipeline.md): import, cook, package, runtime
  loading, cache, and hot reload.
- [Prefab Architecture](./runtime/prefab-architecture.md): dual-role authoring
  templates, runtime dynamic spawning (`CookedPrefab`), and capability tiers.
- [Prefab Override Property Identity and Delta Operations](../adr/093-prefab-override-property-identity-and-delta-operations.md):
  stable component/property/element addressing, typed delta algebra, canonical
  equality/order, transactional rebase and lossless conflict/orphan preservation.
- [Prefab Nested Composition and Variant Inheritance](../adr/094-prefab-nested-composition-and-variant-inheritance.md):
  stable nested-placement edges, single-parent variants, deterministic precedence,
  combined-graph validation, transactional propagation and flattened cook output.
- [Prefab Cook Boundary and Artifact Model](../adr/095-prefab-cook-boundary-and-artifact-model.md):
  single prefab resolver, expanded-scene versus spawnable-template artifacts,
  complete cache identity, generation retention, hot reload and shipping policy.
- [Prefab External Reference and Binding Slot Contract](../adr/096-prefab-external-reference-and-binding-slot-contract.md):
  portable local/asset references, rejected scene capture, stable typed external
  slots, explicit instance bindings and transactional boundary validation.
- [Built-In Scene Primitives](./runtime/built-in-scene-primitives.md): core
  procedural meshes, collider shapes, and scene object primitives available
  without external packages.
- [Debug Console And Overlays](./runtime/debug-console-and-overlays.md): runtime
  console commands, variables, in-game terminal, debug overlays, and diagnostics.
- [Platform Services Architecture](./runtime/platform-services-architecture.md):
  achievements, leaderboards, cloud saves, presence, friends, backend adapters,
  offline queues, and null services.
- [Terrain And Foliage Architecture](./runtime/terrain-and-foliage-architecture.md):
  heightfields, terrain layers, instanced foliage, wind, LOD, collision,
  streaming, and editor tools.
- [Terrain and Foliage Ownership, Data, Tier and Lifecycle](../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md):
  public/runtime module boundaries, authored/cooked/live data strata, typed identity
  and revisions, provider-neutral tier resolution, aggregate lifecycle and retirement.
- [Terrain Source, Cooked Tile, Cache and Streaming Ownership](../adr/138-terrain-source-cooked-tile-cache-and-streaming-ownership.md):
  canonical Terrain import/cook ownership, deterministic dataset manifests, cache
  authorities, typed World Streaming residency and seam-safe generation replacement.
- [Terrain Render Extraction, Material, LOD and Tier Boundary](../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md):
  immutable Terrain/Foliage render candidates, material/permutation admission,
  renderer-owned per-view LOD/visibility and core-1.0 versus post-1.0 GPU recipes.
- [Foliage Placement, Baked/Dynamic State and Eviction Ownership](../adr/140-foliage-placement-baked-dynamic-state-and-eviction-ownership.md):
  deterministic baked placement, ephemeral runtime overlays, durable canonical deltas,
  capacity policy, save handoff and no-loss World Streaming eviction.
- [Terrain/Foliage Cross-System Ownership and Readiness](../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md):
  immutable producer snapshots, typed Render/Physics/Navigation receipts, owner-safe-
  point staging, aggregate activation, rollback and reverse-dependency retirement.
- [Terrain/Foliage Document, Tool, Undo and Preview Ownership](../adr/142-terrain-foliage-document-tool-undo-and-preview-ownership.md):
  persistent authoring documents, typed tool routing, bounded tile-patch history,
  isolated preview and distinct source-save/cook/runtime-persistence boundaries.
- [Terrain/Foliage Scale Budgets, Observability and Feature Boundary](../adr/143-terrain-foliage-scale-budgets-observability-and-feature-boundary.md):
  versioned core/high-end active scale, memory, streaming, cook, editor/headless gates,
  required measurements and core-1.0 versus post-1.0 recipe qualification.
- [World Streaming Architecture](./runtime/world-streaming-architecture.md):
  streaming cells, volumes, priority, budgets, server authority, and editor
  world-composition tools.
- [Coordinate Precision And Origin Rebasing](./runtime/coordinate-precision-and-origin-rebasing.md):
  canonical 64-bit world coordinates, floating origin rebasing, camera-relative
  rendering, and subsystem synchronizations.
- [Save Game And Persistence](./runtime/save-game-and-persistence.md): runtime
  save state, slot format, migration, cloud save, integrity, and secure archive
  loading.
- [Save Archive Container and Compatibility Policy](../adr/112-save-archive-container-and-compatibility-policy.md):
  portable framing, canonical logical state, independent version axes, typed
  state/content/publication identities, release declarations and support horizon.
- [Local Storage, User Profile and Slot Ownership](../adr/113-local-storage-user-profile-and-slot-ownership.md):
  product/environment/user/profile namespaces, slot/category identity, path
  authority, multi-user fallback and profile-switch fencing.
- [Canonical Runtime World Persistence Boundary](../adr/114-canonical-runtime-world-persistence-boundary.md):
  authoring-base/runtime-override composition, state classification,
  subsystem-owned canonical adapters and aggregate capture/restore.
- [Cloud Save Authority, Revision and Conflict Policy](../adr/115-cloud-save-authority-revision-and-conflict-policy.md):
  local authority, provider CAS/lease revisions, offline lineage classification,
  conflict preservation and coordinator-owned resolution.
- [Save Data Threat Model and Trust Policy](../adr/116-save-data-threat-model-and-trust-policy.md):
  untrusted save sources, bounded admission, integrity/authenticity/replay policy,
  tool capabilities, credentials and development/shipping profiles.
- [Navigation And AI Architecture](./runtime/navigation-and-ai-architecture.md):
  NavMesh, pathfinding, dynamic obstacle overlays, perception, crowd, blackboard,
  and editor bake tooling.
- [Default Navigation Provider and Recast-Detour Adoption](../adr/104-default-navigation-provider-and-recast-detour-adoption.md):
  exact Recast/Detour pin, private module/build profile, threading/determinism,
  optional tile-cache/crowd capabilities and grounded-only scope.
- [Navigation Asset and Scene Ownership Boundary](../adr/105-navigation-asset-and-scene-ownership-boundary.md):
  authored definition/Scene intent, immutable bake capture, cooked artifact
  provenance, runtime topology ownership, migration and missing-data policy.
- [Navigation Bake Ownership, Transaction and Cache](../adr/106-navigation-bake-ownership-transaction-and-cache.md):
  shared application operation, latest-wins revision capture, workspace locking,
  complete cache identity, atomic publication, terminal states and recovery.
- [Navigation Query Consistency and Snapshot Ownership](../adr/107-navigation-query-consistency-and-snapshot-ownership.md):
  combined topology/overlay snapshots, immediate versus async queries, owner-thread
  completion publication, coverage outcomes, staleness and lease-safe retirement.
- [Dynamic Overlay, Carving and Tile-Rebuild Policy](../adr/108-dynamic-overlay-carving-and-tile-rebuild-policy.md):
  change-category ownership across avoidance, blockers/gates, optional carving,
  runtime rebuild, authored recook and streamed cooked tiles.
- [Avoidance, Crowd and Renderer-Independent Budget](../adr/109-avoidance-crowd-and-renderer-independent-budget.md):
  path/safe-velocity/movement ownership, optional best-effort DetourCrowd,
  deterministic admission, independent project scale/quality and safe overload.
- [Navigation Editor Surface and Command Ownership](../adr/110-navigation-editor-surface-and-command-ownership.md):
  definition/Scene documents, bake operation projection, dockable inspection,
  transient modals, viewport overlays and provider UI limits.
- [Gameplay AI Document, Panel and Runtime-Debug Ownership](../adr/111-gameplay-ai-document-panel-and-runtime-debug-ownership.md):
  blackboard/decision/EQS document routes, shared graph commands, derived compile
  state, immutable live inspection and safe-point debug actions.

- [Cinematic Sequencer Architecture](./runtime/cinematic-sequencer-architecture.md):
  timeline, tracks, clock authority, typed property bindings, evaluation phase,
  and playback integration.
- [Playback Ownership, Frame Order and Determinism](../adr/117-playback-ownership-frame-order-and-determinism.md):
  runtime-service player lifetime, stable activation identity, immutable evaluation
  batches, replay/headless evidence and random-access seek.
- [Animation, Character and Gameplay Authority During Cinematics](../adr/118-animation-character-and-gameplay-authority-during-cinematics.md):
  per-joint pose override/blend, Character collision-root authority, gameplay control
  leases, typed suppression and whole-game pause semantics.
- [Camera Authority During Cinematics](../adr/119-camera-authority-during-cinematics.md):
  per-view runtime/PIE/editor authority, cut entry and exit handoff, immutable
  frame selection, tiered transitions and backend-neutral render snapshots.
- [Cinematic Event Dispatch and Audio Coupling Boundary](../adr/120-cinematic-event-dispatch-and-audio-coupling-boundary.md):
  cooked typed EventTrack bindings, application-owned safe-point dispatch, explicit
  failure outcomes and AudioFrontend coupling through AUD-family authority.
- [Cinematic Editor Document and Authoring Context](../adr/121-cinematic-editor-document-and-authoring-context.md):
  persistent sequence asset tabs, shared command/save/conflict ownership, detachable
  scene authoring context, stale-reference inspection and disposable preview state.
- [Cinematic Trigger Sources and Capability Policy](../adr/122-cinematic-trigger-sources-and-capability-policy.md):
  common typed start admission for gameplay, scene, event and tooling sources;
  capability/trust/authority checks, packaged-build gating and typed denials.
- [VFX CPU Stage Order, Determinism and Gameplay Coupling](../adr/123-vfx-cpu-stage-order-determinism-and-gameplay-coupling.md):
  ordered CPU particle stages, atomic commit, counter-based per-particle RNG,
  qualified cross-platform reproduction and typed gameplay payload boundaries.
- [VFX GPU Simulation, Readback and Compute Fallback](../adr/124-vfx-gpu-simulation-readback-and-compute-fallback.md):
  visual-only GPU authority, cooked opt-in asynchronous readback, explicit
  compute-less fallback and unified CPU/GPU/readback admission accounting.
- [VFX Transparency, Sorting and Pass Placement](../adr/125-vfx-transparency-sorting-and-pass-placement.md):
  semantic particle pass/depth mapping, stable per-view CPU/GPU sorting, additive
  exemption, finite sort ceilings and measured frame-time targets.
- [VFX Graph Compilation and Runtime Representation Convergence](../adr/126-vfx-graph-compilation-and-runtime-representation-convergence.md):
  stack/graph authoring convergence on one compiled descriptor, offline-only
  lowering, deterministic artifacts and explicit payload/kernel compatibility.
- [VFX Decal Projection, Lifetime and Rendering Path Policy](../adr/127-vfx-decal-projection-lifetime-and-rendering-path-policy.md):
  typed box placement, tagged lifetime/removal authority, finite count admission
  and deferred-default with compatible forward-tier fallback.
- [VFX Spawn Event Mapping, Pooling and Budget Enforcement](../adr/128-vfx-spawn-event-mapping-pooling-and-budget-enforcement.md):
  application-owned semantic bindings, allocation-free playback pools, one finite
  CPU/GPU budget ledger and deterministic cosmetic overload/sleep policy.
- [VFX Editor Document, Live Preview and Module Authoring](../adr/129-vfx-editor-document-live-preview-and-module-authoring.md):
  persistent effect document tabs, independent stack/graph frontends, ordinary
  runtime-pipeline preview and shared decal document/command ownership.
- [Platform Services Frontend, Request Lifetime, Timeout, Null and Error Semantics](../adr/130-platform-services-frontend-request-lifetime-timeout-null-and-error-semantics.md):
  frontend-owned asynchronous requests, exactly-once terminal publication, deferred
  callbacks, typed capability truth and distinct Null/timeout/provider failures.
- [Platform Services Closed SDK, Extension ABI, Package and Composition Boundary](../adr/131-platform-services-closed-sdk-extension-abi-package-and-composition-boundary.md):
  SDK-free public targets, manifest/trust-selected private providers, C ABI value and
  callback rules, transactional activation and profile-specific composition.
- [Post-Processing And Effects Architecture](./runtime/post-processing-and-effects-architecture.md):
  screen-space effects, HDR post chain, tonemapping, color grading, and
  accessibility pass ordering.
- [LOD And Culling Architecture](./runtime/lod-and-culling-architecture.md): mesh
  LOD, HLOD, impostors, occlusion, GPU-driven culling, and editor diagnostics.
- [Accessibility Architecture](./runtime/accessibility-architecture.md): captions,
  colorblind filters, input remapping, screen reader, assists, privacy, and
  persistence.
- [Decal System Architecture](./runtime/decal-system-architecture.md): deferred
  decals, forward fallbacks, material domain, pooling, ownership, and lifetime.
- [Virtual Texturing Architecture](./runtime/virtual-texturing-architecture.md):
  logical page demand/residency, typed asset and renderer integration, capability
  tiers, lifecycle and Post-1.0 scope.
- [Virtual Texturing Ownership, Product Scope and Capability Tier](../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md):
  VTX/Assets/Materials/World Streaming/Renderer/producer ownership, typed composition,
  Atlas/Sparse admission and unsupported-path policy.
- [Virtual Texture Source, Cooked Artifact, Page Store and Cache Ownership](../adr/165-virtual-texture-source-cooked-artifact-page-store-and-cache-ownership.md):
  canonical source/cook authority, immutable root-plus-pack generations, bounded
  exact-generation reads, layered integrity and distinct cache lifetimes.
- [VTX Feature-Local Residency and Eviction Within Global Reservations](../adr/166-vtx-feature-local-residency-and-eviction-within-global-reservations.md):
  global versus feature-local scheduling, multidimensional reservation slices, typed
  pins, shared-page charging, two-phase eviction and pressure accounting.
- [VTX Feedback, Readback, Prediction and Camera-Data Ownership](../adr/167-vtx-feedback-readback-prediction-and-camera-data-ownership.md):
  Renderer-owned feedback/readback resources, immutable delayed observations, typed
  camera/producer hints, bounded prediction and non-authoritative demand evidence.
- [VTX GPU Page Table, Physical Cache, Shader and Material Ownership](../adr/168-vtx-gpu-page-table-physical-cache-shader-and-material-ownership.md):
  logical mapping intent versus Renderer realization, Atlas/Sparse semantic parity,
  atomic frame bindings, material slots and offline sampling variants.
- [VTX Producer, Terrain, World Streaming, Packaging and Server Ownership](../adr/169-vtx-producer-terrain-world-streaming-packaging-and-server-ownership.md):
  immutable producer seams, Terrain/source non-transfer, cell readiness, fallback
  reachability and dedicated/headless exclusion.
- [VTX Settings, Diagnostics, Capture and Qualification Ownership](../adr/170-vtx-settings-diagnostics-capture-and-qualification-ownership.md):
  typed preflight, immutable inspection, cardinality/privacy bounds, authorized control,
  finite captures and native release evidence.
- [Destruction And Fracture Architecture](./runtime/destruction-and-fracture-architecture.md):
  fracture assets, chunk physics, debris, authority, and network reconstruction.
- [Destruction Ownership, Authority, State and Runtime Geometry Boundary](../adr/144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md):
  canonical semantic state and commands, cooked chunk activation, aggregate publication,
  provider-neutral tiers and the post-1.0 runtime mesh-cutting boundary.
- [Destruction Source, Chunk Geometry, Collision and Cook Ownership](../adr/145-destruction-source-chunk-geometry-collision-and-cook-ownership.md):
  normalized source/recipe inputs, canonical DFR geometry/connectivity, solver-neutral
  collision records, Assets publication and separate Physics/Render derived products.
- [Destruction Runtime Activation, Physics, Cleanup and Rollback](../adr/146-destruction-runtime-activation-physics-cleanup-and-rollback.md):
  command and contact safe points, deterministic support loss, private chunk-body
  preparation, aggregate publication, policy-driven cleanup and rollback boundaries.
- [Destruction Event and Cosmetic Consumer Ownership](../adr/147-destruction-event-and-cosmetic-consumer-ownership.md):
  committed fact identity and retention, application-owned dispatch, safe-point fan-out,
  deduplication and gameplay/VFX/Decal/Audio ownership boundaries.
- [Fracture Document, Generator, Undo and Preview Ownership](../adr/148-fracture-document-generator-undo-and-preview-ownership.md):
  persistent authoring document, detached generator transactions, exact bounded history,
  Assets source/cook publication and production-isolated preview.
- [Destruction Persistence, Replication, Streaming and Authority](../adr/149-destruction-persistence-replication-streaming-and-authority.md):
  canonical reconstruction state, server authority, paired Physics motion, snapshot-first
  late join, durable cell handoff and stable-identity compatibility.
- [Procedural Generation Architecture](./runtime/procedural-generation-architecture.md):
  PCG graphs, point clouds, validation, transactions, server authority, and
  streaming-cell ownership.
- [PCG Graph Source, Cooked Plan, Cache and Runtime Ownership](../adr/150-pcg-graph-source-cooked-plan-cache-and-runtime-ownership.md):
  one authored truth, deterministic lowering, disposable cache/intermediates, immutable
  runtime-plan leases and target-owner output commit.
- [PCG Ownership, Authority, Tier and Lifecycle](../adr/151-pcg-ownership-authority-tier-and-lifecycle.md):
  pure evaluator ownership, external commit authority, deterministic capability classes,
  offline/preview/runtime/hybrid modes and headless/null lifecycle.
- [PCG Spatial Input Snapshot and Node-Library Ownership](../adr/152-pcg-spatial-input-snapshot-and-node-library-ownership.md):
  coherent immutable provider capture, stale-result fencing, canonical spatial queries,
  core-node semantics and trusted catalog extensions.
- [PCG Pure Evaluation, Commit and Generated-Output Ownership](../adr/153-pcg-pure-evaluation-commit-and-generated-output-ownership.md):
  immutable generation candidates, target-owner aggregate commit, exact provenance,
  atomic regeneration and cleanup that preserves hand-authored/adopted content.
- [PCG Cross-System Authority, Readiness and Commit Boundary](../adr/154-pcg-cross-system-authority-readiness-and-commit-boundary.md):
  host-composed target adapters, exact readiness receipts, aggregate rollback and
  preserved TRF/WST/NAV/Scene/VFX/network/persistence authority.
- [PCG Graph Document, Preview, Bake and Undo Ownership](../adr/155-pcg-graph-document-preview-bake-and-undo-ownership.md):
  persistent typed-command graph documents, semantic history, isolated ordinary-path
  preview and explicit provenance-aware bake/undo transactions.
- [PCG Scale Budgets, Trust and Release Scope](../adr/156-pcg-scale-budgets-trust-and-release-scope.md):
  exact operational profile ceilings and overload behavior, hostile-input/privacy
  boundaries, qualification workloads and closed 1.0 versus post-1.0 scope.
- [Multiplayer Replication Architecture](./runtime/multiplayer-replication-architecture.md):
  replication roles, property deltas, RPCs, prediction, interest management,
  dedicated servers, and security.
- [XR Architecture](./runtime/vr-ar-architecture.md): OpenXR lifecycle,
  runtime-driven view configurations, renderer/input boundaries, interaction,
  mixed reality, privacy, standalone Android dependencies, and evidence-based
  device qualification.
- [XR Ownership, Runtime Composition and Capability Tier](../adr/157-xr-ownership-runtime-composition-and-capability-tier.md):
  XRApi/XRRuntime/OpenXR ownership, explicit host composition, typed profile admission,
  deliberate Platform/Renderer/Input non-owners and closed 1.0 versus post-1.0 scope.

## Extensions

- [Extension and Package Authority Boundary](../adr/054-extension-and-package-authority-boundary.md):
  one package identity/file/dependency authority, package-scoped extension
  descriptors, trust/activation hand-off, layouts, and legacy migration.
- [Extension Capability Roadmap](./extensions/extension-capability-roadmap.md):
  ecosystem capability stages and their separation from product milestones,
  workstreams, planning horizons, and technical dependencies. Script-consumable
  service authority, binding adapters and context lifetimes are defined by
  [ADR-059](../adr/059-script-consumable-module-boundary.md).
- [Gameplay Module](./extensions/gameplay-module.md): overview for
  project-owned gameplay modules, behavior authoring, runtime integration, and
  verification.
  - [Current Gameplay Module Contract Audit](./extensions/gameplay-module-contract-audit.md):
    implementation snapshot for the exact SDK ABI, generated bundle, build and
    publication path, ownership, reload, persistence, platform behavior, and
    follow-up ownership.
  - [Gameplay Module Boundary](./extensions/gameplay-module-boundary.md):
    native module ABI, registration, capability context, services, hot reload,
    and diagnostics.
  - [Gameplay Behavior Authoring](./extensions/gameplay-behavior-authoring.md):
    editor/IDE workflow, object-attached behaviors, script discovery, visual
    scripting, and iteration-speed goals.
  - [Gameplay Runtime Integration](./extensions/gameplay-runtime-integration.md):
    game-owned asset types, input actions, runtime systems, scene/play
    lifecycle, and component persistence.
  - [Gameplay Module Verification](./extensions/gameplay-module-verification.md):
    contract and regression coverage.
- [Extension System](./extensions/plugin-system.md): editor/tool extension packages,
  C ABI, typed v1 manifests, permissions, registration, and lifecycle. Package
  authority and manifest-model decisions are recorded in
  [ADR-054](../adr/054-extension-and-package-authority-boundary.md) and
  [ADR-055](../adr/055-extension-manifest-v1-typed-model.md); external editor UI
  uses the host-rendered boundary in
  [ADR-056](../adr/056-external-editor-ui-boundary.md).

## Packages

- [Package System](./packages/package-system.md): core package kinds, contributions,
  manifest, sources, resolver, lockfile, cache, and trust model. The canonical
  typed manifest and verified-bundle boundary is
  [ADR-057](../adr/057-package-manifest-v1-typed-model.md); deterministic source,
  mirror, credential and override policy is
  [ADR-058](../adr/058-package-source-policy.md).
- [Package Restore](./packages/package-restore.md): clean-machine project restore,
  bootstrap, CI, offline restore, dev overrides, and non-interactive policy.
- [Package Lifecycle](./packages/package-lifecycle.md): install, trust, enable,
  activation, update, uninstall, ownership states, migration, and conflicts.
- [Package Release Integration](./packages/package-release-integration.md):
  lockfile freeze, `assets.horo`, chunks, DLC, editor-only exclusion, and license notices.

## Editor And GUI

- [GUI Screen Host](./editor/gui-screen-host.md): Welcome, Project Browser, creation
  flows, Editor Workspace, navigation, and leave guards.
- [GUI Design System](./editor/ui-design-system.md): reusable ImGui components, design
  tokens, themes, and accessibility.
- [Localization](./editor/localization.md): message resources, locale resolution,
  formatting, fallback, fonts, and layout verification.
- [Localization Implementation Plan](./editor/localization-implementation-plan.md):
  editor extractor, settings integration, runtime foundation, and migration phases.
- [Localization Extractor Report](./editor/localization-extractor-report.json):
  generated baseline of editor text candidates and technical exclusions.
- [Editor Document Model](./editor/editor-document-model.md): scene documents,
  commands, transactions, history, save, autosave, and recovery.
- [Editor Data Bus](./editor/editor-data-bus.md): session-local typed notifications,
  authoritative editor models, and the process-event bridge.
- [Editor Panel Host](./editor/editor-panel-host.md): layout tree, panel and tab
  lifetime, toolbars, and status bar.
- [Editor Modal Host](./editor/editor-modal-host.md): exclusive-focus screen-like
  workflow surfaces above the editor workspace.
- [Editor AI Agent Architecture](./editor/editor-ai-agent-architecture.md):
  editor-integrated conversational agent, viewport inline editing, MCP tool-calling,
  magic AI tools, conversation persistence, and privacy model.
- [Immersive Agent Ownership, Authoring Mode and Risk](../adr/172-immersive-agent-ownership-authoring-mode-and-risk.md):
  developer-only XR authoring, Edit/Simulate/Play admission, multimodal evidence,
  proposal-bound approval, transaction authority and privacy/risk boundaries.
- [Project Model](./editor/project-model.md): project directory, settings, workspace
  persistence, scene documents, and asset index.

## Hosts And Transport

- [CLI Architecture](./interfaces/cli-architecture.md): command registry, parsing, output,
  exit codes, progress, cancellation, and headless execution.
- [MCP Architecture](./interfaces/mcp-architecture.md): accepted MCP application-capability
  boundary, shared controller/registry, effect categories, request lifecycle, errors,
  bounds, cancellation, and threading.
- [Application Security](./security/application-security.md): project and plugin trust,
  path/process policy, MCP access, credentials, and parser limits.

## Delivery And Operations

- [Product Roadmap Model](./delivery/product-roadmap.md): product checkpoints,
  parallel workstreams, planning dimensions, and milestone assignment rules.
- [Developer Environment](./delivery/developer-environment.md): setup, toolchains, IDEs,
  and daily workflow.
- [Build System](./delivery/build-system.md): CMake targets, presets, dependencies, and
  module creation.
- [Build Cache](./delivery/build-cache.md): compiler and dependency caching.
- [Testing Architecture](./delivery/testing-architecture.md): test layers, fixtures,
  mocks, GUI/MCP harnesses, determinism, and performance budgets.
- [Quality And CI](./delivery/quality-and-ci.md): build matrix, coverage, gates, and CI
  artifacts.
- [Observability Architecture](./observability/observability.md): observability decisions and
  reading paths.
- [Logging, Context, And Diagnostics](./observability/observability-logging.md): log schema,
  MDC, sinks, storage, privacy, diagnostic bundles, and tests.
- [Metrics And Profiling](./observability/observability-performance.md): CPU, memory, frame,
  subsystem metrics, profiler captures, and performance views.
- [Release Architecture](./release/release.md): release jobs, artifacts, reproducibility,
  cancellation, and publishing. Typed job/target/stage state, terminal results
  and presentation-independent ownership are defined by
  [ADR-060](../adr/060-release-domain-model-and-state-machine.md).
- [Release Security](./release/release-security.md): trust boundaries, credentials,
  signing, archive protection, and CI controls.
- [Distribution And Update](./release/distribution-and-update.md): signed update
  manifests, staging, activation, compatibility, rollback, and offline packages.

## UI Reference Designs

HTML reference designs are static panel, modal, or screen mockups that live next
to their owning architecture documents. Panel/tab references do not include the
application menu bar; app-level screen references do.

- Runtime panels and screens: [Physics Debugger](./runtime/physics-debugger.html),
  [Animation Editor](./runtime/animation-editor.html), [Particle Editor](./runtime/particle-editor.html),
  [Audio Mixer](./runtime/audio-mixer.html), [Input Mapping Editor](./runtime/input-mapping-editor.html),
  [Prefab Editor](./runtime/prefab-editor.html), [Material Editor](./runtime/material-editor.html),
  [Network Debugger](./runtime/network-debugger.html), [Platform Services Config](./runtime/platform-services-config.html),
  [Render Settings](./runtime/render-settings.html), [Character Setup](./runtime/character-setup.html),
  [UI Canvas Editor](./runtime/ui-canvas-editor.html), [Scene Primitives](./runtime/primitives-panel.html),
  [Build Output](./runtime/build-output.html), [Cinematic Sequencer](./runtime/cinematic-sequencer.html),
  [Navigation Bake](./runtime/navigation-bake.html), [Save/Load Manager](./runtime/save-load-manager.html),
  [Post-Processing Stack](./runtime/post-processing-stack.html), [LOD Debugger](./runtime/lod-debugger.html),
  [PCG Graph Editor](./runtime/pcg-graph-editor.html), [Decal Placement](./runtime/decal-placement.html),
  [Destruction Setup](./runtime/destruction-setup.html), [Virtual Texturing Debug](./runtime/virtual-texturing-debug.html),
  [XR Setup](./runtime/xr-setup.html), and [Shader Graph](./runtime/shader-graph-editor.html).
- Editor/extension panels: [Localization Editor](./editor/localization-editor.html),
  [Project Settings](./editor/project-settings.html), and [Gameplay Integration Config](./extensions/module-config.html).

## Core Rules

- Horo Engine is provided through GUI and CLI hosts.
- Both hosts expose MCP through shared application services.
- GUI, CLI, and MCP do not duplicate engine business logic.
- Modules have explicit CMake targets and enforced dependency direction.
- The GUI is one editor application; welcome and project-browser flows are GUI
  screens, not a separate application or module.
- Runtime modules remain independent from GUI and transport concerns.
- Expected failures use typed results and stable error codes; logs are not
  control flow.
- Configuration is typed, validated, provenance-aware, and read through
  immutable snapshots.
- Jobs use structured ownership, cooperative cancellation, bounded queues, and
  explicit owner-thread handoff.
- Runtime simulation uses fixed ticks and render interpolation.
- Persistent identity, runtime handles, and memory ownership are distinct.
- ImGui is isolated to the GUI implementation.
- Reusable UI components expose typed size, variant, icon, and interaction
  contracts and consume semantic design tokens.
- Runtime Game UI/HUD is game content and remains separate from HoroEditor
  panels, tabs, modals, and ImGui design-system widgets.
- GUI rendering code contains no visual literals; packaged and custom theme
  resources are resolved dynamically into an immutable frame theme.
- GUI features receive narrow application capabilities rather than an omnibus
  application service.
- CI validates engine, GUI, CLI, and MCP behavior.
- Code coverage and GUI scenario coverage are separate complementary signals.
- Ownership, thread access, cancellation, and shutdown order are explicit.
- Commands and use cases perform operations; data buses publish notifications
  only after state commits.
- C++, Python, editor, CLI, release jobs, and games emit the same structured log
  schema and propagate diagnostic context across asynchronous work.
- Hosts expose bounded CPU, memory, frame, and subsystem metrics; detailed
  profiler tracing is explicit and build-profile gated.
- High-volume logs, profiler samples, and output streams live in bounded
  queryable stores; buses publish revision or availability notifications.
- Editor modals are transient GUI workflow surfaces; they are not tabs, layout
  nodes, application screens, or state authorities.
- Editor document mutations use typed commands and transactions; autosave is
  recovery state and never silently replaces the user's saved file.
- Raw device input is normalized into snapshots and routed by interaction scope;
  high-frequency input does not travel through data buses.
- Native gameplay and plugin extensions cross explicit registration boundaries;
  external plugins use a versioned C ABI rather than a cross-version C++ ABI.
- Real-time audio callbacks allocate nothing, block on nothing, and perform no
  ordinary logging.
- Remote communication uses explicit bounded protocols; process-local data-bus
  events are never serialized automatically.
- Projects, plugins, assets, subprocess requests, and network input are treated
  according to explicit trust and resource-limit policy.

## Review Checklist

- Does the change preserve the documented dependency direction?
- Is the correct authority responsible for the committed state?
- Are ownership, thread affinity, and shutdown behavior explicit?
- Does fallible behavior return a typed error with a stable code?
- Are settings read through the configuration snapshot rather than directly
  from files or environment variables?
- Is asynchronous work owned by a task group with cancellation and
  backpressure?
- Is reusable logic in the owning module instead of a host adapter?
- Can applicable operations run through GUI, CLI, and MCP consistently?
- Are ImGui and renderer-backend details contained within their modules?
- Does UI styling use shared component contracts and semantic theme tokens
  without source-level visual literals?
- Does each GUI feature receive only the application capabilities it needs?
- Is high-volume data owned by a bounded/queryable store instead of copied
  through event payloads?
- Are logs structured, redacted, correctly leveled, and correlated with the
  active operation context?
- Are metrics typed, bounded, low-cardinality, and explicit about units and
  availability?
- Is expensive profiler instrumentation opt-in and excluded from shipping where
  required?
- Are runtime mutations committed at the correct frame or subsystem
  synchronization point?
- Are stable IDs, handles, snapshots, and borrows used for their intended
  lifetimes?
- Are test type, coverage expectation, and CI impact clear?

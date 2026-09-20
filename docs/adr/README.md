# Architecture Decision Records

This directory contains Architecture Decision Records (ADRs) for Horo Engine.
Each ADR documents a significant architectural choice: the context, the
decision, the consequences, and the rejected alternatives.

ADRs are immutable after acceptance. They may be superseded by later ADRs,
but never silently edited. Superseded ADRs remain in the index with a pointer
to the replacement.

## Index

| ID                                    | Title                                | Status   | Date       |
|---------------------------------------|--------------------------------------|----------|------------|
| [001](001-native-ci-builds.md)        | Host-Agnostic Local Release Pipeline | Accepted | 2026-05-30 |
| [002](002-credential-handling.md)     | Credential Handling                  | Superseded | 2026-05-25 |
| [003](003-artifact-identity.md)       | Artifact Identity                    | Accepted | 2026-05-25 |
| [004](004-cli-core-gui-boundary.md)   | CLI, Core, and GUI Boundary          | Accepted | 2026-05-25 |
| [005](005-submodule-compatibility.md) | Submodule Compatibility Constraints  | Accepted | 2026-05-25 |
| [006](006-lua-5-4-gameplay-runtime.md) | Lua 5.4 Gameplay Runtime             | Accepted | 2026-08-02 |
| [007](007-cross-engine-project-interchange.md) | Cross-Engine Project Interchange | Proposed | 2026-08-26 |
| [008](008-error-model-exception-boundary-and-registry.md) | Error Model, Exception Boundary and Registry Ownership | Proposed | 2026-08-27 |
| [009](009-configuration-schema-precedence-and-secret-boundary.md) | Configuration Schema, Precedence and Secret Boundary | Proposed | 2026-08-28 |
| [010](010-job-waiting-and-operation-store-ownership.md) | Job Waiting and Operation Store Ownership | Proposed | 2026-08-28 |
| [011](011-vfx-effect-ownership-simulation-domain-and-renderer-boundary.md) | Effect Ownership, Simulation Domain Policy and Renderer Boundary | Proposed | 2026-08-28 |
| [012](012-world-streaming-partition-authority-and-subsystem-boundaries.md) | World Streaming Partition Authority and Subsystem Boundaries | Proposed | 2026-08-28 |
| [013](013-environment-query-ownership-item-and-scoring-model.md) | Environment Query Ownership, Item and Scoring Model | Proposed | 2026-08-28 |
| [014](014-sequencer-ownership-clock-authority-and-binding-boundary.md) | Sequencer Ownership, Clock Authority and Binding Boundary | Proposed | 2026-08-28 |
| [015](015-accessibility-ownership-typed-transport-and-non-gating-policy.md) | Accessibility Ownership, Typed Transport and Non-Gating Policy | Proposed | 2026-08-28 |
| [016](016-navigation-target-ownership-and-dependency-boundary.md) | Navigation Target Ownership and Dependency Boundary | Proposed | 2026-08-28 |
| [017](017-prefab-role-ownership-and-capability-tiers.md) | Prefab Role, Ownership and Capability-Tier Decision | Proposed | 2026-08-28 |
| [018](018-command-registration-permissions-threading-and-packaged-build-policy.md) | Command Registration, Permissions, Threading and Packaged-Build Policy | Proposed | 2026-08-28 |
| [019](019-cli-host-command-ownership-adapter-equivalence-and-horopak-boundary.md) | CLI Host, Command Ownership, Adapter Equivalence and horopak Boundary Decision | Proposed | 2026-08-28 |
| [020](020-network-target-ownership-and-dependency-boundary.md) | Network Target Ownership and Dependency Boundary | Proposed | 2026-08-28 |
| [021](021-gameplay-ai-ownership-scheduling-and-behavior-boundary.md) | Gameplay AI Ownership, Scheduling and Behavior Boundary | Proposed | 2026-08-28 |
| [022](022-ai-fixed-tick-order-authority-and-simulation-budget.md) | AI Fixed-Tick Order, Authority and Simulation Budget | Proposed | 2026-08-28 |
| [023](023-world-index-and-cell-format-architecture-decision.md) | World Index and Cell Format Architecture Decision | Proposed | 2026-08-28 |
| [024](024-perception-ownership-sense-policy-and-budget.md) | Perception Ownership, Sense Policy and Budget Decision | Proposed | 2026-08-28 |
| [025](025-ai-decision-assets-and-gameplay-behavior-boundary.md) | AI Decision Assets and Shared Gameplay Behavior Boundary | Proposed | 2026-08-28 |
| [026](026-large-world-precision-and-floating-origin-strategy.md) | Large-World Precision and Floating Origin Strategy | Proposed | 2026-08-28 |
| [027](027-renderer-resource-identity-and-descriptors.md) | Renderer Resource Identity and Descriptors | Proposed | 2026-08-31 |
| [028](028-renderer-capability-limits-and-product-profiles.md) | Renderer Capability, Limits and Product Profiles | Proposed | 2026-08-31 |
| [029](029-opengl-core-profile-and-platform-policy.md) | OpenGL Core Profile and Platform Policy | Proposed | 2026-08-31 |
| [030](030-metal-platform-and-feature-baseline.md) | Metal Platform and Feature Baseline | Proposed | 2026-08-31 |
| [031](031-vulkan-loader-platform-and-version-baseline.md) | Vulkan Loader, Platform and Version Baseline | Proposed | 2026-08-31 |
| [032](032-d3d12-baseline-and-agility-sdk-policy.md) | D3D12 Baseline and Agility SDK Policy | Proposed | 2026-08-31 |
| [033](033-presentation-and-display-ownership.md) | Presentation and Display Ownership | Proposed | 2026-08-31 |
| [034](034-gpu-memory-and-residency-ownership.md) | GPU Memory and Residency Ownership | Proposed | 2026-08-31 |
| [035](035-shader-source-and-intermediate-representation.md) | Shader Source and Intermediate Representation | Accepted | 2026-08-31 |
| [036](036-raster-render-path-and-quality-architecture.md) | Raster Render Path and Quality Architecture | Proposed | 2026-09-01 |
| [037](037-scene-color-and-hdr-architecture.md) | Scene Color and HDR Architecture | Proposed | 2026-09-01 |
| [038](038-gpu-scene-and-instance-data-model.md) | GPU Scene and Instance Data Model | Proposed | 2026-09-01 |
| [039](039-ray-tracing-capability-and-abstraction.md) | Ray Tracing Capability and Abstraction | Proposed | 2026-09-01 |
| [040](040-reconstruction-frame-generation-and-latency-providers.md) | Reconstruction, Frame Generation and Latency Providers | Proposed | 2026-09-01 |
| [041](041-backend-neutral-renderer-diagnostics-model.md) | Backend-Neutral Renderer Diagnostics Model | Proposed | 2026-09-01 |
| [042](042-cpu-gpu-timestamps-and-pipeline-statistics.md) | CPU/GPU Timestamps and Pipeline Statistics | Proposed | 2026-09-01 |
| [043](043-gpu-memory-and-resource-inspection.md) | GPU Memory and Resource Inspection | Proposed | 2026-09-01 |
| [044](044-render-markers-and-debug-labels.md) | Render Markers and Debug Labels | Proposed | 2026-09-01 |
| [045](045-backend-validation-and-debug-layer-integration.md) | Backend Validation and Debug Layer Integration | Proposed | 2026-09-01 |
| [046](046-gpu-driver-compatibility-and-workaround-registry.md) | GPU Driver Compatibility and Workaround Registry | Proposed | 2026-09-01 |
| [047](047-renderdoc-pix-and-metal-capture-integration.md) | RenderDoc, PIX and Metal Capture Integration | Proposed | 2026-09-01 |
| [048](048-gpu-crash-and-device-loss-diagnostic-bundles.md) | GPU Crash and Device-Loss Diagnostic Bundles | Proposed | 2026-09-01 |
| [049](049-render-graph-and-resource-inspector-ui.md) | Render Graph and Resource Inspector UI | Proposed | 2026-09-01 |
| [050](050-cross-backend-reference-image-tests.md) | Cross-Backend Reference Image Tests | Proposed | 2026-09-01 |
| [051](051-renderer-benchmark-and-regression-gates.md) | Renderer Benchmark and Regression Gates | Proposed | 2026-09-01 |
| [052](052-first-party-renderer-component-scope.md) | First-Party Renderer Component Scope | Proposed | 2026-09-01 |
| [053](053-renderer-module-manifest-parser.md) | Renderer Module Manifest Parser | Proposed | 2026-09-02 |
| [054](054-extension-and-package-authority-boundary.md) | Extension and Package Authority Boundary | Proposed | 2026-09-02 |
| [055](055-extension-manifest-v1-typed-model.md) | Extension Manifest V1 Typed Model | Proposed | 2026-09-02 |
| [056](056-external-editor-ui-boundary.md) | External Editor UI Boundary | Proposed | 2026-09-02 |
| [057](057-package-manifest-v1-typed-model.md) | Package Manifest V1 Typed Model | Proposed | 2026-09-02 |
| [058](058-package-source-policy.md) | Package Source Policy | Proposed | 2026-09-02 |
| [059](059-script-consumable-module-boundary.md) | Script-Consumable Module Boundary | Proposed | 2026-09-02 |
| [060](060-release-domain-model-and-state-machine.md) | Release Domain Model and State Machine | Proposed | 2026-09-02 |
| [061](061-animation-ownership-update-order-and-clock.md) | Animation Ownership, Update Order and Clock | Proposed | 2026-09-02 |
| [062](062-audio-runtime-ownership-and-update-order.md) | Audio Runtime Ownership and Update Order | Proposed | 2026-09-02 |
| [063](063-audio-sample-format-and-channel-layout.md) | Audio Sample Format and Channel Layout | Proposed | 2026-09-02 |
| [064](064-audio-asset-and-cook-boundary.md) | Audio Asset and Cook Boundary | Proposed | 2026-09-02 |
| [065](065-mixer-topology-and-constrained-dag.md) | Mixer Topology and Constrained DAG | Proposed | 2026-09-02 |
| [066](066-spatial-provider-and-required-capability.md) | Spatial Provider and Required Capability | Proposed | 2026-09-02 |
| [067](067-platform-audio-backend-strategy.md) | Platform Audio Backend Strategy | Proposed | 2026-09-02 |
| [068](068-music-transport-and-cross-system-ownership.md) | Music Transport and Cross-System Ownership | Proposed | 2026-09-02 |
| [069](069-audio-extension-capability-and-abi.md) | Audio Extension Capability and ABI | Proposed | 2026-09-02 |
| [070](070-capture-and-voice-io-ownership.md) | Capture and Voice I/O Ownership | Proposed | 2026-09-02 |
| [071](071-procedural-audio-graph-ownership.md) | Procedural Audio Graph Ownership | Proposed | 2026-09-02 |
| [072](072-audio-middleware-integration-model.md) | Audio Middleware Integration Model | Proposed | 2026-09-02 |
| [073](073-runtime-ui-ownership-scope-and-update-order.md) | Runtime UI Ownership, Scope and Update Order | Proposed | 2026-09-02 |
| [074](074-runtime-ui-layout-units-and-measure-arrange.md) | Runtime UI Layout Units and Measure-Arrange | Proposed | 2026-09-02 |
| [075](075-runtime-ui-font-asset-family-and-fallback.md) | Runtime UI Font Asset, Family and Fallback | Proposed | 2026-09-02 |
| [076](076-runtime-ui-style-asset-token-and-inheritance.md) | Runtime UI Style Asset, Token and Inheritance | Proposed | 2026-09-02 |
| [077](077-runtime-ui-animation-clock-and-time-domain.md) | Runtime UI Animation Clock and Time Domain | Proposed | 2026-09-02 |
| [078](078-runtime-ui-input-context-and-player-routing.md) | Runtime UI Input Context and Player Routing | Proposed | 2026-09-02 |
| [079](079-runtime-ui-binding-provider-schema-identity-and-lifetime.md) | Runtime UI Binding Provider Schema, Identity and Lifetime | Proposed | 2026-09-02 |
| [080](080-runtime-ui-presentation-scope-layer-and-route.md) | Runtime UI Presentation Scope, Layer and Route | Proposed | 2026-09-02 |
| [081](081-runtime-ui-and-localization-ownership-boundary.md) | Runtime UI and Localization Ownership Boundary | Proposed | 2026-09-02 |
| [082](082-runtime-ui-accessibility-capability-and-ownership.md) | Runtime UI Accessibility Capability and Ownership | Proposed | 2026-09-02 |
| [083](083-ui-template-identity-schema-and-expansion.md) | UI Template Identity, Schema and Expansion | Proposed | 2026-09-02 |
| [084](084-canonical-physics-solver-units-and-tolerances.md) | Canonical Physics Solver, Units and Tolerances | Proposed | 2026-09-02 |
| [085](085-physics-shape-authoring-cook-and-runtime-boundary.md) | Physics Shape Authoring, Cook and Runtime Boundary | Proposed | 2026-09-02 |
| [086](086-collision-layer-profile-and-query-channel-policy.md) | Collision Layer, Profile and Query Channel Policy | Proposed | 2026-09-02 |
| [087](087-scene-to-physics-ownership-and-conversion.md) | Scene-to-Physics Ownership and Conversion | Proposed | 2026-09-02 |
| [088](088-physics-determinism-capability-and-support-tiers.md) | Physics Determinism Capability and Support Tiers | Proposed | 2026-09-02 |
| [089](089-character-controller-ownership-implementation-and-update-order.md) | Character Controller Ownership, Implementation and Update Order | Proposed | 2026-09-02 |
| [090](090-character-dynamic-body-visibility-push-and-proxy-policy.md) | Character Dynamic-Body Visibility, Push and Proxy Policy | Proposed | 2026-09-02 |
| [091](091-footstep-and-locomotion-event-ownership.md) | Footstep and Locomotion Event Ownership | Proposed | 2026-09-02 |
| [092](092-character-controller-determinism-and-state-composition.md) | Character Controller Determinism and State Composition | Proposed | 2026-09-02 |
| [093](093-prefab-override-property-identity-and-delta-operations.md) | Prefab Override Property Identity and Delta Operations | Proposed | 2026-09-02 |
| [094](094-prefab-nested-composition-and-variant-inheritance.md) | Prefab Nested Composition and Variant Inheritance | Proposed | 2026-09-02 |
| [095](095-prefab-cook-boundary-and-artifact-model.md) | Prefab Cook Boundary and Artifact Model | Proposed | 2026-09-02 |
| [096](096-prefab-external-reference-and-binding-slot-contract.md) | Prefab External Reference and Binding Slot Contract | Proposed | 2026-09-02 |
| [097](097-default-real-time-transport-backend.md) | Default Real-Time Transport Backend | Proposed | 2026-09-02 |
| [098](098-protocol-session-and-trust-policy.md) | Protocol, Session and Trust Policy | Proposed | 2026-09-02 |
| [099](099-replication-ownership-authority-and-compatibility.md) | Replication Ownership, Authority and Compatibility | Proposed | 2026-09-02 |
| [100](100-prediction-capability-tiers-and-determinism-policy.md) | Prediction Capability Tiers and Determinism Policy | Proposed | 2026-09-02 |
| [101](101-interest-priority-and-network-budget-model.md) | Interest, Priority and Network Budget Model | Proposed | 2026-09-02 |
| [102](102-runtime-network-modes-and-authority-exposure.md) | Runtime Network Modes and Authority Exposure | Proposed | 2026-09-02 |
| [103](103-network-project-configuration-and-build-profile-ownership.md) | Network Project Configuration and Build-Profile Ownership | Proposed | 2026-09-02 |
| [104](104-default-navigation-provider-and-recast-detour-adoption.md) | Default Navigation Provider and Recast-Detour Adoption | Proposed | 2026-09-02 |
| [105](105-navigation-asset-and-scene-ownership-boundary.md) | Navigation Asset and Scene Ownership Boundary | Proposed | 2026-09-02 |
| [106](106-navigation-bake-ownership-transaction-and-cache.md) | Navigation Bake Ownership, Transaction and Cache | Proposed | 2026-09-02 |
| [107](107-navigation-query-consistency-and-snapshot-ownership.md) | Navigation Query Consistency and Snapshot Ownership | Proposed | 2026-09-02 |
| [108](108-dynamic-overlay-carving-and-tile-rebuild-policy.md) | Dynamic Overlay, Carving and Tile-Rebuild Policy | Proposed | 2026-09-02 |
| [109](109-avoidance-crowd-and-renderer-independent-budget.md) | Avoidance, Crowd and Renderer-Independent Budget | Proposed | 2026-09-02 |
| [110](110-navigation-editor-surface-and-command-ownership.md) | Navigation Editor Surface and Command Ownership | Proposed | 2026-09-02 |
| [111](111-gameplay-ai-document-panel-and-runtime-debug-ownership.md) | Gameplay AI Document, Panel and Runtime-Debug Ownership | Proposed | 2026-09-02 |
| [112](112-save-archive-container-and-compatibility-policy.md) | Save Archive Container and Compatibility Policy | Proposed | 2026-09-02 |
| [113](113-local-storage-user-profile-and-slot-ownership.md) | Local Storage, User Profile and Slot Ownership | Proposed | 2026-09-02 |
| [114](114-canonical-runtime-world-persistence-boundary.md) | Canonical Runtime World Persistence Boundary | Proposed | 2026-09-02 |
| [115](115-cloud-save-authority-revision-and-conflict-policy.md) | Cloud Save Authority, Revision and Conflict Policy | Proposed | 2026-09-02 |
| [116](116-save-data-threat-model-and-trust-policy.md) | Save Data Threat Model and Trust Policy | Proposed | 2026-09-02 |
| [117](117-playback-ownership-frame-order-and-determinism.md) | Playback Ownership, Frame Order and Determinism | Proposed | 2026-09-02 |
| [118](118-animation-character-and-gameplay-authority-during-cinematics.md) | Animation, Character and Gameplay Authority During Cinematics | Proposed | 2026-09-02 |
| [119](119-camera-authority-during-cinematics.md) | Camera Authority During Cinematics | Proposed | 2026-09-02 |
| [120](120-cinematic-event-dispatch-and-audio-coupling-boundary.md) | Cinematic Event Dispatch and Audio Coupling Boundary | Proposed | 2026-09-02 |
| [121](121-cinematic-editor-document-and-authoring-context.md) | Cinematic Editor Document and Authoring Context | Proposed | 2026-09-02 |
| [122](122-cinematic-trigger-sources-and-capability-policy.md) | Cinematic Trigger Sources and Capability Policy | Proposed | 2026-09-02 |
| [123](123-vfx-cpu-stage-order-determinism-and-gameplay-coupling.md) | VFX CPU Stage Order, Determinism and Gameplay Coupling | Proposed | 2026-09-02 |
| [124](124-vfx-gpu-simulation-readback-and-compute-fallback.md) | VFX GPU Simulation, Readback and Compute Fallback | Proposed | 2026-09-02 |
| [125](125-vfx-transparency-sorting-and-pass-placement.md) | VFX Transparency, Sorting and Pass Placement | Proposed | 2026-09-02 |
| [126](126-vfx-graph-compilation-and-runtime-representation-convergence.md) | VFX Graph Compilation and Runtime Representation Convergence | Proposed | 2026-09-02 |
| [127](127-vfx-decal-projection-lifetime-and-rendering-path-policy.md) | VFX Decal Projection, Lifetime and Rendering Path Policy | Proposed | 2026-09-02 |
| [128](128-vfx-spawn-event-mapping-pooling-and-budget-enforcement.md) | VFX Spawn Event Mapping, Pooling and Budget Enforcement | Proposed | 2026-09-02 |
| [129](129-vfx-editor-document-live-preview-and-module-authoring.md) | VFX Editor Document, Live Preview and Module Authoring | Proposed | 2026-09-02 |
| [130](130-platform-services-frontend-request-lifetime-timeout-null-and-error-semantics.md) | Platform Services Frontend, Request Lifetime, Timeout, Null and Error Semantics | Proposed | 2026-09-02 |
| [131](131-platform-services-closed-sdk-extension-abi-package-and-composition-boundary.md) | Platform Services Closed SDK, Extension ABI, Package and Composition Boundary | Proposed | 2026-09-02 |
| [132](132-platform-services-project-salt-stable-id-tombstone-and-provider-mapping.md) | Platform Services Project Salt, Stable ID, Tombstone and Provider Mapping | Proposed | 2026-09-02 |
| [133](133-platform-progression-authority-trust-and-idempotency.md) | Platform Progression Authority, Trust and Idempotency | Proposed | 2026-09-02 |
| [134](134-cloud-blob-transport-revision-precondition-and-offline-ownership.md) | Cloud Blob Transport, Revision Preconditions and Offline Ownership | Proposed | 2026-09-02 |
| [135](135-platform-identity-session-generation-privacy-and-consent.md) | Platform Identity, Session Generation, Privacy and Consent | Proposed | 2026-09-02 |
| [136](136-platform-offline-queue-ownership-replay-and-cloud-intent-boundary.md) | Platform Offline Queue Ownership, Replay and Cloud Intent Boundary | Proposed | 2026-09-02 |
| [137](137-terrain-foliage-ownership-data-tier-and-lifecycle.md) | Terrain and Foliage Ownership, Data, Tier and Lifecycle | Proposed | 2026-09-02 |
| [138](138-terrain-source-cooked-tile-cache-and-streaming-ownership.md) | Terrain Source, Cooked Tile, Cache and Streaming Ownership | Proposed | 2026-09-02 |
| [139](139-terrain-render-extraction-material-lod-and-tier-boundary.md) | Terrain Render Extraction, Material, LOD and Tier Boundary | Proposed | 2026-09-02 |
| [140](140-foliage-placement-baked-dynamic-state-and-eviction-ownership.md) | Foliage Placement, Baked/Dynamic State and Eviction Ownership | Proposed | 2026-09-02 |
| [141](141-terrain-foliage-cross-system-ownership-and-readiness.md) | Terrain/Foliage Cross-System Ownership and Readiness | Proposed | 2026-09-02 |
| [142](142-terrain-foliage-document-tool-undo-and-preview-ownership.md) | Terrain/Foliage Document, Tool, Undo and Preview Ownership | Proposed | 2026-09-02 |
| [143](143-terrain-foliage-scale-budgets-observability-and-feature-boundary.md) | Terrain/Foliage Scale Budgets, Observability and Feature Boundary | Proposed | 2026-09-02 |
| [144](144-destruction-ownership-authority-state-and-runtime-geometry-boundary.md) | Destruction Ownership, Authority, State and Runtime Geometry Boundary | Proposed | 2026-09-02 |
| [145](145-destruction-source-chunk-geometry-collision-and-cook-ownership.md) | Destruction Source, Chunk Geometry, Collision and Cook Ownership | Proposed | 2026-09-02 |
| [146](146-destruction-runtime-activation-physics-cleanup-and-rollback.md) | Destruction Runtime Activation, Physics, Cleanup and Rollback | Proposed | 2026-09-02 |
| [147](147-destruction-event-and-cosmetic-consumer-ownership.md) | Destruction Event and Cosmetic Consumer Ownership | Proposed | 2026-09-02 |
| [148](148-fracture-document-generator-undo-and-preview-ownership.md) | Fracture Document, Generator, Undo and Preview Ownership | Proposed | 2026-09-02 |
| [149](149-destruction-persistence-replication-streaming-and-authority.md) | Destruction Persistence, Replication, Streaming and Authority | Proposed | 2026-09-02 |
| [150](150-pcg-graph-source-cooked-plan-cache-and-runtime-ownership.md) | PCG Graph Source, Cooked Plan, Cache and Runtime Ownership | Proposed | 2026-09-02 |
| [151](151-pcg-ownership-authority-tier-and-lifecycle.md) | PCG Ownership, Authority, Tier and Lifecycle | Proposed | 2026-09-02 |
| [152](152-pcg-spatial-input-snapshot-and-node-library-ownership.md) | PCG Spatial Input Snapshot and Node-Library Ownership | Proposed | 2026-09-02 |
| [153](153-pcg-pure-evaluation-commit-and-generated-output-ownership.md) | PCG Pure Evaluation, Commit and Generated-Output Ownership | Proposed | 2026-09-02 |
| [154](154-pcg-cross-system-authority-readiness-and-commit-boundary.md) | PCG Cross-System Authority, Readiness and Commit Boundary | Proposed | 2026-09-02 |
| [155](155-pcg-graph-document-preview-bake-and-undo-ownership.md) | PCG Graph Document, Preview, Bake and Undo Ownership | Proposed | 2026-09-02 |
| [156](156-pcg-scale-budgets-trust-and-release-scope.md) | PCG Scale Budgets, Trust and Release Scope | Proposed | 2026-09-02 |
| [157](157-xr-ownership-runtime-composition-and-capability-tier.md) | XR Ownership, Runtime Composition and Capability Tier | Proposed | 2026-09-02 |
| [158](158-openxr-loader-backend-packaging-and-host-composition.md) | OpenXR Loader, Backend Packaging and Host Composition | Proposed | 2026-09-02 |
| [159](159-xr-action-tracking-and-input-projection-ownership.md) | XR Action, Tracking and Input-Projection Ownership | Proposed | 2026-09-02 |
| [160](160-xr-rendering-openxr-compositor-and-renderer-ownership.md) | XR Rendering, OpenXR Compositor and Renderer Ownership | Proposed | 2026-09-02 |
| [161](161-xr-interaction-runtime-ui-locomotion-and-accessibility-ownership.md) | XR Interaction, Runtime UI, Locomotion and Accessibility Ownership | Proposed | 2026-09-02 |
| [162](162-mixed-reality-ownership-privacy-and-capability-tier.md) | Mixed-Reality Ownership, Privacy and Capability Tier | Proposed | 2026-09-02 |
| [163](163-xr-tooling-diagnostics-privacy-and-qualification-ownership.md) | XR Tooling, Diagnostics, Privacy and Qualification Ownership | Proposed | 2026-09-02 |
| [164](164-virtual-texturing-ownership-product-scope-and-capability-tier.md) | Virtual Texturing Ownership, Product Scope and Capability Tier | Proposed | 2026-09-02 |
| [165](165-virtual-texture-source-cooked-artifact-page-store-and-cache-ownership.md) | Virtual Texture Source, Cooked Artifact, Page Store and Cache Ownership | Proposed | 2026-09-02 |
| [166](166-vtx-feature-local-residency-and-eviction-within-global-reservations.md) | VTX Feature-Local Residency and Eviction Within Global Reservations | Proposed | 2026-09-02 |
| [167](167-vtx-feedback-readback-prediction-and-camera-data-ownership.md) | VTX Feedback, Readback, Prediction and Camera-Data Ownership | Proposed | 2026-09-02 |
| [168](168-vtx-gpu-page-table-physical-cache-shader-and-material-ownership.md) | VTX GPU Page Table, Physical Cache, Shader and Material Ownership | Proposed | 2026-09-02 |
| [169](169-vtx-producer-terrain-world-streaming-packaging-and-server-ownership.md) | VTX Producer, Terrain, World Streaming, Packaging and Server Ownership | Proposed | 2026-09-02 |
| [170](170-vtx-settings-diagnostics-capture-and-qualification-ownership.md) | VTX Settings, Diagnostics, Capture and Qualification Ownership | Proposed | 2026-09-02 |
| [171](171-android-host-target-and-ownership.md) | Android Host, Target and Ownership | Proposed | 2026-09-02 |
| [172](172-immersive-agent-ownership-authoring-mode-and-risk.md) | Immersive Agent Ownership, Authoring Mode and Risk | Proposed | 2026-09-02 |
| [173](173-render-queue-submission-and-fence-contract.md) | Render Queue, Submission and Fence Contract | Proposed | 2026-09-06 |
| [174](174-render-adapter-and-device-discovery-contract.md) | Render Adapter and Device Discovery Contract | Proposed | 2026-09-06 |
| [175](175-render-resource-state-and-barrier-model.md) | Render Resource State and Barrier Model | Proposed | 2026-09-08 |
| [176](176-runtime-ui-element-and-control-taxonomy.md) | Runtime UI Element and Control Taxonomy | Accepted | 2026-09-07 |
| [177](177-mcp-application-capability-and-host-boundary.md) | MCP Application Capability and Host Boundary | Accepted | 2026-09-10 |
| [178](178-application-security-primitive-and-signature-baseline.md) | Application Security Primitive And Signature Baseline | Accepted | 2026-09-10 |
| [179](179-device-loss-and-renderer-restart-lifecycle.md) | Device Loss and Renderer Restart Lifecycle | Proposed | 2026-09-20 |

## Conventions

- **Status**: `Proposed`, `Accepted`, `Deprecated`, `Superseded`
- **Header fields**, in this order when present: Status, Date, Deciders,
  Supersedes, Superseded by, Scope, Issue, Jira, Parent, Related,
  Companion decision, Normative document(s). Use `Jira` (not `JIRA`). Link the
  Jira key to the work item. Optional fields are omitted rather than left empty.
- **Issue / Parent**: use the domain alias as the GitHub issue link text
  (`[RND-011.1](https://github.com/HoroCore/horo-engine/issues/368)`).
  Do not show `#368` as the visible text or in a second parenthesis.
- **Naming**: `NNN-lowercase-title-with-hyphens.md`
- **IDs are stable**: a number is assigned once and is never reused for a
  different decision. ADR-008 is the error-model decision
  ([008](008-error-model-exception-boundary-and-registry.md)). ADR-027 is
  renderer resource identity
  ([027](027-renderer-resource-identity-and-descriptors.md)). Similar titles
  in drafts or review extracts do not renumber either document.
- **Format**: Each ADR includes Context, Decision, Consequences, and
  Rejected Alternatives sections at minimum
- **Supersession**: When an ADR is superseded, its status changes to
  `Superseded` and the `Superseded by` field points to the replacement
- **Index**: Every ADR must be listed here

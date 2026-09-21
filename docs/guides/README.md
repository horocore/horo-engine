# Horo Engine Guides

This directory contains implementation-facing guides for developers who build on
Horo Engine and contributors working on its development tooling. Architecture
documents define the contracts; guides show how to use those contracts in concrete
workflows.

## Available Guides

- [Extension Module Development](./extension-module-development.md): build an
  add-on package that contributes editor tabs, Settings pages, MCP tools,
  commands, and data-bus observers through the extension ABI/API.
- [Local C/C++ Analysis with SonarQube MCP and VS Code](./sonarqube-mcp-local-analysis.md):
  configure the IDE bridge, analyze local changes, and diagnose partial results.
- [Terrain Descriptor Migration](./terrain-descriptor-migration.md): adopt revisioned
  bounds, versioned tier limits, immutable configuration snapshots, and replacement fencing.
- [Foliage Definition Migration](./foliage-definition-migration.md): adopt stable typed
  placement, culling, wind, collision, and exact capability admission.
- [World Spatial Object Descriptor Migration](./world-spatial-object-descriptor-migration.md):
  adopt stable authored-object identity, source, bounds, placement, and revision admission.
- [Cross-Cell Dependency Policy Migration](./cross-cell-dependency-policy-migration.md):
  classify hard co-load and soft deferred references without mixed-policy ambiguity.
- [World Object Ownership Migration](./world-object-ownership-migration.md): adopt explicit
  authored, persistent, cell-bound, and runtime-spawned ownership admission.
- [World Layer Ownership Migration](./world-layer-ownership-migration.md): preserve stable
  layer identity while separating placement, residency, audience, and control authority.
- [World Layer Filtering Migration](./world-layer-filtering-migration.md): resolve editor,
  client, server, and optional-layer inclusion without changing source identities.
- [World Layer State Migration](./world-layer-state-migration.md): keep Loaded and
  Activated guarantees ordered and independent from physical cell residency.
- [World Partition Capability Profile Migration](./world-partition-capability-profile-migration.md):
  validate project grid, precision, capacity, and package settings without fallback.
- [World Partition Registry Snapshot Migration](./world-partition-registry-migration.md):
  publish immutable generation-pinned indices and run bounded allocation-free queries.
- [World Partition Foundation Qualification](./world-partition-foundation-qualification.md):
  qualify stable identities, deterministic quantization, immutable references, bounded
  registry snapshots, capability profiles, and the non-streamed fallback composition.
- [World Streaming Priority Policy Migration](./world-streaming-priority-policy-migration.md):
  rank bounded cell-work snapshots with revision fences, stable ties, and capped age.
- [World Streaming Cell Stability Migration](./world-streaming-cell-stability-migration.md):
  replace boundary flapping and unload cooldowns with fenced hysteresis and linger decisions.
- [World Streaming Velocity Prefetch Migration](./world-streaming-prefetch-migration.md):
  replace ambient extrapolation with exact bounded camera/gameplay path projection.
- [World Streaming Cell Candidate Migration](./world-streaming-cell-candidate-migration.md):
  prepare immutable generation-pinned cell candidates from validated manifest and header facts.
- [World Streaming Cell Asset Request Migration](./world-streaming-cell-asset-request-migration.md):
  join bounded candidate dependencies beneath one explicit cancellation root.
- [XR Coordinate and Pose Contract Migration](./xr-coordinate-pose-migration.md):
  publish generation-fenced coordinate, validity, and time evidence without native
  backend leakage or implicit clock conversion.
- [XR Tracking Snapshot Migration](./xr-tracking-snapshot-migration.md):
  publish immutable, bounded device and pose snapshots with stable identity,
  capability, confidence, and loss-state semantics.
- [CPU Particle Buffer Migration](./cpu-particle-buffer-migration.md):
  replace per-particle storage and freelists with bounded aligned SoA storage and
  generation-safe spawn/kill bookkeeping.
- [CPU Particle Spawn Pipeline Migration](./cpu-particle-spawn-pipeline-migration.md):
  prepare deterministic continuous/burst birth, descriptor initialization and expiry
  over fixed SoA capacity without steady-state allocation.
- [VFX Foundation Qualification](./vfx-foundation-qualification.md): validate the
  authored particle path end to end through headless domain resolution and CPU simulation,
  plus hostile-input, tier, and allocation evidence.
- [Grounded Navigation Provider Composition](./grounded-navigation-provider-composition.md):
  compose the pinned Detour runtime provider from neutral topology with explicit
  capacity, world-generation, cancellation, and teardown behavior.
- [XR View and External Render-Target Contract Migration](./xr-view-render-plan-migration.md):
  publish bounded runtime-ordered views and generation-fenced Horo external target
  requirements without fixed stereo arrays or native image types.
- [XR Loader Preflight Migration](./xr-loader-preflight-migration.md):
  preserve exact loader/runtime/system states, selection policy, and generation fences.
- [Perception Descriptor Registry Migration](./perception-descriptor-registry-migration.md):
  compose stable sense, stimulus, and listener descriptors into an immutable
  capability-resolved sensing-job snapshot.
- [Network I/O Service Migration](./network-io-service-migration.md):
  normalize bounded private transport polling into owner-thread completion handoff.
- [Network Lifecycle Migration](./network-lifecycle-migration.md):
  adopt generation-fenced listener and connection transitions, deadlines, and exactly-once terminal results.
- [Transport Budget Migration](./transport-budget-migration.md):
  enforce versioned connection, queue, byte, rate, and sustained-overload limits before backend mutation.
- [Deterministic Network Transport Migration](./deterministic-network-transport-migration.md):
  compose explicit network-disabled, loopback, and seeded impairment modes with bounded caller-thread delivery.
- [Handshake Negotiation Migration](./handshake-negotiation-migration.md):
  replace transport-owned compatibility flags with one bounded generation-fenced session selection.

## Writing a Guide

Start new guides from [Guide Template](./guide-template.md). Use its Purpose,
Prerequisites, Workflow, Troubleshooting, Limitations, Validation Record, and
References sections; adapt the workflow steps to the task. Use lowercase hyphenated
filenames and add the finished guide to this index.

Use generic placeholders for accounts, organization/project keys, and absolute
paths. Keep secrets out of examples. Include expected results and actual verification
limits. Link architecture contracts instead of redefining them. Existing guides can
adopt the template when substantively revised; avoid format-only rewrites.

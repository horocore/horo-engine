# PCG built-in catalog migration

HORO-2026 replaces the CPU evaluator's independent hardcoded schema/type dispatch
with one immutable host-composed executable catalog, following ADR-152. This keeps
source schema validation, runtime versions, capabilities, costs and execution
semantics tied to the same retained root.

`HoroPCG` owns `Horo/PCG/PCGNodeCatalog.h`. `PCGCpuNodeKind` and `PCGCpuNodeType`
now live in this header; including `PCGCpuEvaluator.h` still exposes them through
its cooked-plan dependency. No target or broad private include dependency is added.
The public-header consumer target covers the new contract.

A background/tooling host creates a validated `PCGCapabilityProjection`, creates
`PCGNodeCatalog`, and explicitly registers the selected built-ins. Creation never
registers anything. Capture one snapshot after composition, derive source
`PCGNodeTypeSupport` and `PCGNodeRuntimeDescriptor` entries from its descriptors,
and register those inert entries in the existing `PCGRegistry`. Validate the graph
against that registry and pass the same catalog root as the fourth argument to
`CompilePCGGraph`. Registry and catalog product projections must match exactly.
The current CPU test compositions are migrated callers of this path; neither
application executable currently schedules PCG evaluation.

The catalog argument is required. An inert snapshot fails cooking with
`pcg.runtime.unavailable`; no registry-only executable cook or implicit catalog
exists. Every cooking caller, including existing cooked-plan/workspace fixtures,
must supply an explicit executable root. `PCGCookedPlan::Catalog` and
`PCGCpuCandidate::Catalog` retain it through completion and replacement. A catalog
owner can close or withdraw a node without invalidating an already admitted
immutable plan. Withdrawal applies to future cooks; host lifecycle admission
still controls whether historical plans may run.

Older synthetic cook tests with arbitrary type IDs/schemas now use real grid,
filter, merge and forward descriptors. All seven canonical typed-value encoding
oracles remain against the actual target-private `BoundedPlanWriter` used by the
compiler, including exact byte and insufficient-capacity checks. Unsupported
built-in setting/pin kinds additionally fail cook admission. Workspace peak-record
coverage uses additional real grid nodes instead of an invented multi-output grid;
independent-output writer revocation coverage uses a narrowly friended test-only
workspace model fixture, defined solely in the owning test TU and never cooked or
evaluated. The production cooking catalog remains mandatory.

The four repository-built schemas accept source version 1.0, runtime contract 1,
and migration policy 1 exactly. Grid settings are one nonzero eight-byte
network-order spatial grid identity; the other settings schemas are empty.
Unsupported versions produce `pcg.node.version_unsupported` with the authored node
location. No automatic reinterpretation or fallback is performed: migrate authored
source explicitly before cooking a supported version. Pin/settings mismatches
produce `pcg.registry.descriptor_invalid`, unavailable entries produce
`pcg.runtime.unavailable`, and missing product capabilities produce
`pcg.capability.unsupported`.

An executable cook additionally binds OfflineBake, RuntimeEvaluation or
HybridEvaluation according to the source graph mode into the portable requirement
union. Its canonical bytes therefore differ from an older registry-only cook with
the same source; consumers must recook rather than substitute old plan bytes. The
source format and node IDs remain unchanged. Process-local catalog generations
and execution pointers are never persisted. Identical version-1 descriptor
replacement leaves portable semantics unchanged and retains historical roots.

`PCGCpuEvaluationLimits::maximumPointVisits` defaults to a finite 32-million
point-visit envelope and may be lowered by the host. Cost admission uses checked
integer arithmetic and the complete declared input/output bounds. The separate
`maximumSnapshotElementVisits` envelope bounds each grid node's lookup over the
actual immutable grid count, with the same finite default. Retained catalog
storage is included in complete candidate/aggregate limits and replacement charges;
callers should use `ReservedBytes` when retaining a prior candidate. New public
limits should be assigned by name rather than extending positional aggregate
initializers. Snapshot descriptor pointers/spans borrow the snapshot, which must
outlive their use.

Third-party callbacks, module loading, additional node families and automatic
migration are not introduced by this registry delivery. The built-in functions are
repository-owned, allocation-admitted pure point operations; later contributions
must follow the separate ADR-152 trust and lifetime contract.

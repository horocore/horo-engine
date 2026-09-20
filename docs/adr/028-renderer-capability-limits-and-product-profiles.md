# ADR-028: Renderer Capability, Limits and Product Profiles

- **Status**: Proposed
- **Date**: 2026-08-31
- **Supersedes**: None
- **Scope**: M0 renderer capability and product-policy contract; no backend implementation
- **Issue**: [RND-003.1](https://github.com/HoroCore/horo-engine/issues/296)
- **Jira**: [HORO-296](https://horo-engine.atlassian.net/browse/HORO-296)
- **Normative document**: [Rendering Architecture](../architecture/runtime/rendering-architecture.md)

## Context

The current `RenderBackendCapabilities` exposes a small immutable set of booleans
from an initialized backend. It does not distinguish native device support from
implemented backend support, driver restrictions, resource limits, or product
preferences. Material and advanced-rendering documents also use API-named tiers
(`es3`, `dx11`, `dx12_vulkan`, `high_end`) as if a single rank could prove all
feature and format support. This lets an API name or a quality setting appear to
authorize resource creation that a particular device or Horo backend cannot do.

RND-001.1 already defines typed resource descriptors and generation-safe handles.
Their validation needs one capability authority that survives device loss,
offline cooking, user settings, and explicit backend selection without inventing
policy in each feature subsystem.

## Decision

### 1. Separate facts, usable support, and requested quality

Use four distinct Horo-owned value contracts. The names below specify the target
contract, not APIs claimed to exist in this M0 change.

| Contract | Contents and authority |
|---|---|
| `ReportedDeviceCapabilities` | Selected backend translates queried adapter/device facts into Horo features, typed limits and format support. Unqueried support is unavailable, never inferred from a vendor or API name. |
| `BackendImplementationSupport` | Selected module declares the operations and combinations it actually implements, including linked optional providers. A native feature that Horo cannot execute remains unusable. |
| `EffectiveRenderCapabilities` | Renderer frontend owns the immutable intersection of reported and implemented support, reduced by the selected driver-workaround policy. Includes restrictions and their provenance. This is the resource/plan admission authority. |
| `RenderProfilePolicy` / `ResolvedRenderProfile` | Application supplies validated product/cook/user preferences and required features. Frontend resolves them against effective support and available cooked variants into an immutable rendering plan selection with recorded fallback reasons. Profiles never grant capabilities. |

`RenderApi` owns the public value contracts and pure support predicates;
`RenderFrontend` owns resolution and admission. Concrete backends own native
queries and translation. Application composition selects backend, adapter and
policy inputs; it does not inspect native graphics types. Feature systems declare
requirements and consume the resolved result. GUI, CLI and MCP are adapters over
the same application queries, not independent capability resolvers.

Reported and effective snapshots are distinct types. Resource creation,
execution-plan admission, material variant selection and feature activation use
only the effective snapshot. Reported facts remain available for diagnostics.
No process-global capability singleton or mutable per-feature copy is allowed.

### 2. Typed features, limits and format predicates

- Features use a Horo-owned `RenderFeature` identity, not extension-name strings.
  Compute, indirect draw, descriptor indexing/bindless, mesh shaders, ray queries,
  ray pipelines and timestamp queries are independently represented. Compute
  does not imply a separate asynchronous compute queue. Ray queries do not imply
  ray pipelines. [ADR-039](039-ray-tracing-capability-and-abstraction.md) further
  separates acceleration-structure build/update/compaction, inline query,
  dedicated pipeline and optional shader/dispatch operations with typed limits.
  The transitional `supportsRayTracing` boolean is not admission authority. A
  provider is usable only when its own requirements also pass.
- Timestamp and pipeline-statistic operations follow
  [ADR-042](042-cpu-gpu-timestamps-and-pipeline-statistics.md). Effective support
  includes queue/stage/scope combinations, timestamp period/valid bits, calibration
  ability/error, counter semantic revisions/widths, nesting rules and finite query
  limits. A manifest hint or feature boolean admits no instrumentation by itself.
- Limits have named fields and units: bytes for buffer/range sizes and shared
  memory; texels for extents; counts for attachments, descriptors, array layers,
  workgroup axes/invocations and frames in flight; bytes for offset alignment.
  Queue operations and shader stages are typed sets, not an assumed queue count.
- Numeric zero is not an unknown/unlimited sentinel. Optional limits are absent
  when the corresponding operation is unavailable. Unknown enum values, malformed
  records, overflow and contradictory advertised feature/limit combinations
  produce a typed capability-data failure before snapshot publication.
- Bounds are checked together: each workgroup axis and the checked product must
  fit; ranges must fit their resource; offsets must satisfy every applicable
  alignment. A maximum capacity is reduced by taking the tighter upper bound;
  alignment is a conjunction of divisibility constraints, not a minimum or a
  casual maximum. Combining alignments uses checked arithmetic and rejects an
  unrepresentable constraint. This rule applies to backend and driver limits.
- `FormatSupportRequest` describes a `TextureFormat`, texture dimension, complete
  usage combination, sample count, and relevant view-format compatibility.
  `SupportsFormat(request)` evaluates the combination, not independent flags.
  Sampled support does not imply filtering, color attachment, blending, storage,
  depth/stencil attachment, copy or resolve support. Sample counts are a typed
  supported set, not a maximum assumed to include all smaller integers.
- The format table is bounded by the versioned Horo format/usage domain and is
  queried without driver calls or allocation on the frame path. Missing entries
  return unsupported. Descriptor validation still checks usage, extent, mip,
  layer, view and cross-attachment compatibility before the backend validates
  native feasibility. Effective support is not a promise that allocation cannot
  fail due to memory pressure or device loss.

Dynamic available-memory estimates are telemetry, not immutable capability limits
or guaranteed reservations. Residency/upload budgets belong to their owners and
are validated separately; a valid capability predicate cannot bypass budget admission.

These predicates extend the validation boundary in
[ADR-027](027-renderer-resource-identity-and-descriptors.md); they do not replace
its identity, pending/ready state, error or deferred-retirement rules. Unsupported
resources fail before registry reservation/native allocation. Resource APIs do
not silently change format, sample count, usage, backend, or profile.

### 3. Driver adjustments only restrict support

The compatibility registry in RND-017.6 supplies a versioned, validated policy
snapshot at composition time. Rules match typed backend/adapter identity,
driver-version ranges, OS/runtime version and module version as applicable.
Unknown driver identity cannot satisfy a version-specific match; general rules
still apply and diagnostics retain the unknown identity.

Each matching rule has a stable ID and reason. It may deny a feature/format/usage,
reduce a capacity, strengthen an alignment requirement, or select a tested private
backend workaround that preserves the public operation's semantics. It cannot
enable an unreported or unimplemented operation. Conflicting restrictions compose
conservatively; rule ordering cannot restore denied support. Duplicate IDs or
invalid ranges reject the policy snapshot rather than silently disabling safety
rules. Diagnostics enumerate matching rule IDs in stable order.

The renderer computes feature dependency closure after restrictions. For example,
disabling compute makes a compute-dependent culling recipe unavailable, even when
indirect draw remains supported. It does not invent a hardware dependency between
otherwise independent feature bits. No project/user setting may bypass a driver
restriction. Missing required support is reported to the host, which may try only
an explicitly permitted backend fallback.

### 4. Product profiles select recipes, not hardware classes

`RenderProductProfile` has four ordered preference levels: `Baseline`, `Standard`,
`High`, `Ultra` (serialized as `baseline`, `standard`, `high`, `ultra`). Ordering
permits policy ceilings; it is not a feature-subset test or a performance promise.
It is separate from Editor/Development/Profile/Shipping/Server build profiles,
roadmap milestones, navigation/AI budgets, and gameplay authority.

| Profile | Preferred rendering recipe | Explicit fallback policy |
|---|---|---|
| Baseline | Forward raster, bounded CPU light/culling preparation, bound resources, baked/ambient lighting and probes. No compute, bindless, ray tracing or mesh-shader requirement. | Required raster/material/host operations must pass; unsupported required operations fail admission. |
| Standard | Forward+ with compute light preparation when supported; shadow atlas, SSR and temporal effects may be enabled by product settings. | CPU light preparation and forward raster; probes for optional SSR; native resolution for optional temporal upscaling. |
| High | Deferred lighting when the full attachment/format/limit contract passes; GPU culling and optional screen-space GI. | Standard recipe; CPU culling and baked GI/probes when those optional paths fail their predicates. |
| Ultra | High recipe plus explicitly enabled ray effects, meshlet/mesh-shader submission, bindless resources and advanced shadow/upscale providers. | High recipe; raster effects, indexed geometry, bound resources and native resolution as declared by each enabled feature. |

These are recipe preferences, not new feature implementations or fixed universal
budgets. Each selected recipe declares exact required feature/format/limit
predicates and finite product budgets before it can be admitted. The
[ADR-036 raster policy](036-raster-render-path-and-quality-architecture.md)
defines Forward, clustered Forward+, Deferred, transparency and complete-recipe
fallback semantics; this ADR owns profile preference and effective capability
admission rather than a second path definition. The
[ADR-040 provider policy](040-reconstruction-frame-generation-and-latency-providers.md)
separates reconstruction, denoising, frame generation and latency; profile names
select none by implication, and native resolution/no generated frames remain
independent fallbacks. The
[interactive parity contract](../architecture/runtime/render-backend-parity-contract.md#required-baseline-capability)
remains mandatory for every interactive backend at every profile. Meeting that
lifecycle contract alone does not prove a material/scene recipe is supported.

There is no `if (profile >= High)` hardware admission. Numeric budgets come from
the typed product/cook settings and must be within effective limits; the profile
name never chooses an undocumented light count, texture size or memory budget.
An optional feature without an implemented and cooked fallback is unavailable;
it cannot become a promised degradation path merely by appearing in this table.
If content explicitly requires it, absence fails admission instead of disabling it.

Resolution is deterministic for the same inputs:

1. Resolve backend selection through existing host precedence and explicit
   backend fallback rules; never select a different API from a profile name.
2. Validate the product's allowed profile set, maximum profile, required features,
   finite budgets and ordered allowed fallback profiles. Reject an empty allowed
   set, unknown values, duplicate fallback entries or contradictory policy. An
   omitted fallback list is not empty: it selects the default descending chain
   below. An explicitly declared empty fallback list is invalid.
3. Select the requested profile from explicit host override, user/project
   preference, then the product default (Baseline when omitted). Overrides cannot
   widen product/cook policy. A request outside the allowed set/ceiling returns a
   typed policy error; the UI may offer supported choices but cannot silently clamp.
4. Intersect product, cook and user feature permissions. Validate material minimum
   profiles, required feature predicates and budgets against the effective snapshot
   and cooked variants.
   Required content never becomes optional because a lower profile was requested.
5. Resolve optional features using their declared ordered fallbacks. If the
   requested profile cannot satisfy required content, evaluate the profile fallback
   list in order, applying the same requirements to each. When the product omits
   `fallbackProfiles`, use the canonical descending chain `Ultra` → `High` →
   `Standard` → `Baseline`, keeping only profiles that are strictly below the
   requested profile, inside the allowed set, at or below the ceiling, and at or
   above every material `minProfile`. When the product declares the list, use that
   order as written and do not inject omitted lower profiles. No viable result
   returns `ProfileUnsupported`; no partial plan becomes active. Defaulting the
   omitted chain never strips a required feature or admits a recipe that failed
   its predicates.
6. Publish requested and selected profile, selected feature/variant recipes,
   effective-snapshot revision, budget/settings revision and bounded diagnostics.
   Successful degradation is observable even when the selected profile name is
   unchanged. A lower profile cannot fix a globally required missing feature.

Fallback profiles may change rendering recipes, not gameplay requirements. For
example, compute disabled by a driver rule selects CPU light preparation only
when its required material variant exists and the product budget permits it. A
required compute-only content path fails; it is not disguised as Baseline success.

### 5. Snapshot lifetime, startup and reconfiguration

Pre-window module metadata and helper-process probes remain advisory for project
capability fit. Probe success establishes component availability, not the final
effective support of the device and surface later created by the host. Final
admission occurs after initialization on the host-declared render-capable thread
and before scene/GUI resource work is accepted. Failure rolls back initialization
and returns an actionable startup result; no half-initialized renderer is exposed.

The frontend publishes a snapshot scoped to its owner ID, device incarnation and
capability revision. Consumers receive immutable owned snapshots or lifetime-bound
read leases, never a reference surviving frontend teardown. CPU queries perform
no native calls or blocking synchronization. Worker plan preparation captures the
snapshot identity; admission revalidates it on the render-capable thread and
rejects stale work with `StaleCapabilities` before reserving resources.
Prepared work also captures the resolved-policy revision. A changed policy requires
revalidation/repreparation before admission, so work prepared under an older user
budget or disabled feature cannot bypass the new policy.

Dynamic user quality/budget changes publish a new resolved-policy revision at a
frame boundary after complete validation; the effective device snapshot stays
unchanged. Failure retains the previous valid policy. Frames already admitted
retain their old policy and resource leases until retirement. Enabling a native
device feature not enabled at creation requires host-owned recreation, not an
in-place boolean change. Surface format/extent/presentation support has its own
generation and is rechecked on resize without mutating device facts.

Device/context loss or adapter/backend replacement invalidates admission for the
old incarnation. Re-query, reapply driver policy and re-resolve the product profile
before reconstruction under ADR-027. Unsupported recovery returns a typed result
to the host; it neither retains old handles nor silently changes API. Driver-policy
updates affecting support take effect through the same quiesce/recreate path;
they cannot revoke live resources halfway through a frame. Host workflows use
ADR-010 job/operation ownership and ADR-018 render safe points when asynchronous
preparation is needed; no frame-thread wait for worker or GPU completion is added.

### 6. Cooking, Null and diagnostics

Offline cooking uses explicit versioned target requirement manifests, never the
cook machine's GPU snapshot. Each profile/feature recipe declares required
features, limits, formats and shader/provider variants. Cook rejects missing
required variants and includes every allowed runtime fallback variant. Cache keys
include target platform/backend shader format, profile-policy version, feature
recipe and shader layout; runtime plan caches also include device/capability and
settings revisions. Runtime admission verifies the actual effective snapshot;
successful cook does not prove arbitrary hardware support.

Null is an explicitly selected headless/test backend, not a product profile or a
surrogate GPU. Production Null reports only its implemented validation operations
and no presentation. Tests may inject a bounded synthetic capability fixture for
the same resolver/validator, marked synthetic in diagnostics. It cannot establish
hardware parity or GPU execution support. Unsupported plans fail identically;
timing/lifetime follow the owning frame/resource contract rather than completing
pending operations early for tests.

Every result identifies requested/selected backend and profile, adapter/device
incarnation, effective revision, policy version, relevant feature/format/limit and
driver restriction IDs. Diagnostics distinguish `InvalidCapabilityData`,
`InvalidProfilePolicy`, `FeatureUnsupported`, `FormatUnsupported`, `LimitExceeded`,
`MissingVariant`, `ProfileUnsupported` and `StaleCapabilities`. These are typed
categories for implementation in the existing error registry, not new string-based
control flow. Messages are bounded and produced at resolution/admission, not by
polling the driver each frame. No report treats an unknown value as supported.

### 7. Migration and downstream ownership

1. RND-003.2 and RND-017.6 implement reported facts, backend implementation support,
   driver rules and the effective snapshot. Existing `RenderBackendCapabilities`
   booleans remain transitional; do not reinterpret hardware support as implemented
   support or add a competing global tier authority.
2. RND-003.3/.4 consume effective queue/feature/format/limit predicates at admission;
   [RND-003.7 / ADR-179](179-device-loss-and-renderer-restart-lifecycle.md) rebuilds
   snapshots and invalidates old plans during host-owned recovery.
3. Material/shader cooking and frontend selection replace API-named tiers with
   profile policy plus explicit feature requirements. The mechanical preference
   mapping is `es3` -> `baseline`, `dx11` -> `standard`, `dx12_vulkan` -> `high`,
   `high_end` -> `ultra`; it never selects an API or grants feature support.
   `minTier`, `preferredTier`, `tierOverrides` migrate to `minProfile`,
   `preferredProfile`, `profileOverrides`. Material minimums restrict profile
   eligibility only; variant predicates still decide actual support.
4. Existing authored requirements stay required. Migration translates the old
   feature expectations into explicit recipe requirements/fallbacks and reports
   incompatible content. Merely renaming `high_end` to `ultra` cannot silently
   drop formerly required ray/mesh/bindless paths. Unknown or conflicting old/new
   fields fail staged validation. Persist changes through the existing ProjectVersion
   migration transaction and recook affected variants; do not maintain two writable
   settings formats. `EditorRenderingTier` and persisted editor preferences migrate
   in the settings owner, not in a backend.
5. Legacy tier tables remaining in feature documents describe visual intent under
   this mapping only. They cannot authorize resources or derive AI/navigation/
   streaming budgets. Accessibility remains available at every supported profile.
   This ADR owns profile meaning; feature documents own concrete recipe predicates,
   budgets and fallbacks. Their implementations must replace legacy tier branches.

No public header or persisted asset schema is changed by this documentation PR.
The new material examples describe the target schema and require the above
migration before implementation. M0 closes the decision; implementation and
hardware qualification remain in their owning downstream tickets.

## Validation obligations

RND-003.9 and the owning subsystem tests must cover:

- identical reported facts with different backend implementation masks or driver
  rules producing different effective support, with stable restriction provenance;
- denied features never restored by profiles, overrides, emulation claims or rule
  ordering; malformed records, duplicate rules and unknown values rejected;
- exact/over-limit extents and workgroups, multiplication overflow, combined
  alignment constraints, and usage/sample/view-format combinations where each
  individual capability exists but the requested combination is unsupported;
- every profile on a baseline-only fixture; optional fallback versus required
  feature failure; invalid ceilings, missing cooked fallback, and empty allowed set;
- identical profile names on different devices selecting different valid recipes;
- probe/device mismatch, stale worker plans, failed transactional quality updates,
  settings changes with frames in flight, and device recovery with reduced support;
- offline cooks independent of the build GPU, legacy profile migration preserving
  requirements, and synthetic Null results never qualifying an interactive backend.

These are downstream executable requirements. This M0 PR validates documentation
links, policy consistency and example syntax; it does not claim GPU test coverage.

## Consequences

Features can ask precise support questions without branching on API identity.
Driver restrictions, absent backend implementations and optional-quality fallback
become explainable. Cooking and runtime admission share requirement semantics while
using different evidence sources. The cost is a richer typed capability schema,
versioned recipe/policy data, explicit fallback variants and migration of legacy
settings. That cost avoids late native failures and silent content degradation.

## Rejected alternatives

- **One capability boolean set for everything**: loses whether a feature is
  physically present, implemented, disabled by a workaround or unwanted by policy.
- **API/vendor tier implies support**: ignores per-device formats, limits, driver
  problems and incomplete Horo backend implementations; violates backend parity.
- **Profiles are mandatory cumulative hardware feature sets**: makes Ultra imply
  every optional technology and cannot represent devices with incomparable features.
- **Always clamp to the highest supported tier**: hides user/product errors and
  missing cooked content; explicit recipe and profile fallbacks are reviewable.
- **Let each feature query the native API**: duplicates policy, leaks native types,
  adds frame-path work and creates inconsistent decisions after device recreation.
- **Live mutation of capability booleans**: invalidates admitted plans and resource
  assumptions without an owner/generation boundary.
- **Cook against the developer GPU or treat Null as fully capable**: confuses
  reproducible target validation with hardware availability and qualification.

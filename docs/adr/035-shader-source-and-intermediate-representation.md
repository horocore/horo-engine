# ADR-035: Shader Source and Intermediate Representation

- **Status**: Accepted
- **Date**: 2026-08-31
- **Supersedes**: None
- **Scope**: Shader source language, compiler routes and reflection authority
- **Issue**: [RND-011.1](https://github.com/HoroCore/horo-engine/issues/368)
- **Jira**: [HORO-368](https://horo-engine.atlassian.net/browse/HORO-368)
- **Parent**: [RND-011](https://github.com/HoroCore/horo-engine/issues/283)
- **Normative document**: [Rendering Architecture](../architecture/runtime/rendering-architecture.md)

## Context

The asset-pipeline target describes an unspecified Horo source dialect and one
canonical SPIR-V representation. That does not define a source language, a D3D12
DXIL path, or how reflection stays consistent through translation. The current
OpenGL/Metal viewport integrations embed separate native shader sources; they
are migration inputs, not an implemented multi-target shader cooker.

This decision owns source language, compiler/IR routes, interface validation and
diagnostic boundaries. It preserves the target baselines in ADR-029–032, resource
lifetime in [ADR-027](027-renderer-resource-identity-and-descriptors.md), and asset
publication in [Asset Pipeline](../architecture/runtime/asset-pipeline.md).
It does not implement a compiler, change material shading algorithms, or activate
a desktop cook target in the existing headless-only Phase A contract.

## Decision

### Authority and adjacent ownership

This ADR is the single normative owner of the production shader source language,
compiler and intermediate-representation routes, target-descriptor requirements,
normalized reflection boundary, artifact identity inputs, source diagnostic
provenance, and packaged-versus-development compilation policy. Summaries in
[Asset Pipeline](../architecture/runtime/asset-pipeline.md),
[Rendering Architecture](../architecture/runtime/rendering-architecture.md), and
[Material And Shader Model](../architecture/runtime/material-and-shader-model.md)
consume this decision and cannot introduce another source language, universal
intermediate, reflection authority, or implicit runtime compiler path.

Adjacent owners remain separate:

- Asset Pipeline owns admitted source/include resolution, bounded cook
  orchestration, cache lookup, staging, and atomic artifact publication.
- Renderer resource and capability contracts own live shader-module/pipeline
  generations and target admission under [ADR-027](027-renderer-resource-identity-and-descriptors.md)
  and [ADR-028](028-renderer-capability-limits-and-product-profiles.md).
- ADR-029 through ADR-032 own native backend baselines; they constrain this
  decision's target descriptors but do not select a different engine-wide
  language or reflection model.
- Material and shader-graph work owns semantic material/graph schemas and emits
  this source contract plus mappings; it does not create another compiler or IR
  authority.

RND-011.2 through RND-011.13 consume this decision within their assigned
implementation, adjacent-decision, or qualification scopes. Their issue numbers
and roadmap milestones do not imply delivery order; native blocked-by relationships
remain the scheduling authority.

### 1. One source contract, target-specific derived artifacts

Use **HLSL with an explicitly selected HLSL 2021 language mode** as the first
production textual authoring language. Shader graphs generate that same source
contract plus source maps; they do not own an independent runtime compiler.
Horo adds a versioned manifest, logical bindings and approved helper library,
not a new language parser or an API-specific public header.

The portable baseline consists of vertex/fragment entry points, 32-bit scalar,
vector and matrix arithmetic, bounded uniform data, sampled textures/samplers
and explicit stage interfaces admitted by every declared target. Boolean material
values use explicit 32-bit integer storage at buffer boundaries. Compute/storage
resources, wave operations, 16/64-bit types, advanced stages, unbounded arrays and
native intrinsics require separate declared capability variants; choosing HLSL
or Shader Model 6.0 does not make them portable. A shader is portable only over
its declared target set and tested requirements, not over every compiler backend.

The manifest declares source identity/revision, entry points/stages, logical
resource IDs, parameter schema, permutation inputs, required features and target
set. Explicit source bindings must match it; implicit compiler-assigned bindings
are not authoritative. Generated target annotations/adapters are cook products.
Portable source cannot branch on a vendor/API macro to silently change material
semantics. Target-specific implementations are separate named variants with the
same declared logical interface, explicit requirements and admitted fallback
policy under [ADR-028](028-renderer-capability-limits-and-product-profiles.md).

### 2. Compiler and intermediate-representation routes

Use DXC for HLSL compilation, SPIRV-Tools validation for SPIR-V artifacts, and
SPIRV-Cross for the selected GLSL/MSL translation routes. These are toolchain
components behind private adapters. Exact releases/build hashes, dependencies,
licenses, host platforms and invocation options must be locked and qualified by
RND-011.3 before implementation enables a target; this ADR adds no dependency or
floating tool download. An installed SDK/compiler default is never the lock.

| Target | Selected route and runtime artifact | Baseline constraint |
|---|---|---|
| Vulkan | HLSL → DXC SPIR-V → validation/normalization → cooked SPIR-V + Horo reflection | [ADR-031](031-vulkan-loader-platform-and-version-baseline.md): explicit Vulkan 1.3 environment, SPIR-V at most 1.6, only declared/enabled capabilities. |
| OpenGL | HLSL → validated SPIR-V → SPIRV-Cross → cooked desktop GLSL + Horo binding map | [ADR-029](029-opengl-core-profile-and-platform-policy.md): GLSL 4.10 / desktop 4.1 Core; native compile/link at controlled resource preparation, not a requirement for GL SPIR-V support. |
| Metal | HLSL → validated SPIR-V → SPIRV-Cross MSL → Apple offline compiler/library → cooked library + Horo binding map | [ADR-030](030-metal-platform-and-feature-baseline.md): explicit MSL 2.4 and macOS 14 deployment/architecture options; no toolchain required by packaged players. |
| D3D12 | Same HLSL → DXC directly → validated DXIL + Horo binding map | [ADR-032](032-d3d12-baseline-and-agility-sdk-policy.md): explicit stage Shader Model 6.0 profile and baseline root signature 1.0; no SPIR-V → HLSL → DXIL round trip. |
| Headless Null | Source/manifest validation and normalized reflection fixture/artifact, without native GPU creation | Only declared validation coverage; no claim that an uncompiled native target or visual result passed. |

The GLSL/MSL translation inputs use an explicit locked SPIR-V environment and
feature allowlist bounded by their **destination** requirements. Vulkan 1.3 is
the default intermediate validation environment; it is not permission to emit a
construct GLSL 4.10/MSL 2.4 cannot realize. Unsupported lowering fails the target
cook instead of raising its shader version, changing the source semantics or
falling back to another compiler route. Optional routes need separate versioned
target descriptors and qualification before selection.

SPIR-V is a shared derived intermediate for three routes, **not the universal
asset authority or required D3D12 interchange format**. Source/manifest/dependency
identity and the normalized Horo interface are common. Native payloads remain
target-specific and cannot be substituted merely because reflection looks alike.

DXC documents both DXIL and SPIR-V outputs in its
[project contract](https://github.com/microsoft/DirectXShaderCompiler).
SPIRV-Cross documents GLSL/MSL translation and reflection in its
[README](https://github.com/KhronosGroup/SPIRV-Cross/blob/main/README.md).
Those capabilities motivate the routes; they do not prove every Horo shader
translates correctly or establish binary reproducibility across tool versions.

### 3. Explicit target options and portability validation

One validated Horo shader-target descriptor is consumed by offline cooking,
permitted editor compilation and backend artifact admission. It identifies:

- target OS/architecture/deployment, backend payload format and version;
- source language version, entry point/stage and explicit compiler profile;
- intermediate environment, allowed features/extensions, optimization/debug and
  floating-point options, matrix packing and buffer-layout rules;
- native language/profile options, binding remap policy and interface-schema
  versions; compiler, translator and validator build identities.

Missing/conflicting options fail before compiler execution. The baseline uses
column-major matrix packing explicitly, and the SPIR-V routes use strict GL
uniform/storage layout selection rather than DXC's relaxed default. The compiler
adapter selects and tests those options; CPU packing still follows each final
target layout. No scalar-layout extension is introduced implicitly. See
[DXC memory-layout rules](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst#memory-layout-rules).
Optimization must not enable unsafe floating-point transformations implicitly;
explicit fast-math variants record their behavior and are separately qualified.
Cross-backend numerical agreement uses declared image/numeric tolerances, not a
claim of bit-identical GPU arithmetic.

SceneMath and renderer projection contracts retain coordinate authority. In
particular, projection already selects the backend depth range; compiler adapters
must disable automatic depth remapping that would apply it twice. Any required
Y/origin/stage-built-in adjustment has one explicit backend lowering policy in the
target descriptor and fixture tests. Source materials cannot add hidden platform
flips. Texture origin, front-face orientation and depth tests are qualified
together, not inferred from a successful shader compile.

### 4. Horo reflection is the binding authority

The shader toolchain normalizes compiler/translator reflection into a versioned
Horo model. It records logical resource/parameter IDs, stage interfaces, scalar
types, array extents, access, stage visibility and required feature predicates.
Each native artifact also carries a target binding/packing map: slots/register
spaces or indices represented as Horo values, member offsets/strides/matrix order,
texture-sampler pairing and generated helper resources. OpenGL 4.1 Core / GLSL
4.10 does not require explicit `layout(binding = N)` locations, so the OpenGL map
also records native GLSL resource and uniform-block names for runtime resolution
through `glGetUniformLocation`, `glGetUniformBlockIndex` and equivalent queries.
Native reflection structs and SPIRV-Cross JSON never become the public renderer
or material schema.

Manifest validation runs before compilation; final-artifact validation checks
reflection against that manifest and the complete variant stage interface after
optimization/translation. D3D12 reflects its actual DXIL; it cannot borrow the
SPIR-V reflection of a different compilation. GLSL/MSL remaps come from their
actual translation, with native link/pipeline reflection checks for the fields
available there. Fields unavailable in native reflection require a validated
lowering/packing contract and qualification fixture, not invented native evidence.
An unprovable required interface rejects the artifact/target.

Optimization may remove an unused declared binding: record it as inactive for
that exact variant, without renumbering logical IDs or treating a missing active
required binding as optional. Manifest stage visibility is the authored maximum;
an active reflected binding records the nonempty subset actually observed in the
final stages. Unknown or undeclared stage bits fail validation. A globally inactive
record retains its declared visibility with `active=false`; no stage consumes it.
Removing a resource from one stage must not remove its surviving stages or make a
missing active native argument optional. Native validation checks the actual
reflected stage set, while logical interface compatibility remains manifest-owned.
Undeclared active resources, incompatible stage I/O,
overlapping native slots, unbounded dimensions or inconsistent parameter types
fail validation. Generated combined samplers/helper bindings have explicit mappings
and count toward actual target limits. OpenGL's flat binding and combined-sampler
mapping must preserve each logical texture/sampler pair; direct descriptor-set
numbers are not valid GL bindings by assumption. The backend binds from the
recorded native names plus the Horo logical IDs, not from Vulkan/DXIL slot
numbers copied into GLSL.

Material/Scene code supplies typed semantic values. The renderer packs them through
the selected target map; it must not memcpy a C++ struct or reuse Vulkan offsets
as DXIL/MSL offsets. A changed logical parameter schema requires material migration
or a typed incompatibility. A changed target packing map changes artifact/layout
identity and pipeline/binding caches even when semantic values remain identical.

### 5. Cook, cache and publication authority

The Asset Pipeline owns trusted source/include resolution, dependency snapshots,
bounded scheduling, cache keys, staging, manifests and atomic publication. The
shader compiler adapter consumes immutable inputs and writes bounded logical
outputs; it does not choose project paths, mutate sources or install tools.
Compilation includes are resolved only within admitted asset/package/toolchain
roots, with cycle/depth/size/count limits. No network include, ambient working
directory search, arbitrary command-line passthrough or timestamp macro may make
the same declared inputs produce untracked behavior. Toolchain processes use
host-selected executables and argument arrays, not source-derived shell commands.

The shader artifact key extends the existing dependency-aware cook identity with
the complete target descriptor, manifest/schema/generator versions, transitive
source/include digests, canonical defines/permutation and specialization values,
and all compiler/translator/validator build identities. Debug/optimization options
participate; the local GPU does not decide a portable cook target. A permutation
key is a logical lookup index, not the complete artifact compatibility key.

Emit payload, normalized reflection, target map, requirements, dependencies and
diagnostics as one validated artifact generation. When the target descriptor
enables debug options, also emit and archive native debug artifacts with that
generation: DXIL PDBs for D3D12, SPIR-V debug info for Vulkan, and equivalent
source-mapping data for translated GLSL/MSL. Debug artifacts are companion
records, not runtime admission inputs; packaged/shipping targets omit them unless
the product explicitly requests a debug payload set. They must not change the
public Horo interface schema or the admitted shader payload. Failure of a required
target or variant prevents publication of the requested target set; previous good
published generations remain active. Optional exclusions must be declared before the cook,
not decided by dropping failed outputs afterward. Tools missing for Metal/DXIL
production produce a clear unsupported cook-host/toolchain error, not placeholder
payloads. Cross-host build services, when composed, obey the same input/output
and cache authority rather than weakening qualification.

Keep three caches distinct: portable cooked shader artifacts owned by Asset
Pipeline; optional native driver/pipeline binary acceleration blobs owned by the
backend; and live GPU pipeline objects owned by the renderer registry. Native
cache keys include the backend/device/driver compatibility identity required by
the API. Reject stale blobs and rebuild from the cooked artifact at a permitted
preparation boundary. Never serialize live handles or make a native cache blob the
only recoverable shader source. Shipping a cache does not guarantee every driver
can reuse it or avoid all native pipeline compilation.

### 6. Runtime compilation, hot reload and diagnostics

Packaged games require cooked variants: no HLSL compilation, graph evaluation,
DXC/SPIRV-Cross installation or source-network fetch on a cache miss. OpenGL's
driver compilation/link of **cooked GLSL** and native pipeline creation are still
required artifact realization, not an exception enabling arbitrary source cooks.
Schedule/prewarm them outside frame-hot execution on the required graphics owner
thread. Pending required pipelines defer dependent work; do not block the core loop
waiting for compilation or display silently substituted materials.

Editor/development compilation is an explicit bounded request using the same
target descriptor, diagnostics and artifact validation. CPU compiler work follows
[ADR-010](010-job-waiting-and-operation-store-ownership.md) `JobId`/`OperationId`
ownership and cancellation; user-visible status uses `OperationStore`. Native
realization and publication follow ADR-027 on the render-capable thread. The
[ADR-018](018-command-registration-permissions-threading-and-packaged-build-policy.md)
`RenderSafePoint` is that same owner boundary. Work retains immutable source/input
leases and project/device generations; cancelled or superseded results cannot
publish. Queue, source, diagnostics and native resource budgets remain finite.

Hot reload stages and validates an entire candidate, then publishes new shader/
pipeline generations and compatible dependent bindings at the owner safe point.
Failure retains the last good generation and reports the failed candidate; it
does not label an old pipeline as the new successful revision. Old in-flight work
retires normally, with old/new overlap admitted under
[ADR-034](034-gpu-memory-and-residency-ownership.md).

Diagnostics use stable Horo categories for source/include errors, unsupported
features, reflection/layout mismatch, toolchain failure, native compile/link error,
missing variant, cancellation and stale result. Include source asset/revision,
entry/stage/variant/target, tool identity and original severity/message. Preserve
include stacks and graph node/pin-to-generated-line mappings across preprocessing
and translation; unmappable messages retain their generated location explicitly.
Bound/truncate payloads with a visible truncation marker. No raw local absolute
paths or source contents enter remote telemetry by default. Build Output, editor
and CLI consume the same typed diagnostics, not independently parsed error text.

## Rejected Alternatives

| Option | Decision and reason |
|---|---|
| Independent GLSL/HLSL/MSL authoring for every backend | Rejected as the portable default: duplicates material semantics and diagnostics. Backend-private bootstrap code and explicitly target-scoped variants may remain, with declared scope and qualification. |
| GLSL → SPIR-V as the universal source path | Not selected: fits GL/Vulkan, but adds an HLSL translation/recompile hop for the direct D3D12 target. |
| HLSL → SPIR-V → HLSL → DXIL for all shaders | Rejected: makes DXIL semantics/reflection depend on an unnecessary round trip. Compile the common source directly for D3D12 and validate both routes. |
| Hand-author SPIR-V or publish it as the material interface | Rejected: poor authoring diagnostics and does not solve native layout/capability differences. SPIR-V remains a derived artifact. |
| Slang or WGSL as the first source language | Deferred: would add a different language/toolchain contract before existing backend baselines and material bindings are qualified. Reconsider through an explicit source-contract revision, not a per-project compiler switch. |
| DXIL → Metal conversion or direct DXC Metal output | Not the selected baseline: would need independent MSL/deployment, binding and device qualification. It cannot silently replace the established MSL 2.4 route. |
| One byte layout and reflection result reused on all backends | Rejected: compiler layout and translation differences can silently corrupt material data. Use semantic IDs plus checked target maps. |

The selected split needs more than one compiler output path and cross-target
fixtures. It avoids inventing a language and preserves direct DXIL production,
but translator coverage is an explicit admission constraint. No M0 claim of full
HLSL feature portability, bitwise determinism or shader performance is made.

## Migration And Verification

This ADR replaces the unspecified Horo dialect/universal-SPIR-V target prose.
Existing embedded GLSL/MSL paths must migrate to source manifests and cooked
artifacts or be explicitly scoped backend bootstrap shaders with the same version,
diagnostic, budget and lifecycle rules. Keeping them is not evidence that the
portable compiler exists. Tool versions become pinned dependencies only when
their owning implementation lands; compiler libraries do not enter public headers.

Persisted source/manifest/target-map schema changes use the existing project and
asset migration/version authorities. Regenerate derived artifacts after toolchain
or layout changes; do not silently rewrite authored shader semantics. This PR
changes no current parser, serialized format, native compilation option or target
availability.

| Delivery | Required implementation evidence |
|---|---|
| RND-011.2 / #369 | Versioned manifest, logical binding/parameter IDs and explicit target descriptor validation. |
| RND-011.3/.4 / #370–371 | Locked compiler/validator/translator adapters, bounded offline pipeline, final-artifact reflection and source mapping. |
| RND-011.5/.6 / #373, #372 | Complete permutation/specialization and artifact/native-cache identity; no raw-handle persistence. |
| RND-011.7/.8 / #374–375 | Prewarm/development policy, nonblocking completion, stale/cancelled candidate rejection and safe replacement. |
| RND-011.9–.13 / #376–380 | Material packing, graph lowering, typed Build Output diagnostics and cross-backend qualification through those contracts. |

Qualification must test all selected routes, not only one intermediate compiler:

- A shared shader corpus covering material buffers, matrix/array/struct layout,
  multiple texture/sampler pairs, stage linking, depth/origin/front-face behavior
  and declared optional-feature rejection; compare outputs with stated tolerances.
- Minimal-runtime GL 4.1, MSL 2.4/macOS 14, Vulkan 1.3 and DXIL SM 6.0 admission;
  record exact compiler arguments/options and emitted versions. A cook-host compile
  alone does not establish runtime support.
- Malformed/overlapping reflection, optimizer-removed bindings, mismatched DXIL
  versus SPIR-V layouts, missing native reflection fields and target-limit overflow.
- Include changes/cycles/root escapes, variant/tool/options changes, corrupted
  artifacts, native driver-cache mismatch, reproducibility under the same locked
  inputs and diagnostic mapping to source/graph nodes.
- Missing required variants, failed multi-target publication, absent tools,
  bounded queue pressure, cancellation/reload races, project/device replacement,
  last-good retention and GPU retirement after hot reload.
- Packaged launch without authoring tools, permitted cooked-GLSL realization and
  refusal to source-compile missing variants. Null tests cover contracts and error
  schedules; native validation and image tests remain required for each backend.

## Consequences

Shader authoring has one concrete entry point and downstream teams can implement
target routes without selecting conflicting languages or reflection authorities.
Target maps and explicit compiler options expose compatibility differences early.
The cost is a locked multi-tool pipeline, retained source maps and mandatory native
qualification. Runtime code and current embedded shader performance are unchanged
by this M0 architecture decision.

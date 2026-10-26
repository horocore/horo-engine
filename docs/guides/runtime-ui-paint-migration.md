# Runtime UI native paint migration

## Reason and ownership

The existing render snapshot retains clip/mask indexes and leaf alpha, but it
cannot express rounded mask ancestry or isolated opacity groups. The existing
frontend UI submission retains image/font/geometry leases without issuing a draw
workload. The new paint path extends those authorities rather than replacing the
canonical document, style resolver, RuntimeHost scheduler or renderer selection.

## Caller migration

Existing alpha masks and ungrouped draw projections remain source-compatible:
new parent/group members default to their absence sentinels and new group capacity
defaults to zero. Callers using groups reserve explicit bounded capacity, publish
parent-before-child nonempty intervals, and assign each command its deepest group.
A group opacity is applied once at composite; existing leaf opacity is unchanged.
Rounded/rectangle masks carry no invented texture identity. Alpha masks retain
exact asset revision and role-specific realization evidence.

Runtime UI producers continue to publish `UiRenderSnapshot`/geometry leases.
The host-owned frontend bridge translates them to renderer-only prepared workload
values. RenderApi does not depend on RuntimeUi. New public renderer headers have
one explicit target owner and public consumer coverage; native handles stay in
private backend targets. Shader realization, buffers and effect targets are
prepared at explicit lifecycle boundaries, not discovered during a widget draw.

The real host source is the committed `UiAnimationRuntimeParticipant::Acquire`
publication. Composition must register/select that source and the paint owner;
a test-only optional callback, arbitrary desktop document or dormant factory is
not a completed host integration. Presentation receipts follow successful native
presentation and the exact view/canvas/snapshot/interaction lineage.

## Lifecycle and qualification

Paint reservations are finite. Capacity, malformed topology, stale source,
unsupported target and preparation failure retain prior admitted state. Frame-hot
work does not allocate, compile, block on I/O or wait for GPU idle. Source, mask,
shader, buffer and transient target generations remain pinned until native
completion, terminal device loss or unsent abandonment.

Every advertised native paint profile requires its actual encoder, format admission
and pixel/lifecycle evidence. This change targets the selected Metal profile;
OpenGL and Vulkan explicitly deny the optional paint capability while their ordinary
editor GUI/preview remains available. No peer or renderer fallback is selected.
Null/model tests qualify admission and lifetime only. Native availability and ticket
closure remain unqualified until the actual source/encoder/presentation path executes.

## Explicit empty startup and source admission

`UiAnimationOwner::Create` requires an issued tree in a real `UiReloadGeneration`;
its admission remains unchanged. The app registers one finite UI lifecycle adapter
before `RuntimeHost::Startup`. That adapter starts in `NoSource`, and performs an
empty `ComposeUi` contribution without manufacturing a document. A real SDK source
attaches a complete admitted generation at an explicit structural safe point. Only
then does the adapter construct the canonical owner and participant with its actual
`RuntimeDispatchSource`. Failed attachment preserves the previous state/publication.

The order is startup, explicit source attachment, VariableUpdate preparation,
RenderExtraction publication and immutable acquisition, native paint composition,
presentation receipt, structural reload, and finally detach/shutdown. Detach closes
new source/paint admission, enters `Retiring`, and drains jobs, frame leases and
native completion leases before destruction. `Failed` is explicit; it never selects
a fallback renderer or silently replaces the canonical source. The adapter remains
registered for the host lifetime; no late `AddParticipant` or ambient registration
is introduced.

## Shared service publisher

Production attachment preserves `RuntimeUiService::Admit` and its
`UiSceneReconciliation` registry as the sole authored-generation publisher. An
animation source reservation pins the shared service state and exact instance;
it does not copy the generation or take a second `UiHotReload`. Animation owns
only derived clocks, styles and frame publications. The service keeps weak bounded
reservation entries, and stores no animation-owner pointer or callback.

The trusted SDK host uses the coordinated service reload overload, synchronously
borrowing its actual animation owner and supplying its existing allocator, style
registry/resolver, complete animation definition and reload policy. Exact service,
instance, current generation and reservation identity must all match. Preparation
retains the old source, derived frame, client grant and queued action on failure.
Successful canonical commit invalidates attribution once and then swaps the fully
prepared derived owner. Native modules retain existing `RuntimeUiClient` grants;
they receive no service, allocator, publisher or animation-policy privilege.

The old service reload overload remains valid for unanimated instances. For an
attached animation source it reports that coordinated preparation is required;
callers migrate to the typed overload rather than receiving permanent busy. Actual
concurrent preparation or retirement remains busy. Scene replacement and unload
must first coordinate or retire the same derived source; they cannot publish a
replacement ahead of its derived candidate. Animation detach never shuts down or
collects the service's publisher. Service shutdown revokes access while immutable
frame and native completion leases drain without a normal-frame wait.

The source reservation retains an exact immutable generation lease even before the
first frame. Service shutdown revokes operation admission immediately, while that
pin prevents canonical entry collection beneath the attached derived owner. A
coordinated reload pins its private candidate before commit, transfers the pin only
on successful publication, and leaves the old pin on every preparation/cancellation
failure. Detach and native completion drain precede final reservation release.

### Reusable native paint slots and shared frame completion

`UiPaintFrameBuffers` names three distinct frontend-owned host-visible residents. Allocate a finite ring when a real source is
admitted, before the first paint frame. `UpdateUiPaintFrame` borrows complete immutable vertex/index/parameter views only for
its synchronous call. It validates the entire payload and resolves exact frontend generations before calling the selected
backend. Native adapters must prove resource namespace, usage, mapped capacity, advancing slot revision and completed prior
GPU use for **all three** destinations before the first write. A pending slot returns a typed pending result; no frame wait or
per-frame buffer/pipeline allocation is permitted. A native upload failure never publishes the new accepted UI revision.
The caller keeps its prior prepared frame until a complete new slot is admitted.

A paint workload names that exact accepted slot revision and uniform range. Vertex colors remain straight-alpha inputs;
ordinary image/glyph sources are premultiplied once in the cooked shader. An isolated group's completed texture is explicitly
`Premultiplied` and its opacity is applied once when composited. Target-origin flags describe render-target sampling without
changing canonical layout or hit testing.

The named UI stage follows scene execution and precedes `ComposeGui`. It extends the renderer's **same** canonical active
completion lease with bounded UI source, resident, isolated-target and cooked-pipeline ownership. It does not reopen generic
`Execute`/`ExecuteGraph` admission, replace already submitted scene pins, invent a second GUI submission authority, or release
an existing native frame lease on a rejected extension. Explicit queues use the existing frontend submission allocator and
wait on already admitted scene signals. GUI preparation then extends that same lease through its existing contract.

The reusable frame update API owns its typed outcomes in RenderApi through
`UiPaintFrameErrors`: malformed/stale inputs are `InvalidInput`, retained native
slots are `Pending`, pre-write cancellation is `Cancelled`, and an inactive or
unsupported owner is `Unavailable`. Native submission failures preserve their
backend error. These new paint callers do not depend on the separate
RenderUpload service or its lifecycle errors. No published compatibility API is
replaced; consumers of the new update contract compare its domain and code.

`UiPaintFrameBufferPool` belongs to RenderFrontend and creates exactly three
host-visible residents per explicitly admitted slot (at most eight and bounded
by the selected device). It owns capacity/revision metadata, while the frontend
remains the sole resource owner. Successful updates reuse resident identities;
pending, cancellation, malformed input and failed admission preserve the previous
revision. Pools close on their creating render thread before normal frontend
shutdown. A lazily allocated private owner marker is revoked before frontend
destruction; a surviving pool rejects update and releases only metadata, while
the frontend drains its own native/GPU resource claims. The marker contains no
callback, ambient lookup, new thread synchronization or second resource registry.
New hosts use this pool at real source admission; NoSource creates no pool.

The unpublished paint raster ABI now uses exactly 64 bytes at each existing
256-byte uniform offset: extent/opacity/sampling/flags/reserved occupy bytes0–31,
mask-local extent and reserved words bytes32–47, and clockwise corner radii
bytes48–63. Runtime validation and native reflection require that exact shape.
Rounded coverage uses the original affine quad UVs and checks each individual
corner region, including asymmetric radii beyond the rectangle midpoint. Its
local distance is antialiased using fragment derivatives; CPU tessellation does
not substitute another mask boundary. Coverage targets begin transparent, paint
the shape, and multiply complete ancestor coverage over the full target using a
separate closed `MultiplyCoverage` operation. No outside-mask white region may
survive. These source additions remain unqualified until actual native execution
and the complete prepared program path are tested.

### Immutable prepared program and ordinary frame changes

`UiPaintProgram::ValidatePreparation` is an owner mutation and is no longer const:
it observes the complete bounded creation inventory and records only exact failed
creation generations already retired by the canonical resource registry. The
first native error remains the result. Clearing those local failed handles keeps
cleanup deterministic after bounded operation-history eviction; unknown outcomes
and stale successful generations still preserve normal release errors. Rebuild
member-pointer consumers with the non-const signature. This changes no resource
publication, runtime source or shader artifact format.

`UiPaintProgram` is owned by RenderFrontend; its public header is staged only by
that target. Its actual consumers supply canonical `UiRenderGeometryPlan` leases
and the existing image/font submission bindings, mapped to the prepared slot via
`TextureResource`. RenderApi remains Foundation-only. `Create` is explicit safe-point
work: it compiles complete potential pass/resource roles, realizes distinct primary
and linear RGBA16F kernels, and reserves two CPU candidates per finite resident slot.
`Prepare` reuses those arrays and native residents. It validates current source,
resource realization and the complete geometry before writing; cancellation or a
busy slot preserves the issued accepted publication. Ordinary geometry, text glyphs,
colors, mask transforms, visibility and opacity do not compile another graph.
Changed source generations, group/mask topology or external texture inventory require
a complete replacement. The App owner must coalesce the latest structural request,
cancel superseded preparation and publish only a complete ready replacement.

Each pass declares a finite sorted table of every potential sampled resource. Each
draw selects at most one source and one coverage entry; native admission resolves
and validates the complete table before creating an encoder. Relative mask scopes
are keyed by the actual leaf mask and an excluded **actual ancestor** at a group
boundary. A child group paints only the remaining mask chain; its completed surface
is masked once at composite into the parent. Ancestor fractional alpha is therefore
not multiplied again inside each child. Independent sibling surfaces retain their
canonical interval order. Binary rectangles additionally use balanced native stencil
push/pop; fractional rounded and texture coverage is applied once through the
coverage texture. CPU tracking regressions qualify these issued graph/workload
relationships only; native image comparison is still separately required.

The prepared sampler key preserves independent minification, magnification and
mipmap selection (twelve closed combinations prepared once). Current source
realizations have one mip level; native nearest/linear mip selection retains its
well-defined single-level behavior. It is not a reason to reject valid source
metadata. Multi-level image realization remains governed by the existing resource
admission boundary. Coverage surfaces exactly match the output extent and use an
explicit corresponding-texel load, independent of a source image's sampler. The
portable shader source revision advances to 3 for this sampling change; the
parameter ABI remains exactly 64 bytes and the two texture bindings are unchanged.

An accepted upload is bound to the exact retained geometry arena-slot issuance,
not merely its view and snapshot revision. Copies of that same immutable plan
remain valid; an independent plan from the same snapshot does not authorize the
accepted draw buffers. Each reserved candidate also copies bounded immutable
image/font realization evidence before upload. Submit requires the same graph
texture/native generation and source sharing evidence, and reruns the canonical
submission-retention validator before entering ComposeUi. For used fonts the
sealed atlas must own the accepted realization's actual PageLease and the same
frame/revision; equal numeric IDs from a foreign atlas cannot substitute ownership.
No new global registry, caller nonce or mutable source authority is introduced.
An atlas supplied with zero font bindings contributes no encoded texture or glyph
identity. Its ordinary canonical capture/lifetime rules still apply; no new atlas
observation API or ownership authority is needed for that unused input.

## Application source producer and retirement port

The non-installed `HoroRuntimeUiPaintHost` application target registers one fixed
participant before the editor consumer. Its `RuntimeUiPaintPort` pins revocable
service storage, never a host-wrapper pointer. Admission, reload and collection
run on that same owner thread. Shutdown revokes new calls and clears the borrowed
frontend pointer. The editor presentation composition retains its port while the
frontend polls native completion, then explicitly collects the stopped derived
owner before releasing the port and native renderer parents. A surviving external
port must be released on the owner thread after successful collection; shared
storage alone does not prove native retirement.

The source declaration carries only actual text/image/resource component inputs,
mask membership and potential isolation scopes keyed by admitted element IDs.
Structural element order, layout, visibility and fills come from the actual
catalog projections and the animation owner's computed style/layout/clipping
publication. `ProjectUiThemeSurface` projects that publication without another
cascade or theme runtime. Reload reserves the replacement declaration and arena
before the existing coordinated source/derived commit; failure keeps the original
source, client grants and prepared program. Structural program preparation occurs
at the next explicit owner safe point, after an actual new extraction exists.

`UiAnimationRuntimeParticipant::CanReclaim` is an additive owner-thread observation
of its existing stopped-owner fence. Callers first stop the participant, release
CPU observations, drain existing retirement work, and only destroy it after this
observation succeeds. It performs no hidden collection or reclamation.

The owning application and actual SDK-host regressions exercise this producer;
CPU program tests alone do not qualify its native pixel output. OpenGL/Vulkan
can explicitly deny this optional capability while ordinary GUI preview remains
available. Selected-Metal native effects and lifecycle still require native proof.

## Output evidence handoff

`RuntimeUiPaintPort::UpdateOutput` accepts committed physical output, independent
per-window display-scale evidence, and physical safe-area insets. The editor
samples its existing surface owner at the pacing boundary. SDL safe bounds are
converted from the same logical window to that committed physical extent; this
conversion is not used as a substitute for SDL's independent display scale.
NoSource does not query or require optional native paint resources.

The port validates the actual admitted canvas policy and coalesces one latest
observation. A change immediately fences old extraction/input. At the next owner
command cutoff, the existing canvas resolver supplies logical content/font scale
and the existing animation participant receives the new viewport revision. An
actual extraction then feeds bounded program refresh at the following owner safe
point. No new layout owner, per-frame graph compilation or GPU wait is introduced.
Only the exact new encoded frame's successful native Present receipt restores
input. Invalid evidence preserves the prior policy; a superseded receipt cannot
restore eligibility. Owning application regressions exercise resize, independent
DPI and inset changes, malformed evidence, coalescing and the old-receipt fence.

OpenGL/Vulkan may explicitly deny the optional UiPaint capability while retaining
ordinary editor GUI/preview startup. This is a declared fail-closed support matrix,
not another renderer fallback, ignored effect or native pixel qualification. The
selected Metal path still requires actual effect/readback and lifecycle evidence.

## Private native qualification inputs

The existing `HoroMetalGraphExecutionSmoke` target consumes a private offline
fixture; it does not create or qualify a production signed host catalog. The
producer uses the checkout's canonical `ui_paint.hlsl` and locked Linux tools to
validate SPIR-V and generate MSL. Its finite data-only manifest binds the same
workflow run/revision, exact source and lock bytes, effective options, entry/stage
identities, binary/archive digests, generated byte hashes and the actual reflected
64-byte uniform layout. Includes currently fail closed because this shader is
self-contained; adding includes requires binding their complete byte closure.

The Apple consumer checks the entire handoff before invoking installed offline
`metal`/`metallib`. It records those actual executable hashes and SDK evidence.
The fixture packages the resulting libraries with `PackageShaderArtifactStages`,
checks their hashes, and uses unchanged `UiPaintNativePayload` and native pipeline
reflection. Its pixel assertions read completed native texture copies, including
rounded edges, transformed masks, fractional isolated/nested opacity, intersecting
coverage, sibling independence and publication replacement. These tests do not
substitute a CPU image or claim an application Present from a readback completion.

Three scalar reserved fields retain offsets 20/24/28 under the production route's
strict OpenGL layout option. A `uint3` there would require 16-byte alignment under
that option and conflict with the fixed C++ ABI. Qualification requires actual
compiler member offsets; neither expected metadata nor a successful package parse
can stand in for that evidence. Missing tools or devices leave native qualification
incomplete. A private fixture never permits an unsigned production host catalog.

## Scene publication candidate migration

The application-owned service remains the only semantic registry and authored
`UiHotReload` publisher. The existing generic Scene activation participant does
not gain a UI, renderer or platform dependency. Scene-owned registrations prepare
against its exact reserved `SceneRuntimeId`; they never retain component-pool pointers.

`UiSceneTransitionRequest::previous` is now optional: absence means a real initial
activation, while an absent `next` means unload. Existing replacements continue to
supply their exact outgoing identity. An empty pair, invalid identity or duplicate
initial Scene admission fails; no fabricated outgoing identity is needed. This is
an in-process host API change, not a serialized source format change.

`RuntimeUiService::PrepareAnimation` can attach to an incoming publisher or provider
replacement inside the same private `UiSceneReconciliation::Prepared` candidate.
The derived owner retains an exact generation lease and the shared service state,
not a facade or component pointer. Its candidate identity is weak: abandonment
revokes mutation, even if a lease keeps bytes readable. Publication promotes that
same publisher and reservation without a second tree or postcommit acquisition.

The coordinated transition overload resolves every requested instance to its actual
live derived reservation and exact outgoing semantic/provider scope. A persistent
provider rebind must also have its replacement derived owner privately prepared;
it cannot silently preserve an old derived frame over a new source generation.
Game, player, viewport and other Scene scopes remain owned by the same service.
Only the matching outgoing scope is retired.

`PreparePublication` allocates a one-use transaction before aggregate validation.
Its owner-thread `Seal` revalidates source/Scene stamps and temporarily fences all
conflicting service/registry commands. Abandoning it restores admission without
publishing. The generic Scene service retains its existing final commit claim as
the sole linearization point. Only after that claim may `Publish()` perform the
already-reserved, noexcept source/derived promotion, with no allocation, source
acquisition, cancellation check or foreign callback. Late cancellation cannot split
that aggregate. Native completion remains separate from successful publication.

Shutdown immediately revokes callers and defers canonical teardown while a sealed
transaction pins the service. The owner consumes or abandons that token before
collection. Old generation and GPU leases still fence reclamation. Prepared
transaction storage is retired at explicit application quiescence; publishing does
not execute cleanup callbacks or destroy a retained old renderer/source owner.
The new initial/private-candidate/seal/rollback/shutdown regressions and the owning
public consumers qualify this contract separately from native effect execution.

### Private source baseline and complete canvas batches

Scene activation must finish native program/resource preparation before the generic Scene publication claim. A newly composed
`UiAnimationRuntimeParticipant::PrepareSourcePresentation` performs the same canonical computed style/layout/clipping transaction
at its explicit load-time boundary, before Startup and before any command or timeline. It returns only immutable baseline values;
clock sequence, source frame, elapsed time and host dispatch cursors remain zero. The participant's ordinary `Acquire` and
`ApplyPresentation` stay closed until real VariableUpdate/RenderExtraction succeeds. No host dispatch evidence is manufactured.
The retained baseline pins its exact source generation and remains readable after candidate abandonment, without preserving mutation
or input authority.

The application projection extracts potential paint roles through `UiRenderExtractor::ExtractPrototype`. Its immutable issuance is
marked preparation-only, including retained copies. `UiPaintProgram::Create` may consume these potential roles, while the geometry
arena rejects them, so a baseline cannot be uploaded or submitted as a normal frame. Actual source extraction continues through
`Extract` after the canonical scheduler publication. This is an additive host preparation route, not a second style/layout owner.

A Scene containing several authored canvases supplies the complete ordered inventory to `UiPaintProgram::SubmitBatch`. The former
single-program Submit delegates to a batch of one. The named frontend UI phase validates every source/slot/binding and all bounded
pass/draw/resource totals before the first native encoding. It admits no generic Execute reopening. A malformed later canvas cannot
encode an earlier prefix. Reentrant UI and GUI composition is fenced until the whole ordered phase finishes. A later native failure
aborts presentation, but all already queued source/image/font/atlas/kernel/transient generations remain attached to the same native
completion lease as Scene and GUI.

`RenderResourceRetirementLimits::maximumUiCapturesPerFrame` is the explicit product bound (default 8, hard limit 64). Each of the
existing finite completion slots reserves its source-retention inventory once at frontend construction. Frame work cannot grow it;
capacity failure rejects the complete composition before encoding. This does not advertise a native capability on an unsupported
backend or qualify pixels, compiler artifacts, presentation, or device restart.

## Transactional normal Scene retirement

`SceneActivationParticipant` contract revision 3 adds optional
`PrepareRetirement(RuntimeSceneView)` for owners whose normal unload requires a
fallible detached transaction. Rebuild every host participant and consumer against
the new virtual contract. Existing owners inherit a no-op result; no second Scene
or UI publication authority is introduced.

`RuntimeSceneService` prepares every retirement candidate against the actual active
Scene, validates all candidates, then invokes their allocation-free, noexcept
`Publish` operations before shutting down the old activation roots or releasing
Scene storage. Preparation, cancellation and validation failures abandon only the
new candidates and remain observable through `TakeOperationError`; the active
Scene and its owners remain unchanged. Reentrant queue admission is closed across
the transaction. Deferred shutdown before publication abandons preparation and
then uses the existing forced-shutdown path.

The application UI adapter must prepare removal of only the outgoing Scene scope;
game-instance and player-owned UI survive. Its activation candidate still owns
forced revocation and GPU/snapshot draining: `Shutdown` never calls this fallible
retirement hook. Borrowed component storage cannot be retained by a candidate.

## Same-boundary compiler reflection output

The application compiler used to discard final-artifact reflection before returning
`ShaderCompilationBatch`. The host could therefore not supply a strict paint kernel
without downcasting the selected compiler adapter or authoring a second native map.
`ShaderCompilationRequest::reflection` now explicitly supplies the admitted Cross
identity and authored logical names. The same adapter operation returns one immutable
`CompiledShaderArtifact::reflection` record, binding actual payload bytes, compilation
key, reflection tool and normalized layout/map into the existing cooked identity.
`ShaderBuildService` retains this record with its ordinary atomic batch result.

Unreflected requests preserve their previous artifact keys and optional absence;
callers requiring a reflected result must request it and reject missing metadata.
Reflection inputs participate in the artifact key. Existing Vulkan cook normalization
is reused; no feature-level adapter cast or parallel reflector is introduced. The
closed Metal UI route additionally validates actual Cross JSON, every fixed member
and stage interface, and the generated MSL constant struct and resource attributes.
Only the actual translated native placements are published. Native Metal pipeline
reflection independently checks the resource stages and all ten 64-byte block members.

`CookedUiPaintKernel` consumes this exact artifact-owned record. Its old separately
supplied `reflection` candidate is removed; hosts and private qualification fixtures
must migrate to the same cook output. Missing, mismatched, malformed or unsupported
route evidence fails with a typed error. No signed catalog trust, tool installation,
backend discovery or runtime shader compilation is added by this migration. Scripted
normalization tests qualify rejection/ownership only; real locked-tool translation,
Apple offline compilation and native pixels remain separate required evidence.

## Host automatic layout bindings

Existing explicit `UiAnimationLayoutBinding` values keep `AllComputedValues` by
default. A host automatically binding the core width/height/padding vocabulary uses
`ExplicitOriginsOnly`: only `RegisteredDefault` is skipped, every explicit zero,
class/state/accessibility/animation value remains authoritative. The owner starts
from the authored/responsive layout on every update, so removal of an override
restores percentages and authored padding rather than retaining previous pixels.
The closed `Padding` field maps the validated Dimension to all four edges and
rejects negative padding. No timeline sample bypasses the sole computed-style owner.

The existing reflected-record canonical codec and identity calculation now belong
to RenderApi as pure portable value operations, with Foundation JSON implementation
private. Private tool normalization depends on that value owner; runtime admission
never depends on compiler/process targets. `ValidateReflectedShaderArtifact`
recomputes the same existing cooked identity directly and binds exact payload,
compiled key, backend and payload format. A structurally valid alternate binding
map with a stale cooked key is rejected by both pipeline and strict kernel
admission. Canonical encoding/hash bytes are unchanged; no second hash scheme or
encode/decode admission round trip is introduced.

### Project Scene composition and retirement

`HoroEditorApp` now creates the application-owned UI service/port from the same
RuntimeHost dispatch source before preparing project preview resources or starting
ScreenHost. The actual `EditorProjectScenePolicy::uiSources` is captured by the
existing Play and preview factories. `ProjectPlaySceneHost` constructs its UI
loader over the same committed cook envelopes and registry as its other project
assets, and registers the generic UI activation/retirement participant last.
Scene retains its sole publication claim. No component-pool pointer, second UI
service, backend discovery, default document or mutable document copy is retained.

The application source factory enumerates every authored `uiCanvas` component in
Scene order, with eight submitted sources and sixteen retained retirement slots.
The complete batch (including an independently attached manual source) is checked
against the frontend's configured capture capacity. Exact prepared graph counts
and potential frame draw capacities are also checked before the Scene claim.
All private canonical source generations, derived baselines, shader pipelines,
queued buffers and attachments must finish preparation and report readiness before
publication. Publication only moves already-owned bounded entries. Failure leaves
the old Scene, UI grants and frame owners unchanged. Old candidate shutdown revokes
only its exact SceneRuntimeId, so replacing the same logical Scene cannot detach
the promoted generation. Retained canonical/native leases drain through the same
application port and renderer completion owner before storage is released.

The source path projects authored structural elements through the canonical core
catalog, computed style and responsive layout owners. `RegisteredDefault` sizing
never overrides authored responsive dimensions; explicit zero and padding retain
their normal meaning. No durable clip/transform/group schema is added: absent
runtime policies remain Visible, identity and ungrouped. Required text, image or
control components without their actual resource/intrinsic owner are rejected with
a typed unsupported/intrinsic failure; they are never rendered as substitute solid
rectangles. This boundary is an explicit remaining component integration limit.

Normal `apps/HoroEditor/main.cpp` calls `RunEditorGuiApp` without a compiler or
catalog. Its application composition now selects `RuntimeUiPaintPackagedAssets`
from the explicit `EditorAssetRoot()` only for the already selected Metal backend.
A real authored Scene canvas requests the fixed pair
`shaders/runtime/ui-paint-metal.horoshdr` and
`shaders/runtime/ui-paint-metal.reflection.json` during candidate preparation.
NoSource and ordinary GUI/document preview startup need neither file. Unsupported
paint backends remain a typed capability denial, without selecting another backend.

`HoroUiPaintShaderPackage` is a noninstalled, RenderApi-only value owner shared by
runtime loading and offline production. It admits the exact Metal profile through
the single canonical reflected-record decoder, recomputed cooked identity and
strict `ValidateCookedUiPaintKernel`; Metal independently checks native reflection.
The loader bounds both reads before allocation and rejects missing, nonregular,
symlinked, escaping, truncated, oversized or mismatched pairs. The application
keeps its explicit asset root quiescent during preparation. Only a completely
admitted immutable kernel enters the source policy cache; a failed attempt cannot
publish Scene/UI state. Loaded bytes own their lifetime across source reload and
queued renderer retirement, with no frame-hot file access or compiler processes.
An explicitly injected admitted compiler/target remains an alternative host policy
using the same ShaderBuildService and reflected record, rather than another source
publisher.

### Authenticated offline Metal package production

`HoroEditorUiPaintShaderCook` is an explicit offline application executable. It
accepts absolute source, new package root, scratch, catalog, detached signature,
independent host trust, DXC, validator, Cross and xcrun paths, in that order. The
host exclusively owns the output parent; an existing destination is rejected.
All output is written into one bounded, exclusively claimed private directory and
published by one directory rename after complete pair validation. Failures remove
only that invocation's claimed directory. Runtime Editor code never invokes this
executable or links its process/security/compiler host target.

`HoroEditorShaderCookHostInternal` authenticates at most 64 KiB of immutable catalog
bytes with the existing `HoroSecurity::ArtifactVerifier` over independently supplied
host-approved roots **before** parsing tool approvals. The shared seeded
`HoroHostSignatureDocuments` decoder parses the trust/envelope documents and grants
no trust itself. A catalog cannot supply its own roots. Schema 1 has exactly
`version`, `host`, and `tools`; each tool has exactly `role`, `release`,
`archive_sha256`, and `executable_sha256`. Both digest fields use the existing
Foundation canonical text form, `sha256:` followed by 64 lowercase hex digits.
Duplicate/unknown fields, duplicate roles,
zero digests, wrong actual host and excessive bounds fail closed. The existing
external compiler adapter independently hashes each selected absolute executable
and matches the complete authenticated host/role/release/archive/executable tuple.
No catalog field becomes a path, shell fragment, or arbitrary process option.

The producer uses the same `CompileShaderTargets` Metal route and explicit Cross
logical names for the canonical `ui_paint.hlsl`. Its immutable same-cook reflection
record is encoded by the sole RenderApi codec; runtime admission does not fabricate
reflection or approve tools. ECDSA test roots and metadata-only package fixtures
are regression inputs, never a production installation approval or native shader
qualification. A reviewed signed Apple installation snapshot, actual approved
tools, real offline compilation, complete application linking and selected-Metal
pixel/presentation/reload/drain evidence remain release qualification requirements;
they have not been established by CPU package tests or fixture discovery.

Metal advertises paint only when its implemented upload/encoding path and queried
primary, linear offscreen and stencil format profile are all available. The two
additional one-pixel format probes run during cold device discovery and retain no
resource. Format admission is not shader, pixel or application qualification.
OpenGL/Vulkan keep explicit capability-denied paint behavior while their ordinary
editor GUI/document preview paths remain available.

### Unassigned authored style namespaces

Canonical document elements may omit `styleAsset` while retaining typed inline
values. A property-only `RuntimeStyleRegistry` now accepts a nonempty validated
property schema with no authored namespaces. The same resolver supplies registered
defaults, permitted inheritance, typed inline values and host presentation policy;
no synthetic asset, font, image or theme is created. Independently qualified class
and token references still resolve only through actual admitted namespaces.

`UiStyleElementInput::asset` and `UiAnimationElementDefinition::asset` use their
existing unassigned identity to select no base asset chain. Explicit unknown asset,
class and token references remain errors. `HasAsset({})` remains false. This aligns
runtime registry/resolver/animation admission with `ProjectUiElementStyle` and
ADR-076 precedence; signatures and authored/cooked formats are unchanged. Existing
callers with assigned assets retain their current cascade and validation. The real
project Scene loader uses this path for default/inline-only canonical documents.

### Animated extraction retention

Real animation callers pass the exact committed `UiAnimationFrameLease` to the
four-argument `UiRenderExtractor::Extract`. The extractor privately checks the
retained tree issuance as well as layout lineage; equal numeric revisions from a
different publisher do not authorize extraction. Static extraction and load-time
prototype preparation retain their original contracts. The application projection
uses this route, and the public integration consumer checks its typed signature.

Snapshot storage pins that frame, geometry pins the snapshot, and the existing
native completion capture pins geometry. Actual last-reader release reserves the
pool slot while clearing its upstream pin before making the slot reusable. Pools
may remain alive after completion without retaining a retired source. Failed
geometry preparation clears its new source pin before returning the slot. No new
renderer lifetime authority or frame allocation is introduced.

The prepared program copies bounded structural command/mask/group/resource values
at cold creation, including safe-point refresh. It does not retain the real
animation publication used to derive those values. Successful slot replacement
releases the superseded CPU geometry and binding evidence; failed or cancelled
preparation releases only its unpublished candidate. The accepted revision and
all independently captured native completion leases retain their original exact
geometry/binding checks and lifetimes. No frame or resource capacity increases
are needed to retire these obsolete cache references.

### Cold project preparation precedes native admission

The application keeps GUI CPU preparation and `ImGui::NewFrame` before screen
updates, then draws/extracts the screen before opening the one canonical native
frame. Both the synchronous Play command from `Draw` and later asynchronous
Scene preparation pumps can therefore admit their real UI resources at a
structural safe point. The preceding viewport request is prepared before GUI CPU
Begin/Draw: publishing a resized viewport after Draw could release the image view
already captured by ImGui. Newly activated viewport mesh generations remain
ineligible until the next prepared frame, using the existing generation guard.
Native execution still orders Scene, UI, GUI and Present
within that same scope/completion lease. An unavailable drawable discards CPU draw
data without manufacturing a native frame, input eligibility or Present receipt.
An open CPU GUI frame is balanced explicitly during abort/shutdown.

The non-installed `IEditorGuiRenderer::BeginFrame` contract now prepares CPU GUI
state independently of a native frame. OpenGL restores its own existing context
on the creating thread before any stock lazy device/font operations; this does
not select a backend or issue a render frame. Metal prepares its existing device
and font objects after application font setup and checks the retained font
identity. Its stock `ImGui_ImplMetal_NewFrame` receives only the actual active
render-pass descriptor immediately before native draw encoding. Vulkan retains
its CPU-only preparation. No synthetic render-pass descriptor or second frame
scheduler is introduced. The real surface-lifecycle qualification additionally
observes completed GUI CPU extraction before native admission and rejects repeat
phase admission.

### Scene view retirement observation

`RuntimeSceneView` now carries a weak cold-owner observation and copied identity
values. Its public value layout changes; rebuild Scene consumers and SDK modules
together. `View`, `IsCurrent` and storage queries allocate nothing, reject foreign
owner-thread storage access and never dereference an expired Scene. Identity
queries preserve the captured values after retirement. A view still does not pin
Scene/component storage or authorize a stale entity; callers must keep returned
component borrows within the current owner-thread phase.

The direct `RuntimeScene` constructor is no longer `noexcept`: allocating its one
cold liveness observation can report `std::bad_alloc`, as other cold Scene
construction allocations already can. The two actual factory callers,
`RuntimeScene::CreateResolved` and `RuntimeSceneService::CloneActive`, translate
owner/lifetime allocation failure to the existing typed invalid-candidate error. Public
consumer compilation and destruction/address-reuse regressions cover migration.
This corrects retained-view checks after replacement/unload without extending
Scene storage or GPU resource lifetimes.

The Unix native CI build presets explicitly retain `all` and add the actual
`HoroEditorUiPaintShaderCook` target, which remains excluded from ordinary default
builds. Sonar's instrumented build explicitly includes the same target alongside
the Vulkan cook. Their registered CLI contract therefore runs the built canonical
producer, rather than failing because only its test registration was available.

## Native SDK22 Scene-view migration

The Scene liveness repair changes `RuntimeScene`/`RuntimeSceneView` C++ layouts.
Gameplay hosts, native modules, generated bundles and public consumers must rebuild
against the paired boundary22 / `gameplay-sdk22` fingerprint. The canonical loader
rejects SDK21 before activation; a stale native module cannot acquire or consume
these new value layouts. Cached view identity remains readable after retirement,
but storage queries require the original live Scene and owner thread.

## Sonar remediation and owner-thread retirement

UI pool/program destruction now requests retirement through a cold-admitted token
retained by the exact renderer registry generations. The atomic request may be set
from a foreign thread; it never carries a frontend pointer across threads. Native
retirement, pending-upload cancellation and reservation release remain bounded by
the existing frontend owner boundary and completion pins. The destructor path
cannot allocate an error diagnostic, throw, wait for the GPU or abandon a native
owner. Explicit Shutdown retains its typed failure/retry contract.

Scene activation preparation/retirement, Play-world commit validation and structural
preparation/baseline validation now borrow
`const RuntimeSceneView&` for the synchronous call instead of copying the view.
The host-only activation and structural participant contract revisions advance
from 3 to 4. Update overrides to the same signature and rebuild C++
Scene/Physics/UI consumers.
The Scene remains the canonical owner. The reference is synchronous; candidates
that need the view after preparation retain their own value as before.
Coordinated Runtime UI reload, transition/animation preparation, exact Scene
revocation and prepared reconciliation helpers use const access where only their
retained canonical owners are mutated. A const service handle does not make that
canonical state immutable; all existing owner-thread, publication and capability
fences still apply. Update exact C++ member/function pointer signatures as well. Installed header ownership,
the native module C ABI, serialized assets and wire formats are unchanged.

Factory-only shader catalogs, paint hosts, paint programs, project source factories
and paint projections use non-aggregate
private construction keys with standard make_shared/make_unique ownership. A caller
cannot construct a catalog without the factory's authenticated evidence or bypass
paint admission with an independently forged key.

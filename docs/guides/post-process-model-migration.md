# Post-process graph and volume model

`HoroEngine::RenderApi` owns `PostProcessSettings`, `PostProcessVolumeSnapshot`
and `PostProcessGraphPlan`. Include the corresponding target-owned headers; no
renderer frontend, platform or concrete backend dependency is required.

Scene extraction provides world-space AABBs, spheres or global bounds, stable
volume IDs, profile IDs and explicit group overrides. `PreparePostProcessVolumes`
copies all inputs and resolves profiles into immutable owned overrides. A profile
is a complete settings value, so its disabled groups override the base settings;
explicit volume overrides take precedence. A volume without a profile inherits
unspecified base groups. `Inherit`, `Disable` and `Replace` remain distinct.

Volumes apply in ascending finite priority, then stable ID order. Higher priority
therefore applies last. Bounds include their surface. Outside a finite bound,
positive blend radius uses a smoothstep fade; radius zero is a hard cut. Weight
multiplies spatial influence. Numeric parameters interpolate convexly when an
effect is already enabled. Presence, integer sample counts, AO algorithm and grain
seed switch at influence 0.5; absent groups are not evaluated or secretly enabled
with guessed neutral parameters. These rules are deterministic, including ties.

Snapshot evaluation copies fixed-size settings and iterates at most 256 volumes
without allocating on success. Preparation is synchronous authoring/load work on
any thread, bounded to 64 profiles and 256 volumes. Host publication replaces an
immutable snapshot at a render safe point. Readers retain their previous value;
discarding a failed/cancelled candidate needs no jobs, locks, backend work or GPU
wait. Public immutable values may be shared with externally managed lifetimes.

Graph preparation accepts resolved settings, their generation, the existing
`TemporalHistoryCompatibility`, exact sampled Horo texture generations, the
exposure owner's buffer/generation and immutable capability facts. Scene/history
color must be canonical ACEScg RGBA16F, unexposed or explicitly pre-exposed with
matching metadata. Auxiliary inputs are data. Every supplied input must have the
same complete compatibility identity and render extent; stale history must be
reset/rescaled by its existing owner before admission. Missing depth, normal,
roughness, velocity, history or exposure produces a typed error. Lens parameters
are authored in the DoF settings and projection identity comes from compatibility.

AO exports independent visibility for host lighting/composition integration.
Other enabled effects follow the authored permutation and preserve the source
color/exposure identity. No-effect settings produce a typed pass-through candidate
with no graph work; consume `SourceSceneColor()` in that case. Graph-backed plans
export `SceneColor()` and optional `AmbientVisibility()`; `Input()` maps semantic
roles to exact imported graph resources. Owned pass metadata retains recipes,
descriptors and representations. Imported GPU generations remain borrowed and
must retire through their existing resource owner after in-flight use completes.

Recipes are explicitly ordered cooked-variant preferences per effect, never
backend families or magic profile labels. Hosts must supply variants cooked for
the exact effect/settings semantic contract. Admission validates capabilities,
queues, output formats, bounded pixel/sample work and cumulative reservation
budgets. The graph supports at most 8 effects, 32 preferences, 6 texture inputs
and a 16384-pixel maximum per dimension. Reservation bytes must cover at least
the output image and declare all recipe intermediates. A supported lower authored
preference is selected explicitly; otherwise the complete candidate fails.
Admission is logical planning, not GPU allocation or proof of native execution.
Existing resource admission/execution owners realize and retire admitted plans.

The implementation adds new public contracts without removing existing APIs.
Current viewport output remains unchanged until its host explicitly adopts this
model. Existing unqualified exposure/color fields must migrate through the
versioned ADR-037 adapter delivered with RND-013.3; this model holds desired
compensation and grading parameters only. It does not publish exposure, cook or
execute ACES transforms, choose a tone curve/HDR target, or implement individual
GPU effects. Accessibility composition remains owned after display/UI composition.

Regression coverage lives in `HoroRenderGraphTests` and the isolated
`HoroRenderApiPublicHeaderConsumer`. It validates volume bounds/overrides/ownership,
graph dependencies, unsupported/missing/stale inputs, explicit preferences,
budget rejection and retention of prior immutable generations. Native visual
qualification remains separate from model tests.

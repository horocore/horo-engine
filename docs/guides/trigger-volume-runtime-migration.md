# Trigger volume runtime binding migration

HORO-874 / #874 / [PHY-004.8] completes the runtime handoff left by HORO-892's
shape-only schema normalization. Previously conversion manufactured profile and
material identities without a project authority or a declared asset dependency;
the resulting sensor metadata could not realize a normal asset-backed scene.

## Host API migration

Capture `SceneSource::TriggerVolumeConversionContext` from the project's selected
immutable collision and material authorities. Its portable binding names the exact
sensor profile, normalized schema semantic fingerprint, material asset/type,
authored material revision and SHA-256 of the cooked material payload bytes (not
the artifact envelope). The material type is `core.physics_material`. Capture the
current nonzero filter generation separately in the context; it fences live
operation completion and is never persisted.

The production producer is
`Application::CaptureProjectTriggerVolumeBinding` in the explicit
`HoroEngine::PhysicsTriggerVolumeHost` composition target. It requires an installed
`PhysicsWorldSimulationBinding`, the chosen profile, validated
`PhysicsMaterialAsset`, and the actual `AssetCookArtifact`. It checks sensor/event
eligibility, matching material identity/type, nonempty payload and recomputed digest.
The supplied artifact must correspond to the registered source and requested cook
target. `CookScenesWithProjectTriggerVolumes` performs this capture and passes it
to the ordinary transactional cook host. The host validates both real source digest
and actual cooked/cache payload before publication. This is the headless host's
public configuration path, with no fallback IDs or ambient Project discovery.

On load, the same normalized Project schema is injected explicitly through
`PhysicsSceneActivationSettings::simulation`; its fresh generation is checked
against activation authority. The durable semantic fingerprint must agree, and the
actual material bytes must match the persisted payload digest. The solver remains
selected by the caller's canonical headless composition, never by the binding API.

Pass the context to every `ConvertSceneSourceToRuntime` overload, retained/cached
prefab conversion, `ScenePrefabExpansionRequest`, editor conversion adapters and
`Application::PrefabSceneCookRequest`. Editor hosts inject the capture through
`EditorWorkspaceDependencies`; direct Play conversion accepts the same explicit
context. No conversion function discovers services, selects a solver or loads a
material. Existing callers without enabled legacy triggers may omit the context.

An enabled legacy trigger without a binding returns
`scene_conversion.trigger_volume_project_binding_missing`. Invalid shape and
legacy/canonical conflict diagnostics retain precedence. Disabled valid legacy
triggers still contribute no Physics producer and need no binding. Scene source
persistence keeps its existing version and original trigger field unchanged.

The scene builder adds the material requirement with `RequireAsset` and retains
`TriggerVolumeMigrationEvidence` alongside canonical producers. Native Physics
activation checks installed schema semantics, selected profile/layer sensor
eligibility, lifecycle-event participation and the digest of the actual immutable
material payload before allocating a native world. Unknown profiles, absent or
wrong material bytes and stale schemas reject the candidate; lookup never becomes
optional and the previously published world remains authoritative.

## Cooked and cell payload migration

Cooked Scene format is now `horo.scene.runtime.v2`. All payloads include the
required `triggerMigration` array. Decode rejects previous v1 data, malformed or
duplicate proof and proof that disagrees with canonical producers. Recook existing
packages from their lossless authored scene documents. No runtime generation is
encoded. A fresh process may bind the same semantic schema to its own world epoch.

The application cook host carries real dependencies through its existing complete
asset closure and preserves evidence while rebuilding a definition. Cell producers
must pass `definition.TriggerVolumeEvidence()` in
`SceneCellPayloadSource::triggerVolumeEvidence`. Cell baselines account for its
owned storage; layer selection retains proof only for included trigger objects.
CoreEcs binary payload version 2 requires the same proof; WorldCook uses the shared
`SceneCellPayloadCodecVersion` authority. Prior CoreEcs v1 content also requires
recook. Cell cook key v2 hashes portable evidence in object order. Conflicting schema or
material commitments cannot be resolved by insertion order or a maximum epoch.

## Ownership and qualification

The existing canonical body/collider and Physics activation participant own native
sensor lifetime, event reduction, rollback and retirement. Reload creates a fresh
world and invalidates old handles and publications; overlap continuity applies
within each exact world generation. Consumers must never synthesize an exit using
a retired world's handles. The editor's explicit Null composition is unchanged.

Parent translation and rotation flow through the ordinary checked Physics plan.
Non-unit body scale and unsupported shear retain the existing explicit unsupported
policy; migration does not approximate geometry or silently relax canonical body
constraints. Native tests cover authored conversion, cooked round trip, actual
asset-backed sensor activation, enter/exit without response, edit/rebuild rollback,
disable, stale identity, foreign-thread rejection and shutdown. Codec, cell,
worker-freshness and public-header consumers protect the remaining boundaries.

`RuntimeComponentSet` and GameModule SDK layout 21 are unchanged. The new portable
proof belongs to scene definitions and source/cook APIs; existing compiled users
of those APIs must rebuild and source callers must supply project bindings when
they author enabled legacy triggers.

## Acceptance evidence plan

| Requirement | Production owner | Regression owner |
|---|---|---|
| Real project binding producer | `PhysicsTriggerVolumeHost` capture and required-context cook entry | `PhysicsTriggerVolumeHostTests`, public host consumer |
| Actual registered material closure and precommit digest check | `PrefabSceneCookHost` / `HostKeyStrategy` | Real-file cook publication and preserved-generation failures |
| Authored trigger to native sensor | Shared Scene converter, cooked v2 codec, existing Physics participant | `PhysicsTriggerVolumeRuntimeTests` using public capture and actual asset-backed canonical world |
| Begin/end without physical response | Existing canonical solver sensor and event projector | Exact native enter/exit counts and moving-body readback |
| Edit/rebuild/disable/rollback/stale handles | Existing play-world detached preparation and safe-point commit | Replacement, wrong-material rollback, shape/parent translation, disabled and stop cases |
| Portable proof cannot be lost or forged by malformed wire | Scene builder and cooked v2 codec | Missing/duplicate/wrong producer/digest/schema/old-version tests |
| Bounded cell and worker freshness | Cell payload/layer projection and exact operation capture | Cell storage ceiling and changed-capture completion tests |

These are authored regression requirements until executed on the final frozen source.
No local or hosted success is implied by this matrix.

## Canonical Physics material payload

Previously `PhysicsMaterialAsset` supplied validated values without a source/cooked
codec. The owning Physics material contract now provides one fixed 56-byte
schema-1 representation for both source and cooked material payload. Its 16-byte
header is `HOMATERL`, little-endian format 1 and descriptor schema 1. It then stores
16 identity bytes, an eight-byte authored revision, three four-byte IEEE float
fields (friction, restitution, density), two one-byte combine modes and two zero
reserved bytes. All scalar words are little endian; signed zero is canonicalized.
There are no duplicate keyed fields or unbounded arrays. Extra/truncated fields,
nonfinite coefficients, unsupported versions/modes and reserved bytes reject.

Author tools use `EncodePhysicsMaterialAsset` on the validated immutable value.
Hosts register `MakePhysicsMaterialCooker()` for `core.physics_material` on their
selected target. It decodes actual registered source bytes, verifies source asset
identity, and emits that same canonical representation. Capture decodes the actual
artifact and compares the full descriptor with the immutable supplied snapshot;
equal material IDs/revisions alone cannot hide different coefficients. Actual
source-byte freshness is attributed to the subsequent cook strategy, not to
capture. Native migration admission validates the parsed ID/revision and exact
digest before allocating a world.

Mandatory parsing is scoped to legacy-trigger migration proof. Existing ordinary
canonical collider material paths retain their established contract and are not
silently reinterpreted by this migration. Old ad hoc trigger material bytes have
no compatibility fallback: encode/cook the selected real project material first.

## Host structural participant contract migration

`SceneStructuralParticipantContractVersion` is now 2. The new virtual
`SceneStructuralParticipant::ValidateBaselines` receives every exact attached or
replacement baseline and its immutable resource leases for one transaction. Scene
calls all owners' prevalidation before any owner `Prepare`; the existing single
candidate per owner and atomic publication/rollback remain unchanged. No first/last
baseline shortcut or per-baseline publication is permitted.

ALL external host structural-participant implementations and binaries must rebuild
against this vtable/header contract. This is a host ABI break even for implementations
that use the default method. Non-Physics owners may use the no-op default. A Physics
owner must implement proof validation: the default fails closed when any baseline
requires trigger migration proof. Rebuild host header-contract fingerprints together
with those implementations; an old structural-owner binary is not compatible.

The supported GameModule exports remain SDK21: the minimal package exports only
Foundation/NetworkApi/SaveApi/GameplayApi; Physics/NAV/AI SDK component exports do not
export RuntimeScene or PhysicsSceneIntegration. GameModule lifecycle contexts and
exported descriptor/factory bundles do not pass a structural participant.
`BehaviorRuntime::MakeStructuralParticipant` belongs to the host GameplayRuntime
target, which is not exported by the supported GameModule SDK. The new host contract
does not claim old external host binary compatibility merely because SDK21 remains.

The native multi-baseline regression registers a second host owner before Physics,
then mixes an ordinary group with valid/invalid baseline inputs in both orders.
Both owners' preparation counters remain zero on rejection. Valid batches still
prepare once per owner. A later mixed replacement failure retains both old baseline
revisions, the Physics world and exact old body handle.

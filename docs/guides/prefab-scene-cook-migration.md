# Prefab Scene Cook Contract Migration

Delivery identity: HORO-1067 / #1067 / [PFB-007.2].

## Header And Target Ownership

`Horo/Scene/SceneRuntimeConversion.h` belongs to
`HoroEngine::SceneRuntimeConversion`. Headless callers pass `SceneSourceView`,
a stable scene ID, a captured content revision and the immutable Prefab resolver.
The existing Editor `ConvertSceneDocumentToRuntime` entry points remain thin
snapshot adapters over this owner. Editor snapshots, private headers and editor
target include paths are not cook dependencies. The supported component
projections and legacy trigger normalization are unchanged by the ownership move.

`Horo/Scene/CookedSceneDefinition.h` belongs to `HoroEngine::SceneCook`. Its
independently versioned runtime payload contains only entity identity, hierarchy,
transforms, typed components and runtime asset requirements. It cannot encode raw
prefab references, names, editor state or filesystem paths. Unsupported required
component modes fail explicitly. Consumers must verify the Assets envelope before
decoding this inner payload and must not fall back to authoring source.

`Horo/Assets/AssetCookInputSnapshot.h` belongs to `HoroEngine::Assets`. Capture
reads and validates real source/identity-sidecar pairs after migration has finished.
Identity-sidecar schema 1 exposes only identity/type as cooker metadata; import
bookkeeping and native paths do not enter its canonical digest. Cooker-specific
semantic settings remain bound by the immutable contribution's cache identity.
The snapshot retains owned bytes and exact raw-file digests for drift checks.

## Cache And Publication

Existing callers without `AssetCookRequest::pinnedInputs` retain their V1 cache
namespace. Captured operations use a separate V2 namespace that binds the complete
ordered source/metadata closure, in addition to every V1 source, contribution,
settings, target and envelope input. Capturing a conservative superset can cause
extra misses; it cannot permit stale reuse. Process-local registry counters and
native paths do not affect that semantic commitment.

Fresh output and cache hits pass the same requested-envelope and domain payload
validation before generation publication. A captured operation rechecks actual
source/sidecar bytes at the common writer's selector commit barrier. Drift,
corruption, cancellation or pre-commit publication failure leaves the prior
`current.json` authoritative. A committed replacement remains committed even if
its later durability confirmation fails.

Byte revalidation is not a lock: the host must retain its project read authority
through the irreversible selector replacement. Capture itself does not migrate
sources, activate descriptors or acquire project mutation authority.

## Delivery Evidence Still Required

`Horo/Application/PrefabSceneCookHost.h` belongs to `HoroEngine::PrefabCookHost`,
a separate composition target. Its constructor borrows the live registry,
compatibility inspector and existing Editor project mutation/migration owners;
it performs no I/O or activation. `Cook` acquires the shared project-mutation
lease, excludes pending migration recovery, reads actual project/package-lock
inputs, checks restored archive evidence, and builds the static resolver and scene
strategies from captured canonical sources. The generic request's registry field
is not a second authority: this host captures its borrowed live registry instead.
Registry reload, raw metadata drift and source drift are rejected at the final
AssetCook commit fence. The host-only dependency on Editor transaction services
does not introduce Editor dependencies into Assets or the scene conversion owner.

`PrefabCookSchemaContext::Capture` copies a bounded frozen component registry and
explicit inert behavior descriptors. Retained scene/prefab envelopes must match
current available schemas; missing/skewed providers, incompatible behavior field
kinds and disallowed multiplicity fail admission. Descriptor defaults, migration
edges and behavior phase/access settings enter its derived semantic digest. The
request retains this immutable context through joined cooking/publication. It
contains no implementation factories, provider discovery or registration side
effects. Without it, opaque project components/behaviors are not cook-admissible.
HORO-1068 can reuse this same authority for dynamic-template admission.

`Horo/Packages/PackageRequest.h` belongs to `HoroEngine::Packages`. Its inert
schema-v1 decoder binds canonical named sources, explicit assignments, exact/
caret/any version ranges, feature and contribution sets, optional roots and
artifact pins. It accepts the existing `vendored` example spelling and emits
`vendored-artifact`; omitted schemaVersion/default fields normalize explicitly.
Legacy inline sources, local overrides, unknown fields, credentials, unsigned
remote pins and malformed/oversized input fail rather than becoming a second
request authority. Lock generation must use this codec's digest. Existing locks
with another hash recipe require explicit resolution/review, never an automatic
rewrite. The cook host reads intent and lock itself, validates direct-root
membership/source/version/pins, and checks restored immutable archive evidence.
Raw intent bytes are rechecked at publication even when their semantic digest
would remain unchanged. Restore, trust, enablement and activation remain separate.

`CookForRelease` validates the release plan's project and package-lock identity,
rechecks the host facts against the frozen plan, and returns the immutable
generation root and manifest digest for the release owner's packaging stage.
It does not publish a release candidate, migrate sources or activate descriptors.
Dynamic prefab roots, template payload/provider composition and resource-before-
template selection belong to HORO-1068; static-only prefabs remain excluded
from this operation's runtime inventory.

`AssetCookRequest::dependentPhase` is the generic extension boundary: selected
resource jobs finish first; the synchronous factory borrows their validated full
envelopes through callback return and creates immutable dependent strategies.
Those strategies own retained payload/evidence. The generic owner binds exact
resource envelope digests into dependent V2 keys and publishes both phases'
actual bytes once. It never consults an old generation for candidate resource
hashes. The static host clears this optional caller field; the dynamic host
extension selects roots and installs its own phase explicitly.

These changes do not by themselves prove the complete host operation. Full
integration regressions, affected public consumers, local preflight and exact-head
hosted checks are required before claiming #1067 complete.

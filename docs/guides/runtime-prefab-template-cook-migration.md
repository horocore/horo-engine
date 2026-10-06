# Runtime prefab template cook integration

`PrefabTemplateCook.h` belongs to `HoroEngine::PrefabAuthoring`. The additive
`CookPrefabTemplate` operation produces the existing HPFB version 1 wire format;
the runtime provider requires no codec or source-format migration. Existing
scene conversion and runtime provider callers keep their contracts.

The operation uses `PrefabSourceResolverSnapshot::Resolve`, the same authority
used by authored scene conversion. It preserves effective topology, transforms,
stable component/behavior schema identities, payloads and source provenance while
replacing sparse object slots with a dense parent-before-child entity table.
Nested and variant sources are flattened, rather than shipped as runtime source
dependencies. Resource dependencies retain exact identity, type and SHA-256 of
the full canonical AssetCook envelope. Missing, duplicate, foreign, corrupt or
mistargeted resources reject the detached candidate.
Resource envelope byte ceilings use `AssetCookLimits`; they are independent of
the smaller HPFB template payload ceiling in `PrefabLimitProfile`.

`PrefabSceneCookRequest::runtimePrefabRoots` selects unique typed runtime-spawnable
roots. This trailing additive field defaults to empty, preserving the static-only
host and existing aggregate callers. The same `PrefabSceneCookHost` owns migration
preconditions, project-mutation exclusion, actual source/sidecar capture, package
intent/lock/archive evidence, schema availability and final freshness fences.
No second host, registry, package resolver or publication authority is introduced.

Non-prefab resources and static cooked scenes enter the first unpublished
candidate phase. The template phase uses the same captured resolver, validates
post-override portable members through `PrefabCookSchemaContext`, and consumes
only each root's complete resource closure from the exact staged envelope bytes.
Its returned strategies own their payloads; borrowed candidate views expire at
callback return. Templates, resources and scenes publish through one generation
selector replacement. Failed or cancelled staging leaves the prior generation
current.

Canonical sorted root selection and dynamic-policy/output version enter template
settings. The shared V2 cache key includes actual source/metadata closure,
project/package/schema/settings and contribution identities, plus sorted full
staged-envelope hashes, identities and types. Envelope bytes include target and
format version. Fresh and cached template payloads pass the same HPFB validation
and exact expected-byte comparison. The host's `CookForRelease` retains the same
frozen-fact checks and returns the published generation root and manifest digest
to the existing Cook-to-Package handoff. It never returns authoring sources or a
cache directory as package authority.

Provider admission continues to require a full root AssetCook envelope, its exact
digest and cook target. Runtime loads the immutable generation selected by the
host and never receives the authoring resolver. Reload affects future preparation
and spawn requests; previously held immutable template leases remain readable.

The focused regression targets are `HoroPrefabTemplateCookTests`,
`HoroPrefabTemplateCookPublicHeaderConsumer` and
`HoroPrefabTemplateProviderTests`, with
`HoroPrefabTemplateCookGenerationTests` covering native generation publication.
The transformation tests compare scene/template
resolution, check nested/variant flattening, preserve portable provider schema
requirements, reject malformed resource envelopes and cover limits, cancellation
and registry replacement. The provider test prepares source-produced bytes and
hands the resulting lease to Scene's structural group transaction.
The generation test loads source-produced bytes through `FilesystemAssetProvider`
and the existing asynchronous template provider, rejects a cancelled replacement
publication and checks that the previous generation and held lease remain valid.
Execution evidence must accompany delivery; authoring these tests is not a passing
result.

Host coverage is in `HoroPrefabSceneCookHostTests` and
`HoroPrefabCookPublicHeaderConsumer`. It covers shared scene/template/resource
publication, actual generation-provider loading, cache reuse, canonical root
order, changed selection and resource inputs under a stable registry revision,
invalid roots, schema availability, exact payload bounds, cancellation/source
freshness/publication failure and the frozen release executor handoff.

The independent six-target Release/headless validation passed 48/48 focused
CTest cases at `6cb60f6a`, before integration of the published scene-cook parent.
That result does not validate the later host integration; final merged-input
build, test and quality evidence must accompany the PR.

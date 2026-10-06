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

The calling host must select declared runtime-spawnable roots, complete source
migration first, capture the registry/source/settings/package/schema inputs and
pin actual resource artifact bytes. It must bind those inputs and dependency
envelopes into the cache key and publish templates, resources and cooked scenes
through one complete Assets generation transaction. Captured provider schemas
must admit every retained component and behavior before the host accepts the
template; preserving an opaque portable envelope does not prove its runtime
capability is available. The pure transformation does
not schedule resource cooks, publish generations, activate providers or construct
release archives. Those host steps remain required for the complete PFB-007.3
delivery; the initial transformation alone does not make a project cook or release
support runtime templates.

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

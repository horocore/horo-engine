#include "Horo/Prefab/PrefabAssetDependencyClosure.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabSceneCookState.h"

#include <algorithm>

namespace Horo::Application::PrefabCookDetail {
    namespace {
        /** @brief Length-delimits registered semantic values without paths or mutable catalog publication counters. */
        void Append(std::string &value, const std::string_view field) {
            value += std::to_string(field.size());
            value += ':';
            value += field;
        }

        /** @brief Derives a nonzero compact runtime identity; the host checks containing-scene collisions before publication. */
        std::uint64_t Compact(const Sha256Digest &digest) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < 8; ++index)
                value |= static_cast<std::uint64_t>(digest.bytes[index]) << (8U * index);
            return value == 0 ? 1 : value;
        }

        /** @brief Parses every captured prefab and commits its canonical revision before any resolver is built. */
        Result<std::vector<Prefab::PrefabDependencySource>> PrefabSources(const Assets::AssetCookInputSnapshot &inputs,
                                                                          const HostCapture &host, const Prefab::PrefabLimitProfile &limits,
                                                                          const CancellationToken &cancellation,
                                                                          const std::shared_ptr<const PrefabCookSchemaContext> &schemas) {
            std::vector<Prefab::PrefabDependencySource> sources;
            for (const auto &input : inputs.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (input.record.type.Value() != "core.prefab")
                    continue;
                const std::string_view bytes{reinterpret_cast<const char *>(input.bytes.data()), input.bytes.size()};
                auto document = Prefab::PrefabDocument::Parse(bytes, limits, {.expectedProjectVersion = host.metadata.horoVersion.value});
                if (document.HasError())
                    return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(document.ErrorValue());
                auto canonical = document.Value().SerializeCanonical();
                if (canonical.HasError())
                    return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(canonical.ErrorValue());
                if (document.Value().Data().assetId != input.record.id || canonical.Value() != bytes)
                    return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                for (const auto &object : document.Value().Data().objects) {
                    std::vector<Gameplay::SerializedComponent> components;
                    for (const auto &component : object.components)
                        components.push_back(component.component);
                    if ((!schemas && (!components.empty() || !object.behaviors.empty())) ||
                        (schemas && schemas->Validate(components, object.behaviors).HasError()))
                        return Result<std::vector<Prefab::PrefabDependencySource>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                }
                sources.push_back({std::move(document).Value(), {host.metadata.horoVersion.value, input.sourceDigest}});
            }
            return Result<std::vector<Prefab::PrefabDependencySource>>::Success(std::move(sources));
        }

        /** @brief Immutable, fully validated domain output; no raw source or mutable resolver escapes the host operation. */
        struct PreparedScene final {
            Assets::AssetId asset;
            Sha256Digest sourceDigest;
            Runtime::SceneDefinitionId id;
            Runtime::SceneDefinitionRevision revision;
            std::vector<std::uint8_t> payload;
            std::vector<Assets::AssetId> dependencies;
        };

        /** @brief Pure strategy over exact prepared outputs; Assets owns envelope construction and publication. */
        class SceneStrategy final : public Assets::ICookerStrategy {
        public:
            SceneStrategy(std::vector<PreparedScene> scenes, const Sha256Digest settings, const SceneCook::CookedSceneLimits limits)
                : scenes_(std::move(scenes)), settings_(settings), limits_(limits) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "horo.scene.prefab-inline.v1", .settingsDigest = settings_, .settingsSchemaVersion = 1};
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                const auto *scene = Find(source);
                if (!scene)
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                return Result<Assets::CookOutputSink>::Success({scene->payload, scene->dependencies, {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                const auto *scene = Find(source);
                if (!scene)
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                auto decoded = SceneCook::DecodeCookedSceneDefinition(payload, scene->id, scene->revision, limits_);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                // A valid but different payload under the requested full key is not a usable cache hit.
                if (!std::ranges::equal(payload, scene->payload))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                return Result<void>::Success();
            }

        private:
            /** @brief Requires exact admitted source identity for both fresh output and cached domain validation. */
            const PreparedScene *Find(const Assets::CookSourceView &source) const {
                const auto found = std::ranges::find(scenes_, source.id, &PreparedScene::asset);
                return found != scenes_.end() && found->sourceDigest == source.sourceDigest ? &*found : nullptr;
            }

            const std::vector<PreparedScene> scenes_;
            const Sha256Digest settings_;
            const SceneCook::CookedSceneLimits limits_;
        };

        /** @brief Preserves registered resource strategies while adding actual host semantics to every derived cache key. */
        class HostKeyStrategy final : public Assets::ICookerStrategy {
        public:
            HostKeyStrategy(std::shared_ptr<const Assets::ICookerStrategy> strategy, const Sha256Digest host)
                : strategy_(std::move(strategy)), identity_(strategy_->CacheIdentity()) {
                std::string preimage{"horo.host.cooker-input.v1"};
                Append(preimage, FormatSha256(host));
                Append(preimage, FormatSha256(identity_.settingsDigest));
                Append(preimage, std::to_string(identity_.settingsSchemaVersion));
                identity_.settingsDigest = ComputeSha256(std::as_bytes(std::span{preimage}));
                identity_.settingsSchemaVersion = 1;
            }

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return identity_;
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                return strategy_->Cook(source, cancellation);
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                return strategy_->ValidateCookedPayload(source, payload);
            }

        private:
            const std::shared_ptr<const Assets::ICookerStrategy> strategy_;
            Assets::CookerCacheIdentity identity_;
        };

        /** @brief Admits retained project schemas and rejects unsupported required mesh projections before conversion. */
        Result<void> AdmitSceneObjects(const std::span<const SceneSource::SceneObjectSnapshot> objects,
                                       const std::shared_ptr<const PrefabCookSchemaContext> &schemas) {
            for (const auto &object : objects) {
                if ((!schemas && (!object.components.gameplayComponents.empty() || !object.components.behaviors.empty())) ||
                    (schemas && schemas->Validate(object.components.gameplayComponents, object.components.behaviors).HasError()))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                // RuntimeEntityDefinition has no imported-mesh field: never silently drop a required authored asset reference.
                if (object.meshAsset)
                    return Result<void>::Failure(MakeError(SceneCook::SceneCookErrors::Unsupported));
            }
            return Result<void>::Success();
        }

        /** @brief Builds one complete runtime scene and its source-free prefab/resource dependency closure. */
        Result<PreparedScene> PrepareScene(const Assets::AssetCookPinnedSource &input, const Assets::AssetRegistrySnapshot &registry,
                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                           const Prefab::PrefabDependencyGraphSnapshot &graph, const Prefab::PrefabLimitProfile &limits,
                                           const SceneCook::CookedSceneLimits &sceneLimits, const Sha256Digest settings,
                                           const std::shared_ptr<const PrefabCookSchemaContext> &schemas) {
            const std::string_view bytes{reinterpret_cast<const char *>(input.bytes.data()), input.bytes.size()};
            auto parsed = SceneSource::DecodeSceneSource(bytes);
            if (parsed.HasError())
                return Result<PreparedScene>::Failure(parsed.ErrorValue());
            const auto &source = parsed.Value();
            const SceneSource::SceneSourceView view{source.objects, source.prefabInstances};
            if (SceneSource::EncodeSceneSource(view) != bytes || source.objects.size() > sceneLimits.maximumEntities)
                return Result<PreparedScene>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            if (auto admitted = AdmitSceneObjects(source.objects, schemas); admitted.HasError())
                return Result<PreparedScene>::Failure(admitted.ErrorValue());
            const Runtime::SceneDefinitionId id{Compact(ComputeSha256(std::as_bytes(std::span{input.record.id.Bytes()})))};
            const std::string revisionInput = FormatSha256(input.sourceDigest) + FormatSha256(settings);
            const Runtime::SceneDefinitionRevision revision{Compact(ComputeSha256(std::as_bytes(std::span{revisionInput})))};
            auto definition = SceneSource::ConvertSceneSourceToRuntime(view, id, revision, resolver, limits);
            if (definition.HasError())
                return Result<PreparedScene>::Failure(definition.ErrorValue());
            if (definition.Value().Entities().size() > sceneLimits.maximumEntities)
                return Result<PreparedScene>::Failure(MakeError(SceneCook::SceneCookErrors::TooLarge));
            std::vector<Assets::AssetId> roots;
            for (const auto &placement : source.prefabInstances)
                roots.push_back(placement.sourcePrefab.Asset());
            std::vector<Prefab::PrefabAssetDependency> existing;
            for (const auto &dependency : definition.Value().AssetDependencies())
                existing.push_back({dependency.id, dependency.expectedType, {}});
            auto closure = Prefab::BuildPrefabAssetDependencyClosure(registry, graph, roots, existing, sceneLimits.maximumDependencies);
            if (closure.HasError())
                return Result<PreparedScene>::Failure(closure.ErrorValue());
            Runtime::SceneDefinitionBuilder builder{id, revision};
            for (const auto &entity : definition.Value().Entities())
                builder.Add(entity);
            PreparedScene prepared{input.record.id, input.sourceDigest, id, revision, {}, {}};
            for (const auto &dependency : closure.Value().RuntimeDependencies()) {
                if (auto admitted = builder.RequireAsset(dependency); admitted.HasError())
                    return Result<PreparedScene>::Failure(admitted.ErrorValue());
                prepared.dependencies.push_back(dependency.id);
            }
            auto complete = std::move(builder).Build();
            if (complete.HasError())
                return Result<PreparedScene>::Failure(complete.ErrorValue());
            auto encoded = SceneCook::EncodeCookedSceneDefinition(complete.Value(), sceneLimits);
            if (encoded.HasError())
                return Result<PreparedScene>::Failure(encoded.ErrorValue());
            prepared.payload = std::move(encoded).Value();
            return Result<PreparedScene>::Success(std::move(prepared));
        }

        /** @brief Couples admitted resource strategies with the complete source, host and resource-settings commitment. */
        struct ResourceCatalogInputs final {
            std::vector<Assets::CookerContribution> contributions;
            Sha256Digest settings;
        };

        /** @brief Captures every registered resource strategy identity before constructing scene or resource wrappers. */
        Result<ResourceCatalogInputs> PrepareResourceCatalogInputs(const PrefabSceneCookRequest &request, const HostCapture &host,
                                                                   const Assets::AssetCookInputSnapshot &inputs,
                                                                   const Assets::CookerCatalogSnapshot &catalog,
                                                                   const CancellationToken &cancellation) {
            std::string identity{"horo.static-scene-cook.resolver-v1.output-v1"};
            Append(identity, FormatSha256(host.semanticDigest));
            Append(identity, FormatSha256(inputs.ClosureDigest()));
            std::vector<Assets::CookerContribution> contributions;
            for (const auto &input : inputs.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Result<ResourceCatalogInputs>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (input.record.type.Value() == "core.prefab" || input.record.type.Value() == "core.scene")
                    continue;
                const auto *original = catalog.FindContribution(input.record.type, request.assets.target);
                if (!original || !original->strategy)
                    return Result<ResourceCatalogInputs>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                const auto cache = original->strategy->CacheIdentity();
                Append(identity, input.record.type.Value());
                Append(identity, original->contributionId);
                Append(identity, cache.version);
                Append(identity, FormatSha256(cache.settingsDigest));
                Append(identity, std::to_string(cache.settingsSchemaVersion));
                if (std::ranges::find(contributions, input.record.type, &Assets::CookerContribution::assetType) == contributions.end())
                    contributions.push_back(*original);
            }
            const auto settings = ComputeSha256(std::as_bytes(std::span{identity}));
            for (auto &contribution : contributions)
                contribution.strategy = std::make_shared<HostKeyStrategy>(contribution.strategy, settings);
            return Result<ResourceCatalogInputs>::Success({std::move(contributions), settings});
        }

        /** @brief Owns complete source-free scenes after aggregate payload and compact-identity collision admission. */
        Result<std::vector<PreparedScene>> PrepareScenes(const PrefabSceneCookRequest &request,
                                                         const Assets::AssetCookInputSnapshot &inputs,
                                                         const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                         const Prefab::PrefabDependencyGraphSnapshot &graph,
                                                         const Prefab::PrefabLimitProfile &limits, const Sha256Digest settings,
                                                         const CancellationToken &cancellation) {
            std::vector<PreparedScene> scenes;
            std::uint64_t totalPayloadBytes{};
            for (const auto &input : inputs.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<PreparedScene>>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                if (input.record.type.Value() != "core.scene")
                    continue;
                auto scene =
                    PrepareScene(input, inputs.Registry(), resolver, graph, limits, request.sceneLimits, settings, request.schemas);
                if (scene.HasError())
                    return Result<std::vector<PreparedScene>>::Failure(scene.ErrorValue());
                if (scene.Value().payload.size() > request.maximumCapturedBytes - totalPayloadBytes ||
                    std::ranges::any_of(scenes, [&scene](const PreparedScene &existing) {
                    return existing.id == scene.Value().id;
                }))
                    return Result<std::vector<PreparedScene>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                totalPayloadBytes += scene.Value().payload.size();
                scenes.push_back(std::move(scene).Value());
            }
            return Result<std::vector<PreparedScene>>::Success(std::move(scenes));
        }
    }  // namespace

    /** @copydoc PrepareCatalog */
    Result<PreparedCatalog> PrepareCatalog(const PrefabSceneCookRequest &request, const HostCapture &host,
                                           const Assets::AssetCookInputSnapshot &inputs, const Prefab::PrefabLimitProfile &limits,
                                           const Assets::CookerCatalogSnapshot &catalog, const CancellationToken &cancellation) {
        auto sources = PrefabSources(inputs, host, limits, cancellation, request.schemas);
        if (sources.HasError())
            return Result<PreparedCatalog>::Failure(sources.ErrorValue());
        auto graph = Prefab::BuildPrefabDependencyGraph(inputs.Registry(), sources.Value(), limits);
        if (graph.HasError())
            return Result<PreparedCatalog>::Failure(graph.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<PreparedCatalog>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
        auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(inputs.Registry(), std::move(sources).Value(), limits);
        if (resolver.HasError())
            return Result<PreparedCatalog>::Failure(resolver.ErrorValue());
        auto resources = PrepareResourceCatalogInputs(request, host, inputs, catalog, cancellation);
        if (resources.HasError())
            return Result<PreparedCatalog>::Failure(resources.ErrorValue());
        const auto settings = resources.Value().settings;
        auto scenes = PrepareScenes(request, inputs, resolver.Value(), graph.Value(), limits, settings, cancellation);
        if (scenes.HasError())
            return Result<PreparedCatalog>::Failure(scenes.ErrorValue());
        auto contributions = std::move(resources).Value().contributions;
        if (!scenes.Value().empty())
            contributions.push_back({"horo.builtin.scene_prefab_inline",
                                     Assets::AssetTypeId::Parse("core.scene").Value(),
                                     {request.assets.target},
                                     std::make_shared<SceneStrategy>(std::move(scenes).Value(), settings, request.sceneLimits)});
        Assets::CookerCatalog candidate;
        for (auto &contribution : contributions) {
            if (auto registered = candidate.Register(std::move(contribution)); registered.HasError())
                return Result<PreparedCatalog>::Failure(registered.ErrorValue());
        }
        auto published = candidate.Publish();
        if (published.HasError())
            return Result<PreparedCatalog>::Failure(published.ErrorValue());
        PreparedCatalog prepared{published.Value(), {}};
        if (!request.runtimePrefabRoots.empty()) {
            auto phase = PrepareTemplatePhase({std::move(resolver).Value(), std::move(graph).Value(), inputs.Registry(),
                                               request.runtimePrefabRoots, request.assets.target, limits, request.assets.limits, settings,
                                               request.schemas, request.maximumCapturedBytes});
            if (phase.HasError())
                return Result<PreparedCatalog>::Failure(phase.ErrorValue());
            prepared.templates = std::move(phase).Value();
        }
        return Result<PreparedCatalog>::Success(std::move(prepared));
    }
}  // namespace Horo::Application::PrefabCookDetail

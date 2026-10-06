#include "Horo/Prefab/PrefabTemplateCook.h"
#include "PrefabSceneCookState.h"

#include <algorithm>

namespace Horo::Application::PrefabCookDetail {
    namespace {
        /** @brief Owns the exact expected logical output through fresh and cache admission. */
        struct PreparedTemplate final {
            Assets::AssetId asset;
            Sha256Digest sourceDigest;
            std::vector<std::uint8_t> payload;
            std::vector<Assets::AssetId> dependencies;
        };

        /** @brief Admits effective post-override member schemas using the same inert authority as static scene cooking. */
        Result<void> ValidateSchemas(const Prefab::CookedPrefabData &data, const TemplateCookState &state,
                                     const CancellationToken &cancellation) {
            for (const auto &entity : data.entities) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                std::vector<Gameplay::SerializedComponent> components;
                std::vector<Gameplay::BehaviorComponent> behaviors;
                for (const auto &member : entity.members) {
                    if (const auto *component = std::get_if<Prefab::RawComponentPayload>(&member))
                        components.push_back(component->component);
                    else
                        behaviors.push_back(std::get<Gameplay::BehaviorComponent>(member));
                }
                if (!state.schemas && (!components.empty() || !behaviors.empty()))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (state.schemas) {
                    if (auto valid = state.schemas->Validate(components, behaviors); valid.HasError())
                        return valid;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Borrows only the root's resource closure from the verified candidate envelopes, never from cache paths. */
        Result<std::vector<Prefab::PrefabTemplateCookResource>> RootResources(
            const TemplateCookState &state, const Assets::AssetId root,
            const std::span<const Assets::AssetCookCandidateArtifactView> artifacts) {
            auto closure = state.graph.DependencyClosure(std::span{&root, 1});
            if (closure.HasError())
                return Result<std::vector<Prefab::PrefabTemplateCookResource>>::Failure(closure.ErrorValue());
            std::vector<Prefab::PrefabTemplateCookResource> resources;
            for (const auto asset : closure.Value()) {
                const auto *node = state.graph.FindNode(asset);
                if (!node)
                    return Result<std::vector<Prefab::PrefabTemplateCookResource>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (node->sourceRevision)
                    continue;
                const auto found = std::ranges::find_if(artifacts, [asset](const auto &artifact) {
                    return artifact.identity.id == asset;
                });
                if (found == artifacts.end())
                    return Result<std::vector<Prefab::PrefabTemplateCookResource>>::Failure(
                        MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                resources.push_back({asset, found->envelope});
            }
            return Result<std::vector<Prefab::PrefabTemplateCookResource>>::Success(std::move(resources));
        }

        /** @brief Transforms one explicit root and retains only owned validated bytes after resource views expire. */
        Result<PreparedTemplate> PrepareTemplate(const TemplateCookState &state, const Assets::AssetId root,
                                                 const std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                 const CancellationToken &cancellation) {
            auto resources = RootResources(state, root, artifacts);
            if (resources.HasError())
                return Result<PreparedTemplate>::Failure(resources.ErrorValue());
            auto cooked = Prefab::CookPrefabTemplate(state.resolver, state.registry, root, resources.Value(), state.target, state.limits,
                                                     cancellation, state.assetLimits);
            if (cooked.HasError())
                return Result<PreparedTemplate>::Failure(cooked.ErrorValue());
            if (auto admitted = ValidateSchemas(cooked.Value().Data(), state, cancellation); admitted.HasError())
                return Result<PreparedTemplate>::Failure(admitted.ErrorValue());
            const auto source = std::ranges::find_if(state.resolver.Sources(), [root](const auto &entry) {
                return entry.document.Data().assetId == root;
            });
            if (source == state.resolver.Sources().end())
                return Result<PreparedTemplate>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            PreparedTemplate prepared{root, source->sourceRevision.contentDigest, {}, {}};
            prepared.payload.reserve(cooked.Value().Bytes().size());
            for (const auto byte : cooked.Value().Bytes())
                prepared.payload.push_back(std::to_integer<std::uint8_t>(byte));
            for (const auto &dependency : cooked.Value().Data().dependencies)
                prepared.dependencies.push_back(dependency.asset.id);
            return Result<PreparedTemplate>::Success(std::move(prepared));
        }

        /** @brief Supplies immutable templates to the generic joined cook, with identical fresh/cache domain validation. */
        class TemplateStrategy final : public Assets::ICookerStrategy {
        public:
            TemplateStrategy(std::vector<PreparedTemplate> templates, const Sha256Digest settings, Prefab::PrefabLimitProfile limits)
                : templates_(std::move(templates)), settings_(settings), limits_(std::move(limits)) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "horo.prefab.template.v1", .settingsDigest = settings_, .settingsSchemaVersion = 1};
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                const auto *prepared = Find(source);
                if (!prepared)
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                return Result<Assets::CookOutputSink>::Success({prepared->payload, prepared->dependencies, {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                const auto *prepared = Find(source);
                if (!prepared)
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
                auto decoded = Prefab::CookedPrefab::Parse(std::as_bytes(payload), source.id, limits_);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                if (!std::ranges::equal(payload, prepared->payload))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                return Result<void>::Success();
            }

        private:
            /** @brief Matches captured source identity rather than accepting another valid template under this key. */
            const PreparedTemplate *Find(const Assets::CookSourceView &source) const {
                const auto found = std::ranges::find(templates_, source.id, &PreparedTemplate::asset);
                return found != templates_.end() && found->sourceDigest == source.sourceDigest ? &*found : nullptr;
            }

            const std::vector<PreparedTemplate> templates_;
            const Sha256Digest settings_;
            const Prefab::PrefabLimitProfile limits_;
        };

        /** @brief Creates an owned template-only second-phase catalog after every first-phase envelope has been verified. */
        Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> TemplateCatalog(
            const TemplateCookState &state, const std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
            const CancellationToken &cancellation) {
            std::vector<PreparedTemplate> templates;
            std::uint64_t totalBytes{};
            for (const auto root : state.roots) {
                auto prepared = PrepareTemplate(state, root, artifacts, cancellation);
                if (prepared.HasError())
                    return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(prepared.ErrorValue());
                if (prepared.Value().payload.size() > state.maximumOutputBytes - totalBytes)
                    return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(
                        MakeError(Prefab::PrefabErrors::PayloadTooLarge));
                totalBytes += prepared.Value().payload.size();
                templates.push_back(std::move(prepared).Value());
            }
            Assets::CookerCatalog candidate;
            auto registered = candidate.Register({"horo.builtin.prefab_template",
                                                  Assets::AssetTypeId::Parse("core.prefab").Value(),
                                                  {state.target},
                                                  std::make_shared<TemplateStrategy>(std::move(templates), state.settings, state.limits)});
            if (registered.HasError())
                return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(registered.ErrorValue());
            return candidate.Publish();
        }
    }  // namespace

    /** @copydoc PrepareTemplatePhase */
    Result<std::shared_ptr<const Assets::AssetCookDependentPhase>> PrepareTemplatePhase(TemplateCookState state) {
        if (state.roots.empty() || state.roots.size() > state.assetLimits.maximumAssets)
            return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        std::ranges::sort(state.roots);
        if (std::ranges::adjacent_find(state.roots) != state.roots.end())
            return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        std::string identity = "horo.dynamic-prefab-policy.v1.output-v" + std::to_string(Prefab::CurrentCookedPrefabVersion) + ":" +
                               FormatSha256(state.settings);
        for (const auto root : state.roots) {
            const auto *node = state.graph.FindNode(root);
            if (!node || !node->sourceRevision)
                return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            identity += root.ToString();
        }
        state.settings = ComputeSha256(std::as_bytes(std::span{identity}));
        auto phase = std::make_shared<Assets::AssetCookDependentPhase>();
        for (const auto &record : state.registry.Records()) {
            if (record.type.Value() != "core.prefab")
                phase->resourceIds.push_back(record.id);
        }
        const auto captured = std::make_shared<const TemplateCookState>(std::move(state));
        phase->makeCatalog = [captured](const std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                        const CancellationToken &cancellation) {
            return TemplateCatalog(*captured, artifacts, cancellation);
        };
        return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Success(std::move(phase));
    }
}  // namespace Horo::Application::PrefabCookDetail

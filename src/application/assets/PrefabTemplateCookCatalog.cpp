#include "Horo/Prefab/PrefabTemplateCook.h"
#include "PrefabCookBuiltinProjection.h"
#include "PrefabSceneCookState.h"

#include <algorithm>
#include <format>
#include <memory>

namespace Horo::Application::PrefabCookDetail {
    namespace {
        /** @brief Owns the exact expected logical output through fresh and cache admission. */
        struct PreparedTemplate final {
            Assets::AssetId asset;
            Sha256Digest sourceDigest;
            std::vector<std::uint8_t> payload;
            std::vector<Assets::AssetId> dependencies;
        };

        Result<void> CollectEntitySchemas(const Prefab::CookedPrefabEntity &entity, std::vector<Gameplay::SerializedComponent> &components,
                                          std::vector<Gameplay::BehaviorComponent> &behaviors) {
            CookBuiltinProjection builtins;
            for (const auto &member : entity.members) {
                if (const auto *component = std::get_if<Prefab::RawComponentPayload>(&member)) {
                    if (auto projected = builtins.Collect(*component, components); projected.HasError())
                        return projected;
                } else
                    behaviors.push_back(std::get<Gameplay::BehaviorComponent>(member));
            }
            return Result<void>::Success();
        }

        /** @brief Admits fully resolved member schemas using the same inert authority as static scene cooking. */
        Result<void> ValidateSchemas(const Prefab::CookedPrefabData &data, const TemplateCookState &state,
                                     const CancellationToken &cancellation) {
            for (const auto &entity : data.entities) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
                std::vector<Gameplay::SerializedComponent> components;
                std::vector<Gameplay::BehaviorComponent> behaviors;
                if (auto collected = CollectEntitySchemas(entity, components, behaviors); collected.HasError())
                    return collected;
                if (!state.schemas && (!components.empty() || !behaviors.empty()))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (state.schemas) {
                    if (auto valid = state.schemas->Validate(components, behaviors); valid.HasError())
                        return valid;
                }
            }
            return Result<void>::Success();
        }

        struct RootResourceProjection final {
            std::vector<Prefab::PrefabTemplateCookResource> envelopes;
            std::vector<Assets::AssetDependency> expected;
        };

        Result<std::vector<Assets::AssetDependency>> DirectResources(const TemplateCookState &state, Assets::AssetId root) {
            using Return = Result<std::vector<Assets::AssetDependency>>;
            auto closure = state.graph.DependencyClosure(std::span{&root, 1});
            if (closure.HasError())
                return Return::Failure(std::move(closure).ErrorValue());
            std::vector<Assets::AssetDependency> roles;
            for (const auto asset : closure.Value()) {
                const auto *node = state.graph.FindNode(asset);
                if (!node)
                    return Return::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (!node->sourceRevision)
                    roles.push_back({node->assetId, node->assetType});
            }
            return Return::Success(std::move(roles));
        }

        /** @brief One actual Animation projection determines BOTH envelope selection and the canonical expected role set. */
        Result<RootResourceProjection> RootResources(const TemplateCookState &state, Assets::AssetId root,
                                                     std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                     const CancellationToken &cancellation) {
            using Return = Result<RootResourceProjection>;
            auto direct = DirectResources(state, root);
            if (direct.HasError())
                return Return::Failure(std::move(direct).ErrorValue());
            auto roles = std::move(direct).Value();
            if (state.animation) {
                auto expanded = state.animation->ExpandDependencies(roles, artifacts,
                                                                    std::min<std::size_t>(state.assetLimits.maximumAssets,
                                                                                          state.limits.Policy().maximumReferencedAssets),
                                                                    cancellation);
                if (expanded.HasError())
                    return Return::Failure(std::move(expanded).ErrorValue());
                roles = std::move(expanded).Value();
            }
            RootResourceProjection projection;
            for (const auto &role : roles) {
                const auto found = std::ranges::find(artifacts, role.id, [](const auto &artifact) {
                    return artifact.identity.id;
                });
                if (found == artifacts.end())
                    return Return::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                projection.envelopes.push_back({role.id, found->envelope});
            }
            if (state.animation)
                projection.expected = std::move(roles);
            return Return::Success(std::move(projection));
        }

        /** @brief Projects raw identity only after the same sealed source bytes match the resolver's exact canonical document. */
        Result<Sha256Digest> CapturedTemplateDigest(const TemplateCookState &state, Assets::AssetId root) {
            const auto *captured = state.inputs.Find(root);
            if (!captured || captured->record.type.Value() != "core.prefab")
                return Result<Sha256Digest>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            const auto document = std::ranges::find_if(state.resolver.Sources(), [root](const auto &source) {
                return source.document.Data().assetId == root;
            });
            if (document == state.resolver.Sources().end())
                return Result<Sha256Digest>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            const std::string_view bytes{reinterpret_cast<const char *>(captured->bytes.data()), captured->bytes.size()};
            const auto same = document->document.MatchesCanonicalInput(bytes);
            if (same.HasError())
                return Result<Sha256Digest>::Failure(same.ErrorValue());
            if (!same.Value())
                return Result<Sha256Digest>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            return Result<Sha256Digest>::Success(captured->sourceDigest);
        }

        /** @brief Transforms one explicit root and retains only owned validated bytes after resource views expire. */
        Result<PreparedTemplate> PrepareTemplate(const TemplateCookState &state, const Assets::AssetId root,
                                                 const std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                 const CancellationToken &cancellation) {
            const auto source = CapturedTemplateDigest(state, root);
            if (source.HasError())
                return Result<PreparedTemplate>::Failure(source.ErrorValue());
            auto resources = RootResources(state, root, artifacts, cancellation);
            if (resources.HasError())
                return Result<PreparedTemplate>::Failure(resources.ErrorValue());
            auto cooked = Prefab::CookPrefabTemplate(state.resolver, state.registry, root, resources.Value().envelopes, state.target,
                                                     state.limits, {cancellation, state.assetLimits, resources.Value().expected});
            if (cooked.HasError())
                return Result<PreparedTemplate>::Failure(cooked.ErrorValue());
            if (auto admitted = ValidateSchemas(cooked.Value().Data(), state, cancellation); admitted.HasError())
                return Result<PreparedTemplate>::Failure(admitted.ErrorValue());
            PreparedTemplate prepared{root, source.Value(), {}, {}};
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
            TemplateStrategy(std::vector<PreparedTemplate> templates, const Sha256Digest &settings, Prefab::PrefabLimitProfile limits)
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
                if (auto decoded = Prefab::CookedPrefab::Parse(std::as_bytes(payload), source.id, limits_); decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                if (!std::ranges::equal(payload, prepared->payload))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                return Result<void>::Success();
            }

        private:
            /** @brief Matches captured source identity rather than accepting another valid template under this key. */
            const PreparedTemplate *Find(const Assets::CookSourceView &source) const {
                const auto found = std::ranges::find(templates_, source.id, &PreparedTemplate::asset);
                return found != templates_.end() && found->sourceDigest == source.sourceDigest ? std::to_address(found) : nullptr;
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
            if (auto registered =
                    candidate.Register({"horo.builtin.prefab_template",
                                        Assets::AssetTypeId::Parse("core.prefab").Value(),
                                        {state.target},
                                        std::make_shared<TemplateStrategy>(std::move(templates), state.settings, state.limits)});
                registered.HasError())
                return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(registered.ErrorValue());
            return candidate.Publish();
        }
    }  // namespace

    static Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> TemplateCatalogAfterAnimationValidation(
        const TemplateCookState &state, std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
        const CancellationToken &cancellation) {
        if (state.animation) {
            if (auto valid = state.animation->ValidateEmitted(artifacts, cancellation); valid.HasError())
                return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(std::move(valid).ErrorValue());
        }
        return TemplateCatalog(state, artifacts, cancellation);
    }

    /** @brief Binds semantic graph and raw producer identities to the same actual sealed capture before admitting a template phase. */
    static bool SameTemplateCapture(const TemplateCookState &state) {
        return state.inputs.Registry().Revision() == state.registry.Revision() &&
               state.resolver.RegistryRevision() == state.registry.Revision() &&
               state.graph.RegistryRevision() == state.registry.Revision() &&
               (!state.animation || state.animation->UsesCapture(state.inputs));
    }

    /** @copydoc PrepareTemplatePhase */
    Result<std::shared_ptr<const Assets::AssetCookDependentPhase>> PrepareTemplatePhase(TemplateCookState state) {
        if (!SameTemplateCapture(state) || state.roots.empty() || state.roots.size() > state.assetLimits.maximumAssets)
            return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        std::ranges::sort(state.roots);
        if (std::ranges::adjacent_find(state.roots) != state.roots.end())
            return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        std::string identity =
            std::format("horo.dynamic-prefab-policy.v1.output-v{}:{}", Prefab::CurrentCookedPrefabVersion, FormatSha256(state.settings));
        for (const auto root : state.roots) {
            if (const auto *node = state.graph.FindNode(root); !node || !node->sourceRevision)
                return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
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
            return TemplateCatalogAfterAnimationValidation(*captured, artifacts, cancellation);
        };
        return Result<std::shared_ptr<const Assets::AssetCookDependentPhase>>::Success(std::move(phase));
    }
}  // namespace Horo::Application::PrefabCookDetail

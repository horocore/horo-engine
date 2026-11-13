#include "PrefabCookSemanticInputs.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <exception>
#include <map>
#include <new>
#include <set>

namespace Horo::Application::PrefabCookDetail {
    namespace {
        /** @brief Encodes canonical bounded fields without ambiguous concatenation. */
        void Append(std::string &identity, const std::string_view value) {
            identity += std::to_string(value.size());
            identity += ':';
            identity += value;
        }

        /** @brief Owns one finite closure walk; stack storage never depends on native recursion. */
        struct Closure final {
            const Assets::AssetCookInputSnapshot &inputs;
            const Prefab::PrefabDependencyGraphSnapshot &graph;
            const Prefab::PrefabSourceResolverSnapshot &resolver;
            const Assets::CookerCatalogSnapshot &catalog;
            const AssetCookTargetId &target;
            std::size_t maximum;
            const CancellationToken &cancellation;
            Assets::AssetId root;
            std::span<const Assets::AssetId> sceneResources;
            std::set<Gameplay::ComponentTypeId> components;
            std::set<Gameplay::ComponentTypeId> builtins;
            std::set<Gameplay::BehaviorTypeId> behaviors;
            std::map<Assets::AssetId, std::uint8_t> colors;
            std::map<Assets::AssetId, std::string> providers;
            std::size_t edges{};
            bool nativeScene{};

            void CollectPrefabTypes(const Prefab::PrefabObjectNode &object) {
                for (const auto &component : object.components) {
                    const auto &type = component.component.typeId;
                    if (Runtime::FindBuiltinSceneComponent(type.Value()))
                        builtins.insert(type);
                    else
                        components.insert(type);
                }
                for (const auto &behavior : object.behaviors)
                    behaviors.insert(behavior.typeId);
            }

            Result<std::vector<Assets::AssetId>> PrefabDependencies(const Assets::AssetCookPinnedSource &source) {
                using Return = Result<std::vector<Assets::AssetId>>;
                const auto found = std::ranges::find_if(resolver.Sources(), [&](const auto &entry) {
                    return entry.document.Data().assetId == source.record.id;
                });
                if (found == resolver.Sources().end() || !graph.FindNode(source.record.id))
                    return Return::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                for (const auto &object : found->document.Data().objects)
                    CollectPrefabTypes(object);
                std::vector<Assets::AssetId> dependencies;
                for (const auto &edge : graph.DirectDependencies(source.record.id))
                    dependencies.push_back(edge.targetAsset);
                return Return::Success(std::move(dependencies));
            }

            Result<std::vector<Assets::AssetId>> SceneDependencies(const Assets::AssetCookPinnedSource &source) {
                using Return = Result<std::vector<Assets::AssetId>>;
                if (source.record.id != root)
                    return Return::Failure(MakeError(PrefabSceneCookErrors::Invalid,
                                                     "A resource producer cannot consume an undeclared Scene conversion context."));
                const auto parsed =
                    SceneSource::DecodeSceneSource({reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()});
                if (parsed.HasError())
                    return Return::Failure(parsed.ErrorValue());
                nativeScene = true;
                for (const auto &object : parsed.Value().objects) {
                    for (const auto &component : object.components.gameplayComponents)
                        components.insert(component.typeId);
                    for (const auto &behavior : object.components.behaviors)
                        behaviors.insert(behavior.typeId);
                }
                std::vector<Assets::AssetId> dependencies;
                for (const auto &placement : parsed.Value().prefabInstances)
                    dependencies.push_back(placement.sourcePrefab.Asset());
                dependencies.insert(dependencies.end(), sceneResources.begin(), sceneResources.end());
                return Return::Success(std::move(dependencies));
            }

            Result<std::vector<Assets::AssetId>> ResourceDependencies(const Assets::AssetCookPinnedSource &source) {
                using Return = Result<std::vector<Assets::AssetId>>;
                const auto *contribution = catalog.FindContribution(source.record.type, target);
                if (!contribution || !contribution->strategy)
                    return Return::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                const Assets::CookSourceView view{source.record.id,    source.record.type, target,
                                                  source.sourceDigest, source.bytes,       source.record.sourcePath.String()};
                auto declared = contribution->strategy->InputDependencies(view, cancellation);
                if (declared.HasError())
                    return Return::Failure(declared.ErrorValue());
                if (!declared.Value())
                    return Return::Failure(
                        MakeError(PrefabSceneCookErrors::Invalid,
                                  "Resource cooker has no complete input dependency contract: " + contribution->contributionId));
                const auto cache = contribution->strategy->CacheIdentity();
                if (contribution->contributionId.size() > 1024 || cache.version.size() > 1024)
                    return Return::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                auto &identity = providers[source.record.id];
                Append(identity, source.record.type.Value());
                Append(identity, contribution->contributionId);
                Append(identity, cache.version);
                Append(identity, FormatSha256(cache.settingsDigest));
                Append(identity, std::to_string(cache.settingsSchemaVersion));
                return Return::Success(std::move(*std::move(declared).Value()));
            }

            Result<std::vector<Assets::AssetId>> SelectDependencies(const Assets::AssetCookPinnedSource &source) {
                if (source.record.type.Value() == "core.prefab")
                    return PrefabDependencies(source);
                if (source.record.type.Value() == "core.scene")
                    return SceneDependencies(source);
                return ResourceDependencies(source);
            }

            Result<std::vector<Assets::AssetId>> Dependencies(const Assets::AssetCookPinnedSource &source) {
                using Return = Result<std::vector<Assets::AssetId>>;
                auto selected = SelectDependencies(source);
                if (selected.HasError())
                    return selected;
                auto dependencies = std::move(selected).Value();
                if (dependencies.size() > maximum || components.size() > 1024 || behaviors.size() > 1024 ||
                    dependencies.size() > 1'048'576U - edges)
                    return Return::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                edges += dependencies.size();
                std::ranges::sort(dependencies);
                dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
                return Return::Success(std::move(dependencies));
            }

            Result<void> Walk() {
                std::vector<std::pair<Assets::AssetId, bool>> pending{{root, false}};
                while (!pending.empty()) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
                    const auto [asset, leaving] = pending.back();
                    pending.pop_back();
                    if (leaving) {
                        colors[asset] = 2;
                        continue;
                    }
                    const auto prior = colors.find(asset);
                    if (prior != colors.end()) {
                        if (prior->second == 1)
                            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::CyclicComposition));
                        continue;
                    }
                    if (colors.size() >= maximum)
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                    const auto *source = inputs.Find(asset);
                    if (!asset.IsValid() || !source)
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                    colors.emplace(asset, 1);
                    auto dependencies = Dependencies(*source);
                    if (dependencies.HasError())
                        return Result<void>::Failure(dependencies.ErrorValue());
                    pending.emplace_back(asset, true);
                    for (auto it = dependencies.Value().rbegin(); it != dependencies.Value().rend(); ++it)
                        pending.emplace_back(*it, false);
                }
                return Result<void>::Success();
            }
        };

        /** @brief Admits actual used project schemas before committing any descriptor digest into cook identity. */
        Result<void> AppendBuiltinSchemas(const Closure &closure, std::string &identity) {
            if (!closure.nativeScene && closure.builtins.empty())
                return Result<void>::Success();
            Append(identity, "horo.prefab.cook.native-scene-components");
            Append(identity, FormatSha256(SceneSource::PrefabBuiltinComponentCodecFingerprint()));
            for (const auto &type : closure.builtins) {
                if (!Runtime::FindBuiltinSceneComponent(type.Value()))
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                Append(identity, type.Value());
            }
            return Result<void>::Success();
        }

        Result<void> AppendUsedSchemas(const Closure &closure, const std::shared_ptr<const PrefabCookSchemaContext> &schemas,
                                       std::string &identity) {
            if (auto native = AppendBuiltinSchemas(closure, identity); native.HasError())
                return native;
            if (closure.components.empty() && closure.behaviors.empty())
                return Result<void>::Success();
            if (!schemas)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            const std::vector<Gameplay::ComponentTypeId> components(closure.components.begin(), closure.components.end());
            const std::vector<Gameplay::BehaviorTypeId> behaviors(closure.behaviors.begin(), closure.behaviors.end());
            const auto digest = schemas->DigestForTypes(components, behaviors);
            if (digest.HasError())
                return Result<void>::Failure(digest.ErrorValue());
            Append(identity, FormatSha256(digest.Value()));
            return Result<void>::Success();
        }

        /** @brief Builds one canonical source/provider/schema key from the already validated dependency closure. */
        Result<SemanticCookInputs> CaptureIdentity(const Closure &closure, const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                   const AssetCookTargetId &target,
                                                   const std::shared_ptr<const PrefabCookSchemaContext> &schemas) {
            SemanticCookInputs result;
            for (const auto &[asset, color] : closure.colors)
                result.sources.push_back(asset);
            std::string identity{"horo.prefab.cook.root-inputs.v1"};
            Append(identity, target.Value());
            Append(identity, FormatSha256(resolver.ValueSchemaRevision()));
            for (const auto &[asset, provider] : closure.providers) {
                Append(identity, asset.ToString());
                Append(identity, provider);
            }
            if (auto admitted = AppendUsedSchemas(closure, schemas, identity); admitted.HasError())
                return Result<SemanticCookInputs>::Failure(admitted.ErrorValue());
            result.producerSettings = ComputeSha256(std::as_bytes(std::span{identity}));
            return Result<SemanticCookInputs>::Success(std::move(result));
        }

    }  // namespace

    /** @copydoc CaptureSemanticCookInputs */
    Result<SemanticCookInputs> CaptureSemanticCookInputs(const Assets::AssetCookInputSnapshot &inputs,
                                                         const Prefab::PrefabDependencyGraphSnapshot &graph,
                                                         const Prefab::PrefabSourceResolverSnapshot &resolver, const Assets::AssetId root,
                                                         const std::shared_ptr<const PrefabCookSchemaContext> &schemas,
                                                         const Assets::CookerCatalogSnapshot &catalog, const AssetCookTargetId &target,
                                                         const std::size_t maximumInputs, const CancellationToken &cancellation,
                                                         const std::span<const Assets::AssetId> sceneResources) {
        if (maximumInputs == 0 || maximumInputs > 16U * 1024U || graph.RegistryRevision() != inputs.Registry().Revision() ||
            resolver.RegistryRevision() != inputs.Registry().Revision())
            return Result<SemanticCookInputs>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        try {
            Closure closure{inputs, graph, resolver, catalog, target, maximumInputs, cancellation, root, sceneResources};
            if (auto walked = closure.Walk(); walked.HasError())
                return Result<SemanticCookInputs>::Failure(walked.ErrorValue());
            return CaptureIdentity(closure, resolver, target, schemas);
        } catch (const std::bad_alloc &) {
            return Result<SemanticCookInputs>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
        } catch (const std::exception &) {
            return Result<SemanticCookInputs>::Failure(
                MakeError(PrefabSceneCookErrors::Invalid, "Resource dependency provider failed during immutable input preparation."));
        } catch (...) {
            return Result<SemanticCookInputs>::Failure(
                MakeError(PrefabSceneCookErrors::Invalid, "Resource dependency provider failed during immutable input preparation."));
        }
    }
}  // namespace Horo::Application::PrefabCookDetail

#include "PrefabCookSemanticInputs.h"

#include "Horo/Prefab/PrefabErrors.h"
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
            std::set<Gameplay::BehaviorTypeId> behaviors;
            std::map<Assets::AssetId, std::uint8_t> colors;
            std::map<Assets::AssetId, std::string> providers;
            std::size_t edges{};

            Result<std::vector<Assets::AssetId>> Dependencies(const Assets::AssetCookPinnedSource &source) {
                std::vector<Assets::AssetId> dependencies;
                if (source.record.type.Value() == "core.prefab") {
                    const auto found = std::ranges::find_if(resolver.Sources(), [&](const auto &entry) {
                        return entry.document.Data().assetId == source.record.id;
                    });
                    if (found == resolver.Sources().end() || !graph.FindNode(source.record.id))
                        return Result<std::vector<Assets::AssetId>>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                    for (const auto &object : found->document.Data().objects) {
                        for (const auto &component : object.components)
                            components.insert(component.component.typeId);
                        for (const auto &behavior : object.behaviors)
                            behaviors.insert(behavior.typeId);
                    }
                    for (const auto &edge : graph.DirectDependencies(source.record.id))
                        dependencies.push_back(edge.targetAsset);
                } else if (source.record.type.Value() == "core.scene") {
                    if (source.record.id != root)
                        return Result<std::vector<Assets::AssetId>>::Failure(
                            MakeError(PrefabSceneCookErrors::Invalid,
                                      "A resource producer cannot consume an undeclared Scene conversion context."));
                    const auto parsed =
                        SceneSource::DecodeSceneSource({reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()});
                    if (parsed.HasError())
                        return Result<std::vector<Assets::AssetId>>::Failure(parsed.ErrorValue());
                    for (const auto &object : parsed.Value().objects) {
                        for (const auto &component : object.components.gameplayComponents)
                            components.insert(component.typeId);
                        for (const auto &behavior : object.components.behaviors)
                            behaviors.insert(behavior.typeId);
                    }
                    for (const auto &placement : parsed.Value().prefabInstances)
                        dependencies.push_back(placement.sourcePrefab.Asset());
                    dependencies.insert(dependencies.end(), sceneResources.begin(), sceneResources.end());
                } else {
                    const auto *contribution = catalog.FindContribution(source.record.type, target);
                    if (!contribution || !contribution->strategy)
                        return Result<std::vector<Assets::AssetId>>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                    const Assets::CookSourceView view{source.record.id,    source.record.type, target,
                                                      source.sourceDigest, source.bytes,       source.record.sourcePath.String()};
                    auto declared = contribution->strategy->InputDependencies(view, cancellation);
                    if (declared.HasError())
                        return Result<std::vector<Assets::AssetId>>::Failure(declared.ErrorValue());
                    if (!declared.Value())
                        return Result<std::vector<Assets::AssetId>>::Failure(
                            MakeError(PrefabSceneCookErrors::Invalid,
                                      "Resource cooker has no complete input dependency contract: " + contribution->contributionId));
                    dependencies = std::move(*declared.Value());
                    const auto cache = contribution->strategy->CacheIdentity();
                    if (contribution->contributionId.size() > 1024 || cache.version.size() > 1024)
                        return Result<std::vector<Assets::AssetId>>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                    auto &identity = providers[source.record.id];
                    Append(identity, source.record.type.Value());
                    Append(identity, contribution->contributionId);
                    Append(identity, cache.version);
                    Append(identity, FormatSha256(cache.settingsDigest));
                    Append(identity, std::to_string(cache.settingsSchemaVersion));
                }
                if (dependencies.size() > maximum || components.size() > 1024 || behaviors.size() > 1024 ||
                    dependencies.size() > 1'048'576U - edges)
                    return Result<std::vector<Assets::AssetId>>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                edges += dependencies.size();
                std::ranges::sort(dependencies);
                dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
                return Result<std::vector<Assets::AssetId>>::Success(std::move(dependencies));
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
            SemanticCookInputs result;
            for (const auto &[asset, color] : closure.colors)
                result.sources.push_back(asset);
            std::string identity{"horo.prefab.cook.root-inputs.v1"};
            Append(identity, target.Value());
            for (const auto &[asset, provider] : closure.providers) {
                Append(identity, asset.ToString());
                Append(identity, provider);
            }
            if (!closure.components.empty() || !closure.behaviors.empty()) {
                if (!schemas)
                    return Result<SemanticCookInputs>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                const std::vector<Gameplay::ComponentTypeId> components(closure.components.begin(), closure.components.end());
                const std::vector<Gameplay::BehaviorTypeId> behaviors(closure.behaviors.begin(), closure.behaviors.end());
                const auto digest = schemas->DigestForTypes(components, behaviors);
                if (digest.HasError())
                    return Result<SemanticCookInputs>::Failure(digest.ErrorValue());
                Append(identity, FormatSha256(digest.Value()));
            }
            result.producerSettings = ComputeSha256(std::as_bytes(std::span{identity}));
            return Result<SemanticCookInputs>::Success(std::move(result));
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

#include "BlackboardUsageSceneSources.h"

#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>

namespace Horo::Application::BlackboardUsageSources {
    namespace {
        constexpr std::size_t MaximumPlacementResolutions = 64;

        /** @brief Exact placement selected from one immutable Scene source. */
        struct ScenePlacement final {
            const Assets::AssetRecord *scene;
            Assets::AssetId prefab;
            Prefab::PrefabInstanceId instance;
        };

        /** @brief Admits simple Scene placements without interpreting instance override records. */
        Result<void> CollectPlacements(BlackboardUsageReview &review, const Assets::AssetRecord &file,
                                       std::span<const SceneSource::ScenePrefabInstance> authored,
                                       std::vector<ScenePlacement> &placements) {
            std::vector<Prefab::PrefabInstanceId> ids;
            ids.reserve(authored.size());
            for (const auto &placement : authored)
                ids.push_back(placement.instanceId);
            std::ranges::sort(ids);
            if ((!ids.empty() && !ids.front().IsValid()) || std::ranges::adjacent_find(ids) != ids.end())
                return AddGap(review, {BlackboardUsageGapKind::SceneSource, file.id, {}, MakeError(DecisionCookErrors::Invalid)});
            bool hadOverrides = false;
            bool exceededBudget = false;
            for (const auto &placement : authored) {
                if (placement.valueOverrides) {
                    hadOverrides = true;
                    continue;
                }
                if (placements.size() == MaximumPlacementResolutions) {
                    exceededBudget = true;
                    break;
                }
                placements.push_back({&file, placement.sourcePrefab.Asset(), placement.instanceId});
            }
            if (hadOverrides)
                if (auto gap = AddGap(review, {BlackboardUsageGapKind::SceneSource, file.id}); gap.HasError())
                    return gap;
            if (exceededBudget)
                return AddGap(review,
                              {BlackboardUsageGapKind::SceneSource, file.id, {}, MakeError(Prefab::PrefabErrors::WorkBudgetExceeded)});
            return Result<void>::Success();
        }

        /** @brief Records a direct schema link without asserting a particular key or version. */
        Result<void> AddBinding(BlackboardUsageReview &review, const Assets::AssetRecord &file, const BlackboardSchemaBindingObject &object,
                                const AI::AiControllerComponent &controller) {
            if (controller.blackboardSchema != review.selection.schema)
                return Result<void>::Success();
            if (review.schemaBindings.size() == MaximumFindings)
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
            review.schemaBindings.push_back(
                {file.id, std::string{file.sourcePath.String()}, object, controller.controller, controller.decisionAsset});
            return Result<void>::Success();
        }

        /** @brief Decodes the Scene source without preparing a runtime Scene or resolving prefab placements. */
        Result<void> InspectScene(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file, std::string_view bytes,
                                  std::vector<ScenePlacement> &placements, const CancellationToken &cancellation) {
            const auto decoded = SceneSource::DecodeSceneSource(bytes);
            if (decoded.HasError())
                return AddGap(review, {BlackboardUsageGapKind::SceneSource, file.record.id, {}, decoded.ErrorValue()});

            const auto &scene = decoded.Value();
            std::vector<SceneSource::SceneObjectId> objectIds;
            objectIds.reserve(scene.objects.size());
            for (const auto &object : scene.objects)
                objectIds.push_back(object.id);
            std::ranges::sort(objectIds);
            if ((!objectIds.empty() && !objectIds.front().IsValid()) || std::ranges::adjacent_find(objectIds) != objectIds.end())
                return AddGap(review, {BlackboardUsageGapKind::SceneSource, file.record.id, {}, MakeError(DecisionCookErrors::Invalid)});

            bool opaque = !scene.unsupportedComponentOwners.empty();
            for (const auto &object : scene.objects) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
                const auto &components = object.components;
                opaque |= !components.gameplayComponents.empty() || !components.behaviors.empty();
                if (components.aiController) {
                    if (auto added = AddBinding(review, file.record, object.id, *components.aiController); added.HasError())
                        return added;
                }
            }
            if (opaque)
                if (auto gap = AddGap(review, {BlackboardUsageGapKind::SceneSource, file.record.id}); gap.HasError())
                    return gap;
            return CollectPlacements(review, file.record, scene.prefabInstances, placements);
        }

        /** @brief Uses the runtime source codec solely for one portable AI controller component envelope. */
        Result<void> InspectPrefabObject(BlackboardUsageReview &review, const Assets::AssetRecord &file,
                                         const Prefab::PrefabObjectNode &object, const BlackboardSchemaBindingObject &address,
                                         bool &opaque) {
            Runtime::RuntimeComponentSet candidate;
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
            for (const auto &component : object.components) {
                const auto kind = Runtime::FindBuiltinSceneComponent(component.component.typeId.Value());
                opaque |= !kind.has_value();
                if (kind != Runtime::BuiltinSceneComponentKind::AiController)
                    continue;
                const auto projected = SceneSource::ProjectPrefabBuiltinComponent(component, candidate, primitive);
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
            }
            opaque |= !object.behaviors.empty();
            return candidate.aiController ? AddBinding(review, file, address, *candidate.aiController) : Result<void>::Success();
        }

        /** @brief Reviews concrete authored object fields and keeps composition and extension coverage explicit. */
        Result<void> InspectPrefab(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file, std::string_view bytes,
                                   const CancellationToken &cancellation) {
            // Review uses the engine read ceilings. It does not authorize a project policy or a runtime expansion.
            const auto profile = Prefab::PrefabLimitProfile::Create({});
            if (profile.HasError())
                return Result<void>::Failure(profile.ErrorValue());
            const auto decoded = Prefab::PrefabDocument::Parse(bytes, profile.Value());
            if (decoded.HasError())
                return AddGap(review, {BlackboardUsageGapKind::PrefabSource, file.record.id, {}, decoded.ErrorValue()});
            const auto &prefab = decoded.Value().Data();
            if (prefab.assetId != file.record.id)
                return AddGap(review, {BlackboardUsageGapKind::PrefabSource, file.record.id, {}, MakeError(DecisionCookErrors::Stale)});

            bool opaque = prefab.composition.has_value();
            const auto begin = review.schemaBindings.size();
            for (const auto &object : prefab.objects) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
                if (auto inspected = InspectPrefabObject(review, file.record, object, object.localId, opaque); inspected.HasError()) {
                    review.schemaBindings.resize(begin);
                    if (inspected.ErrorValue().code.Value() == AI::AIErrors::DecisionAssetLimitExceeded.code.Value())
                        return inspected;
                    return AddGap(review, {BlackboardUsageGapKind::PrefabSource, file.record.id, {}, inspected.ErrorValue()});
                }
            }
            return opaque ? AddGap(review, {BlackboardUsageGapKind::PrefabSource, file.record.id}) : Result<void>::Success();
        }

        /** @brief Builds the normal strict source resolver using only bytes and sidecars in the frozen capture. */
        Result<Prefab::PrefabSourceResolverSnapshot> BuildResolver(const Assets::AssetCookInputSnapshot &owners, const HoroVersion &version,
                                                                   const Prefab::PrefabLimitProfile &limits,
                                                                   const CancellationToken &cancellation) {
            using Resolver = Result<Prefab::PrefabSourceResolverSnapshot>;
            std::vector<Prefab::PrefabDependencySource> sources;
            for (const auto &file : owners.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Resolver::Failure(MakeError(DecisionCookErrors::Cancelled));
                if (OwnerKind(file.record.type.Value()) != BlackboardUsageGapKind::PrefabSource)
                    continue;
                const std::string_view bytes{reinterpret_cast<const char *>(file.bytes.data()), file.bytes.size()};
                auto document = Prefab::PrefabDocument::Parse(bytes, limits, {.expectedProjectVersion = version});
                // An unavailable source cannot satisfy any strict root or dependency edge.
                if (document.HasError() || document.Value().Data().assetId != file.record.id)
                    continue;
                auto canonical = document.Value().SerializeCanonical();
                if (canonical.HasError())
                    return Resolver::Failure(canonical.ErrorValue());
                const auto digest = ComputeSha256(std::as_bytes(std::span{canonical.Value()}));
                sources.push_back({std::move(document).Value(), {version, digest}});
            }
            return Prefab::BuildPrefabSourceResolverSnapshot(owners.Registry(), std::move(sources), limits);
        }

        /** @brief Records one unavailable resolution per affected Scene, preserving direct source evidence. */
        Result<void> PlacementGaps(BlackboardUsageReview &review, std::span<const ScenePlacement> placements, const Error &error) {
            std::vector<Assets::AssetId> scenes;
            for (const auto &placement : placements)
                scenes.push_back(placement.scene->id);
            std::ranges::sort(scenes);
            const auto unique = std::ranges::unique(scenes);
            for (const auto &scene : std::ranges::subrange(scenes.begin(), unique.begin()))
                if (auto gap = AddGap(review, {BlackboardUsageGapKind::SceneSource, scene, {}, error}); gap.HasError())
                    return gap;
            return Result<void>::Success();
        }

        /** @brief Records schema associations for the complete resolver-owned placement candidate. */
        Result<void> InspectPlacement(BlackboardUsageReview &review, const ScenePlacement &placement,
                                      const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits,
                                      const CancellationToken &cancellation) {
            const auto resolved = resolver.Resolve(placement.prefab, placement.instance, limits, cancellation);
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
            if (resolved.HasError())
                return AddGap(review, {BlackboardUsageGapKind::SceneSource, placement.scene->id, {}, resolved.ErrorValue()});
            bool opaque = false;
            const auto begin = review.schemaBindings.size();
            for (const auto &object : resolved.Value().Objects()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
                const BlackboardPlacedPrefabObject address{placement.prefab, object.sourcePrefab, object.key};
                if (auto inspected = InspectPrefabObject(review, *placement.scene, object.object, address, opaque); inspected.HasError()) {
                    review.schemaBindings.resize(begin);
                    if (inspected.ErrorValue().code.Value() == AI::AIErrors::DecisionAssetLimitExceeded.code.Value())
                        return inspected;
                    return AddGap(review, {BlackboardUsageGapKind::SceneSource, placement.scene->id, {}, inspected.ErrorValue()});
                }
            }
            return opaque ? AddGap(review, {BlackboardUsageGapKind::SceneSource, placement.scene->id}) : Result<void>::Success();
        }

        /** @brief Captures and rechecks the durable project version and exact Prefab policy used by this resolver. */
        Result<void> InspectPlacements(BlackboardUsageReview &review, const std::filesystem::path &root,
                                       const Assets::AssetCookInputSnapshot &owners, std::span<const ScenePlacement> placements,
                                       const CancellationToken &cancellation) {
            if (placements.empty())
                return Result<void>::Success();
            const auto project = LoadProjectSourceDocument(root);
            if (project.HasError())
                return PlacementGaps(review, placements, project.ErrorValue());
            const auto policy = LoadPrefabProjectSettings(root);
            if (policy.HasError())
                return PlacementGaps(review, placements, policy.ErrorValue());
            const auto &limits = policy.Value().settings->Limits();
            const auto resolver = BuildResolver(owners, project.Value().metadata.horoVersion.value, limits, cancellation);
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
            if (resolver.HasError()) {
                if (auto gaps = PlacementGaps(review, placements, resolver.ErrorValue()); gaps.HasError())
                    return gaps;
            } else {
                for (const auto &placement : placements)
                    if (auto inspected = InspectPlacement(review, placement, resolver.Value(), limits, cancellation); inspected.HasError())
                        return inspected;
            }

            const auto currentProject = LoadProjectSourceDocument(root);
            const auto currentPolicy = LoadPrefabProjectSettings(root);
            const auto &fingerprint = project.Value().sourceFingerprint;
            if (!fingerprint || currentProject.HasError() || currentProject.Value().sourceFingerprint != fingerprint ||
                currentPolicy.HasError() || currentPolicy.Value().identity != policy.Value().identity)
                return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
            const std::array evidence{review.closure, *fingerprint, policy.Value().identity.digest};
            review.closure = ComputeSha256(std::as_bytes(std::span{evidence}));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OwnerKind */
    std::optional<BlackboardUsageGapKind> OwnerKind(std::string_view assetType) noexcept {
        if (assetType == "core.scene")
            return BlackboardUsageGapKind::SceneSource;
        if (assetType == "core.prefab")
            return BlackboardUsageGapKind::PrefabSource;
        return std::nullopt;
    }

    /** @copydoc AddGap */
    Result<void> AddGap(BlackboardUsageReview &review, BlackboardUsageGap gap) {
        if (review.gaps.size() == MaximumFindings)
            return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
        review.gaps.push_back(std::move(gap));
        return Result<void>::Success();
    }

    /** @copydoc Inspect */
    Result<void> Inspect(BlackboardUsageReview &review, const std::filesystem::path &root, const Assets::AssetCookInputSnapshot &owners,
                         const CancellationToken &cancellation) {
        std::vector<ScenePlacement> placements;
        for (const auto &file : owners.Sources()) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
            const std::string_view bytes{reinterpret_cast<const char *>(file.bytes.data()), file.bytes.size()};
            const auto result = OwnerKind(file.record.type.Value()) == BlackboardUsageGapKind::SceneSource
                                    ? InspectScene(review, file, bytes, placements, cancellation)
                                    : InspectPrefab(review, file, bytes, cancellation);
            if (result.HasError())
                return result;
        }
        return InspectPlacements(review, root, owners, placements, cancellation);
    }
}  // namespace Horo::Application::BlackboardUsageSources

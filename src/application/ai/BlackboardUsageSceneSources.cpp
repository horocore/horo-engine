#include "BlackboardUsageSceneSources.h"

#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>

namespace Horo::Application::BlackboardUsageSources {
    namespace {
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
                                  const CancellationToken &cancellation) {
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

            bool opaque = !scene.unsupportedComponentOwners.empty() || !scene.prefabInstances.empty();
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
            return opaque ? AddGap(review, {BlackboardUsageGapKind::SceneSource, file.record.id}) : Result<void>::Success();
        }

        /** @brief Uses the runtime source codec solely for one portable AI controller component envelope. */
        Result<void> InspectPrefabObject(BlackboardUsageReview &review, const Assets::AssetRecord &file,
                                         const Prefab::PrefabObjectNode &object, bool &opaque) {
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
            return candidate.aiController ? AddBinding(review, file, object.localId, *candidate.aiController) : Result<void>::Success();
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
                if (auto inspected = InspectPrefabObject(review, file.record, object, opaque); inspected.HasError()) {
                    review.schemaBindings.resize(begin);
                    if (inspected.ErrorValue().code.Value() == AI::AIErrors::DecisionAssetLimitExceeded.code.Value())
                        return inspected;
                    return AddGap(review, {BlackboardUsageGapKind::PrefabSource, file.record.id, {}, inspected.ErrorValue()});
                }
            }
            return opaque ? AddGap(review, {BlackboardUsageGapKind::PrefabSource, file.record.id}) : Result<void>::Success();
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
    Result<void> Inspect(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file, const CancellationToken &cancellation) {
        const std::string_view bytes{reinterpret_cast<const char *>(file.bytes.data()), file.bytes.size()};
        if (OwnerKind(file.record.type.Value()) == BlackboardUsageGapKind::SceneSource)
            return InspectScene(review, file, bytes, cancellation);
        return InspectPrefab(review, file, bytes, cancellation);
    }
}  // namespace Horo::Application::BlackboardUsageSources

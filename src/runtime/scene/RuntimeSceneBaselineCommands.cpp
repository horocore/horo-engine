#include "Horo/Assets/AssetCook.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc SceneCommandBuffer::AttachBaseline */
    Result<void> SceneCommandBuffer::AttachBaseline(RuntimeSceneDefinition definition, std::vector<RuntimeGroupAssetLease> resources,
                                                    const SceneStructuralAdmission &admission, const SceneBaselineAttachmentLimits limits,
                                                    const SceneDefinitionRevision expectedRevision,
                                                    std::shared_ptr<const ScenePublicationCheck> publicationCheck,
                                                    std::shared_ptr<const SceneBaselineOwnership> ownership) {
        if (!publicationCheck || !ownership || !admission.scene.IsValid() || limits.maximumAttachments == 0 ||
            limits.maximumEntities == 0 || limits.maximumResources == 0)
            return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
        if (definition.Entities().size() > limits.maximumEntities || resources.size() > limits.maximumResources)
            return Result<void>::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
        const auto dependencies = definition.AssetDependencies();
        if (resources.size() != dependencies.size())
            return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
        for (std::size_t index = 0; index < dependencies.size(); ++index) {
            if (resources[index].metadata.id != dependencies[index].id ||
                resources[index].metadata.expectedType != dependencies[index].expectedType || resources[index].artifact.Bytes().empty())
                return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
            auto artifact = Assets::DecodeCookedArtifactBytes(resources[index].artifact.Bytes());
            if (artifact.HasError())
                return Result<void>::Failure(artifact.ErrorValue());
            if (artifact.Value().id != dependencies[index].id || artifact.Value().type != dependencies[index].expectedType ||
                artifact.Value().payload.empty())
                return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
        }
        if (admission.cancellation.IsCancellationRequested() || admission.ownerCancellation.IsCancellationRequested())
            return JobCancelled();
        if (const auto valid = publicationCheck->ValidatePublication(); valid.HasError())
            return valid;
        commands_.emplace_back(AttachBaselineCommand{std::move(definition), std::move(resources), admission, limits, expectedRevision,
                                                     std::move(publicationCheck), std::move(ownership)});
        return Result<void>::Success();
    }

    /** @copydoc SceneCommandBuffer::DetachBaseline */
    Result<void> SceneCommandBuffer::DetachBaseline(const SceneDefinitionId id, const SceneDefinitionRevision revision,
                                                    const SceneStructuralAdmission &admission,
                                                    std::shared_ptr<const ScenePublicationCheck> publicationCheck,
                                                    std::shared_ptr<const SceneBaselineOwnership> ownership) {
        if (!id.IsValid() || revision.value == 0 || !publicationCheck || !ownership || !admission.scene.IsValid())
            return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
        if (admission.cancellation.IsCancellationRequested() || admission.ownerCancellation.IsCancellationRequested())
            return JobCancelled();
        if (const auto valid = publicationCheck->ValidatePublication(); valid.HasError())
            return valid;
        commands_.emplace_back(DetachBaselineCommand{id, revision, admission, std::move(publicationCheck), std::move(ownership)});
        return Result<void>::Success();
    }

    /** @copydoc SceneCommandBuffer::ValidateBaselineResources */
    Result<void> SceneCommandBuffer::ValidateBaselineResources(const Assets::AssetRegistry *registry) const {
        for (const auto &command : commands_) {
            const auto *baseline = std::get_if<AttachBaselineCommand>(&command);
            if (!baseline || baseline->resources.empty())
                continue;
            if (!registry)
                return Result<void>::Failure(MakeError(SceneErrors::AssetServicesUnavailable));
            const auto snapshot = registry->Snapshot();
            for (const auto &resource : baseline->resources) {
                const auto *record = snapshot.Find(resource.metadata.id);
                if (!record)
                    return Result<void>::Failure(MakeError(SceneErrors::AssetMissing));
                if (record->type != resource.metadata.expectedType)
                    return Result<void>::Failure(MakeError(SceneErrors::AssetTypeMismatch));
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime

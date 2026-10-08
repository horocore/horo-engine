#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Stores the exact durable cell owner and mounted partition incarnation without retaining host services. */
        class AttachmentOwnership final : public SceneBaselineOwnership {
        public:
            AttachmentOwnership(const SceneCellPayloadIdentity &identity, const WorldStreaming::StreamingFence &fence) noexcept
                : identity_(identity), fence_(fence) {}

            bool Matches(const SceneBaselineOwnership &other) const noexcept override {
                const auto *cell = dynamic_cast<const AttachmentOwnership *>(&other);
                return cell && identity_.partition == cell->identity_.partition && identity_.cell == cell->identity_.cell &&
                       identity_.scene == cell->identity_.scene && fence_.epoch == cell->fence_.epoch;
            }

            bool MatchesOperation(const SceneBaselineOwnership &other) const noexcept override {
                const auto *cell = dynamic_cast<const AttachmentOwnership *>(&other);
                return cell && identity_ == cell->identity_ && fence_ == cell->fence_;
            }

        private:
            SceneCellPayloadIdentity identity_;
            WorldStreaming::StreamingFence fence_;
        };

        /** @brief Retains exact durable content and residency fence until the pending structural transaction retires. */
        class AttachmentPublicationCheck final : public ScenePublicationCheck {
        public:
            AttachmentPublicationCheck(const SceneCellPayloadIdentity &identity, SceneCellAttachmentRequest request,
                                       std::shared_ptr<const SceneCellPayloadAuthority> authority)
                : identity_(identity), request_(std::move(request)), authority_(std::move(authority)) {}

            Result<void> ValidatePublication() const override {
                if (request_.cancellation.IsCancellationRequested() || request_.ownerCancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                return authority_->ValidatePublication(identity_, request_.fence);
            }

        private:
            SceneCellPayloadIdentity identity_;
            SceneCellAttachmentRequest request_;
            std::shared_ptr<const SceneCellPayloadAuthority> authority_;
        };

        /** @brief Rejects invalid or foreign cell/runtime evidence before consuming resources. */
        Result<void> ValidateRequest(const RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                     const SceneCellAttachmentRequest &request,
                                     const std::shared_ptr<const SceneCellPayloadAuthority> &authority) {
            if (!identity.partition.IsValid() || !identity.cell.IsValid() || !identity.scene.IsValid() || identity.revision.value == 0 ||
                !authority || !request.fence.IsValid() || !request.runtime.IsValid())
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
            if (request.fence.partition != identity.partition || request.fence.cell != identity.cell)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
            if (const auto active = service.ActiveScene(); !active || active->RuntimeId() != request.runtime)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc FindRuntimeSceneCellAttachment */
    std::optional<SceneBaselineAttachmentView> FindRuntimeSceneCellAttachment(const RuntimeSceneView scene,
                                                                              const SceneCellPayloadIdentity &identity,
                                                                              const WorldStreaming::PartitionEpoch epoch) noexcept {
        const auto found = scene.FindBaseline(identity.scene);
        if (const AttachmentOwnership expected{identity, {identity.partition, epoch, identity.cell, {}}};
            !found || found->revision != identity.revision || !found->ownership || !found->ownership->Matches(expected))
            return std::nullopt;
        return found;
    }

    /** @copydoc CancelRuntimeSceneCellOperation */
    Result<void> CancelRuntimeSceneCellOperation(RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                                 const SceneCellAttachmentRequest &request) {
        const AttachmentOwnership expected{identity, request.fence};
        return service.CancelPendingBaseline(request.runtime, identity.scene, expected);
    }

    /** @copydoc QueueRuntimeSceneCellAttachment */
    Result<void> QueueRuntimeSceneCellAttachment(RuntimeSceneService &service, const RuntimeSceneCellPayload &payload,
                                                 const SceneCellAttachmentRequest &request, std::vector<RuntimeGroupAssetLease> resources,
                                                 std::shared_ptr<const SceneCellPayloadAuthority> authority) {
        if (const auto valid = ValidateRequest(service, payload.Identity(), request, authority); valid.HasError())
            return valid;
        if (resources.size() > request.limits.maximumResources)
            return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        SceneCommandBuffer commands;
        // Keep cancellation in the cell publication check so deferred commits retain the cell-specific error code.
        if (auto queued =
                commands.AttachBaseline(payload.Definition(), std::move(resources), {request.runtime, request.registry, {}, {}},
                                        request.limits, request.expectedRevision,
                                        std::make_shared<AttachmentPublicationCheck>(payload.Identity(), request, std::move(authority)),
                                        std::make_shared<AttachmentOwnership>(payload.Identity(), request.fence));
            queued.HasError())
            return queued;
        return service.QueueStructuralCommands(std::move(commands));
    }

    /** @copydoc QueueRuntimeSceneCellDetachment */
    Result<void> QueueRuntimeSceneCellDetachment(RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                                 const SceneCellAttachmentRequest &request,
                                                 std::shared_ptr<const SceneCellPayloadAuthority> authority) {
        if (const auto valid = ValidateRequest(service, identity, request, authority); valid.HasError())
            return valid;
        SceneCommandBuffer commands;
        // The publication check owns both cancellation tokens and reports their cell-specific outcome.
        if (auto queued = commands.DetachBaseline(identity.scene, identity.revision, {request.runtime, request.registry, {}, {}},
                                                  std::make_shared<AttachmentPublicationCheck>(identity, request, std::move(authority)),
                                                  std::make_shared<AttachmentOwnership>(identity, request.fence));
            queued.HasError())
            return queued;
        return service.QueueStructuralCommands(std::move(commands));
    }
}  // namespace Horo::Runtime

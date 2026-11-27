#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <algorithm>
#include <new>
#include <thread>
#include <utility>

namespace Horo::Runtime {
    namespace Detail {
        /** @brief Shares the existing Scene receipt and cancellation handle with its cell integration owner. */
        class StreamedSceneInstanceAccess final {
        public:
            static const SceneBaselineOwnership *Ownership(const SceneInstanceOperation &operation) noexcept {
                return operation.ownership_.get();
            }

            static SceneInstanceOperation Make(SceneInstanceAddress address, SceneDefinitionRevision revision,
                                               std::shared_ptr<const SceneBaselineOwnership> ownership,
                                               std::shared_ptr<const SceneStructuralReceipt> receipt) noexcept {
                return SceneInstanceOperation{address, revision, std::move(ownership), std::move(receipt)};
            }

            static bool CanPrepare(const RuntimeSceneService &service, SceneRuntimeId runtime) noexcept {
                return std::this_thread::get_id() == service.restoreRetirementOwner_ && service.NextRuntimeId() == runtime;
            }

            static Result<ScenePublicationReceipt> Prepare(RuntimeSceneService &service, const RuntimeSceneDefinition &parent,
                                                           SceneCommandBuffer commands) {
                return service.QueuePreparationInternal(parent, {}, {}, std::move(commands));
            }
        };
    }  // namespace Detail

    namespace {
        /** @brief Stores durable cell ownership independently of one exact structural attempt. */
        class AttachmentOwnership final : public SceneBaselineOwnership {
        public:
            AttachmentOwnership(const SceneCellPayloadIdentity &identity, const WorldStreaming::StreamingFence &fence,
                                std::optional<SceneInstanceAddress> instance = std::nullopt) noexcept
                : identity_(identity), fence_(fence), instance_(instance) {}

            bool IsCell(const SceneCellPayloadIdentity &identity, const WorldStreaming::PartitionEpoch epoch) const noexcept {
                return identity_.partition == identity.partition && identity_.cell == identity.cell && identity_.scene == identity.scene &&
                       fence_.epoch == epoch;
            }

            const SceneCellPayloadIdentity &Identity() const noexcept {
                return identity_;
            }

            const std::optional<SceneInstanceAddress> &Instance() const noexcept {
                return instance_;
            }

            bool Matches(const SceneBaselineOwnership &other) const noexcept override {
                const auto *cell = dynamic_cast<const AttachmentOwnership *>(&other);
                return cell && IsCell(cell->identity_, cell->fence_.epoch) && instance_ == cell->instance_;
            }

            bool MatchesOperation(const SceneBaselineOwnership &other) const noexcept override {
                if (instance_)
                    return this == &other;
                const auto *cell = dynamic_cast<const AttachmentOwnership *>(&other);
                return cell && !cell->instance_ && identity_ == cell->identity_ && fence_ == cell->fence_;
            }

        private:
            SceneCellPayloadIdentity identity_;
            WorldStreaming::StreamingFence fence_;
            std::optional<SceneInstanceAddress> instance_;
        };

        /** @brief Retains exact durable content and residency fence until the pending structural transaction retires. */
        class AttachmentPublicationCheck final : public ScenePublicationCheck {
        public:
            AttachmentPublicationCheck(const SceneCellPayloadIdentity &identity, const SceneCellAttachmentRequest &request,
                                       std::shared_ptr<const SceneCellPayloadAuthority> authority)
                : identity_(identity), fence_(request.fence), cancellation_(request.cancellation),
                  ownerCancellation_(request.ownerCancellation), authority_(std::move(authority)) {}

            Result<void> ValidatePublication() const override {
                if (cancellation_.IsCancellationRequested() || ownerCancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                return authority_->ValidatePublication(identity_, fence_);
            }

        private:
            SceneCellPayloadIdentity identity_;
            WorldStreaming::StreamingFence fence_;
            CancellationToken cancellation_;
            CancellationToken ownerCancellation_;
            std::shared_ptr<const SceneCellPayloadAuthority> authority_;
        };

        /** @brief Rejects invalid or foreign cell/runtime evidence before consuming resources. */
        Result<void> ValidateCellRequest(const SceneCellPayloadIdentity &identity, const SceneCellAttachmentRequest &request,
                                         const std::shared_ptr<const SceneCellPayloadAuthority> &authority) {
            if (!identity.partition.IsValid() || !identity.cell.IsValid() || !identity.scene.IsValid() || identity.revision.value == 0 ||
                !authority || !request.fence.IsValid() || !request.runtime.IsValid())
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
            if (request.fence.partition != identity.partition || request.fence.cell != identity.cell)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief Requires that ordinary attachment addresses the live runtime, separate from startup admission. */
        Result<void> ValidateRequest(const RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                     const SceneCellAttachmentRequest &request,
                                     const std::shared_ptr<const SceneCellPayloadAuthority> &authority) {
            if (const auto valid = ValidateCellRequest(identity, request, authority); valid.HasError())
                return valid;
            if (const auto active = service.ActiveScene(); !active || active->RuntimeId() != request.runtime)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief Restricts startup materialization to portable data that needs no separate structural subsystem publication. */
        bool IsStartupBaselineSupported(const RuntimeSceneDefinition &definition) {
            if (!definition.AssetDependencies().empty() || !definition.ChildScenes().empty())
                return false;
            RuntimeComponentSet supported;
            return std::ranges::all_of(definition.Entities(), [&](const auto &entity) {
                const auto &components = entity.components;
                supported.camera = components.camera;
                supported.light = components.light;
                return components == supported;
            });
        }

        /** @brief Stages ordinary and Streamed cells through the same bounded structural attach command. */
        Result<void> StageAttach(SceneCommandBuffer &commands, const RuntimeSceneCellPayload &payload,
                                 const SceneCellAttachmentRequest &request, std::vector<RuntimeGroupAssetLease> resources,
                                 std::shared_ptr<const SceneCellPayloadAuthority> authority,
                                 std::shared_ptr<const AttachmentOwnership> ownership) {
            // Keep cancellation in the publication check so deferred commits retain the cell-specific error.
            return commands.AttachBaseline(payload.Definition(), std::move(resources), {request.runtime, request.registry, {}, {}},
                                           request.limits, request.expectedRevision,
                                           std::make_shared<AttachmentPublicationCheck>(payload.Identity(), request, std::move(authority)),
                                           std::move(ownership));
        }

        /** @brief Stages the same baseline eviction and admission fences for both entry points. */
        Result<void> StageDetach(SceneCommandBuffer &commands, const SceneCellPayloadIdentity &identity,
                                 const SceneCellAttachmentRequest &request, std::shared_ptr<const SceneCellPayloadAuthority> authority,
                                 std::shared_ptr<const AttachmentOwnership> ownership) {
            return commands.DetachBaseline(identity.scene, identity.revision, {request.runtime, request.registry, {}, {}},
                                           std::make_shared<AttachmentPublicationCheck>(identity, request, std::move(authority)),
                                           std::move(ownership));
        }

        /** @brief Returns the exact resident operation under the actual current parent. */
        const AttachmentOwnership *ResidentInstance(const RuntimeSceneService &service, const SceneInstanceOperation &current) noexcept {
            const auto *ownership = dynamic_cast<const AttachmentOwnership *>(Detail::StreamedSceneInstanceAccess::Ownership(current));
            if (!ownership || !ownership->Instance())
                return nullptr;
            const auto parent = service.ActiveScene();
            const auto &address = *ownership->Instance();
            if (!parent || parent->RuntimeId() != address.parent)
                return nullptr;
            const auto resident = parent->FindBaseline(address.definition);
            return resident && resident->ownership == ownership ? ownership : nullptr;
        }

        /** @brief Publishes scoped receipt ownership after the one canonical queue accepts the command. */
        Result<SceneInstanceOperation> QueueInstance(RuntimeSceneService &service, SceneCommandBuffer commands,
                                                     std::shared_ptr<const AttachmentOwnership> ownership,
                                                     std::shared_ptr<const SceneGroupPublication> publication) {
            if (publication) {
                if (auto bound = commands.BindBaselinePublication(std::move(publication)); bound.HasError())
                    return Result<SceneInstanceOperation>::Failure(bound.ErrorValue());
            }
            auto queued = service.QueueTrackedStructuralCommands(std::move(commands));
            if (queued.HasError())
                return Result<SceneInstanceOperation>::Failure(queued.ErrorValue());
            return Result<SceneInstanceOperation>::Success(Detail::StreamedSceneInstanceAccess::Make(*ownership->Instance(),
                                                                                                     ownership->Identity().revision,
                                                                                                     ownership, std::move(queued).Value()));
        }
    }  // namespace

    /** @copydoc FindRuntimeSceneCellAttachment */
    std::optional<SceneBaselineAttachmentView> FindRuntimeSceneCellAttachment(const RuntimeSceneView scene,
                                                                              const SceneCellPayloadIdentity &identity,
                                                                              const WorldStreaming::PartitionEpoch epoch) noexcept {
        const auto found = scene.FindBaseline(identity.scene);
        const auto *owner = found ? dynamic_cast<const AttachmentOwnership *>(found->ownership) : nullptr;
        return owner && found->revision == identity.revision && owner->IsCell(identity, epoch) ? found : std::nullopt;
    }

    /** @copydoc QueuePreparationWithInitialStreamedScene */
    Result<ScenePublicationReceipt> QueuePreparationWithInitialStreamedScene(RuntimeSceneService &service,
                                                                             const RuntimeSceneDefinition &parent,
                                                                             const SceneInstanceAddress &address,
                                                                             const RuntimeSceneCellPayload &payload,
                                                                             const SceneCellAttachmentRequest &request,
                                                                             std::shared_ptr<const SceneCellPayloadAuthority> authority) {
        using Outcome = Result<ScenePublicationReceipt>;
        const auto &identity = payload.Identity();
        if (!address.parent.IsValid() || !address.instance.IsValid() || address.definition != identity.scene ||
            address.mode > SceneInstanceMode::Activated)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Invalid));
        if (const auto valid = ValidateCellRequest(identity, request, authority); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (address.parent != request.runtime || request.expectedRevision.value != 0 ||
            !Detail::StreamedSceneInstanceAccess::CanPrepare(service, address.parent))
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Stale));
        const auto &baseline = payload.Definition();
        if (baseline.Entities().size() > request.limits.maximumEntities)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        if (address.mode != SceneInstanceMode::Streamed)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Unsupported));
        try {
            if (!IsStartupBaselineSupported(baseline))
                return Outcome::Failure(MakeError(SceneCellPayloadErrors::Unsupported));
            SceneCommandBuffer commands;
            auto ownership = std::make_shared<const AttachmentOwnership>(identity, request.fence, address);
            if (auto staged = StageAttach(commands, payload, request, {}, std::move(authority), std::move(ownership)); staged.HasError())
                return Outcome::Failure(staged.ErrorValue());
            return Detail::StreamedSceneInstanceAccess::Prepare(service, parent, std::move(commands));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        }
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
        if (auto staged = StageAttach(commands, payload, request, std::move(resources), std::move(authority),
                                      std::make_shared<const AttachmentOwnership>(payload.Identity(), request.fence));
            staged.HasError())
            return staged;
        return service.QueueStructuralCommands(std::move(commands));
    }

    /** @copydoc QueueStreamedSceneInstance */
    Result<SceneInstanceOperation> QueueStreamedSceneInstance(RuntimeSceneService &service, const SceneInstanceAddress &address,
                                                              const RuntimeSceneCellPayload &payload,
                                                              const SceneCellAttachmentRequest &request,
                                                              std::vector<RuntimeGroupAssetLease> resources,
                                                              std::shared_ptr<const SceneCellPayloadAuthority> authority,
                                                              const SceneInstanceOperation *replaces) {
        using Outcome = Result<SceneInstanceOperation>;
        const auto &identity = payload.Identity();
        if (!address.parent.IsValid() || !address.instance.IsValid() || address.definition != identity.scene ||
            address.mode > SceneInstanceMode::Activated)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Invalid));
        if (address.mode != SceneInstanceMode::Streamed)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Unsupported));
        if (const auto valid = ValidateRequest(service, identity, request, authority); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (request.runtime != address.parent)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Stale));

        const auto *prior = replaces ? ResidentInstance(service, *replaces) : nullptr;
        if ((replaces && (!prior || prior->Instance() != address || !prior->IsCell(identity, request.fence.epoch) ||
                          prior->Identity().revision != request.expectedRevision)) ||
            (!replaces && request.expectedRevision.value != 0))
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Stale));

        if (resources.size() > request.limits.maximumResources)
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));

        try {
            SceneCommandBuffer commands;
            auto ownership = std::make_shared<const AttachmentOwnership>(identity, request.fence, address);
            if (auto staged = StageAttach(commands, payload, request, std::move(resources), std::move(authority), ownership);
                staged.HasError())
                return Outcome::Failure(staged.ErrorValue());
            return QueueInstance(service, std::move(commands), std::move(ownership), request.publication);
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        }
    }

    /** @copydoc FindStreamedSceneInstance */
    std::optional<SceneBaselineAttachmentView> FindStreamedSceneInstance(const RuntimeSceneView &parent,
                                                                         const SceneInstanceAddress &address,
                                                                         const SceneCellPayloadIdentity &identity,
                                                                         const WorldStreaming::PartitionEpoch epoch) noexcept {
        if (!parent.IsCurrent() || parent.RuntimeId() != address.parent || address.mode != SceneInstanceMode::Streamed ||
            address.definition != identity.scene)
            return std::nullopt;
        const auto found = FindRuntimeSceneCellAttachment(parent, identity, epoch);
        const auto *ownership = found ? static_cast<const AttachmentOwnership *>(found->ownership) : nullptr;
        return ownership && ownership->Instance() == address ? found : std::nullopt;
    }

    /** @copydoc QueueStreamedSceneRemoval */
    Result<SceneInstanceOperation> QueueStreamedSceneRemoval(RuntimeSceneService &service, const SceneInstanceOperation &current,
                                                             const SceneCellAttachmentRequest &request,
                                                             std::shared_ptr<const SceneCellPayloadAuthority> authority) {
        using Outcome = Result<SceneInstanceOperation>;
        const auto *prior = ResidentInstance(service, current);
        if (!prior || request.runtime != prior->Instance()->parent || !prior->IsCell(prior->Identity(), request.fence.epoch))
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::Stale));
        const auto &identity = prior->Identity();
        if (const auto valid = ValidateRequest(service, identity, request, authority); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            SceneCommandBuffer commands;
            auto removal = std::make_shared<const AttachmentOwnership>(identity, request.fence, prior->Instance());
            if (auto staged = StageDetach(commands, identity, request, std::move(authority), removal); staged.HasError())
                return Outcome::Failure(staged.ErrorValue());
            return QueueInstance(service, std::move(commands), std::move(removal), request.publication);
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        }
    }

    /** @copydoc QueueRuntimeSceneCellDetachment */
    Result<void> QueueRuntimeSceneCellDetachment(RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                                 const SceneCellAttachmentRequest &request,
                                                 std::shared_ptr<const SceneCellPayloadAuthority> authority) {
        if (const auto valid = ValidateRequest(service, identity, request, authority); valid.HasError())
            return valid;
        SceneCommandBuffer commands;
        if (auto staged = StageDetach(commands, identity, request, std::move(authority),
                                      std::make_shared<const AttachmentOwnership>(identity, request.fence));
            staged.HasError())
            return staged;
        return service.QueueStructuralCommands(std::move(commands));
    }
}  // namespace Horo::Runtime

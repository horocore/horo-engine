#include "Horo/Physics/PhysicsCellAttachments.h"

#include <algorithm>
#include <utility>

namespace Horo::Physics {
    namespace W = WorldStreaming;

    namespace {
        /** @brief Retains immutable cooked geometry beside the real native Physics/Character candidate. */
        class PhysicsAttachment final : public Runtime::SceneActivationCandidate {
        public:
            PhysicsAttachment(PhysicsCookedShapeLease shape, std::unique_ptr<Runtime::SceneActivationCandidate> native)
                : shape_(std::move(shape)), native_(std::move(native)) {}

            ~PhysicsAttachment() override {
                Shutdown();
            }

            Result<void> ValidatePublication() const override {
                if (!native_)
                    return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Closed));
                return native_->ValidatePublication();
            }

            void Publish() noexcept override {
                native_->Publish();
            }

            void Shutdown() noexcept override {
                if (native_) {
                    native_->Shutdown();
                    native_.reset();
                }
                shape_.reset();
            }

        private:
            std::optional<PhysicsCookedShapeLease> shape_;
            std::unique_ptr<Runtime::SceneActivationCandidate> native_;
        };

        /** @brief Validates exact semantic address and cooked integrity evidence before constructing the factory. */
        bool Valid(const PhysicsCellAttachment &attachment, const std::uint32_t version) noexcept {
            const auto &reference = attachment.reference;
            const auto &shape = attachment.shape;
            return reference.provider == W::StreamingCellProvider::PhysicsMesh && reference.version == version &&
                   reference.asset.IsValid() && reference.asset == shape.asset && reference.subresource.IsValid() &&
                   reference.subresource.Value() == shape.subresource.Value() && reference.revision.IsValid() && reference.bytes != 0 &&
                   reference.requirement < W::StreamingCellPayloadRequirement::Count && shape.payloadDigest &&
                   reference.digest == *shape.payloadDigest && shape.cacheKeyDigest && shape.target;
        }
    }  // namespace

    /** @copydoc MakePhysicsCellAttachmentProvider */
    Result<Runtime::SceneCellAttachmentProvider> MakePhysicsCellAttachmentProvider(
        W::StreamingRuntimeServiceId identity, W::StreamingRuntimeServiceRevision revision, const std::uint32_t version,
        PhysicsCookedShapeCache &cache, PhysicsSceneActivationParticipant &participant,
        const std::span<const PhysicsCellAttachment> attachments, const std::size_t maximumAttachments) {
        if (!identity.IsValid() || !revision.IsValid() || version == 0 || maximumAttachments == 0 || attachments.empty())
            return Result<Runtime::SceneCellAttachmentProvider>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
        if (attachments.size() > maximumAttachments)
            return Result<Runtime::SceneCellAttachmentProvider>::Failure(MakeError(W::CellAttachmentErrors::CapacityExceeded));
        for (std::size_t index{}; index < attachments.size(); ++index) {
            if (!Valid(attachments[index], version))
                return Result<Runtime::SceneCellAttachmentProvider>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
            for (std::size_t prior{}; prior < index; ++prior)
                if (attachments[prior].reference.asset == attachments[index].reference.asset &&
                    attachments[prior].reference.subresource == attachments[index].reference.subresource)
                    return Result<Runtime::SceneCellAttachmentProvider>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
        }
        Runtime::SceneCellAttachmentFactory factory =
            [&cache, &participant,
             owned = std::vector<PhysicsCellAttachment>{attachments.begin(),
                                                        attachments.end()}](const W::CellAttachmentReference &reference,
                                                                            Assets::AssetPayloadLease bytes,
                                                                            const Runtime::RuntimeSceneDefinition &definition,
                                                                            Runtime::RuntimeSceneView scene)
            -> Result<std::unique_ptr<Runtime::SceneActivationCandidate>> {
            const auto found = std::ranges::find(owned, reference, &PhysicsCellAttachment::reference);
            if (found == owned.end())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(W::CellAttachmentErrors::Stale));
            if (bytes.Bytes().size() != reference.bytes || bytes.Digest() != reference.digest)
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(W::CellAttachmentErrors::Stale));
            const auto payload = bytes.Bytes();
            auto shape = cache.Acquire(found->shape, {reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()});
            if (shape.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(shape.ErrorValue());
            auto native = participant.Prepare(definition, scene);
            if (native.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(native.ErrorValue());
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                std::make_unique<PhysicsAttachment>(std::move(shape).Value(), std::move(native).Value()));
        };
        return Result<Runtime::SceneCellAttachmentProvider>::Success(
            {W::StreamingCellProvider::PhysicsMesh, identity, revision, version, std::move(factory)});
    }
}  // namespace Horo::Physics

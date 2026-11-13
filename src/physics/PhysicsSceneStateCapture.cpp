#include "PhysicsSceneStateStorage.h"

#include <new>
#include <stdexcept>

namespace Horo::Physics::Detail {
    /** @copydoc PreparedPhysicsSceneCapture::Initialize */
    Result<void> PreparedPhysicsSceneCapture::Initialize(const PhysicsStructuralState &state,
                                                         const Runtime::PersistentEntityIdentityMap &identities,
                                                         SceneStateStoragePolicy policy, const Sha256Digest &content,
                                                         std::shared_ptr<const void> contentLease) {
        if (!HasSceneStateContent(content, contentLease))
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        auto projection = PhysicsSceneStateProjection::Build(*state.world, state, identities, policy.projection);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        projection_ = std::make_unique<PhysicsSceneStateProjection>(std::move(projection).Value());
        auto remaining =
            SceneStateRemainingBytes(policy.maximumRetainedBytes, std::array{sizeof(*this), projection_->RetainedPayloadBytes()});
        if (remaining.HasError())
            return Result<void>::Failure(remaining.ErrorValue());
        const auto &bindings = projection_->Bindings();
        const PhysicsStateCaptureConfiguration configuration{.snapshotSlots = policy.snapshotSlots,
                                                             .maximumRetainedBytes = remaining.Value(),
                                                             .content = projection_->ContentFingerprint(content),
                                                             .bodies = bindings.bodies,
                                                             .shapes = bindings.shapes,
                                                             .constraints = bindings.constraints,
                                                             .contentLease = std::move(contentLease),
                                                             .owners = projection_->OwnerRecords()};
        auto configured = state.world->ConfigureStateSnapshots(configuration);
        if (configured.HasError())
            return Result<void>::Failure(configured.ErrorValue());
        configurationBytes_ = configured.Value();
        auto owner = PhysicsWorldStateRestartAccess::ConfigurationOwner(*state.world);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        configuration_ = std::move(owner).Value();
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneStateTarget::PrepareCapture */
    Result<std::shared_ptr<PreparedPhysicsSceneCapture>> PhysicsSceneStateTarget::PrepareCapture(
        const Runtime::PersistentEntityIdentityMap &identities, SceneStateStoragePolicy policy, const Sha256Digest &content,
        std::shared_ptr<const void> contentLease) const {
        using Outcome = Result<std::shared_ptr<PreparedPhysicsSceneCapture>>;
        auto state = CoveredOwner(true);
        if (state.HasError())
            return Outcome::Failure(state.ErrorValue());
        try {
            auto result = std::shared_ptr<PreparedPhysicsSceneCapture>{new PreparedPhysicsSceneCapture{*this}};
            if (auto prepared = result->Initialize(*state.Value(), identities, policy, content, std::move(contentLease));
                prepared.HasError())
                return Outcome::Failure(prepared.ErrorValue());
            return Outcome::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PreparedPhysicsSceneCapture::Capture */
    Result<PhysicsStateSnapshotLease> PreparedPhysicsSceneCapture::Capture() const {
        auto state = target_.CoveredOwner(true);
        if (state.HasError())
            return Result<PhysicsStateSnapshotLease>::Failure(state.ErrorValue());
        return PhysicsWorldStateRestartAccess::CaptureConfigured(*state.Value()->world, configuration_);
    }

    /** @copydoc PreparedPhysicsSceneCapture::RetainedPayloadBytes */
    std::size_t PreparedPhysicsSceneCapture::RetainedPayloadBytes() const noexcept {
        return SceneStatePayloadSum(std::array{sizeof(*this), projection_ ? projection_->RetainedPayloadBytes() : 0, configurationBytes_});
    }
}  // namespace Horo::Physics::Detail

#include "EditorPlaySavePhysicsReceipt.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "PhysicsSceneStateStorage.h"

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsReceipt::PlaySavePhysicsReceipt */
    PlaySavePhysicsReceipt::PlaySavePhysicsReceipt(Physics::Detail::PhysicsSceneStateTarget target, PlaySavePhysicsPolicy policy,
                                                   std::shared_ptr<const PlaySavePhysicsContent> content,
                                                   std::shared_ptr<const std::vector<std::byte>> source,
                                                   Runtime::StagedRestoreContext context, Runtime::RuntimeSceneView scene,
                                                   const Runtime::PersistentEntityIdentityMap &identities, CancellationToken cancellation,
                                                   std::shared_ptr<PlaySavePhysicsRestoreFence> fence)
        : target_(std::move(target)), policy_(policy), content_(std::move(content)), source_(std::move(source)), context_(context),
          scene_(scene), identities_(&identities), cancellation_(std::move(cancellation)), fence_(std::move(fence)) {
        const auto descriptor = Physics::Detail::PhysicsStateSaveDescriptor();
        requirement_ = {descriptor.participant, descriptor.schemaVersion, descriptor.scope};
    }

    /** @copydoc PlaySavePhysicsReceipt::Requirement */
    const Runtime::StagedRestoreParticipantRequirement &PlaySavePhysicsReceipt::Requirement() const noexcept {
        return requirement_;
    }

    /** @copydoc PlaySavePhysicsReceipt::CheckContext */
    Result<void> PlaySavePhysicsReceipt::CheckContext(const Runtime::StagedRestoreContext &context) const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (context != context_ || !scene_.IsCurrent() || scene_.RuntimeId() != target_.Scene())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
        if (cancellation_.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationCancelled));
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsReceipt::Decode */
    Result<void> PlaySavePhysicsReceipt::Decode(const Runtime::StagedRestoreContext &context) {
        if (auto valid = CheckContext(context); valid.HasError())
            return valid;
        if (input_ || state_.prepared || !source_ || !content_)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        auto remaining = Physics::Detail::SceneStateRemainingBytes(policy_.state.maximumRetainedBytes,
                                                                   std::array{sizeof(*this), content_->RetainedPayloadBytes(),
                                                                              Physics::Detail::SceneStatePayloadSum(
                                                                                  std::array{sizeof(*source_), source_->capacity()})});
        if (remaining.HasError())
            return Result<void>::Failure(remaining.ErrorValue());
        auto decoded = target_.DecodeInput(*source_, policy_.maximumWireBytes, remaining.Value());
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        input_ = std::move(decoded).Value();
        source_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsReceipt::Validate */
    Result<void> PlaySavePhysicsReceipt::Validate(const Runtime::StagedRestoreContext &context) {
        if (auto valid = CheckContext(context); valid.HasError())
            return valid;
        if (!input_ || validated_)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        if (auto materials = content_->ValidateMaterials(input_->Construction()); materials.HasError())
            return materials;
        validated_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsReceipt::Instantiate */
    Result<void> PlaySavePhysicsReceipt::Instantiate(const Runtime::StagedRestoreContext &context) {
        if (auto valid = CheckContext(context); valid.HasError())
            return valid;
        if (!validated_ || !input_ || !identities_ || state_.prepared)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        auto remaining = Physics::Detail::SceneStateRemainingBytes(policy_.state.maximumRetainedBytes,
                                                                   std::array{sizeof(*this), content_->RetainedPayloadBytes()});
        if (remaining.HasError())
            return Result<void>::Failure(remaining.ErrorValue());
        auto policy = policy_.state;
        policy.maximumRetainedBytes = remaining.Value();
        auto prepared = target_.PrepareRestore(std::move(input_), *identities_, policy, content_->Fingerprint(), content_, cancellation_);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (auto installed = fence_->Install(prepared.Value()); installed.HasError())
            return installed;
        state_.prepared = std::move(prepared).Value();
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsReceipt::PreparedState */
    const Runtime::ICanonicalRestorePreparedState *PlaySavePhysicsReceipt::PreparedState() const noexcept {
        return state_.prepared ? &state_ : nullptr;
    }

    /** @copydoc PlaySavePhysicsReceipt::CheckNative */
    Result<void> PlaySavePhysicsReceipt::CheckNative() const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!state_.prepared)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        return state_.prepared->CheckPrepared();
    }

    /** @copydoc PlaySavePhysicsReceipt::ApplyState */
    Result<void> PlaySavePhysicsReceipt::ApplyState(const Runtime::ICanonicalRestoreDependencyLookup &) {
        return CheckNative();
    }

    /** @copydoc PlaySavePhysicsReceipt::FixupReferences */
    Result<void> PlaySavePhysicsReceipt::FixupReferences(const Runtime::ICanonicalRestoreDependencyLookup &) {
        return CheckNative();
    }

    /** @copydoc PlaySavePhysicsReceipt::PublishPrepared */
    void PlaySavePhysicsReceipt::PublishPrepared() noexcept {
        // The same prepared native owner already belongs to Scene's unpublished candidate.
        // Canonical BeginCommit has won; the following aggregate Publish performs the only live ownership transfer.
        published_ = true;
    }

    /** @copydoc PlaySavePhysicsReceipt::RollbackPrepared */
    void PlaySavePhysicsReceipt::RollbackPrepared() noexcept {
        if (published_)
            return;
        fence_->Clear(state_.prepared);
        state_.prepared.reset();
        input_.reset();
        source_.reset();
        identities_ = nullptr;
        // SceneRestoreBundle still owns actual pending Scene rollback/native teardown.
    }

    /** @copydoc PlaySavePhysicsReceipt::RetirePublishedTransient */
    Result<bool> PlaySavePhysicsReceipt::RetirePublishedTransient() {
        if (owner_ != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!published_)
            return Result<bool>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        if (retired_)
            return Result<bool>::Success(true);
        fence_->Clear(state_.prepared);
        state_.prepared.reset();
        input_.reset();
        content_.reset();
        source_.reset();
        identities_ = nullptr;
        retired_ = true;
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Application::Internal

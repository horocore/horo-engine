#include "EditorPlaySavePhysicsServices.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "PhysicsSceneStateStorage.h"

#include <new>

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsServices::PlaySavePhysicsServices */
    PlaySavePhysicsServices::PlaySavePhysicsServices(ScenePhysicsHost &native, Runtime::RuntimeSceneService &scenes,
                                                     PlaySavePhysicsPolicy policy) noexcept
        : native_(native), scenes_(scenes), policy_(policy) {}

    /** @copydoc PlaySavePhysicsServices::Create */
    Result<std::shared_ptr<PlaySavePhysicsServices>> PlaySavePhysicsServices::Create(ScenePhysicsHost &native,
                                                                                     Runtime::RuntimeSceneService &scenes,
                                                                                     PlaySavePhysicsPolicy policy) {
        using Outcome = Result<std::shared_ptr<PlaySavePhysicsServices>>;
        if (!policy.maximumWireBytes || policy.maximumWireBytes > Physics::Detail::MaximumPhysicsStateSaveRecordBytes ||
            !policy.state.maximumRetainedBytes || !policy.state.snapshotSlots)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
        try {
            return Outcome::Success(std::shared_ptr<PlaySavePhysicsServices>{new PlaySavePhysicsServices{native, scenes, policy}});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PlaySavePhysicsServices::PrepareCapture */
    Result<void> PlaySavePhysicsServices::PrepareCapture(const Runtime::PersistentEntityIdentityMap &identities) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        const auto scene = scenes_.ActiveScene();
        if (!scene)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        auto target = native_.PublishedStateTarget();
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        if (target.Value().Scene() != scene->RuntimeId())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        auto content = PlaySavePhysicsContent::Capture(*scene, policy_.content);
        if (content.HasError())
            return Result<void>::Failure(content.ErrorValue());
        auto remaining = Physics::Detail::SceneStateRemainingBytes(policy_.state.maximumRetainedBytes,
                                                                   std::array{sizeof(*this), content.Value()->RetainedPayloadBytes()});
        if (remaining.HasError())
            return Result<void>::Failure(remaining.ErrorValue());
        auto statePolicy = policy_.state;
        statePolicy.maximumRetainedBytes = remaining.Value();
        auto prepared = target.Value().PrepareCapture(identities, statePolicy, content.Value()->Fingerprint(), content.Value());
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        snapshot_ = {};
        provenance_.reset();
        content_ = std::move(content).Value();
        capture_ = std::move(prepared).Value();
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsServices::CloseCapture */
    void PlaySavePhysicsServices::CloseCapture() noexcept {
        // Source host serializes owner retirement and retains all native owners through its ordinary Save/Scene drain.
        if (owner_ != std::this_thread::get_id())
            return;
        snapshot_ = {};
        provenance_.reset();
        capture_.reset();
        content_.reset();
    }

    /** @copydoc PlaySavePhysicsServices::RetainedPayloadBytes */
    std::size_t PlaySavePhysicsServices::RetainedPayloadBytes() const noexcept {
        return Physics::Detail::SceneStatePayloadSum(
            std::array{sizeof(*this), content_ ? content_->RetainedPayloadBytes() : 0, capture_ ? capture_->RetainedPayloadBytes() : 0});
    }
}  // namespace Horo::Application::Internal

#include "EditorPlaySavePhysicsServices.h"
#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsRestoreFence::Install */
    Result<void> PlaySavePhysicsRestoreFence::Install(std::shared_ptr<Physics::Detail::PreparedPhysicsSceneRestore> prepared) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!prepared || prepared_)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        prepared_ = std::move(prepared);
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsRestoreFence::Clear */
    void PlaySavePhysicsRestoreFence::Clear(const std::shared_ptr<Physics::Detail::PreparedPhysicsSceneRestore> &prepared) noexcept {
        if (owner_ == std::this_thread::get_id() && prepared_ == prepared)
            prepared_.reset();
    }

    /** @copydoc PlaySavePhysicsRestoreFence::ValidateAndArm */
    Result<void> PlaySavePhysicsRestoreFence::ValidateAndArm() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!prepared_)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        return prepared_->ValidateAndArm();
    }
}  // namespace Horo::Application::Internal

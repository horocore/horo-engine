#include "EditorPlaySavePhysicsReceipt.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <new>

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsServices::StageRestore */
    Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>> PlaySavePhysicsServices::StageRestore(
        std::shared_ptr<const std::vector<std::byte>> source, Runtime::StagedRestoreContext context, Runtime::RuntimeSceneView scene,
        const Runtime::PersistentEntityIdentityMap &identities, const CancellationToken &cancellation,
        std::shared_ptr<PlaySavePhysicsRestoreFence> fence) {
        using Outcome = Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>>;
        if (owner_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!scene.IsCurrent() || !source || !fence || source->size() > policy_.maximumWireBytes)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
        auto target = native_.PendingStateTarget(scene.RuntimeId());
        if (target.HasError())
            return Outcome::Failure(target.ErrorValue());
        auto content = PlaySavePhysicsContent::Capture(scene, policy_.content);
        if (content.HasError())
            return Outcome::Failure(content.ErrorValue());
        try {
            return Outcome::Success(std::make_unique<PlaySavePhysicsReceipt>(std::move(target).Value(), policy_, std::move(content).Value(),
                                                                             std::move(source), context, scene, identities, cancellation,
                                                                             std::move(fence)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Application::Internal

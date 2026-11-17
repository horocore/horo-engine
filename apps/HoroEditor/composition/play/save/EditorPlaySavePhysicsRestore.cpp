#include "EditorPlaySavePhysicsReceipt.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <new>

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsServices::StageRestore */
    Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>> PlaySavePhysicsServices::StageRestore(
        std::shared_ptr<const std::vector<std::byte>> source, const Runtime::StagedRestoreContext &contextInput,
        const Runtime::RuntimeSceneView &sceneInput, const Runtime::PersistentEntityIdentityMap &identities,
        const CancellationToken &cancellation, std::shared_ptr<PlaySavePhysicsRestoreFence> fence) {
        // Own both descriptors before native/provider access; the receipt retains its own copies.
        const PlaySavePhysicsRestoreScene restore{contextInput, sceneInput, identities};
        using Outcome = Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>>;
        if (owner_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!restore.scene.IsCurrent() || !source || !fence || source->size() > policy_.maximumWireBytes)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
        auto target = native_.PendingStateTarget(restore.scene.RuntimeId());
        if (target.HasError())
            return Outcome::Failure(target.ErrorValue());
        auto content = PlaySavePhysicsContent::Capture(restore.scene, policy_.content);
        if (content.HasError())
            return Outcome::Failure(content.ErrorValue());
        try {
            return Outcome::Success(std::make_unique<PlaySavePhysicsReceipt>(std::move(target).Value(), policy_, std::move(content).Value(),
                                                                             std::move(source), restore, cancellation, std::move(fence)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Application::Internal

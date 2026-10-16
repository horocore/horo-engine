#include "CollisionProjectSettingsSessionState.h"
#include "CollisionSourceInventory.h"

namespace Horo::Editor {
    /** @copydoc CollisionProjectSettingsSession::State::Fresh */
    Result<void> CollisionProjectSettingsSession::State::Fresh(const CollisionSettingsReview::State &review) const {
        if (const auto admitted = Admit(); admitted.HasError())
            return admitted;
        if (review.opening.lock() != opening || review.revision != revision || review.inventory != inventory ||
            review.activation != activation || review.registryRevision != registry->Snapshot().Revision())
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        if (auto physical = ValidateCollisionSourceInventory(root, review.registrySnapshot); physical.HasError())
            return physical;
        const auto collision = Assets::CaptureAssetSource(root, root / ".horo/collision.json", 4 * 1024 * 1024);
        if (collision.HasError())
            return Result<void>::Failure(collision.ErrorValue());
        if (collision.Value().identity != saved.identity)
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        if (review.activeScene) {
            if (!activeScene)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Closed)));
            auto current = activeScene->Snapshot();
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().session != review.activeScene->session || current.Value().revision != review.activeScene->revision ||
                current.Value().state != review.activeScene->state || current.Value().dirty != review.activeScene->dirty)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        }
        for (const auto &fence : review.sources) {
            const auto current = Assets::CaptureAssetSource(root, fence.path, 16 * 1024 * 1024);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().identity != fence.identity)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Editor

#include "RuntimeUiAccessState.h"
#include "UiAnimationPublisher.h"
#include "UiSceneReconciliationInternal.h"

#include <exception>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    struct RuntimeUiService::ScenePublication::Storage final {
        Storage(std::shared_ptr<Detail::RuntimeUiServiceState> state, UiSceneReconciliation::Prepared prepared) noexcept
            : state(std::move(state)), prepared(std::move(prepared)) {}

        ~Storage() {
            RuntimeUiService::ReleasePublication(*this);
        }

        std::shared_ptr<Detail::RuntimeUiServiceState> state;
        UiSceneReconciliation::Prepared prepared;
        bool sealed{};
        bool published{};
    };

    /** @copydoc RuntimeUiService::PreparePublication */
    Result<RuntimeUiService::ScenePublication> RuntimeUiService::PreparePublication(UiSceneReconciliation::Prepared prepared) {
        using Publication = Result<ScenePublication>;
        if (const auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Publication::Failure(valid.ErrorValue());
        if (!prepared.storage_ || !state_->registry.storage_.Matches(prepared.storage_->owner) || prepared.storage_->consumed ||
            prepared.storage_->abandoned)
            return Publication::Failure(MakeError(UiErrors::HandleOwnerMismatch));
        try {
            return Publication::Success(ScenePublication{std::make_unique<ScenePublication::Storage>(state_, std::move(prepared))});
        } catch (const std::bad_alloc &) {
            return Publication::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc RuntimeUiService::ScenePublication::ScenePublication */
    RuntimeUiService::ScenePublication::ScenePublication(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc RuntimeUiService::ScenePublication::~ScenePublication */
    RuntimeUiService::ScenePublication::~ScenePublication() = default;
    /** @copydoc RuntimeUiService::ScenePublication::ScenePublication */
    RuntimeUiService::ScenePublication::ScenePublication(ScenePublication &&) noexcept = default;
    /** @copydoc RuntimeUiService::ScenePublication::operator= */
    RuntimeUiService::ScenePublication &RuntimeUiService::ScenePublication::operator=(ScenePublication &&) noexcept = default;

    /** @copydoc RuntimeUiService::ScenePublication::Seal */
    Result<void> RuntimeUiService::ScenePublication::Seal(const UiStructuralCommitPoint point) {
        if (!storage_)
            return Result<void>::Failure(MakeError(UiErrors::InstanceStateInvalid));
        return RuntimeUiService::SealPublication(*storage_, point);
    }

    /** @copydoc RuntimeUiService::ScenePublication::CanPublish */
    Result<void> RuntimeUiService::ScenePublication::CanPublish() const {
        if (!storage_)
            return Result<void>::Failure(MakeError(UiErrors::InstanceStateInvalid));
        if (storage_->state->thread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::CapabilityWrongThread));
        if (!storage_->sealed || storage_->published || storage_->prepared.storage_->abandoned || !storage_->state->active ||
            storage_->state->shutdownRequested)
            return Result<void>::Failure(MakeError(UiErrors::CapabilityRevoked));
        if (storage_->prepared.storage_->cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiErrors::AssetLoadCancelled));
        if (storage_->prepared.storage_->revision != storage_->state->registry.storage_.Get()->revision)
            return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiService::SealPublication */
    Result<void> RuntimeUiService::SealPublication(ScenePublication::Storage &storage, const UiStructuralCommitPoint point) {
        if (storage.state->thread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::CapabilityWrongThread));
        if (storage.sealed || storage.published)
            return Result<void>::Failure(MakeError(UiErrors::InstanceStateInvalid));
        if (const auto valid = CanCommitPrepared(storage.state, storage.prepared, point); valid.HasError())
            return valid;
        storage.state->registry.storage_.Get()->publicationSeal = &storage;
        storage.state->scenePublication = true;
        storage.sealed = true;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiService::ScenePublication::Publish */
    void RuntimeUiService::ScenePublication::Publish() noexcept {
        if (!storage_)
            std::terminate();
        RuntimeUiService::PublishScene(*storage_);
    }

    /** @copydoc RuntimeUiService::PublishScene */
    void RuntimeUiService::PublishScene(ScenePublication::Storage &storage) noexcept {
        if (storage.state->thread != std::this_thread::get_id() || !storage.sealed || storage.published ||
            storage.prepared.storage_->abandoned || storage.state->registry.storage_.Get()->publicationSeal != &storage)
            std::terminate();
        // The generic Scene service has claimed its aggregate commit. No cancellation check, generation acquisition,
        // allocation or foreign callback is permitted between this canonical swap and the Scene's own no-fail transfer.
        for (const auto &source : storage.prepared.storage_->animationSources)
            source->retiring = true;
        (void)storage.state->registry.CommitValidated(storage.prepared);
        storage.published = true;
        storage.state->registry.storage_.Get()->publicationSeal = nullptr;
        storage.state->scenePublication = false;
        storage.sealed = false;
        Detail::FinishSceneRevocations(*storage.state);
        DiscardExpiredRoutes(*storage.state);
        if (storage.state->shutdownRequested)
            Detail::FinishShutdown(*storage.state);
    }

    /** @copydoc RuntimeUiService::ReleasePublication */
    void RuntimeUiService::ReleasePublication(ScenePublication::Storage &storage) noexcept {
        if (storage.state->thread != std::this_thread::get_id())
            std::terminate();
        if (!storage.published)
            storage.prepared.Abandon();
        if (storage.sealed) {
            if (storage.state->registry.storage_.Get()->publicationSeal != &storage)
                std::terminate();
            storage.state->registry.storage_.Get()->publicationSeal = nullptr;
            storage.state->scenePublication = false;
            storage.sealed = false;
        }
        if (storage.state->shutdownRequested && !storage.state->dispatching)
            Detail::FinishShutdown(*storage.state);
        else
            Detail::FinishSceneRevocations(*storage.state);
    }
}  // namespace Horo::Runtime::Ui

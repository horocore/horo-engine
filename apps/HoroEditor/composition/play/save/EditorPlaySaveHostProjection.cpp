#include "EditorPlaySaveHostInternal.h"

#include <array>
#include <limits>

namespace Horo::Application::Internal {
    /** @copydoc EditorPlaySaveHost::State::Publish */
    Result<void> EditorPlaySaveHost::State::Publish() {
        if (!dirty)
            return Result<void>::Success();
        if (revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        auto publication = std::make_shared<Editor::PlaySaveSnapshot>();
        publication->revision = ++revision;
        publication->availability = Availability();
        publication->error = error;
        publication->operationPending = request.has_value() || static_cast<bool>(storageWork);
        publication->loading = loading;
        if (storage && catalog) {
            auto manager = ProjectManager();
            if (manager.HasError())
                return Result<void>::Failure(manager.ErrorValue());
            publication->manager = std::make_shared<const Runtime::SaveManagerProjection>(std::move(manager).Value());
        }
        snapshot = std::move(publication);
        dirty = false;
        return Result<void>::Success();
    }

    /** @brief Derives presentation readiness from actual owner/storage/worker state; it grants no runtime capability. */
    Editor::PlaySaveAvailability EditorPlaySaveHost::State::Availability() const noexcept {
        if (retiring)
            return Editor::PlaySaveAvailability::Retiring;
        if (error)
            return Editor::PlaySaveAvailability::Failed;
        if (suspending || suspended)
            return Editor::PlaySaveAvailability::Opening;
        return storage ? Editor::PlaySaveAvailability::Ready : Editor::PlaySaveAvailability::Opening;
    }

    /** @brief Copies the real detached catalog and canonical operation progress without reading the filesystem. */
    Result<Runtime::SaveManagerProjection> EditorPlaySaveHost::State::ProjectManager() const {
        const Runtime::SaveNamespaceBindingSnapshot binding{storage->Namespace(), Runtime::SaveNamespaceBindingState::Available, 1};
        const std::array profiles{Runtime::SaveManagerProfileSource{storage->Namespace(), true}};
        std::array<Runtime::SaveManagerOperationSource, 1> operations;
        if (operation)
            operations[0].snapshot = *operation;
        const Runtime::SaveManagerProjectionLimits limits{.maximumProfiles = 1,
                                                          .maximumSlots = PlaySaveMaximumSlots,
                                                          .maximumOperations = 1,
                                                          .maximumDiagnostics = 4,
                                                          .maximumPageSize = PlaySaveMaximumSlots};
        return Runtime::SaveManagerProjection::Create({binding,
                                                       *catalog,
                                                       revision,
                                                       profiles,
                                                       {},
                                                       operation ? std::span<const Runtime::SaveManagerOperationSource>{operations}
                                                                 : std::span<const Runtime::SaveManagerOperationSource>{},
                                                       {}},
                                                      limits);
    }
}  // namespace Horo::Application::Internal

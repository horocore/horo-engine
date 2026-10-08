#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <utility>

namespace Horo::Editor {
    /** @copydoc TerrainAuthoringDocument::TerrainAuthoringDocument */
    TerrainAuthoringDocument::TerrainAuthoringDocument(TerrainAuthoringDocument &&other) noexcept : TerrainAuthoringDocument() {
        *this = std::move(other);
    }

    /** @copydoc TerrainAuthoringDocument::operator= */
    TerrainAuthoringDocument &TerrainAuthoringDocument::operator=(TerrainAuthoringDocument &&other) noexcept {
        if (this == &other)
            return *this;
        session_ = other.session_;
        source_ = std::move(other.source_);
        placements_ = std::move(other.placements_);
        capability_ = other.capability_;
        limits_ = other.limits_;
        tileCells_ = other.tileCells_;
        state_ = other.state_;
        savedState_ = other.savedState_;
        nextState_ = other.nextState_;
        undo_ = std::move(other.undo_);
        redo_ = std::move(other.redo_);
        historyBytes_ = std::exchange(other.historyBytes_, 0);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    /** @copydoc TerrainAuthoringDocument::Fence */
    TerrainEditFence TerrainAuthoringDocument::Fence() const noexcept {
        return {session_, source_.dataset, source_.revision, source_.capability};
    }

    /** @copydoc TerrainAuthoringDocument::Admit */
    Result<void> TerrainAuthoringDocument::Admit(const TerrainEditFence &fence, const CancellationToken &cancellation) const {
        if (closed_)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
        if (fence.session != session_ || fence.dataset != source_.dataset)
            return Result<void>::Failure(MakeError(TerrainEditErrors::WrongDocument));
        if (fence.expectedRevision != source_.revision)
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        if (capability_ != TerrainAuthoringCapability::Edit || fence.capabilityRevision != source_.capability)
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc TerrainAuthoringDocument::AcceptSavedState */
    Result<void> TerrainAuthoringDocument::AcceptSavedState(TerrainDocumentSessionId session, TerrainDocumentStateId state) {
        if (closed_)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        if (session != session_)
            return Result<void>::Failure(MakeError(TerrainEditErrors::WrongDocument));
        if (capability_ != TerrainAuthoringCapability::Edit)
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        if (!state.IsValid() || state.Value() >= nextState_)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
        savedState_ = state;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor

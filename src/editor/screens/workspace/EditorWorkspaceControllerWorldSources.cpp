#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::DetachWorldSourceOwner */
    void EditorWorkspaceController::DetachWorldSourceOwner() noexcept {
        m_partitionedWorld = nullptr;
        m_viewModel.worldViewport = {};
        m_viewModel.worldSourceError.reset();
    }

    /** @copydoc EditorWorkspaceController::SetWorldViewportBinding */
    Result<void> EditorWorkspaceController::SetWorldViewportBinding(const WorldViewportBinding &binding) {
        if (!m_partitionedWorld || !binding.snapshot || binding.viewRevision == 0)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        const auto current = m_partitionedWorld->Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto &previous = m_viewModel.worldViewport;
        if (binding.snapshot != current.Value() || binding.viewRevision < previous.viewRevision ||
            (binding.viewRevision == previous.viewRevision && binding.globalOrigin != previous.globalOrigin))
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
        m_viewModel.worldViewport = binding;
        RefreshWorldSourceProjection();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::RefreshWorldSourceProjection */
    void EditorWorkspaceController::RefreshWorldSourceProjection() {
        if (!m_partitionedWorld || m_viewModel.worldViewport.viewRevision == 0) {
            m_viewModel.worldViewport.snapshot.reset();
            return;
        }
        const auto snapshot = m_partitionedWorld->Snapshot();
        if (snapshot.HasError()) {
            m_viewModel.worldViewport.snapshot.reset();
            return;
        }
        m_viewModel.worldViewport.snapshot = snapshot.Value();
        for (const auto &source : snapshot.Value()->document.sources) {
            if (source.id.Value() == std::numeric_limits<std::uint64_t>::max())
                m_nextWorldSourceId = 0;
            else if (m_nextWorldSourceId != 0)
                m_nextWorldSourceId = std::max(m_nextWorldSourceId, source.id.Value() + 1);
        }
    }

    /** @copydoc EditorWorkspaceController::ProcessWorldSourceCommand */
    bool EditorWorkspaceController::ProcessWorldSourceCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::WorldSourceTool)
            return false;
        if (!m_partitionedWorld || !command.worldSource || m_viewModel.playState != EditorPlayState::Idle) {
            m_viewModel.worldSourceError = MakeError(WorldDocumentErrors::Lifecycle);
            return true;
        }
        auto request = *command.worldSource;
        if (request.viewRevision == 0 || request.viewRevision != m_viewModel.worldViewport.viewRevision ||
            request.globalOrigin != m_viewModel.worldViewport.globalOrigin) {
            m_viewModel.worldSourceError = MakeError(WorldDocumentErrors::Stale);
            return true;
        }
        Result<PartitionedWorldSnapshotLease> result =
            Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Invalid));
        switch (request.action) {
            case WorldSourceToolAction::Create:
                if (request.edit.value && m_nextWorldSourceId != 0) {
                    request.edit.source = WorldStreaming::StreamingSourceId::Create(m_nextWorldSourceId).Value();
                    request.edit.value->id = request.edit.source;
                    result = m_partitionedWorld->EditSource(request.edit);
                    if (result.HasValue())
                        m_nextWorldSourceId =
                            m_nextWorldSourceId == std::numeric_limits<std::uint64_t>::max() ? 0 : m_nextWorldSourceId + 1;
                }
                break;
            case WorldSourceToolAction::Edit:
            case WorldSourceToolAction::Remove:
                if (request.action == WorldSourceToolAction::Remove)
                    request.edit.value.reset();
                result = m_partitionedWorld->EditSource(request.edit);
                break;
            case WorldSourceToolAction::Undo:
            case WorldSourceToolAction::Redo:
                result = m_partitionedWorld->ReplaySources(request.edit.expected, request.action == WorldSourceToolAction::Redo);
                break;
        }
        m_viewModel.worldSourceError = result.HasError() ? std::optional<Error>{result.ErrorValue()} : std::nullopt;
        RefreshWorldSourceProjection();
        return true;
    }
}  // namespace Horo::Editor

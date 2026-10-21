#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Executes an existing source/history intent without manufacturing a missing value or identity. */
        Result<PartitionedWorldSnapshotLease> ExecuteExistingSource(PartitionedWorldWorkspace &owner,
                                                                    const WorldSourceToolCommand &request) {
            using enum WorldSourceToolAction;
            switch (request.action) {
                case Edit:
                    if (request.edit.value)
                        return owner.EditSource(request.edit);
                    return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Invalid));
                case Remove: {
                    auto edit = request.edit;
                    edit.value.reset();
                    return owner.EditSource(edit);
                }
                case Undo:
                case Redo:
                    return owner.ReplaySources(request.edit.expected, request.action == WorldSourceToolAction::Redo);
                default:
                    return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Unsupported));
            }
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::DetachWorldSourceOwner */
    void EditorWorkspaceController::DetachWorldSourceOwner() noexcept {
        m_partitionedWorld = nullptr;
        m_viewModel.worldViewport = {};
        m_viewModel.worldSourceError.reset();
        m_viewModel.worldDocument.reset();
        m_viewModel.worldBrowser.reset();
        m_viewModel.worldDocumentSource.clear();
    }

    /** @copydoc EditorWorkspaceController::SetWorldViewportBinding(const WorldViewportBinding &) */
    Result<void> EditorWorkspaceController::SetWorldViewportBinding(const WorldViewportBinding &binding) {
        if (!m_partitionedWorld)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        return SetWorldViewportBinding(*m_partitionedWorld, binding);
    }

    /** @copydoc EditorWorkspaceController::SetWorldViewportBinding(PartitionedWorldWorkspace &, const WorldViewportBinding &) */
    Result<void> EditorWorkspaceController::SetWorldViewportBinding(PartitionedWorldWorkspace &canonicalOwner,
                                                                    const WorldViewportBinding &binding) {
        if (m_partitionedWorld && m_partitionedWorld != &canonicalOwner)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Conflict));
        if (!binding.snapshot || binding.viewRevision == 0)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        const auto current = canonicalOwner.Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto &previous = m_viewModel.worldViewport;
        if (const bool replaced =
                previous.viewRevision != 0 && (!previous.snapshot || previous.snapshot->fence.owner != binding.snapshot->fence.owner ||
                                               previous.snapshot->fence.generation != binding.snapshot->fence.generation);
            binding.snapshot != current.Value() || binding.viewRevision < previous.viewRevision ||
            (binding.viewRevision == previous.viewRevision && (binding.globalOrigin != previous.globalOrigin || replaced)))
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
        m_partitionedWorld = &canonicalOwner;
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
        if (const auto previous = m_viewModel.worldViewport.snapshot; !previous || previous->fence.owner != snapshot.Value()->fence.owner ||
                                                                      previous->fence.generation != snapshot.Value()->fence.generation) {
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
        if (!m_partitionedWorld || !m_viewModel.worldViewport.snapshot || !command.worldSource ||
            m_viewModel.playState != EditorPlayState::Idle) {
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
        if (request.action == WorldSourceToolAction::Create) {
            if (m_nextWorldSourceId == 0) {
                result = Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Exhausted));
            } else if (request.edit.value) {
                request.edit.source = WorldStreaming::StreamingSourceId::Create(m_nextWorldSourceId).Value();
                request.edit.value->id = request.edit.source;
                result = m_partitionedWorld->EditSource(request.edit);
                if (result.HasValue())
                    m_nextWorldSourceId = m_nextWorldSourceId == std::numeric_limits<std::uint64_t>::max() ? 0 : m_nextWorldSourceId + 1;
            }
        } else {
            result = ExecuteExistingSource(*m_partitionedWorld, request);
        }
        m_viewModel.worldSourceError = result.HasError() ? std::optional<Error>{result.ErrorValue()} : std::nullopt;
        RefreshWorldSourceProjection();
        return true;
    }
}  // namespace Horo::Editor

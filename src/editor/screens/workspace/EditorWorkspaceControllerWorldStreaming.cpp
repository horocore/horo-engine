#include "Horo/Platform/ProjectReadFiles.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <chrono>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Reads actual canonical index bytes through retained project authority, with no redirects or draw-path I/O. */
        Result<WS::DecodedWorldIndex> ReadIndex(const Platform::IProjectReadFiles &files, const std::string_view path,
                                                const Assets::AssetId workspace, const WorldStreamingPreviewRuntimePolicy &policy) {
            if (!path.ends_with(".index") || !Platform::IsSafeProjectReadPath(path))
                return Result<WS::DecodedWorldIndex>::Failure(MakeError(WorldDocumentErrors::Invalid));
            const Platform::ProjectReadContext context{.projectIdentity = workspace.ToString(),
                                                       .projectGeneration = 1,
                                                       .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5)};
            const auto bytes = files.SourceText(path, context,
                                                {.maximumFileBytes = policy.artifact.maximumArtifactBytes,
                                                 .maximumDuration = std::chrono::milliseconds(5000)});
            if (bytes.HasError())
                return Result<WS::DecodedWorldIndex>::Failure(bytes.ErrorValue());
            const auto &owned = *bytes.Value().text;
            auto decoded = WS::DecodeWorldIndexArtifact(std::as_bytes(std::span(owned.data(), owned.size())), policy.artifact);
            if (decoded.HasError())
                return decoded;
            if (decoded.Value().coordinates != WS::WorldIndexCoordinateSystem::RightHandedYUp)
                return Result<WS::DecodedWorldIndex>::Failure(MakeError(WorldDocumentErrors::Unsupported));
            return decoded;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::MountWorldStreamingPreview */
    Result<void> EditorWorkspaceController::MountWorldStreamingPreview(const std::string_view path, const WorldDocumentFence expected) {
        if (!m_streamingPreview || !m_streamingPreviewPolicy || !m_worldSourceFiles)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        const auto &view = m_viewModel.worldViewport;
        if (!view.snapshot || view.snapshot->fence != expected)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
        auto decoded = ReadIndex(*m_worldSourceFiles, path, m_workspaceSession, *m_streamingPreviewPolicy);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        auto index = std::move(decoded).Value();
        if (index.descriptor.Partition() != view.snapshot->document.settings.partition)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
        const auto owner = m_streamingPreview->IssueMountIdentity(index.descriptor.Partition());
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        auto candidate =
            WorldStreamingPreviewRuntime::Create(owner.Value(), expected, std::move(index.descriptor), *m_streamingPreviewPolicy);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        m_pendingStreamingPath = path;
        m_pendingStreamingRuntime = std::move(candidate).Value();
        m_viewModel.worldStreamingPreparing = true;
        m_viewModel.worldStreamingMountError.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::StopWorldStreamingPreview */
    void EditorWorkspaceController::StopWorldStreamingPreview() noexcept {
        m_viewModel.worldStreamingPreview.reset();
        if (m_streamingRuntime) {
            if (m_streamingPreview)
                static_cast<void>(m_streamingPreview->UnbindRuntime(m_streamingRuntime->Owner()));
            static_cast<void>(m_streamingRuntime->Shutdown());
            m_streamingRuntime.reset();
        }
        m_pendingStreamingRuntime.reset();
        m_pendingStreamingPath.clear();
        m_viewModel.worldStreamingIndexPath.clear();
        m_viewModel.worldStreamingPreparing = false;
    }

    /** @copydoc EditorWorkspaceController::CommitPendingWorldStreamingPreview */
    Result<void> EditorWorkspaceController::CommitPendingWorldStreamingPreview() {
        const auto *diagnostics = m_pendingStreamingRuntime->Diagnostics();
        if (!diagnostics)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        if (m_streamingRuntime) {
            const auto unbound = m_streamingPreview->UnbindRuntime(m_streamingRuntime->Owner());
            if (unbound.HasError())
                return unbound;
        }
        const auto bound =
            m_streamingPreview->BindRuntime(m_pendingStreamingRuntime->Registry(), m_pendingStreamingRuntime->Planner(), *diagnostics);
        if (bound.HasError()) {
            if (m_streamingRuntime && m_streamingRuntime->Diagnostics())
                static_cast<void>(m_streamingPreview->BindRuntime(m_streamingRuntime->Registry(), m_streamingRuntime->Planner(),
                                                                  *m_streamingRuntime->Diagnostics()));
            return bound;
        }
        // The old native owner is already unbound before unique ownership replacement shuts it down.
        m_streamingRuntime = std::move(m_pendingStreamingRuntime);
        m_viewModel.worldStreamingIndexPath = std::move(m_pendingStreamingPath);
        m_viewModel.worldStreamingPreparing = false;
        m_viewModel.worldStreamingMountError.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ServiceWorldStreamingPreview */
    void EditorWorkspaceController::ServiceWorldStreamingPreview() {
        const auto &source = m_viewModel.worldViewport.snapshot;
        if (!source) {
            StopWorldStreamingPreview();
            return;
        }
        if (m_pendingStreamingRuntime) {
            const auto prepared = m_pendingStreamingRuntime->Service(source);
            if (prepared.HasError()) {
                m_viewModel.worldStreamingMountError = prepared.ErrorValue();
                m_pendingStreamingRuntime.reset();
                m_viewModel.worldStreamingPreparing = false;
            } else if (prepared.Value()) {
                const auto committed = CommitPendingWorldStreamingPreview();
                if (committed.HasError()) {
                    m_viewModel.worldStreamingMountError = committed.ErrorValue();
                    m_pendingStreamingRuntime.reset();
                    m_viewModel.worldStreamingPreparing = false;
                }
            }
        }
        if (m_streamingRuntime) {
            const auto current = m_streamingRuntime->Service(source);
            if (current.HasError()) {
                m_viewModel.worldStreamingMountError = current.ErrorValue();
                StopWorldStreamingPreview();
            }
        }
    }

    /** @copydoc EditorWorkspaceController::ProcessWorldStreamingPreviewCommand */
    bool EditorWorkspaceController::ProcessWorldStreamingPreviewCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::MountWorldStreamingPreview &&
            command.command != EditorWorkspaceViewCommand::UnmountWorldStreamingPreview)
            return false;
        const auto &view = m_viewModel.worldViewport;
        if (!view.snapshot || !command.worldDocumentFence || *command.worldDocumentFence != view.snapshot->fence) {
            m_viewModel.worldStreamingMountError = MakeError(WorldDocumentErrors::Stale);
            return true;
        }
        if (command.command == EditorWorkspaceViewCommand::UnmountWorldStreamingPreview) {
            StopWorldStreamingPreview();
            m_viewModel.worldStreamingMountError.reset();
        } else {
            const auto mounted = command.stringPayload ? MountWorldStreamingPreview(*command.stringPayload, *command.worldDocumentFence)
                                                       : Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
            if (mounted.HasError())
                m_viewModel.worldStreamingMountError = mounted.ErrorValue();
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::RefreshWorldStreamingPreview */
    void EditorWorkspaceController::RefreshWorldStreamingPreview() {
        const auto &view = m_viewModel.worldViewport;
        if (!view.snapshot) {
            m_viewModel.worldStreamingPreview.reset();
            m_viewModel.worldStreamingPreviewError.reset();
            if (m_streamingPreview)
                m_streamingPreview->ClearPresentation();
            return;
        }
        if (!m_streamingPreview) {
            m_viewModel.worldStreamingPreview.reset();
            m_viewModel.worldStreamingPreviewError = MakeError(WorldDocumentErrors::Lifecycle);
            return;
        }
        const auto captured = m_streamingPreview->Refresh(view);
        m_viewModel.worldStreamingPreview = captured.HasValue() ? captured.Value() : nullptr;
        m_viewModel.worldStreamingPreviewError = captured.HasError() ? std::optional<Error>{captured.ErrorValue()} : std::nullopt;
    }
}  // namespace Horo::Editor

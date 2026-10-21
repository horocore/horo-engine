#include "Horo/Platform/ProjectReadFiles.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::RefreshWorldDocumentProjection */
    void EditorWorkspaceController::RefreshWorldDocumentProjection(const bool forceQuery) {
        auto &world = m_partitionedWorld ? *m_partitionedWorld : WorldDocuments();
        const auto current = world.Snapshot();
        const auto lease = current.HasValue() ? current.Value() : PartitionedWorldSnapshotLease{};
        if (m_viewModel.worldViewport.snapshot != lease)
            RefreshWorldSourceProjection();
        RefreshWorldStreamingPreview();
        if (!forceQuery && m_viewModel.worldDocument == lease)
            return;
        m_viewModel.worldDocument = lease;
        m_viewModel.worldBrowser.reset();
        if (!lease) {
            m_viewModel.worldViewport.snapshot.reset();
            return;
        }
        if (m_viewModel.worldBrowserQuery.layer && std::ranges::none_of(lease->document.layers, [&](const auto &layer) {
            return layer.id == *m_viewModel.worldBrowserQuery.layer;
        }))
            m_viewModel.worldBrowserQuery.layer.reset();
        const auto browser = world.Browse(m_viewModel.worldBrowserQuery, lease->fence);
        if (browser.HasValue()) {
            m_viewModel.worldBrowser = browser.Value();
            m_viewModel.worldDocumentError.reset();
        } else {
            m_viewModel.worldDocumentError = browser.ErrorValue();
        }
    }

    /** @copydoc EditorWorkspaceController::OpenWorldCompositionSource */
    Result<void> EditorWorkspaceController::OpenWorldCompositionSource(const std::string_view relativePath,
                                                                       const std::optional<WorldDocumentFence> expected) {
        if (m_partitionedWorld && m_partitionedWorld != &WorldDocuments())
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Conflict));
        if (m_viewModel.worldViewport.viewRevision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Exhausted));
        if (!relativePath.ends_with(".horoworld") || !Platform::IsSafeProjectReadPath(relativePath))
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
        const auto current = WorldDocuments().Snapshot();
        if ((current.HasValue() && (!expected || *expected != current.Value()->fence)) || (current.HasError() && expected))
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
        const std::string path{relativePath};
        if (!m_worldSourceFiles)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        const Platform::ProjectReadContext context{.projectIdentity = m_workspaceSession.ToString(),
                                                   .projectGeneration = 1,
                                                   .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        const auto source =
            m_worldSourceFiles->SourceText(relativePath, context,
                                           {.maximumFileBytes = 8 * 1024 * 1024, .maximumDuration = std::chrono::milliseconds(5000)});
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        auto decoded = DecodePartitionedWorldDocument(*source.Value().text);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        const auto opened = expected ? WorldDocuments().Replace(decoded.Value(), *expected) : WorldDocuments().Open(decoded.Value());
        if (opened.HasError())
            return Result<void>::Failure(opened.ErrorValue());
        const auto bound = SetWorldViewportBinding(WorldDocuments(), {opened.Value(), decoded.Value().settings.grid.Origin(),
                                                                      m_viewModel.worldViewport.viewRevision + 1});
        HORO_INVARIANT_MSG(bound.HasValue(), "World source admission and view binding must use the same canonical owner.");
        m_viewModel.worldDocumentSource = path;
        m_viewModel.worldBrowserQuery.layer.reset();
        RefreshWorldDocumentProjection(true);
        EditorWorkspaceViewCommandData focus;
        focus.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        focus.targetIndex = static_cast<int>(WorkspaceDockArea::Document);
        focus.stringPayload = "horo.world_composition";
        static_cast<void>(ProcessActivePanelCommand(focus));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessWorldCompositionCommand */
    bool EditorWorkspaceController::ProcessWorldCompositionCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == OpenWorldCompositionSource) {
            const auto opened = command.stringPayload ? OpenWorldCompositionSource(*command.stringPayload, command.worldDocumentFence)
                                                      : Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
            if (opened.HasError())
                m_viewModel.worldDocumentError = opened.ErrorValue();
            return true;
        }
        if (command.command != QueryWorldComposition && command.command != CloseWorldComposition)
            return false;
        auto &world = m_partitionedWorld ? *m_partitionedWorld : WorldDocuments();
        const auto current = world.Snapshot();
        if (current.HasError() || !command.worldDocumentFence || current.Value()->fence != *command.worldDocumentFence) {
            m_viewModel.worldDocumentError = MakeError(WorldDocumentErrors::Stale);
            return true;
        }
        if (command.command == CloseWorldComposition) {
            const auto closed = world.Close(*command.worldDocumentFence);
            if (closed.HasError())
                m_viewModel.worldDocumentError = closed.ErrorValue();
            else {
                m_viewModel.worldDocumentSource.clear();
                m_viewModel.worldDocumentError.reset();
            }
            RefreshWorldDocumentProjection();
            return true;
        }
        const auto queried = command.worldBrowserQuery ? world.Browse(*command.worldBrowserQuery, *command.worldDocumentFence)
                                                       : Result<WorldBrowserSnapshot>::Failure(MakeError(WorldDocumentErrors::Invalid));
        if (queried.HasError()) {
            m_viewModel.worldDocumentError = queried.ErrorValue();
            return true;
        }
        m_viewModel.worldBrowserQuery = *command.worldBrowserQuery;
        m_viewModel.worldBrowser = queried.Value();
        m_viewModel.worldDocumentError.reset();
        return true;
    }
}  // namespace Horo::Editor

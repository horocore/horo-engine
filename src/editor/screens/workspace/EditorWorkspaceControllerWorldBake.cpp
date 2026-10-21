#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::InitializeWorldBake */
    void EditorWorkspaceController::InitializeWorldBake(const EditorWorkspaceDependencies &dependencies) {
        m_worldBakeOperations = dependencies.worldBakeOperations;
        if (dependencies.jobs && dependencies.worldBakeOperations && dependencies.worldBakeOutput)
            m_worldBake = std::make_unique<Application::WorldBakeApplication>(*dependencies.jobs, *dependencies.worldBakeOperations,
                                                                              *dependencies.worldBakeOutput);
        if (dependencies.worldBakeProject)
            m_worldBakeProject = *dependencies.worldBakeProject;
        else {
            constexpr std::string_view core = "Horo.Editor.WorldBake.Core.v1";
            const auto digest = ComputeSha256(std::as_bytes(std::span{core.data(), core.size()}));
            m_worldBakeProject.fence = {dependencies.documentSessionOwner, 1, digest};
            m_worldBakeProject.settings = digest;
            m_worldBakeProject.triggers = dependencies.triggerBinding;
        }
        m_viewModel.worldBakeAvailable = m_worldBake != nullptr;
    }

    /** @copydoc EditorWorkspaceController::SetWorldBakeProject */
    Result<void> EditorWorkspaceController::SetWorldBakeProject(Application::WorldBakeProjectCapture capture) {
        if (auto admitted = Application::ValidateWorldBakeProjectCapture(capture); admitted.HasError())
            return admitted;
        m_worldBakeProject = std::move(capture);
        UpdateWorldBake();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::StartWorldBake */
    Result<OperationId> EditorWorkspaceController::StartWorldBake(const Application::WorldBakeMode mode,
                                                                  const WorldDocumentFence expected) {
        if (!m_worldBake)
            return Result<OperationId>::Failure(MakeError(Application::WorldBakeErrors::Closed));
        auto &owner = m_partitionedWorld ? *m_partitionedWorld : WorldDocuments();
        const auto current = owner.Snapshot();
        if (current.HasError())
            return Result<OperationId>::Failure(current.ErrorValue());
        if (current.Value()->fence != expected)
            return Result<OperationId>::Failure(MakeError(WorldDocumentErrors::Stale));
        auto project = m_worldBakeProject;
        project.fence.registryRevision = m_assetRegistry.Revision().value;
        Application::WorldBakeRequest request{.document = current.Value(),
                                              .project = std::move(project),
                                              .files = m_worldSourceFiles,
                                              .mode = mode};
        if (m_localization)
            request.title = m_localization->Get("editor", mode == Application::WorldBakeMode::Bake ? "workspace.world.bake"
                                                                                                   : "workspace.world.validate");
        for (const auto &page : current.Value()->document.pages) {
            const auto *record = m_assetRegistry.Find(page.head.sourceAsset);
            if (record) {
                const auto relative = record->sourcePath.String();
                const auto absolute = (std::filesystem::path{m_viewModel.projectRoot} / relative).lexically_normal();
                request.sources.push_back({page.head, {absolute.string(), 1, 1}, relative});
            } else if (!m_viewModel.worldDocumentSource.empty()) {
                const auto absolute = (std::filesystem::path{m_viewModel.projectRoot} / m_viewModel.worldDocumentSource).lexically_normal();
                request.sources.push_back({page.head, {absolute.string(), 1, 1}, {}});
            }
        }
        const auto accepted = m_worldBake->Submit(std::move(request));
        UpdateWorldBake();
        return accepted;
    }

    /** @copydoc EditorWorkspaceController::UpdateWorldBake */
    void EditorWorkspaceController::UpdateWorldBake() {
        if (!m_worldBake)
            return;
        const auto current = (m_partitionedWorld ? *m_partitionedWorld : WorldDocuments()).Snapshot();
        auto project = m_worldBakeProject.fence;
        project.registryRevision = m_assetRegistry.Revision().value;
        const auto lease = current.HasValue() ? current.Value() : PartitionedWorldSnapshotLease{};
        m_worldBake->Pump(lease, project);
        m_viewModel.worldBakeOperation = m_worldBake->ActiveOperation();
        const auto output = m_worldBake->Published();
        m_viewModel.worldBakeCurrent = output && lease && output->document == lease->fence && output->project == project;
    }

    /** @copydoc EditorWorkspaceController::RequestWorldBakeCancellation */
    Result<void> EditorWorkspaceController::RequestWorldBakeCancellation(const std::optional<OperationId> expected) {
        if (!m_worldBakeOperations || !expected || expected != m_viewModel.worldBakeOperation)
            return Result<void>::Failure(MakeError(Application::WorldBakeErrors::Stale));
        if (!m_worldBakeOperations->RequestCancel(*expected))
            return Result<void>::Failure(MakeError(Application::WorldBakeErrors::Stale));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessWorldBakeCommand */
    bool EditorWorkspaceController::ProcessWorldBakeCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == CancelWorldBake) {
            if (auto cancelled = RequestWorldBakeCancellation(command.worldBakeOperation); cancelled.HasError())
                m_viewModel.worldDocumentError = cancelled.ErrorValue();
            return true;
        }
        if (command.command != BakeWorldCells && command.command != ValidateWorldCells)
            return false;
        const auto accepted = command.worldDocumentFence
                                  ? StartWorldBake(command.command == BakeWorldCells ? Application::WorldBakeMode::Bake
                                                                                     : Application::WorldBakeMode::Validate,
                                                   *command.worldDocumentFence)
                                  : Result<OperationId>::Failure(MakeError(WorldDocumentErrors::Invalid));
        m_viewModel.worldDocumentError = accepted.HasError() ? std::optional{accepted.ErrorValue()} : std::nullopt;
        return true;
    }
}  // namespace Horo::Editor
